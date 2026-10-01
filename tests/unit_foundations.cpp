// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Foundations: checked arithmetic, exact quantities, digests, identities,
// timestamps, limits and the interchange parser. These are the layers every
// other claim rests on, so they are tested against their own specifications
// rather than against the library's behaviour.

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/checked.hpp"
#include "dccp/cooling_observatory/checksum.hpp"
#include "dccp/cooling_observatory/clock.hpp"
#include "dccp/cooling_observatory/limits.hpp"
#include "dccp/cooling_observatory/textproto.hpp"
#include "dccp/cooling_observatory/units.hpp"
#include "dccp/cooling_observatory/version.hpp"
#include "support.hpp"
#include "test_harness.hpp"

using namespace dccp::cooling_observatory;

namespace {

int64_t require_i64(const std::optional<std::int64_t>& value) {
  CO_REQUIRE(value.has_value());
  return value.value();
}

/// Present an optional as a Result so that the error-reporting assertion macro
/// applies to both. An absent optional is reported as the outcome the parser
/// would have produced for malformed input.
Result<Quantity> as_result(const std::optional<Quantity>& value) {
  if (!value.has_value()) {
    return Error(Code::MalformedInput, "empty_optional", "the optional was empty");
  }
  return value.value();
}

Result<Quantity> as_result(const Result<Quantity>& value) { return value; }

}  // namespace

CO_TEST("checked addition refuses overflow rather than wrapping") {
  CO_CHECK_EQ(require_i64(checked_add(2, 3)), 5);
  CO_CHECK_EQ(require_i64(checked_add(kMaxInt64, 0)), kMaxInt64);
  CO_CHECK(!checked_add(kMaxInt64, 1).has_value());
  CO_CHECK(!checked_add(kMinInt64, -1).has_value());
  CO_CHECK_EQ(require_i64(checked_add(kMinInt64, kMaxInt64)), -1);
}

CO_TEST("checked subtraction handles the most negative value") {
  // 0 - min is +2^63, which a signed 64-bit value cannot hold even though it is
  // exactly kMinInt64 reinterpreted, so it is an overflow and not a value.
  CO_CHECK(!checked_sub(0, kMinInt64).has_value());
  CO_CHECK(!checked_sub(1, kMinInt64).has_value());
  // -1 - min is max, which is representable and therefore computed.
  CO_CHECK_EQ(require_i64(checked_sub(-1, kMinInt64)), kMaxInt64);
  CO_CHECK_EQ(require_i64(checked_sub(kMinInt64, kMinInt64)), 0);
  CO_CHECK_EQ(require_i64(checked_sub(5, 9)), -4);
}

CO_TEST("checked multiplication refuses every overflowing case") {
  CO_CHECK_EQ(require_i64(checked_mul(0, kMinInt64)), 0);
  CO_CHECK_EQ(require_i64(checked_mul(-1, kMaxInt64)), -kMaxInt64);
  CO_CHECK(!checked_mul(-1, kMinInt64).has_value());
  CO_CHECK(!checked_mul(kMaxInt64, 2).has_value());
  CO_CHECK_EQ(require_i64(checked_mul(3037000499LL, 3037000499LL)), 9223372030926249001LL);
  CO_CHECK(!checked_mul(3037000500LL, 3037000500LL).has_value());
  // The most negative value times one is representable; times two is not.
  CO_CHECK_EQ(require_i64(checked_mul(kMinInt64, 1)), kMinInt64);
  CO_CHECK(!checked_mul(kMinInt64, 2).has_value());
  CO_CHECK(!checked_mul(kMinInt64, kMinInt64).has_value());
}

CO_TEST("checked division refuses zero and the one overflowing quotient") {
  CO_CHECK(!checked_div(1, 0).has_value());
  CO_CHECK(!checked_div(kMinInt64, -1).has_value());
  CO_CHECK_EQ(require_i64(checked_div(kMinInt64, 1)), kMinInt64);
  CO_CHECK_EQ(require_i64(checked_div(-7, 2)), -3);
}

CO_TEST("multiply-divide keeps full precision in the intermediate product") {
  // 9.2e18 / 1 is representable, but the naive value * num would overflow if the
  // intermediate were a 64-bit product.
  CO_CHECK_EQ(require_i64(checked_mul_div(9223372036854775807LL, 1, 1)), kMaxInt64);
  CO_CHECK_EQ(require_i64(checked_mul_div(9223372036854775807LL, 2, 2)), kMaxInt64);
  CO_CHECK_EQ(require_i64(checked_mul_div(kMinInt64, 2, 2)), kMinInt64);
  // min * -1 is +2^63, which is not representable, so the quotient is refused
  // rather than returned as the bit pattern that looks like min.
  CO_CHECK(!checked_mul_div(kMinInt64, -1, 1).has_value());
  CO_CHECK_EQ(require_i64(checked_mul_div(1000000, 4180000, 1000000)), 4180000);
  // A product that does not fit 64 bits but whose quotient does.
  CO_CHECK_EQ(require_i64(checked_mul_div(4611686018427387904LL, 4, 4)), 4611686018427387904LL);
  CO_CHECK_EQ(require_i64(checked_mul_div(7, 3, 2)), 10);
  CO_CHECK_EQ(require_i64(checked_mul_div(-7, 3, 2)), -10);
  CO_CHECK_EQ(require_i64(checked_mul_div(-7, -3, 2)), 10);
  CO_CHECK(!checked_mul_div(1, 1, 0).has_value());
  CO_CHECK(!checked_mul_div(kMaxInt64, 2, 1).has_value());
}

CO_TEST("absolute value and summation report their boundary cases") {
  CO_CHECK(!checked_abs(kMinInt64).has_value());
  CO_CHECK_EQ(require_i64(checked_abs(-5)), 5);
  const std::int64_t values[] = {1, 2, 3, -6};
  CO_CHECK_EQ(require_i64(checked_sum(values, 4)), 0);
  const std::int64_t overflow[] = {kMaxInt64, 1};
  CO_CHECK(!checked_sum(overflow, 2).has_value());
}

CO_TEST("CRC-32C matches the published check value") {
  // The CRC-32C check value for "123456789" is 0xE3069283.
  CO_CHECK_EQ(crc32c(std::string_view("123456789")), 0xE3069283u);
  CO_CHECK_EQ(crc32c(std::string_view("")), 0u);
  // A single flipped bit changes the checksum.
  CO_CHECK_NE(crc32c(std::string_view("abcdef")), crc32c(std::string_view("abcdeg")));
}

CO_TEST("SHA-256 matches the published test vectors") {
  CO_CHECK_EQ(sha256_hex(std::string_view("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CO_CHECK_EQ(sha256_hex(std::string_view("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CO_CHECK_EQ(sha256_hex(std::string_view(
                  "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // A message that crosses several compression blocks exercises the buffering.
  const std::string long_message(1000, 'a');
  CO_CHECK_EQ(sha256_hex(long_message),
              "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

CO_TEST("SHA-256 streaming agrees with one-shot hashing") {
  Sha256 hasher;
  for (int i = 0; i < 100; ++i) {
    hasher.update(std::string_view("chunk"));
  }
  std::string joined;
  for (int i = 0; i < 100; ++i) {
    joined += "chunk";
  }
  CO_CHECK_EQ(hasher.hex_digest(), sha256_hex(joined));
}

CO_TEST("digest normalisation accepts both cases and rejects everything else") {
  const std::string upper = "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855";
  const auto normalised = normalise_sha256_hex(upper);
  CO_REQUIRE_OK(normalised);
  CO_CHECK_EQ(normalised.value(), sha256_hex(std::string_view("")));
  CO_CHECK_ERR(normalise_sha256_hex("abc"), Code::MalformedInput);
  CO_CHECK_ERR(normalise_sha256_hex(std::string(64, 'z')), Code::MalformedInput);
}

CO_TEST("identities are validated once and reject every unsafe byte") {
  CO_REQUIRE_OK(StrongId::parse("loop.primary"));
  CO_REQUIRE_OK(StrongId::parse("_x"));
  CO_CHECK_ERR(StrongId::parse(""), Code::InvalidArgument);
  CO_CHECK_ERR(StrongId::parse("1loop"), Code::InvalidArgument);
  CO_CHECK_ERR(StrongId::parse("loop primary"), Code::InvalidArgument);
  // A forward slash is part of the accepted identity vocabulary: adjacent DCCP
  // authorities use path-shaped identities, and this runtime must be able to
  // name what they name.
  CO_REQUIRE_OK(StrongId::parse("loop/primary"));
  CO_CHECK_ERR(StrongId::parse("loop\\primary"), Code::InvalidArgument);
  CO_CHECK_ERR(StrongId::parse(std::string(kMaxIdentityLength + 1, 'a')), Code::LimitExceeded);
  CO_CHECK(StrongId::valid("zone.a"));
  CO_CHECK(!StrongId::valid(""));
}

CO_TEST("quantities render canonically and parse back to themselves") {
  const Quantity values[] = {
      Quantity::flow(0),        Quantity::flow(-1),        Quantity::flow(123456789),
      Quantity::pressure(-1200), Quantity::temperature(12500), Quantity::power(1234567),
      Quantity::energy(-5),     Quantity::volume(7),       Quantity::frequency(50000000),
  };
  for (const Quantity& value : values) {
    const std::string rendered = render_quantity(value);
    const auto parsed = parse_quantity(rendered);
    CO_REQUIRE_OK(parsed);
    CO_CHECK(parsed.value() == value);
  }
  CO_CHECK_EQ(render_quantity(Quantity::flow(-1)), "-1ul/s");
  CO_CHECK_EQ(render_quantity(Quantity::temperature(12500)), "12500mK");
}

CO_TEST("quantities accept every published unit spelling and convert exactly") {
  const auto litres_per_minute = parse_quantity("60l/min");
  CO_REQUIRE_OK(litres_per_minute);
  CO_CHECK(litres_per_minute.value().dimension == Dimension::Flow);
  CO_CHECK_EQ(litres_per_minute.value().value, 1000000000);

  const auto litres_per_second = parse_quantity("1l/s");
  CO_REQUIRE_OK(litres_per_second);
  CO_CHECK_EQ(litres_per_second.value().value, 1000000);
  const auto cubic_metres_per_hour = parse_quantity("3.6m3/h");
  CO_REQUIRE_OK(cubic_metres_per_hour);
  CO_CHECK_EQ(cubic_metres_per_hour.value().value, 1000000);  // exactly 1 L/s

  const auto millilitres = parse_quantity("1500ml/s");
  CO_REQUIRE_OK(millilitres);
  CO_CHECK_EQ(millilitres.value().value, 1500000);

  const auto kilopascals = parse_quantity("2.5kPa");
  CO_REQUIRE_OK(kilopascals);
  CO_CHECK_EQ(kilopascals.value().value, 2500);

  const auto fahrenheit = parse_quantity("-40degF");
  CO_REQUIRE_OK(fahrenheit);
  // A Fahrenheit difference of -40 is a Celsius difference of -22.2 K.
  CO_CHECK_EQ(fahrenheit.value().value, -22222);

  const auto percent = parse_quantity("12.5%");
  CO_REQUIRE_OK(percent);
  CO_CHECK(percent.value().dimension == Dimension::Ratio);
  CO_CHECK_EQ(percent.value().value, 125000);
  CO_CHECK_EQ(render_ppm_as_percent(125000), "12.5%");
}

CO_TEST("quantity parsing refuses malformed input with a reason") {
  CO_CHECK_ERR(as_result(parse_quantity("")), Code::MalformedInput);
  CO_CHECK_ERR(as_result(parse_quantity("12")), Code::MalformedInput);
  CO_CHECK_ERR(as_result(parse_quantity("12furlongs")), Code::MalformedInput);
  CO_CHECK_ERR(as_result(parse_quantity("12.")), Code::MalformedInput);
  CO_CHECK_ERR(as_result(parse_quantity(".5l/s")), Code::MalformedInput);
  CO_CHECK_ERR(as_result(parse_quantity("1e9ul/s")), Code::MalformedInput);
  CO_CHECK_ERR(as_result(parse_quantity("999999999999999999999ul/s")), Code::LimitExceeded);
  CO_CHECK_ERR(as_result(parse_quantity_in("12Pa", Dimension::Flow)), Code::UnsupportedValue);
  CO_CHECK_ERR(parse_percent_as_ppm("12"), Code::MalformedInput);
  CO_CHECK_ERR(parse_percent_as_ppm("12 %%"), Code::MalformedInput);
}

CO_TEST("quantity arithmetic is dimension checked") {
  const Quantity flow = Quantity::flow(1000);
  const Quantity pressure = Quantity::pressure(1000);
  CO_CHECK(!add(flow, pressure).has_value());
  CO_CHECK(!sub(flow, pressure).has_value());
  CO_CHECK(!compare(flow, pressure).has_value());
  CO_CHECK(!within_tolerance(flow, pressure, Quantity::flow(1)).has_value());
  CO_CHECK_EQ(compare(flow, Quantity::flow(999)).value_or(99), 1);
  CO_CHECK(within_tolerance(flow, Quantity::flow(1001), Quantity::flow(1)).value_or(false));
  CO_CHECK(!within_tolerance(flow, Quantity::flow(1002), Quantity::flow(1)).value_or(true));
  const auto scaled = as_result(scale(flow, 1, 3));
  CO_REQUIRE_OK(scaled);
  CO_CHECK_EQ(scaled.value().value, 333);
  const auto derated = as_result(apply_ratio_ppm(Quantity::flow(1000000), -100000));
  CO_REQUIRE_OK(derated);
  CO_CHECK_EQ(derated.value().value, 900000);
  const auto increased = as_result(apply_ratio_ppm(Quantity::flow(1000000), 100000));
  CO_REQUIRE_OK(increased);
  CO_CHECK_EQ(increased.value().value, 1100000);
  CO_CHECK(!add(Quantity::flow(kMaxInt64), Quantity::flow(1)).has_value());
}

CO_TEST("timestamps format and parse round-trip including the epoch") {
  const TimestampMs values[] = {0, 1, -1, 1'800'000'000'000LL, -1'800'000'000'000LL, 951782400000LL};
  for (const TimestampMs value : values) {
    const std::string rendered = format_timestamp(value);
    const auto parsed = parse_timestamp(rendered);
    CO_REQUIRE_OK(parsed);
    CO_CHECK_EQ(parsed.value(), value);
  }
  CO_CHECK_EQ(format_timestamp(0), "1970-01-01T00:00:00.000Z");
  CO_CHECK_EQ(format_timestamp(1'800'000'000'000LL), "2027-01-15T08:00:00.000Z");
  CO_CHECK_ERR(parse_timestamp("2027-01-15"), Code::MalformedInput);
  CO_CHECK_ERR(parse_timestamp("2027-13-15T08:00:00Z"), Code::MalformedInput);
  CO_CHECK_ERR(parse_timestamp("2027-01-15T08:00:00+01:00"), Code::MalformedInput);
  // A space is accepted where the 'T' belongs, so that a timestamp copied out
  // of a log line parses.
  CO_REQUIRE_OK(parse_timestamp("2027-01-15 08:00:00Z"));
  const auto fractional = parse_timestamp("2027-01-15T08:00:00.123456Z");
  CO_REQUIRE_OK(fractional);
  CO_CHECK_EQ(fractional.value(), 1'800'000'000'123LL);
}

CO_TEST("elapsed time saturates instead of wrapping") {
  CO_CHECK_EQ(elapsed_ms(1000, 400), 600);
  CO_CHECK_EQ(elapsed_ms(400, 1000), -600);
  CO_CHECK_EQ(elapsed_ms(kMaxInt64, kMinInt64), std::numeric_limits<DurationMs>::max());
  CO_CHECK_EQ(elapsed_ms(kMinInt64, kMaxInt64), std::numeric_limits<DurationMs>::min());
}

CO_TEST("limit validation refuses configurations that cannot work") {
  Limits limits;
  CO_CHECK_OK(validate(limits));
  Limits zero_depth = limits;
  zero_depth.max_traversal_depth = 0;
  CO_CHECK_ERR(validate(zero_depth), Code::InvalidArgument);
  Limits absurd_depth = limits;
  absurd_depth.max_traversal_depth = 100000;
  CO_CHECK_ERR(validate(absurd_depth), Code::InvalidArgument);
  Limits negative_tolerance = limits;
  negative_tolerance.max_tolerance = -1;
  CO_CHECK_ERR(validate(negative_tolerance), Code::InvalidArgument);
  Limits impossible = limits;
  impossible.max_measurements = limits.max_snapshot_bytes + 1;
  CO_CHECK_ERR(validate(impossible), Code::InvalidArgument);
}

CO_TEST("the version the library reports is the version it was built as") {
  CO_CHECK_EQ(library_version().major, 1);
  CO_CHECK_EQ(version_string(), std::string("1.0.0"));
  CO_CHECK(producer_identity().find("cooling-observatory") != std::string_view::npos);
}

CO_TEST("the interchange parser is total: every malformed line names its line") {
  const auto statements = parse_statements("# comment\n\nkey value\nflag\n  spaced   value  \n");
  CO_REQUIRE_OK(statements);
  CO_CHECK_EQ(statements.value().size(), 3u);
  CO_CHECK_EQ(statements.value()[0].key, std::string("key"));
  CO_CHECK_EQ(statements.value()[0].value, std::string("value"));
  CO_CHECK_EQ(statements.value()[0].line, 3u);
  CO_CHECK_EQ(statements.value()[1].key, std::string("flag"));
  CO_CHECK_EQ(statements.value()[1].value, std::string(""));
  CO_CHECK_EQ(statements.value()[2].key, std::string("spaced"));
  CO_CHECK_EQ(statements.value()[2].value, std::string("value"));

  Statement statement;
  statement.line = 7;
  statement.key = "seq";
  statement.value = "not-a-number";
  const auto parsed = parse_i64(statement);
  CO_CHECK_ERR(parsed, Code::MalformedInput);
  CO_CHECK(parsed.error().detail.find("line 7") != std::string::npos);
}

CO_TEST("records parse from the interchange format, and bad records are refused") {
  const std::string text =
      "record add_observation\n"
      "observation measurement\n"
      "subject measurement:zone.a.flow\n"
      "sensor sensor.a.flow\n"
      "value 30000ml/s\n"
      "observed_at 1800000000000\n"
      "seq 1\n"
      "epoch 42\n"
      "record add_observation\n"
      "observation equipment_state\n"
      "subject pump:pump.p1\n"
      "state running\n"
      "seq 2\n"
      "epoch 42\n";
  const auto records = parse_records(text);
  CO_REQUIRE_OK(records);
  CO_CHECK_EQ(records.value().size(), 2u);
  CO_CHECK(records.value()[0].kind == RecordKind::AddObservation);
  CO_CHECK(records.value()[0].observation.kind == ObservationKind::Measurement);
  CO_CHECK(records.value()[0].observation.subject.kind == SubjectKind::Measurement);
  CO_CHECK_EQ(records.value()[0].observation.measured.value, 30000000);
  CO_CHECK_EQ(records.value()[0].observation.record_seq.value(), 1u);
  CO_CHECK(records.value()[1].observation.state == LifecycleState::Running);

  CO_CHECK_ERR(parse_records("observation measurement\n"), Code::MalformedInput);
  CO_CHECK_ERR(parse_records("record nonsense\n"), Code::UnknownToken);
  CO_CHECK_ERR(parse_records("record add_observation\nobservation measurement\n"), Code::MalformedInput);
  CO_CHECK_ERR(parse_records("record add_observation\nobservation measurement\nsubject nope:x\n"),
               Code::UnknownSubjectKind);
  CO_CHECK_ERR(parse_records("record add_observation\nobservation measurement\nsubject loop:x\n"
                             "sensor s\nvalue nonsense\n"),
               Code::MalformedInput);
}

CO_TEST("a structure record round-trips through the interchange format") {
  const std::string text =
      "record adopt_structure\n"
      "generation 7\n"
      "witness test_rev\n"
      "facility dc1\n"
      "plant plant.a dc1 plant_a\n"
      "loop loop.primary plant.a primary\n"
      "component pump pump.p1 loop.primary primary_pump\n"
      "zone zone.a dc1 hall_a 180kW\n"
      "link loop:loop.primary pump:pump.p1 supply\n";
  const auto records = parse_records(text);
  CO_REQUIRE_OK(records);
  CO_CHECK_EQ(records.value().size(), 1u);
  const PlantModel& model = records.value()[0].structure;
  CO_CHECK_EQ(model.facilities().size(), 1u);
  CO_CHECK_EQ(model.plants().size(), 1u);
  CO_CHECK_EQ(model.loops().size(), 1u);
  CO_CHECK_EQ(model.components().size(), 1u);
  CO_CHECK_EQ(model.zones().size(), 1u);
  CO_CHECK_EQ(model.links().size(), 1u);
  CO_CHECK(model.zone(ZoneId(StrongId::from_validated("zone.a"))).has_value());
  CO_CHECK_EQ(model.zone(ZoneId(StrongId::from_validated("zone.a"))).value().declared_load.value().value,
              180000);
}

CO_TEST("requests parse with every filter, and unknown fields are refused") {
  const auto request = parse_request(
      "query divergence\nloop loop.primary\ninclude_consistent false\ntolerance 25000\n");
  CO_REQUIRE_OK(request);
  CO_CHECK(request.value().kind == QueryKind::Divergence);
  CO_CHECK(request.value().filter.loop.has_value());
  CO_CHECK(!request.value().filter.include_consistent);
  CO_CHECK_EQ(request.value().filter.relative_tolerance_ppm, 25000);

  CO_CHECK_ERR(parse_request("loop loop.primary\n"), Code::MalformedInput);
  CO_CHECK_ERR(parse_request("query nonsense\n"), Code::UnknownToken);
  CO_CHECK_ERR(parse_request("query image\nwhat ever\n"), Code::UnknownToken);
}