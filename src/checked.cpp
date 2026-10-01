// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/checked.hpp"

namespace dccp::cooling_observatory {
namespace {

// The exact multiply-divide below needs the full 128-bit product of two 64-bit
// values. Both supported toolchains provide it: GCC and Clang as __int128, and
// MSVC on 64-bit targets as the _umul128 intrinsic. A target that provides
// neither is refused at compile time rather than silently given approximate
// arithmetic, because an approximate quantity would enter a physical answer.
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#define DCCP_COOLING_OBSERVATORY_WIDE_VIA_INTRIN 1
#elif defined(__SIZEOF_INT128__)
#define DCCP_COOLING_OBSERVATORY_WIDE_VIA_INT128 1
#else
#error "cooling_observatory requires a 64x64->128 multiply (MSVC x64 _umul128 or __int128)"
#endif

/// A 128-bit value as (high, low). Negation and narrowing are written on this
/// form so that no intermediate product ever wraps silently.
struct Wide {
  std::uint64_t high = 0;
  std::uint64_t low = 0;
};

[[nodiscard]] Wide multiply(std::int64_t a, std::int64_t b) noexcept {
  Wide out;
#if defined(DCCP_COOLING_OBSERVATORY_WIDE_VIA_INTRIN)
  out.low = _umul128(static_cast<std::uint64_t>(a), static_cast<std::uint64_t>(b), &out.high);
#else
  const unsigned __int128 product =
      static_cast<unsigned __int128>(static_cast<std::uint64_t>(a)) *
      static_cast<unsigned __int128>(static_cast<std::uint64_t>(b));
  out.low = static_cast<std::uint64_t>(product);
  out.high = static_cast<std::uint64_t>(product >> 64);
#endif
  return out;
}

[[nodiscard]] bool negative(const Wide& value) noexcept { return (value.high >> 63) != 0; }

/// Two's-complement negation of a 128-bit value, exact for every input.
///
/// The result is the plain 128-bit negation, with no canonicalisation: negating
/// a magnitude yields the magnitude back, and negating a negative value yields
/// its magnitude, which is what the narrowing checks read. An earlier version
/// forced the high word to all ones to normalise the sign, which silently
/// turned every magnitude it produced into a number far outside the range and
/// made representable results look like overflows.
[[nodiscard]] Wide negate(const Wide& value) noexcept {
  Wide out;
  out.low = ~value.low + 1ull;
  out.high = ~value.high + (out.low == 0 ? 1ull : 0ull);
  return out;
}

/// True when an unsigned magnitude exceeds the most negative signed value's
/// magnitude (2^63).
[[nodiscard]] bool magnitude_exceeds_min(Wide value) noexcept {
  return value.high != 0 || value.low > 0x8000000000000000ull;
}

/// Compare a non-negative signed value against an unsigned magnitude.
[[nodiscard]] bool magnitude_exceeds(std::uint64_t limit, Wide value) noexcept {
  return value.high != 0 || value.low > limit;
}

/// Unsigned division of a 128-bit value by a non-zero 64-bit divisor, reporting
/// the remainder as well when the caller asks for it.
///
/// Long division from the most significant bit: bounded to 128 iterations, and
/// the remainder never exceeds the divisor, so no intermediate can overflow.
[[nodiscard]] Wide divide(const Wide& dividend, std::uint64_t divisor,
                          std::uint64_t* remainder_out = nullptr) noexcept {
  Wide quotient;
  std::uint64_t remainder = 0;
  for (int bit = 127; bit >= 0; --bit) {
    const std::uint64_t next =
        bit >= 64 ? ((dividend.high >> (bit - 64)) & 1ull) : ((dividend.low >> bit) & 1ull);
    // remainder = remainder * 2 + next. The remainder is below the divisor, so
    // the shift can only overflow when the divisor's top bit is set; that case is
    // handled explicitly by subtracting unconditionally.
    const bool shift_overflows = (remainder >> 63) != 0;
    remainder = (remainder << 1) | next;
    if (shift_overflows || remainder >= divisor) {
      remainder -= divisor;
      if (bit >= 64) {
        quotient.high |= 1ull << (bit - 64);
      } else {
        quotient.low |= 1ull << bit;
      }
    }
  }
  if (remainder_out != nullptr) {
    *remainder_out = remainder;
  }
  return quotient;
}

/// Narrow to a signed 64-bit value, or report that the value does not fit.
[[nodiscard]] std::optional<std::int64_t> narrow(const Wide& value) noexcept {
  if (negative(value)) {
    const Wide magnitude = negate(value);
    if (magnitude_exceeds_min(magnitude)) {
      return std::nullopt;
    }
    if (magnitude.low == 0x8000000000000000ull) {
      return kMinInt64;
    }
    return -static_cast<std::int64_t>(magnitude.low);
  }
  if (magnitude_exceeds(0x7FFFFFFFFFFFFFFFull, value)) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(value.low);
}

/// One step of 128-bit increment, used to round a quotient up.
[[nodiscard]] Wide increment(const Wide& value) noexcept {
  Wide out;
  out.low = value.low + 1ull;
  out.high = value.high + (out.low == 0 ? 1ull : 0ull);
  return out;
}

/// The unsigned magnitude of a signed value, exact for the most negative value.
[[nodiscard]] std::uint64_t magnitude_u64(std::int64_t value) noexcept {
  return value < 0 ? ~static_cast<std::uint64_t>(value) + 1ull : static_cast<std::uint64_t>(value);
}

/// The exact product of two signed values as an unsigned magnitude plus a sign.
///
/// The sign is taken from the operands and the magnitudes are multiplied. Using
/// the top bit of the unsigned bit pattern instead would be wrong: the pattern
/// for min * 2 is 2^64, whose top bit is clear, so a negative product would be
/// read as positive.
struct SignedProduct {
  Wide magnitude;
  bool negative;
};

[[nodiscard]] SignedProduct signed_product(std::int64_t value, std::int64_t num) noexcept {
  SignedProduct out;
  out.magnitude = multiply(static_cast<std::int64_t>(magnitude_u64(value)),
                           static_cast<std::int64_t>(magnitude_u64(num)));
  out.negative = (value < 0) != (num < 0);
  return out;
}

[[nodiscard]] std::uint64_t magnitude_divisor(std::int64_t den) noexcept {
  return den < 0 ? ~static_cast<std::uint64_t>(den) + 1ull : static_cast<std::uint64_t>(den);
}

/// value * num / den, truncated toward zero.
[[nodiscard]] std::optional<std::int64_t> wide_mul_div(std::int64_t value, std::int64_t num,
                                                       std::int64_t den) noexcept {
  const SignedProduct product = signed_product(value, num);
  const bool divisor_negative = den < 0;
  Wide quotient = divide(product.magnitude, magnitude_divisor(den));
  if (product.negative != divisor_negative) {
    quotient = negate(quotient);
  }
  return narrow(quotient);
}

/// value * num / den, rounded to the nearest step with halves away from zero.
[[nodiscard]] std::optional<std::int64_t> wide_mul_div_round(std::int64_t value, std::int64_t num,
                                                             std::int64_t den) noexcept {
  const SignedProduct product = signed_product(value, num);
  const bool divisor_negative = den < 0;
  const std::uint64_t divisor = magnitude_divisor(den);
  std::uint64_t remainder = 0;
  Wide quotient = divide(product.magnitude, divisor, &remainder);
  // Round up when the remainder is at least half the divisor. Comparing against
  // divisor - remainder avoids doubling either value, so neither can overflow.
  if (remainder != 0 && remainder >= divisor - remainder) {
    quotient = increment(quotient);
  }
  if (product.negative != divisor_negative) {
    quotient = negate(quotient);
  }
  return narrow(quotient);
}

}  // namespace

std::optional<std::int64_t> checked_add(std::int64_t a, std::int64_t b) noexcept {
  if (b > 0 && a > kMaxInt64 - b) {
    return std::nullopt;
  }
  if (b < 0 && a < kMinInt64 - b) {
    return std::nullopt;
  }
  return a + b;
}

std::optional<std::int64_t> checked_sub(std::int64_t a, std::int64_t b) noexcept {
  if (b == kMinInt64) {
    // a - min is 2^63 + a. That is representable only for a < 0, and it is then
    // exactly -(2^63 - |a|), which is the two's-complement sum reinterpreted.
    if (a >= 0) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + 0x8000000000000000ull);
  }
  return checked_add(a, -b);
}

std::optional<std::int64_t> checked_mul(std::int64_t a, std::int64_t b) noexcept {
  if (a == 0 || b == 0) {
    return 0;
  }
  // The product is formed from the magnitudes and the sign is applied
  // afterwards. Multiplying the two's-complement patterns directly and testing
  // the high word would be wrong: min * 1 has a bit pattern whose top bit is
  // set, so a sign-extension test reads a representable product as an overflow.
  const bool negative_product = (a < 0) != (b < 0);
  const std::uint64_t magnitude_a =
      a < 0 ? ~static_cast<std::uint64_t>(a) + 1ull : static_cast<std::uint64_t>(a);
  const std::uint64_t magnitude_b =
      b < 0 ? ~static_cast<std::uint64_t>(b) + 1ull : static_cast<std::uint64_t>(b);
  const Wide exact = multiply(static_cast<std::int64_t>(magnitude_a),
                              static_cast<std::int64_t>(magnitude_b));
  if (!negative_product) {
    return narrow(exact);
  }
  return narrow(negate(exact));
}

std::optional<std::int64_t> checked_div(std::int64_t a, std::int64_t b) noexcept {
  if (b == 0) {
    return std::nullopt;
  }
  if (a == kMinInt64 && b == -1) {
    return std::nullopt;
  }
  return a / b;
}

std::optional<std::int64_t> checked_mul_div(std::int64_t value, std::int64_t num, std::int64_t den) noexcept {
  if (den == 0) {
    return std::nullopt;
  }
  if (value == 0 || num == 0) {
    return 0;
  }
  return wide_mul_div(value, num, den);
}

std::optional<std::int64_t> checked_mul_div_round(std::int64_t value, std::int64_t num,
                                                      std::int64_t den) noexcept {
  if (den == 0) {
    return std::nullopt;
  }
  if (value == 0 || num == 0) {
    return 0;
  }
  return wide_mul_div_round(value, num, den);
}

std::optional<std::int64_t> checked_div_round(std::int64_t a, std::int64_t b) noexcept {
  if (b == 0) {
    return std::nullopt;
  }
  if (a == kMinInt64 && b == -1) {
    return std::nullopt;
  }
  const std::optional<std::int64_t> magnitude = checked_abs(b);
  if (!magnitude.has_value()) {
    return std::nullopt;
  }
  const std::int64_t half = magnitude.value() / 2;
  // Adding half the divisor's magnitude to the dividend shifts the quotient to
  // the nearest step while leaving the sign to the division itself.
  const std::optional<std::int64_t> biased = a >= 0 ? checked_add(a, half) : checked_sub(a, half);
  if (!biased.has_value()) {
    // The shift overflowed, which can only happen when the quotient is already
    // at the edge of the range; the plain division is still exact enough to
    // report.
    return checked_div(a, b);
  }
  const std::optional<std::int64_t> quotient = checked_div(biased.value(), b);
  if (!quotient.has_value()) {
    return checked_div(a, b);
  }
  return quotient;
}

std::optional<std::int64_t> checked_abs(std::int64_t value) noexcept {
  if (value == kMinInt64) {
    return std::nullopt;
  }
  return value < 0 ? -value : value;
}

std::optional<std::int64_t> checked_sum(const std::int64_t* values, std::size_t count) noexcept {
  std::int64_t total = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const std::optional<std::int64_t> next = checked_add(total, values[i]);
    if (!next.has_value()) {
      return std::nullopt;
    }
    total = next.value();
  }
  return total;
}

}  // namespace dccp::cooling_observatory