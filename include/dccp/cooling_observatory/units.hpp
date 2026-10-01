// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_UNITS_HPP
#define DCCP_COOLING_OBSERVATORY_UNITS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "dccp/cooling_observatory/checked.hpp"
#include "dccp/cooling_observatory/error.hpp"

namespace dccp::cooling_observatory {

/// How a scalar is interpreted. A quantity without an explicit dimension is a
/// number, and this library refuses to compare numbers across dimensions: a
/// pressure in pascals is never "greater than" a flow in microlitres per
/// second, and a temperature in millikelvin is never "greater than" a power in
/// watts.
enum class Dimension : std::uint8_t {
  None = 0,
  Flow = 1,         ///< microlitres per second
  Pressure = 2,     ///< pascals
  Temperature = 3,  ///< millikelvin
  Power = 4,        ///< watts
  Energy = 5,       ///< joules
  Volume = 6,       ///< millilitres
  Ratio = 7,        ///< parts per million
  Frequency = 8,    ///< microhertz
};

[[nodiscard]] std::string_view to_token(Dimension dimension) noexcept;
[[nodiscard]] std::optional<Dimension> parse_dimension(std::string_view token) noexcept;

/// The canonical unit symbol for a dimension's base scale. This is the symbol
/// render_quantity() writes, and the symbol parse_quantity() reads as the
/// canonical spelling of a bare number.
[[nodiscard]] std::string_view unit_symbol(Dimension dimension) noexcept;

/// One accepted way of writing a value in a dimension: a suffix and the factor
/// that converts the written value into the dimension's base scale.
struct UnitScale {
  std::string_view suffix;   ///< e.g. "ml/s"
  Dimension dimension;
  std::int64_t numerator;    ///< multiply the written value by numerator/denominator
  std::int64_t denominator;
};

/// Every accepted unit spelling, ordered so that the longest matching suffix is
/// found first. Published because the CLI and the documentation both have to
/// agree with the parser rather than restate it.
[[nodiscard]] const UnitScale* unit_scales(std::size_t& count) noexcept;

/// A scalar with an explicit dimension, stored as an exact fixed-point integer
/// at the dimension's base scale.
///
/// Floating point is not used anywhere in this library: a measured value that
/// is compared, summed, differenced or published must be reproducible bit for
/// bit, and exact integer arithmetic is the only way to guarantee that across
/// compilers and optimisation levels.
struct Quantity {
  Dimension dimension = Dimension::None;
  std::int64_t value = 0;

  constexpr Quantity() = default;
  constexpr Quantity(Dimension d, std::int64_t v) noexcept : dimension(d), value(v) {}

  [[nodiscard]] static constexpr Quantity none(std::int64_t v) noexcept {
    return Quantity(Dimension::None, v);
  }
  /// Microlitres per second.
  [[nodiscard]] static constexpr Quantity flow(std::int64_t ul_per_s) noexcept {
    return Quantity(Dimension::Flow, ul_per_s);
  }
  /// Pascals.
  [[nodiscard]] static constexpr Quantity pressure(std::int64_t pa) noexcept {
    return Quantity(Dimension::Pressure, pa);
  }
  /// Millikelvin.
  [[nodiscard]] static constexpr Quantity temperature(std::int64_t mk) noexcept {
    return Quantity(Dimension::Temperature, mk);
  }
  /// Watts.
  [[nodiscard]] static constexpr Quantity power(std::int64_t w) noexcept {
    return Quantity(Dimension::Power, w);
  }
  /// Joules.
  [[nodiscard]] static constexpr Quantity energy(std::int64_t j) noexcept {
    return Quantity(Dimension::Energy, j);
  }
  /// Millilitres.
  [[nodiscard]] static constexpr Quantity volume(std::int64_t ml) noexcept {
    return Quantity(Dimension::Volume, ml);
  }
  /// Parts per million.
  [[nodiscard]] static constexpr Quantity ratio(std::int64_t ppm) noexcept {
    return Quantity(Dimension::Ratio, ppm);
  }
  /// Microhertz.
  [[nodiscard]] static constexpr Quantity frequency(std::int64_t uhz) noexcept {
    return Quantity(Dimension::Frequency, uhz);
  }

  [[nodiscard]] constexpr bool is_zero() const noexcept { return value == 0; }
  [[nodiscard]] constexpr bool negative() const noexcept { return value < 0; }

  friend constexpr bool operator==(Quantity a, Quantity b) noexcept {
    return a.dimension == b.dimension && a.value == b.value;
  }
  friend constexpr bool operator!=(Quantity a, Quantity b) noexcept { return !(a == b); }
};

/// Dimension-checked addition. Different dimensions, or an overflowing sum,
/// yield nothing.
[[nodiscard]] std::optional<Quantity> add(Quantity a, Quantity b) noexcept;
[[nodiscard]] std::optional<Quantity> sub(Quantity a, Quantity b) noexcept;

/// Scale by an exact rational factor. Used for derates, tolerances and unit
/// conversion.
[[nodiscard]] std::optional<Quantity> scale(Quantity q, std::int64_t num, std::int64_t den) noexcept;

/// Apply a signed ratio in parts per million, e.g. +50000ppm is +5%.
[[nodiscard]] std::optional<Quantity> apply_ratio_ppm(Quantity q, std::int64_t ppm) noexcept;

/// Compare two quantities of the same dimension.
///
/// Returns nothing when the dimensions differ. This is what makes an accidental
/// cross-dimension comparison a visible failure at the call site rather than a
/// silent policy decision inside the engine.
[[nodiscard]] std::optional<int> compare(Quantity a, Quantity b) noexcept;

/// True when |a - b| <= tolerance, with all three operands in one dimension.
[[nodiscard]] std::optional<bool> within_tolerance(Quantity a, Quantity b, Quantity tolerance) noexcept;

/// The absolute difference between two quantities of one dimension.
[[nodiscard]] std::optional<Quantity> difference(Quantity a, Quantity b) noexcept;

/// Canonical fixed-point rendering: no exponent, no locale, no trailing noise.
/// Examples: "0ul/s", "-1200Pa", "12500mK", "3.5%".
[[nodiscard]] std::string render_quantity(Quantity q);

/// Parse a value written in any accepted unit for its dimension, returning the
/// quantity at the base scale. The dimension is taken from the unit suffix, so
/// "12l/s" and "12000000ul/s" are the same quantity and both are accepted.
[[nodiscard]] Result<Quantity> parse_quantity(std::string_view text);

/// Parse a value that must land in one specific dimension. A value written in
/// another dimension is refused rather than converted.
[[nodiscard]] Result<Quantity> parse_quantity_in(std::string_view text, Dimension dimension);

/// Percentage rendering for a ratio quantity, and parsing of "12.5%".
[[nodiscard]] std::string render_ppm_as_percent(std::int64_t ppm);
[[nodiscard]] Result<std::int64_t> parse_percent_as_ppm(std::string_view text);

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_UNITS_HPP
