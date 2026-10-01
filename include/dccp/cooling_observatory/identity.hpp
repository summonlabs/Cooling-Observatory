// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_IDENTITY_HPP
#define DCCP_COOLING_OBSERVATORY_IDENTITY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "dccp/cooling_observatory/error.hpp"

namespace dccp::cooling_observatory {

/// Longest accepted identity string. Bounded so that a hostile or corrupt
/// input cannot make the engine allocate without limit.
inline constexpr std::size_t kMaxIdentityLength = 96;

/// A checked, opaque identity string.
///
/// Identities come from outside the process boundary. They are validated once,
/// at construction, and every later use is a copy of an already-valid value, so
/// no code path has to re-decide what a legal identity looks like.
class StrongId {
 public:
  StrongId() = default;

  /// Validate an identity: 1..kMaxIdentityLength bytes, first byte an ASCII
  /// letter or underscore, remaining bytes ASCII alphanumerics, '_', '.', ':',
  /// '-' or '/'. The grammar is deliberately narrow: identities are compared
  /// and rendered, never interpreted as paths or format strings.
  [[nodiscard]] static Result<StrongId> parse(std::string_view text);

  /// Adopt already-validated text. Used by rebind() and by the engine when it
  /// re-types an identity it parsed earlier; never a second, weaker validation.
  [[nodiscard]] static StrongId from_validated(std::string text) {
    StrongId out;
    out.text_ = std::move(text);
    return out;
  }

  [[nodiscard]] static bool valid(std::string_view text) noexcept;

  [[nodiscard]] const std::string& str() const noexcept { return text_; }
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
  [[nodiscard]] std::string_view view() const noexcept { return text_; }

  friend bool operator==(const StrongId& a, const StrongId& b) noexcept { return a.text_ == b.text_; }
  friend bool operator!=(const StrongId& a, const StrongId& b) noexcept { return !(a == b); }
  friend bool operator<(const StrongId& a, const StrongId& b) noexcept { return a.text_ < b.text_; }

 private:
  std::string text_;
};

/// Equality and ordering are total and byte-exact on the validated text, so all
/// containers ordered by a strong identity are deterministic.
#define DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(Name)                      \
  class Name {                                                               \
   public:                                                                   \
    Name() = default;                                                        \
    explicit Name(StrongId id) : id_(std::move(id)) {}                       \
    [[nodiscard]] static Result<Name> parse(std::string_view text) {         \
      auto parsed = StrongId::parse(text);                                   \
      if (!parsed) {                                                         \
        return parsed.error();                                               \
      }                                                                      \
      return Name(parsed.value());                                           \
    }                                                                        \
    [[nodiscard]] const std::string& str() const noexcept { return id_.str(); } \
    [[nodiscard]] std::string_view view() const noexcept { return id_.view(); } \
    [[nodiscard]] bool empty() const noexcept { return id_.empty(); }        \
    friend bool operator==(const Name& a, const Name& b) noexcept { return a.id_ == b.id_; } \
    friend bool operator!=(const Name& a, const Name& b) noexcept { return !(a == b); }      \
    friend bool operator<(const Name& a, const Name& b) noexcept { return a.id_ < b.id_; }   \
                                                                             \
   private:                                                                  \
    StrongId id_;                                                            \
  };

/// Identities of the cooling entities this runtime observes. It names them; it
/// does not own them.
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(FacilityId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(PlantId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(LoopId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(PumpId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(ValveId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(ChillerId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(CduId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(CrahId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(ManifoldId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(ZoneId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(BranchId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(RackGroupId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(MeasurementId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(SensorId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(ClaimId)
DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID(RecordId)

#undef DCCP_COOLING_OBSERVATORY_DEFINE_STRONG_ID

/// Re-type a validated identity. The text was validated when it was first
/// parsed, so re-typing is a pure relabelling and never a second validation
/// with different rules.
template <typename To, typename From>
[[nodiscard]] To rebind(const From& id) {
  return To(StrongId::from_validated(id.str()));
}

/// The kind of entity an identity refers to. Explicit, so a query for a pump
/// never silently resolves to a loop that happens to share a spelling.
enum class SubjectKind : std::uint8_t {
  Facility = 0,
  Plant = 1,
  Loop = 2,
  Pump = 3,
  Valve = 4,
  Chiller = 5,
  Cdu = 6,
  Crah = 7,
  Manifold = 8,
  Zone = 9,
  Branch = 10,
  RackGroup = 11,
  Measurement = 12,
  Sensor = 13,
};

[[nodiscard]] std::string_view to_token(SubjectKind kind) noexcept;
[[nodiscard]] std::optional<SubjectKind> parse_subject_kind(std::string_view token) noexcept;

/// Monotonic counter of committed state. Refuses to move backwards.
class Revision {
 public:
  Revision() = default;
  explicit constexpr Revision(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return value_ == 0; }

  /// Next revision. Checked: a full counter is a hard refusal, never a wrap.
  [[nodiscard]] Result<Revision> next() const;

  friend constexpr bool operator==(Revision a, Revision b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Revision a, Revision b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Revision a, Revision b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator<=(Revision a, Revision b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>(Revision a, Revision b) noexcept { return b < a; }
  friend constexpr bool operator>=(Revision a, Revision b) noexcept { return b <= a; }

 private:
  std::uint64_t value_ = 0;
};

/// Identifies one structural generation adopted from the cooling topology
/// authority. A generation is a claim about structure at a moment; the
/// observatory records which generation an answer was computed against.
class GenerationId {
 public:
  GenerationId() = default;
  explicit constexpr GenerationId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return value_ == 0; }

  friend constexpr bool operator==(GenerationId a, GenerationId b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(GenerationId a, GenerationId b) noexcept { return !(a == b); }
  friend constexpr bool operator<(GenerationId a, GenerationId b) noexcept { return a.value_ < b.value_; }

 private:
  std::uint64_t value_ = 0;
};

/// Epoch of the control-plane incarnation that produced a piece of evidence.
/// Evidence from a superseded epoch is never promoted to current.
class EpochId {
 public:
  EpochId() = default;
  explicit constexpr EpochId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return value_ == 0; }

  friend constexpr bool operator==(EpochId a, EpochId b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(EpochId a, EpochId b) noexcept { return !(a == b); }
  friend constexpr bool operator<(EpochId a, EpochId b) noexcept { return a.value_ < b.value_; }

 private:
  std::uint64_t value_ = 0;
};

/// Identity of one observation record as supplied by its producer. Used for
/// idempotent ingestion: a repeated record id at or below the committed
/// revision is a duplicate, not a second fact.
class RecordSeq {
 public:
  RecordSeq() = default;
  explicit constexpr RecordSeq(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return value_ == 0; }

  friend constexpr bool operator==(RecordSeq a, RecordSeq b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(RecordSeq a, RecordSeq b) noexcept { return !(a == b); }
  friend constexpr bool operator<(RecordSeq a, RecordSeq b) noexcept { return a.value_ < b.value_; }

 private:
  std::uint64_t value_ = 0;
};

/// Millisecond-resolution wall-clock instant, counted from the Unix epoch.
///
/// Signed on purpose, so that an instant before the epoch is representable and
/// therefore comparable instead of being silently clamped.
using TimestampMs = std::int64_t;

/// A duration in milliseconds. Signed for the same reason.
using DurationMs = std::int64_t;

// A subject reference is a (kind, id) pair. It is defined in semantics.hpp,
// which includes this header, because it is part of the evidence-addressing
// model rather than of identity itself.

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_IDENTITY_HPP