// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Persistence under attack: torn tails, interior corruption, a wrong schema, a
// truncated header, an implausible length and a state file that is not a state
// file at all. The store must refuse to guess in every one of them.

#include <cstdint>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/checksum.hpp"
#include "dccp/cooling_observatory/persistence.hpp"
#include "dccp/cooling_observatory/version.hpp"
#include "support.hpp"
#include "test_harness.hpp"

using namespace dccp::cooling_observatory;

namespace {

constexpr std::size_t kHeaderBytes = 64;

std::string state_path(const cotest::TempDir& directory) {
  return directory.file("cooling-observatory.state");
}

/// Recompute the CRC over a payload and install it, so that a state is valid by
/// checksum but wrong in some other way. Used to prove that the checksum is not
/// the only thing standing between a corrupt file and an answer.
void repair_checksum(std::string& bytes) {
  if (bytes.size() <= kHeaderBytes) {
    return;
  }
  const std::string_view payload(bytes.data() + kHeaderBytes, bytes.size() - kHeaderBytes);
  const std::uint32_t crc = crc32c(payload);
  bytes[12] = static_cast<char>(crc & 0xFFu);
  bytes[13] = static_cast<char>((crc >> 8) & 0xFFu);
  bytes[14] = static_cast<char>((crc >> 16) & 0xFFu);
  bytes[15] = static_cast<char>((crc >> 24) & 0xFFu);
}

}  // namespace

CO_TEST("a round trip through the encoder preserves the observable state") {
  ObservationImage image;
  image.revision = Revision(9);
  image.sequence = RecordSeq(4);
  image.epoch = EpochId(77);
  image.generation = GenerationId(3);
  image.as_of_ms = cotest::at(1500);
  image.structure = cotest::synthetic_plant();
  image.freshness_policy.delivery_max_age_ms = 12'345;
  image.thermal_policy.coolant_heat_capacity_uj_per_l_k = 3'900'000;
  image.applied_keys.push_back(RecordSeq(1));
  image.applied_keys.push_back(RecordSeq(2));
  image.retired_sensors.emplace_back(SensorId(StrongId::from_validated("s.doomed")), Revision(8));

  Observation observation;
  observation.kind = ObservationKind::Measurement;
  observation.subject = SubjectRef(SubjectKind::Measurement, StrongId::from_validated("zone.a.flow"));
  observation.sensor = SensorId(StrongId::from_validated("s.flow"));
  observation.measured = Quantity::flow(1'234'567);
  observation.observed_at_ms = cotest::at(100);
  observation.epoch = image.epoch;
  observation.record_seq = RecordSeq(3);
  observation.authority.domain = AuthorityDomain::FacilityTelemetry;
  observation.authority.authority = "synthetic/1.0.0";
  observation.committed_revision = Revision(9);
  observation.committed_ordinal = 5;
  image.evidence.push_back(observation);

  DeliveryPoint point;
  point.id = StrongId::from_validated("dp.zone.a");
  point.zone = ZoneId(StrongId::from_validated("zone.a"));
  point.medium = CoolantMedium::Liquid;
  point.flow_measurement = zone_flow(point.zone);
  point.declared_load = Maybe<Quantity>::of(Quantity::power(180'000));
  image.delivery_points.push_back(point);

  const auto encoded = encode_image(image, default_limits());
  CO_REQUIRE_OK(encoded);
  const auto decoded = decode_image(encoded.value(), default_limits().max_snapshot_bytes);
  CO_REQUIRE_OK(decoded);
  CO_CHECK(decoded.value().revision == image.revision);
  CO_CHECK(decoded.value().epoch == image.epoch);
  CO_CHECK(decoded.value().generation == image.generation);
  CO_CHECK_EQ(decoded.value().structure.digest(), image.structure.digest());
  CO_CHECK_EQ(decoded.value().evidence.size(), 1u);
  CO_CHECK_EQ(decoded.value().evidence[0].measured.value, 1'234'567);
  CO_CHECK(decoded.value().evidence[0].subject == observation.subject);
  CO_CHECK_EQ(decoded.value().delivery_points.size(), 1u);
  CO_CHECK(decoded.value().delivery_points[0].declared_load.has_value());
  CO_CHECK_EQ(decoded.value().applied_keys.size(), 2u);
  CO_CHECK_EQ(decoded.value().retired_sensors.size(), 1u);
  CO_CHECK_EQ(decoded.value().freshness_policy.delivery_max_age_ms, 12'345);
  CO_CHECK_EQ(image_digest(decoded.value()), image_digest(image));
}

CO_TEST("a single flipped bit anywhere in the payload is refused") {
  cotest::Fixture fixture("persistence-bitflip", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  CO_REQUIRE_OK(fixture.engine.close());

  const std::string path = state_path(fixture.directory);
  const std::string original = cotest::read_bytes(path);
  CO_REQUIRE(original.size() > kHeaderBytes + 32);
  const std::size_t offsets[] = {kHeaderBytes + 1, kHeaderBytes + 17, original.size() / 2,
                                 original.size() - 1};
  for (const std::size_t offset : offsets) {
    std::string damaged = original;
    damaged[offset] = static_cast<char>(damaged[offset] ^ 0x40);
    cotest::write_bytes(path, damaged);
    const auto decoded = decode_image(damaged, default_limits().max_snapshot_bytes);
    CO_CHECK_ERR(decoded, Code::CorruptSnapshot);
    // The store refuses to open the directory rather than starting empty, which
    // would make a corrupt facility look like a facility with no equipment.
    Engine engine;
    const Status opened = engine.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, fixture.clock);
    CO_CHECK(!opened.ok());
  }
  cotest::write_bytes(path, original);
}

CO_TEST("a torn tail is refused because the frame length no longer matches") {
  cotest::Fixture fixture("persistence-torn", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  CO_REQUIRE_OK(fixture.engine.close());

  const std::string path = state_path(fixture.directory);
  const std::string original = cotest::read_bytes(path);
  const std::size_t full = original.size();

  const std::size_t lengths[] = {kHeaderBytes, kHeaderBytes + 1, full - 1, full / 2, 3};
  for (const std::size_t length : lengths) {
    cotest::truncate_file(path, length);
    const std::string truncated = cotest::read_bytes(path);
    const auto decoded = decode_image(truncated, default_limits().max_snapshot_bytes);
    CO_CHECK_ERR(decoded, Code::CorruptSnapshot);
    Engine engine;
    const Status opened = engine.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, fixture.clock);
    CO_CHECK(!opened.ok());
  }

  // Restoring the file restores service: the store never destroyed the evidence
  // it could not decode, so an operator can decide what to do with it.
  cotest::write_bytes(path, original);
  Engine engine;
  const Status opened = engine.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, fixture.clock);
  CO_CHECK_OK(opened);
  CO_REQUIRE_OK(engine.close());
}

CO_TEST("interior corruption under a repaired checksum is still refused") {
  cotest::Fixture fixture("persistence-interior", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  CO_REQUIRE_OK(fixture.engine.close());

  const std::string path = state_path(fixture.directory);
  const std::string original = cotest::read_bytes(path);

  // Cut a byte out of the middle of the payload and repair the checksum. The
  // frame now verifies, and its contents no longer describe a state: the
  // decoder's own length discipline has to catch it.
  std::string damaged = original;
  damaged.erase(damaged.size() / 2, 1);
  repair_checksum(damaged);
  const auto decoded = decode_image(damaged, default_limits().max_snapshot_bytes);
  CO_CHECK_ERR(decoded, Code::CorruptSnapshot);

  cotest::write_bytes(path, original);
}

CO_TEST("a schema this build does not implement is refused as incompatible") {
  cotest::Fixture fixture("persistence-schema", true);
  cotest::open_fixture(fixture);
  CO_REQUIRE_OK(fixture.engine.close());
  const std::string path = state_path(fixture.directory);
  std::string bytes = cotest::read_bytes(path);
  CO_REQUIRE(bytes.size() > kHeaderBytes);
  // The schema version is the two bytes after the magic.
  bytes[4] = static_cast<char>(kSnapshotSchemaVersion + 1);
  bytes[5] = 0;
  cotest::write_bytes(path, bytes);
  const auto decoded = decode_image(bytes, default_limits().max_snapshot_bytes);
  CO_CHECK_ERR(decoded, Code::IncompatibleSchema);
  CO_CHECK(decoded.error().reason == std::string("snapshot_schema_unsupported"));
}

CO_TEST("a reserved header flag is refused rather than ignored") {
  cotest::Fixture fixture("persistence-flags", true);
  cotest::open_fixture(fixture);
  CO_REQUIRE_OK(fixture.engine.close());
  const std::string path = state_path(fixture.directory);
  std::string bytes = cotest::read_bytes(path);
  bytes[6] = 1;
  bytes[7] = 0;
  cotest::write_bytes(path, bytes);
  const auto decoded = decode_image(bytes, default_limits().max_snapshot_bytes);
  CO_CHECK_ERR(decoded, Code::IncompatibleSchema);
}

CO_TEST("a file that is not a state file at all is refused on its magic") {
  cotest::TempDir directory("persistence-magic");
  const std::string path = state_path(directory);
  cotest::write_bytes(path, std::string(4096, 'x'));
  Engine engine;
  const Status opened = engine.open(EngineOptions{directory.path(), {}, {}, {}, {}}, cotest::FixedClock(cotest::kStart));
  CO_CHECK(!opened.ok());
  CO_CHECK(opened.code() == Code::CorruptSnapshot);
  CO_CHECK(opened.reason() == std::string("snapshot_bad_magic"));
}

CO_TEST("an empty file is refused rather than treated as an empty state") {
  cotest::TempDir directory("persistence-empty");
  cotest::write_bytes(state_path(directory), "");
  Engine engine;
  const Status opened = engine.open(EngineOptions{directory.path(), {}, {}, {}, {}}, cotest::FixedClock(cotest::kStart));
  CO_CHECK(!opened.ok());
  CO_CHECK(opened.code() == Code::CorruptSnapshot);
  CO_CHECK(opened.reason() == std::string("snapshot_truncated_header"));
}

CO_TEST("the header's own summary must agree with the payload it frames") {
  cotest::Fixture fixture("persistence-summary", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  CO_REQUIRE_OK(fixture.engine.close());

  const std::string path = state_path(fixture.directory);
  std::string bytes = cotest::read_bytes(path);
  // The header carries the revision at offset 20. Changing it leaves the
  // payload valid by checksum and inconsistent with its own frame.
  bytes[20] = static_cast<char>(bytes[20] + 1);
  const auto decoded = decode_image(bytes, default_limits().max_snapshot_bytes);
  CO_CHECK_ERR(decoded, Code::CorruptSnapshot);
  CO_CHECK(decoded.error().reason == std::string("snapshot_header_payload_disagreement") ||
           decoded.error().reason == std::string("snapshot_checksum_mismatch"));
}

CO_TEST("an oversized state file is refused by the read limit") {
  cotest::Fixture fixture("persistence-oversize", true);
  cotest::open_fixture(fixture);
  CO_REQUIRE_OK(fixture.engine.close());
  const std::string path = state_path(fixture.directory);
  std::string bytes = cotest::read_bytes(path);
  bytes.resize(1024);
  Limits limits = default_limits();
  limits.max_snapshot_bytes = 128;
  const auto decoded = decode_image(bytes, limits.max_snapshot_bytes);
  CO_CHECK_ERR(decoded, Code::LimitExceeded);
}

CO_TEST("the encoder refuses a state that would exceed the configured size") {
  ObservationImage image;
  image.structure = cotest::synthetic_plant();
  Limits limits = default_limits();
  limits.max_snapshot_bytes = 64;
  const auto encoded = encode_image(image, limits);
  CO_CHECK_ERR(encoded, Code::LimitExceeded);
}

CO_TEST("a store refuses a second open while the first holds the writer lock") {
  cotest::Fixture fixture("persistence-double", true);
  cotest::open_fixture(fixture);
  Store second;
  const Status opened = second.open(fixture.directory.path(), StoreOptions{});
  CO_CHECK(!opened.ok());
  CO_CHECK(opened.code() == Code::StoreBusy);
  CO_CHECK(second.open_state() == false);
}

CO_TEST("committing after close is refused rather than written") {
  cotest::Fixture fixture("persistence-closed", true);
  cotest::open_fixture(fixture);
  Store store;
  CO_REQUIRE_OK(store.open(fixture.directory.path() + "-store", StoreOptions{}));
  store.close();
  const ObservationImage image = fixture.engine.image().value();
  StoreManifest manifest;
  const Status committed = store.commit(image, manifest);
  CO_CHECK(!committed.ok());
  CO_CHECK(committed.code() == Code::StoreClosed);
}

CO_TEST("an export encodes deterministically for one state") {
  cotest::Fixture fixture("persistence-determinism", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  const ObservationImage image = fixture.engine.image().value();
  const auto first = encode_image(image, default_limits());
  const auto second = encode_image(image, default_limits());
  CO_REQUIRE_OK(first);
  CO_REQUIRE_OK(second);
  CO_CHECK_EQ(first.value(), second.value());
}

CO_TEST("two commits in a row leave exactly one live state file, with no temporary residue") {
  cotest::Fixture fixture("persistence-residue", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::zone_measurement(99, epoch, "zone.a", "flow", "s.flow", Quantity::flow(1),
                                fixture.clock.now_ms())}));
  CO_REQUIRE_OK(fixture.engine.close());
  CO_CHECK(cotest::file_size(state_path(fixture.directory)) > 0);
  CO_CHECK(!std::filesystem::exists(state_path(fixture.directory) + ".tmp"));
  // Only the state file and the writer lock remain.
  std::size_t entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(fixture.directory.path())) {
    (void)entry;
    ++entries;
  }
  CO_CHECK_EQ(entries, 2u);
}