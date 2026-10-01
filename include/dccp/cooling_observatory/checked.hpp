// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_CHECKED_HPP
#define DCCP_COOLING_OBSERVATORY_CHECKED_HPP

#include <cstdint>
#include <limits>
#include <optional>

#include "dccp/cooling_observatory/error.hpp"

namespace dccp::cooling_observatory {

/// Checked signed-64-bit integer arithmetic.
///
/// Every physical computation in this library runs through these functions.
/// Overflow is reported, never wrapped: a wrapped quantity would silently
/// become a different physical claim.

[[nodiscard]] std::optional<std::int64_t> checked_add(std::int64_t a, std::int64_t b) noexcept;

/// Exact difference.
///
/// Returns nothing only when the exact result is not representable. Subtracting
/// the most negative value is not automatically an overflow: 0 - min is 2^63 and
/// -1 - min is max, and both are representable, so both are computed. Refusing
/// them would report an overflow that did not happen.
[[nodiscard]] std::optional<std::int64_t> checked_sub(std::int64_t a, std::int64_t b) noexcept;
[[nodiscard]] std::optional<std::int64_t> checked_mul(std::int64_t a, std::int64_t b) noexcept;

/// Exact division. Returns nothing when b is zero, or when the quotient is not
/// representable (the single overflowing case is min / -1).
[[nodiscard]] std::optional<std::int64_t> checked_div(std::int64_t a, std::int64_t b) noexcept;

/// Multiply then divide, keeping full precision in the intermediate product.
///
/// Scaling a quantity by a rational factor is the single most common operation
/// in this library (unit conversion, derate, tolerance application, percentage
/// of a limit) and it is exactly where naive code overflows. Uses a 128-bit
/// intermediate where the compiler provides one and a decomposed exact path
/// where it does not.
[[nodiscard]] std::optional<std::int64_t> checked_mul_div(std::int64_t value, std::int64_t num,
                                                           std::int64_t den) noexcept;

/// Multiply then divide, rounding halves away from zero.
///
/// The same exact intermediate product as checked_mul_div, reported at the
/// nearest step. Unit conversion uses this: truncation would turn an exact
/// conversion such as 3.6 m3/h into 1 l/s minus 1 ul/s.
[[nodiscard]] std::optional<std::int64_t> checked_mul_div_round(std::int64_t value, std::int64_t num,
                                                               std::int64_t den) noexcept;

/// Divide, rounding halves away from zero.
///
/// This is the division this library uses wherever an exact quotient is not
/// required: a physical figure derived from measured inputs is reported at the
/// nearest representable step rather than truncated, because truncation biases
/// every derived figure downwards by up to one step and the bias accumulates
/// across a roll-up. Truncation is reserved for those few places where the
/// mathematical remainder is the answer, and those call checked_div directly.
[[nodiscard]] std::optional<std::int64_t> checked_div_round(std::int64_t a, std::int64_t b) noexcept;

/// Clamp without relying on unspecified conversions.
[[nodiscard]] constexpr std::int64_t clamp(std::int64_t value, std::int64_t low,
                                          std::int64_t high) noexcept {
  return value < low ? low : (value > high ? high : value);
}

/// Absolute value with the min/-1 case handled explicitly.
[[nodiscard]] std::optional<std::int64_t> checked_abs(std::int64_t value) noexcept;

/// Sum a range of values with overflow reporting.
[[nodiscard]] std::optional<std::int64_t> checked_sum(const std::int64_t* values, std::size_t count) noexcept;

/// Largest and smallest representable values, named so that domain code reads
/// as intent rather than as a numeric constant.
inline constexpr std::int64_t kMaxInt64 = std::numeric_limits<std::int64_t>::max();
inline constexpr std::int64_t kMinInt64 = std::numeric_limits<std::int64_t>::min();

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_CHECKED_HPP
