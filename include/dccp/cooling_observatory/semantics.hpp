// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_SEMANTICS_HPP
#define DCCP_COOLING_OBSERVATORY_SEMANTICS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "dccp/cooling_observatory/error.hpp"
#include "dccp/cooling_observatory/identity.hpp"

namespace dccp::cooling_observatory {

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

/// Which authority a piece of evidence belongs to.
///
/// The observatory consumes evidence from adjacent authorities and never
/// acquires their authority by doing so. Seeing a topology generation does not
/// make this runtime the topology authority; seeing a declared capacity does
/// not make it the accounting authority; carrying an incident reference does
/// not make it the incident-state authority.
enum class AuthorityDomain : std::uint8_t {
  CoolingTopology = 0,     ///< structural connectivity, owned by Cooling Topology
  CoolingCapacity = 1,     ///< accounting of declared and derived capacity
  CoolingFailure = 2,      ///< failure and incident declaration
  CoolingControl = 3,      ///< pump/valve/CDU/chiller/CRAH command authority
  FacilityTelemetry = 4,   ///< BMS/DCIM and sensor collection
  WorkloadPlacement = 5,   ///< thermal load placement and scheduling
  ThermalObservation = 6,  ///< thermal state observation
  PowerObservation = 7,    ///< electrical state observation
  LocalObservation = 8,    ///< evidence the observatory itself collected
};

[[nodiscard]] std::string_view to_token(AuthorityDomain domain) noexcept;
[[nodiscard]] std::optional<AuthorityDomain> parse_authority_domain(std::string_view token) noexcept;

/// True when this runtime is permitted to originate evidence in a domain.
/// It is permitted only for its own domain. Everything else is consumed.
[[nodiscard]] constexpr bool originates(AuthorityDomain domain) noexcept {
  return domain == AuthorityDomain::LocalObservation;
}

/// A reference to one generation of another authority's state.
///
/// Recorded verbatim and never interpreted as permission. Freshness of the
/// referenced generation is owned by the authority that produced it: this
/// runtime can observe that a reference is old, and cannot make it new.
struct AuthorityRef {
  AuthorityDomain domain = AuthorityDomain::FacilityTelemetry;
  std::string authority{};     ///< e.g. "dccp-cooling-topology/1.0.0"
  std::uint64_t generation = 0;  ///< that authority's generation counter, 0 = none
  std::string digest{};        ///< optional canonical digest of the referenced content

  friend bool operator==(const AuthorityRef& a, const AuthorityRef& b) noexcept {
    return a.domain == b.domain && a.authority == b.authority && a.generation == b.generation &&
           a.digest == b.digest;
  }
  friend bool operator!=(const AuthorityRef& a, const AuthorityRef& b) noexcept { return !(a == b); }
  friend bool operator<(const AuthorityRef& a, const AuthorityRef& b) noexcept;
};

/// A (kind, id) pair: the thing an observation is about.
///
/// A subject is addressed by kind and identity together, so a query for a pump
/// never resolves to a loop that happens to share a spelling.
struct SubjectRef {
  SubjectKind kind = SubjectKind::Loop;
  StrongId id{};

  SubjectRef() = default;
  SubjectRef(SubjectKind k, StrongId i) : kind(k), id(std::move(i)) {}

  [[nodiscard]] static Result<SubjectRef> parse(SubjectKind kind, std::string_view text) {
    auto parsed = StrongId::parse(text);
    if (!parsed) {
      return parsed.error();
    }
    return SubjectRef(kind, parsed.value());
  }

  friend bool operator==(const SubjectRef& a, const SubjectRef& b) noexcept {
    return a.kind == b.kind && a.id == b.id;
  }
  friend bool operator!=(const SubjectRef& a, const SubjectRef& b) noexcept { return !(a == b); }
  friend bool operator<(const SubjectRef& a, const SubjectRef& b) noexcept {
    if (a.kind != b.kind) {
      return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
    }
    return a.id < b.id;
  }
};

// ---------------------------------------------------------------------------
// Evidence axes and freshness
// ---------------------------------------------------------------------------

/// Which question a piece of evidence answers.
///
/// Freshness is per axis. A pressure reading from two seconds ago says nothing
/// about whether the structural topology is still current, and a topology
/// generation adopted an hour ago says nothing about whether the pumps are
/// still running. Collapsing the axes into one "age" is how an observatory ends
/// up reporting a stale structure as a live constraint.
enum class EvidenceAxis : std::uint8_t {
  Structure = 0,    ///< which entities exist and how they are connected
  Delivery = 1,     ///< measured flow, pressure, airflow, temperature, power
  Condition = 2,    ///< reported equipment lifecycle state
  Capability = 3,   ///< declared capacity and derates
  Reserve = 4,      ///< declared reserve claims
  Fault = 5,        ///< declared failures and constraints
};

[[nodiscard]] std::string_view to_token(EvidenceAxis axis) noexcept;
[[nodiscard]] std::optional<EvidenceAxis> parse_evidence_axis(std::string_view token) noexcept;
inline constexpr std::size_t kEvidenceAxisCount = 6;

/// How a piece of evidence was obtained. Provenance is descriptive: it is
/// recorded, and it is never turned into permission or authority.
enum class EvidenceOrigin : std::uint8_t {
  Observed = 0,    ///< measured by a sensor or collector
  Configured = 1,  ///< read from a configuration store
  Declared = 2,    ///< asserted by an operator or an adjacent authority
  Derived = 3,     ///< computed by this runtime from other evidence
  Recovered = 4,   ///< reconstructed during store recovery, never fresh
};

[[nodiscard]] std::string_view to_token(EvidenceOrigin origin) noexcept;
[[nodiscard]] std::optional<EvidenceOrigin> parse_evidence_origin(std::string_view token) noexcept;

/// The freshness of one piece of evidence, or of a value derived from several.
///
/// The lattice is ordered by severity. A derived value carries the worst
/// freshness of its contributors: a number computed from a stale input is
/// stale, and a number computed from an unknown input is unknown, no matter how
/// recent the other inputs were.
enum class Freshness : std::uint8_t {
  Fresh = 0,         ///< within its axis age bound, from the current epoch and generation
  Stale = 1,         ///< known, but past the age bound for its axis
  Recovered = 2,     ///< restored from durable state after a restart; not current
  Conflicting = 3,   ///< two or more current sources disagree beyond tolerance
  Unknown = 4,       ///< no evidence at all. Silence is not a zero and not health.
};

[[nodiscard]] std::string_view to_token(Freshness freshness) noexcept;
[[nodiscard]] std::optional<Freshness> parse_freshness(std::string_view token) noexcept;

/// Severity order of the freshness lattice. Higher is worse.
[[nodiscard]] constexpr int freshness_rank(Freshness f) noexcept {
  return static_cast<int>(f);
}

/// The worst (most severe) of two freshness values: the join of the lattice.
[[nodiscard]] constexpr Freshness worst(Freshness a, Freshness b) noexcept {
  return freshness_rank(a) >= freshness_rank(b) ? a : b;
}

/// True when a value at this freshness may be used as a current fact.
[[nodiscard]] constexpr bool is_current(Freshness f) noexcept { return f == Freshness::Fresh; }

/// True when the value is known at all, even if not current.
[[nodiscard]] constexpr bool is_known(Freshness f) noexcept { return f != Freshness::Unknown; }

/// Whether a value can be relied on for a decision, given a policy minimum.
[[nodiscard]] constexpr bool meets(Freshness f, Freshness required) noexcept {
  return freshness_rank(f) <= freshness_rank(required);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/// Observed lifecycle of a piece of cooling equipment.
///
/// These are observations of reported state. "Running" here means the control
/// plane reports the equipment as running, not that this runtime commanded it
/// and not that the command took effect. Observation is not ownership;
/// acknowledgement is not effect; configured state is not observed state.
enum class LifecycleState : std::uint8_t {
  Unknown = 0,       ///< no report
  Off = 1,           ///< reported stopped
  Starting = 2,      ///< reported in a start transition
  Running = 3,       ///< reported running
  Degraded = 4,      ///< reported running with reduced capability
  Stopping = 5,      ///< reported in a stop transition
  Faulted = 6,       ///< reported failed
  Maintenance = 7,   ///< reported withdrawn from service by an operator
};

[[nodiscard]] std::string_view to_token(LifecycleState state) noexcept;
[[nodiscard]] std::optional<LifecycleState> parse_lifecycle_state(std::string_view token) noexcept;

/// True when the reported state can deliver anything at all. Being able to
/// deliver is not a claim that it is delivering, and a faulted unit may still
/// have residual delivery, which is why this is only a hint used by the
/// divergence analysis and never a substitute for measurement.
[[nodiscard]] constexpr bool may_deliver(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::Running:
    case LifecycleState::Degraded:
    case LifecycleState::Starting:
      return true;
    default:
      return false;
  }
}

/// True when the reported state is a terminal non-delivering state.
[[nodiscard]] constexpr bool is_failed(LifecycleState state) noexcept {
  return state == LifecycleState::Faulted;
}

// ---------------------------------------------------------------------------
// Unknown / unsupported
// ---------------------------------------------------------------------------

/// A value that may be absent. Absence is a first-class outcome, not a
/// sentinel: zero is a measurement, and a missing measurement is not zero.
template <typename T>
class Maybe {
 public:
  Maybe() = default;
  Maybe(T value) : storage_(std::move(value)), present_(true) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] static Maybe missing() { return Maybe(); }
  [[nodiscard]] static Maybe of(T value) { return Maybe(std::move(value)); }

  [[nodiscard]] bool has_value() const noexcept { return present_; }
  [[nodiscard]] explicit operator bool() const noexcept { return present_; }

  [[nodiscard]] const T& value() const noexcept { return storage_; }
  [[nodiscard]] T& value() noexcept { return storage_; }
  /// The value, or the fallback when absent.
  ///
  /// Returned by value, deliberately. Returning a reference would let a caller
  /// bind it to a temporary fallback that dies at the end of the full
  /// expression, leaving a dangling reference that still reads as a value.
  /// std::optional::value_or returns by value for the same reason.
  [[nodiscard]] T value_or(const T& fallback) const noexcept { return present_ ? storage_ : fallback; }

  friend bool operator==(const Maybe& a, const Maybe& b) noexcept {
    if (a.present_ != b.present_) {
      return false;
    }
    return !a.present_ || a.storage_ == b.storage_;
  }
  friend bool operator!=(const Maybe& a, const Maybe& b) noexcept { return !(a == b); }

 private:
  T storage_{};
  bool present_ = false;
};

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_SEMANTICS_HPP