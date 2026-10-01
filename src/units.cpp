// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/units.hpp"

#include <array>
#include <cstdlib>

namespace dccp::cooling_observatory {
namespace {

struct DimensionName {
  Dimension dimension;
  std::string_view token;
  std::string_view symbol;
};

constexpr std::array<DimensionName, 9> kDimensionNames{{
    {Dimension::None, "none", ""},
    {Dimension::Flow, "flow", "ul/s"},
    {Dimension::Pressure, "pressure", "Pa"},
    {Dimension::Temperature, "temperature", "mK"},
    {Dimension::Power, "power", "W"},
    {Dimension::Energy, "energy", "J"},
    {Dimension::Volume, "volume", "ml"},
    {Dimension::Ratio, "ratio", "ppm"},
    {Dimension::Frequency, "frequency", "uHz"},
}};

/// Accepted spellings, longest first so that "ml/s" is never matched as "l/s".
constexpr std::array<UnitScale, 26> kUnitScales{{
    // Flow, base microlitres per second.
    {"m3/h", Dimension::Flow, 1000000000, 3600},
    {"l/min", Dimension::Flow, 1000000000, 60},
    {"l/h", Dimension::Flow, 1000000000, 3600},
    {"ml/s", Dimension::Flow, 1000, 1},
    {"l/s", Dimension::Flow, 1000000, 1},
    {"cfm", Dimension::Flow, 471947, 1000},
    {"ul/s", Dimension::Flow, 1, 1},
    {"ml", Dimension::Volume, 1, 1},
    {"l", Dimension::Volume, 1000, 1},
    {"m3", Dimension::Volume, 1000000, 1},

    // Pressure, base pascals.
    {"kPa", Dimension::Pressure, 1000, 1},
    {"psi", Dimension::Pressure, 6895, 1},
    {"bar", Dimension::Pressure, 100000, 1},
    {"Pa", Dimension::Pressure, 1, 1},

    // Temperature, base millikelvin.
    {"degC", Dimension::Temperature, 1000, 1},
    {"degF", Dimension::Temperature, 5000, 9},
    {"mK", Dimension::Temperature, 1, 1},

    // Power and energy.
    {"kW", Dimension::Power, 1000, 1},
    {"W", Dimension::Power, 1, 1},
    {"kHz", Dimension::Frequency, 1000000000, 1},
    {"uHz", Dimension::Frequency, 1, 1},
    {"Hz", Dimension::Frequency, 1000000, 1},
    {"mHz", Dimension::Frequency, 1000, 1},
    {"kJ", Dimension::Energy, 1000, 1},
    {"MJ", Dimension::Energy, 1000000, 1},
    {"J", Dimension::Energy, 1, 1},
}};

}  // namespace

std::string_view to_token(Dimension dimension) noexcept {
  for (const DimensionName& entry : kDimensionNames) {
    if (entry.dimension == dimension) {
      return entry.token;
    }
  }
  return "none";
}

std::optional<Dimension> parse_dimension(std::string_view token) noexcept {
  for (const DimensionName& entry : kDimensionNames) {
    if (entry.token == token) {
      return entry.dimension;
    }
  }
  return std::nullopt;
}

std::string_view unit_symbol(Dimension dimension) noexcept {
  for (const DimensionName& entry : kDimensionNames) {
    if (entry.dimension == dimension) {
      return entry.symbol;
    }
  }
  return {};
}

const UnitScale* unit_scales(std::size_t& count) noexcept {
  count = kUnitScales.size();
  return kUnitScales.data();
}

std::optional<Quantity> add(Quantity a, Quantity b) noexcept {
  if (a.dimension != b.dimension) {
    return std::nullopt;
  }
  const std::optional<std::int64_t> sum = checked_add(a.value, b.value);
  if (!sum.has_value()) {
    return std::nullopt;
  }
  return Quantity(a.dimension, sum.value());
}

std::optional<Quantity> sub(Quantity a, Quantity b) noexcept {
  if (a.dimension != b.dimension) {
    return std::nullopt;
  }
  const std::optional<std::int64_t> difference_value = checked_sub(a.value, b.value);
  if (!difference_value.has_value()) {
    return std::nullopt;
  }
  return Quantity(a.dimension, difference_value.value());
}

std::optional<Quantity> scale(Quantity q, std::int64_t num, std::int64_t den) noexcept {
  const std::optional<std::int64_t> scaled = checked_mul_div(q.value, num, den);
  if (!scaled.has_value()) {
    return std::nullopt;
  }
  return Quantity(q.dimension, scaled.value());
}

std::optional<Quantity> apply_ratio_ppm(Quantity q, std::int64_t ppm) noexcept {
  const std::optional<std::int64_t> delta = checked_mul_div(q.value, ppm, 1000000);
  if (!delta.has_value()) {
    return std::nullopt;
  }
  return add(q, Quantity(q.dimension, delta.value()));
}

std::optional<int> compare(Quantity a, Quantity b) noexcept {
  if (a.dimension != b.dimension) {
    return std::nullopt;
  }
  if (a.value < b.value) {
    return -1;
  }
  return a.value > b.value ? 1 : 0;
}

std::optional<Quantity> difference(Quantity a, Quantity b) noexcept {
  if (a.dimension != b.dimension) {
    return std::nullopt;
  }
  const std::optional<std::int64_t> magnitude = checked_abs(a.value);
  if (!magnitude.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::int64_t> other = checked_abs(b.value);
  if (!other.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::int64_t> gap = checked_sub(a.value, b.value);
  if (!gap.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::int64_t> absolute = checked_abs(gap.value());
  if (!absolute.has_value()) {
    return std::nullopt;
  }
  return Quantity(a.dimension, absolute.value());
}

std::optional<bool> within_tolerance(Quantity a, Quantity b, Quantity tolerance) noexcept {
  if (a.dimension != b.dimension || tolerance.dimension != a.dimension) {
    return std::nullopt;
  }
  if (tolerance.value < 0) {
    return std::nullopt;
  }
  const std::optional<Quantity> gap = difference(a, b);
  if (!gap.has_value()) {
    return std::nullopt;
  }
  return gap.value().value <= tolerance.value;
}

namespace {

/// Render an exact fixed-point value with a scale denominator chosen so that
/// the rendered form round-trips.
std::string render_scaled(std::int64_t value, std::string_view suffix) {
  const bool negative_value = value < 0;
  const std::optional<std::int64_t> magnitude = checked_abs(value);
  if (!magnitude.has_value()) {
    return std::string("-9223372036854775808") + std::string(suffix);
  }
  std::string out;
  if (negative_value) {
    out.push_back('-');
  }
  out += std::to_string(magnitude.value());
  out += suffix;
  return out;
}

/// Render a value with exactly three decimal places, trimming trailing zeros,
/// used for percentage output.
std::string render_fraction(std::int64_t value, std::int64_t scale) {
  const bool negative_value = value < 0;
  const std::optional<std::int64_t> magnitude = checked_abs(value);
  if (!magnitude.has_value()) {
    return "-9223372036854775.808";
  }
  const std::int64_t whole = magnitude.value() / scale;
  const std::int64_t fraction = magnitude.value() % scale;
  std::string out;
  if (negative_value) {
    out.push_back('-');
  }
  out += std::to_string(whole);
  if (fraction != 0) {
    std::string frac = std::to_string(fraction);
    while (frac.size() < 3) {
      frac.insert(frac.begin(), '0');
    }
    while (!frac.empty() && frac.back() == '0') {
      frac.pop_back();
    }
    out.push_back('.');
    out += frac;
  }
  return out;
}

}  // namespace

std::string render_quantity(Quantity q) {
  if (q.dimension == Dimension::Ratio) {
    return render_ppm_as_percent(q.value);
  }
  return render_scaled(q.value, unit_symbol(q.dimension));
}

std::string render_ppm_as_percent(std::int64_t ppm) {
  // 10000 ppm is 1%, so the rendered scale is 10000.
  return render_fraction(ppm, 10000) + "%";
}

/// Internal, but deliberately not in an anonymous namespace: a Result of a
/// type with internal linkage cannot be instantiated in this translation unit
/// without leaving the value accessor undefined (MSVC C5046 under /W4 /WX).
namespace parse_detail {

struct ParsedNumber {
  bool negative = false;
  std::string_view integer_digits{};
  std::string_view fraction_digits{};
  bool has_fraction = false;
};

Result<ParsedNumber> split_number(std::string_view text, std::size_t& consumed) {
  ParsedNumber out;
  std::size_t index = 0;
  if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
    out.negative = text[index] == '-';
    ++index;
  }
  const std::size_t integer_start = index;
  while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
    ++index;
  }
  if (index == integer_start) {
    return fail(Code::MalformedInput, "quantity_missing_digits", "a quantity must start with digits");
  }
  out.integer_digits = text.substr(integer_start, index - integer_start);
  if (index < text.size() && text[index] == '.') {
    ++index;
    const std::size_t fraction_start = index;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
      ++index;
    }
    if (index == fraction_start) {
      return fail(Code::MalformedInput, "quantity_missing_fraction", "a decimal point must be followed by digits");
    }
    out.fraction_digits = text.substr(fraction_start, index - fraction_start);
    out.has_fraction = true;
  }
  consumed = index;
  return out;
}

}  // namespace parse_detail

Result<Quantity> parse_quantity(std::string_view text) {
  using parse_detail::ParsedNumber;
  using parse_detail::split_number;
  std::size_t consumed = 0;
  auto number = split_number(text, consumed);
  if (!number) {
    return number.error();
  }
  const std::string_view suffix = text.substr(consumed);
  if (suffix.empty()) {
    return fail(Code::MalformedInput, "quantity_missing_unit",
                "a quantity must name its unit; a bare number has no dimension");
  }

  // A trailing '%' is the ratio unit.
  if (suffix == "%") {
    auto ppm = parse_percent_as_ppm(text);
    if (!ppm) {
      return ppm.error();
    }
    return Quantity(Dimension::Ratio, ppm.value());
  }

  const UnitScale* match = nullptr;
  for (const UnitScale& entry : kUnitScales) {
    if (entry.suffix == suffix) {
      match = &entry;
      break;
    }
  }
  if (match == nullptr) {
    return fail(Code::MalformedInput, "quantity_unknown_unit",
                std::string("unknown unit '") + std::string(suffix) + "'");
  }

  // Build the exact integer value: integer digits scaled, then the fraction.
  std::optional<std::int64_t> value = 0;
  for (const char c : number.value().integer_digits) {
    const std::optional<std::int64_t> shifted = checked_mul(value.value(), 10);
    if (!shifted.has_value()) {
      return fail(Code::LimitExceeded, "quantity_out_of_range", "the integer part does not fit in 64 bits");
    }
    value = checked_add(shifted.value(), static_cast<std::int64_t>(c - '0'));
    if (!value.has_value()) {
      return fail(Code::LimitExceeded, "quantity_out_of_range", "the integer part does not fit in 64 bits");
    }
  }

  // Apply the unit scale to the integer part, rounding to the nearest base-scale
  // step rather than truncating: 3.6 m3/h is exactly 1 l/s, and truncation would
  // report it as 999999 ul/s.
  std::optional<std::int64_t> scaled =
      checked_mul_div_round(value.value(), match->numerator, match->denominator);
  if (!scaled.has_value()) {
    return fail(Code::LimitExceeded, "quantity_out_of_range",
                "the value written in this unit does not fit the base scale");
  }

  if (number.value().has_fraction) {
    const std::string_view fraction = number.value().fraction_digits;
    if (fraction.size() > 18) {
      return fail(Code::MalformedInput, "quantity_precision",
                  "at most 18 fractional digits are accepted");
    }
    std::int64_t fraction_value = 0;
    std::int64_t fraction_scale = 1;
    for (const char c : fraction) {
      fraction_value = fraction_value * 10 + static_cast<std::int64_t>(c - '0');
      fraction_scale *= 10;
    }
    const std::optional<std::int64_t> fraction_part =
        checked_mul_div_round(fraction_value, match->numerator, fraction_scale * match->denominator);
    if (!fraction_part.has_value()) {
      return fail(Code::LimitExceeded, "quantity_out_of_range", "the fractional part does not fit the base scale");
    }
    scaled = checked_add(scaled.value(), fraction_part.value());
    if (!scaled.has_value()) {
      return fail(Code::LimitExceeded, "quantity_out_of_range", "the value does not fit the base scale");
    }
  }

  const std::int64_t signed_value = number.value().negative ? -(scaled.value()) : scaled.value();
  return Quantity(match->dimension, signed_value);
}

Result<Quantity> parse_quantity_in(std::string_view text, Dimension dimension) {
  auto parsed = parse_quantity(text);
  if (!parsed) {
    return parsed.error();
  }
  if (parsed.value().dimension != dimension) {
    return fail(Code::UnsupportedValue, "quantity_wrong_dimension",
                std::string("expected a value in ") + std::string(to_token(dimension)) + ", got " +
                    std::string(to_token(parsed.value().dimension)));
  }
  return parsed.value();
}

Result<std::int64_t> parse_percent_as_ppm(std::string_view text) {
  std::string_view body = text;
  if (!body.empty() && body.back() == '%') {
    body.remove_suffix(1);
  } else {
    return fail(Code::MalformedInput, "percent_missing_sign", "a percentage must end with '%'");
  }
  std::size_t consumed = 0;
  auto number = parse_detail::split_number(body, consumed);
  if (!number) {
    return number.error();
  }
  if (consumed != body.size()) {
    return fail(Code::MalformedInput, "percent_trailing", "unexpected characters after the percentage value");
  }
  std::optional<std::int64_t> value = 0;
  for (const char c : number.value().integer_digits) {
    const std::optional<std::int64_t> shifted = checked_mul(value.value(), 10);
    if (!shifted.has_value()) {
      return fail(Code::LimitExceeded, "percent_out_of_range", "the percentage does not fit in 64 bits");
    }
    value = checked_add(shifted.value(), static_cast<std::int64_t>(c - '0'));
    if (!value.has_value()) {
      return fail(Code::LimitExceeded, "percent_out_of_range", "the percentage does not fit in 64 bits");
    }
  }
  // One percent is 10000 ppm.
  std::optional<std::int64_t> ppm = checked_mul(value.value(), 10000);
  if (!ppm.has_value()) {
    return fail(Code::LimitExceeded, "percent_out_of_range", "the percentage does not fit in 64 bits");
  }
  if (number.value().has_fraction) {
    const std::string_view fraction = number.value().fraction_digits;
    if (fraction.size() > 6) {
      return fail(Code::MalformedInput, "percent_precision", "at most 6 fractional digits are accepted");
    }
    std::int64_t fraction_value = 0;
    std::int64_t fraction_scale = 1;
    for (const char c : fraction) {
      fraction_value = fraction_value * 10 + static_cast<std::int64_t>(c - '0');
      fraction_scale *= 10;
    }
    const std::optional<std::int64_t> fraction_ppm =
        checked_mul_div_round(fraction_value, 10000, fraction_scale);
    if (!fraction_ppm.has_value()) {
      return fail(Code::LimitExceeded, "percent_out_of_range", "the percentage does not fit in 64 bits");
    }
    ppm = checked_add(ppm.value(), fraction_ppm.value());
    if (!ppm.has_value()) {
      return fail(Code::LimitExceeded, "percent_out_of_range", "the percentage does not fit in 64 bits");
    }
  }
  return number.value().negative ? -ppm.value() : ppm.value();
}

}  // namespace dccp::cooling_observatory