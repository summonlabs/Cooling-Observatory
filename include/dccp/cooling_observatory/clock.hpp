// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_CLOCK_HPP
#define DCCP_COOLING_OBSERVATORY_CLOCK_HPP

#include <cstdint>
#include <string>

#include "dccp/cooling_observatory/error.hpp"
#include "dccp/cooling_observatory/identity.hpp"

namespace dccp::cooling_observatory {

/// The engine never reads the ambient clock directly. Every decision that
/// depends on "now" goes through this interface, so that a test, a replay or a
/// downstream authority can pin time exactly and get a bit-identical answer.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = default;
  Clock& operator=(const Clock&) = default;
  virtual ~Clock();

  /// Milliseconds since the Unix epoch. May move backwards if the host clock
  /// does; the engine treats a backwards step as a stale-authority signal
  /// rather than pretending that time is monotonic.
  [[nodiscard]] virtual TimestampMs now_ms() const = 0;
};

/// Reads the host wall clock. The only place in the library that does.
class SystemClock final : public Clock {
 public:
  [[nodiscard]] TimestampMs now_ms() const override;
};

/// A clock pinned to one instant. Used by examples, tests and replay.
class FixedClock final : public Clock {
 public:
  explicit FixedClock(TimestampMs instant) noexcept : instant_(instant) {}

  [[nodiscard]] TimestampMs now_ms() const override { return instant_; }

  void set(TimestampMs instant) noexcept { instant_ = instant; }
  /// Advance by a duration. A negative duration is accepted: a host clock that
  /// steps backwards is a real condition this runtime must survive.
  void advance(DurationMs delta) noexcept { instant_ += delta; }

 private:
  TimestampMs instant_ = 0;
};

/// Format an instant as ISO-8601 UTC with millisecond precision:
/// "2026-01-31T01:24:28.011Z". Deterministic, locale-independent.
[[nodiscard]] std::string format_timestamp(TimestampMs instant_ms);

/// Parse the format above. Accepts an optional trailing 'Z' or nothing.
[[nodiscard]] Result<TimestampMs> parse_timestamp(std::string_view text);

/// Saturating subtraction of instants: never wraps, reports the extreme when
/// the true difference is not representable.
[[nodiscard]] DurationMs elapsed_ms(TimestampMs later, TimestampMs earlier) noexcept;

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_CLOCK_HPP
