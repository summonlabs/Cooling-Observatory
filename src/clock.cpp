// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/clock.hpp"

#include <chrono>
#include <cstdio>
#include <limits>

namespace dccp::cooling_observatory {

Clock::~Clock() = default;

TimestampMs SystemClock::now_ms() const {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

namespace {

/// Days from 1970-01-01 for a proleptic Gregorian date. Howard Hinnant's
/// days_from_civil, which is exact for the whole representable range.
constexpr std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153u * (month + (month > 2 ? static_cast<unsigned>(-3) : 9u)) + 2u) / 5u + day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

/// Inverse of days_from_civil.
constexpr void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const unsigned mp = (5u * doy + 2u) / 153u;
  const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
  const unsigned m = mp < 10u ? mp + 3u : mp - 9u;
  year = y + (m <= 2 ? 1 : 0);
  month = m;
  day = d;
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
  const std::int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

std::int64_t floor_mod(std::int64_t a, std::int64_t b) noexcept { return a - floor_div(a, b) * b; }

}  // namespace

std::string format_timestamp(TimestampMs instant_ms) {
  const std::int64_t days = floor_div(instant_ms, 86400000);
  const std::int64_t ms_of_day = floor_mod(instant_ms, 86400000);
  std::int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civil_from_days(days, year, month, day);

  const std::int64_t hour = ms_of_day / 3600000;
  const std::int64_t minute = (ms_of_day / 60000) % 60;
  const std::int64_t second = (ms_of_day / 1000) % 60;
  const std::int64_t milli = ms_of_day % 1000;

  char buffer[40];
  const int written = std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
                                    static_cast<long long>(year), month, day, static_cast<long long>(hour),
                                    static_cast<long long>(minute), static_cast<long long>(second),
                                    static_cast<long long>(milli));
  if (written <= 0) {
    return {};
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

Result<TimestampMs> parse_timestamp(std::string_view text) {
  if (text.size() < 19) {
    return fail(Code::MalformedInput, "timestamp_too_short", "expected YYYY-MM-DDTHH:MM:SS[.mmm][Z]");
  }
  auto digit = [&](std::size_t index) -> std::optional<int> {
    if (index >= text.size()) {
      return std::nullopt;
    }
    const char c = text[index];
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    return c - '0';
  };
  auto number = [&](std::size_t index, std::size_t count, int& out) -> bool {
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const std::optional<int> d = digit(index + i);
      if (!d.has_value()) {
        return false;
      }
      value = value * 10 + d.value();
    }
    out = value;
    return true;
  };

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!number(0, 4, year) || text[4] != '-' || !number(5, 2, month) || text[7] != '-' ||
      !number(8, 2, day) || (text[10] != 'T' && text[10] != ' ') || !number(11, 2, hour) ||
      text[13] != ':' || !number(14, 2, minute) || text[16] != ':' || !number(17, 2, second)) {
    return fail(Code::MalformedInput, "timestamp_syntax", "expected YYYY-MM-DDTHH:MM:SS[.mmm][Z]");
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
    return fail(Code::MalformedInput, "timestamp_range", "a timestamp component is out of range");
  }

  std::size_t index = 19;
  int milli = 0;
  if (index < text.size() && text[index] == '.') {
    ++index;
    std::size_t digits = 0;
    int value = 0;
    while (index < text.size() && digits < 3) {
      const std::optional<int> d = digit(index);
      if (!d.has_value()) {
        return fail(Code::MalformedInput, "timestamp_fraction", "the fractional second is not numeric");
      }
      value = value * 10 + d.value();
      ++index;
      ++digits;
    }
    while (digits < 3) {
      value *= 10;
      ++digits;
    }
    while (index < text.size() && digit(index).has_value()) {
      ++index;  // sub-millisecond precision is dropped, not rounded
    }
    milli = value;
  }
  if (index < text.size() && (text[index] == 'Z' || text[index] == 'z')) {
    ++index;
  }
  if (index != text.size()) {
    return fail(Code::MalformedInput, "timestamp_trailing",
                "unexpected characters after the timestamp: offsets are not accepted");
  }

  const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  const std::int64_t total = days * 86400000 + static_cast<std::int64_t>(hour) * 3600000 +
                             static_cast<std::int64_t>(minute) * 60000 + static_cast<std::int64_t>(second) * 1000 +
                             milli;
  return total;
}

DurationMs elapsed_ms(TimestampMs later, TimestampMs earlier) noexcept {
  // Saturating subtraction written so that no intermediate can overflow:
  // the difference of two representable instants can lie outside the
  // representable range by at most a factor of two.
  if (later >= earlier) {
    const std::uint64_t difference =
        static_cast<std::uint64_t>(later) - static_cast<std::uint64_t>(earlier);
    if (difference > static_cast<std::uint64_t>(std::numeric_limits<DurationMs>::max())) {
      return std::numeric_limits<DurationMs>::max();
    }
    return static_cast<DurationMs>(difference);
  }
  const std::uint64_t difference =
      static_cast<std::uint64_t>(earlier) - static_cast<std::uint64_t>(later);
  const std::uint64_t limit =
      static_cast<std::uint64_t>(std::numeric_limits<DurationMs>::max()) + 1ull;
  if (difference > limit) {
    return std::numeric_limits<DurationMs>::min();
  }
  if (difference == limit) {
    return std::numeric_limits<DurationMs>::min();
  }
  return -static_cast<DurationMs>(difference);
}

}  // namespace dccp::cooling_observatory
