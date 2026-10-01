// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/engine.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <utility>

#include "analysis.hpp"
#include "dccp/cooling_observatory/clock.hpp"
#include "dccp/cooling_observatory/version.hpp"

namespace dccp::cooling_observatory {
namespace {

constexpr std::array<std::pair<QueryKind, std::string_view>, 9> kQueryKinds{{
    {QueryKind::Image, "image"},
    {QueryKind::Delivery, "delivery"},
    {QueryKind::Constraints, "constraints"},
    {QueryKind::Failures, "failures"},
    {QueryKind::Reserve, "reserve"},
    {QueryKind::Divergence, "divergence"},
    {QueryKind::Coverage, "coverage"},
    {QueryKind::Dependencies, "dependencies"},
    {QueryKind::History, "history"},
}};

/// Choose an epoch for an incarnation that was not given one. Derived from the
/// clock so that two incarnations do not share an epoch by accident; an
/// operator who needs a specific epoch passes one.
EpochId derive_epoch(TimestampMs now_ms) noexcept {
  const std::uint64_t value = static_cast<std::uint64_t>(now_ms);
  return EpochId(value == 0 ? 1 : value);
}

}  // namespace

std::string_view to_token(QueryKind kind) noexcept {
  for (const auto& entry : kQueryKinds) {
    if (entry.first == kind) {
      return entry.second;
    }
  }
  return "image";
}

std::optional<QueryKind> parse_query_kind(std::string_view token) noexcept {
  for (const auto& entry : kQueryKinds) {
    if (entry.second == token) {
      return entry.first;
    }
  }
  return std::nullopt;
}

struct Engine::Impl {
  /// One lock, held for every public operation. There is exactly one mutex in
  /// this runtime and no nested locking of a second one, so the lock-order
  /// question has exactly one answer: there is no order to get wrong. It is
  /// recursive because a public entry point may call another public entry point
  /// (open calls reopen, close calls the store) and a non-recursive lock would
  /// turn that into a self-deadlock.
  mutable std::recursive_mutex mutex{};
  EngineOptions options{};
  const Clock* clock = nullptr;
  Store store{};
  ObservationImage image{};
  std::vector<HistoryEntry> history{};
  CounterSet counters{};
  bool open = false;
  bool durable = false;
  bool recovered_state = false;
  std::uint64_t next_internal_sequence = 1;
  std::uint64_t next_ordinal = 1;

  [[nodiscard]] AnalysisContext context(TimestampMs now_ms) const {
    AnalysisContext out;
    out.image = &image;
    out.limits = &options.store.limits;
    out.now_ms = now_ms;
    return out;
  }

  void record_history(const IngestRecord& record, const Observation& observation, TimestampMs now_ms) {
    HistoryEntry entry;
    entry.revision = image.revision;
    entry.record_sequence = record.record_seq;
    entry.ordinal = observation.committed_ordinal;
    entry.kind = record.kind;
    entry.observation_kind = observation.kind;
    entry.subject = observation.subject;
    entry.sensor = observation.sensor;
    entry.freshness = classify(observation, image.freshness_policy, now_ms, image.epoch, image.generation);
    entry.epoch = observation.epoch;
    entry.generation = observation.generation;
    entry.committed_at_ms = now_ms;
    entry.authority = observation.authority.authority;
    history.push_back(std::move(entry));
    if (history.size() > options.store.limits.max_report_rows) {
      history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(
                                                     history.size() - options.store.limits.max_report_rows));
    }
  }

  [[nodiscard]] bool already_applied(const RecordSeq& key) const {
    return std::find(image.applied_keys.begin(), image.applied_keys.end(), key) !=
           image.applied_keys.end();
  }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::~Engine() {
  if (impl_) {
    (void)close();
  }
}

Status Engine::open(const EngineOptions& options, const Clock& clock) {
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  if (impl_->open) {
    return Status::failure(Code::LifecycleRefused, "engine_already_open",
                           "this engine is already open");
  }
  const Status limits = validate(options.store.limits);
  if (!limits.ok()) {
    return limits;
  }
  const Status freshness = validate(options.freshness);
  if (!freshness.ok()) {
    return freshness;
  }
  const Status thermal = validate(options.thermal);
  if (!thermal.ok()) {
    return thermal;
  }

  impl_->options = options;
  impl_->clock = &clock;

  if (!options.state_directory.empty()) {
    const Status opened = impl_->store.open(options.state_directory, options.store);
    if (!opened.ok()) {
      impl_->options = EngineOptions{};
      impl_->clock = nullptr;
      return opened;
    }
    impl_->image = impl_->store.image();
    impl_->durable = true;
    if (!impl_->image.freshness_policy.accept_other_generation_as_current &&
        impl_->image.epoch.empty()) {
      impl_->image.freshness_policy = options.freshness;
    }
    if (!impl_->image.evidence.empty()) {
      // Everything restored from disk describes the world before the restart.
      // Relabelling it as recovered is what makes recovered evidence
      // distinguishable from evidence that arrived after the restart.
      for (Observation& observation : impl_->image.evidence) {
        observation.origin = EvidenceOrigin::Recovered;
      }
      impl_->image.recovered = true;
      impl_->recovered_state = true;
      impl_->counters.increment(impl_->counters.reopen_count);
    }
    if (impl_->image.epoch.empty()) {
      impl_->image.epoch = options.epoch.empty() ? derive_epoch(clock.now_ms()) : options.epoch;
    }
    if (impl_->image.freshness_policy == FreshnessPolicy{}) {
      impl_->image.freshness_policy = options.freshness;
    }
    if (impl_->image.thermal_policy == ThermalPolicy{}) {
      impl_->image.thermal_policy = options.thermal;
    }
  } else {
    impl_->image = ObservationImage{};
    impl_->image.epoch = options.epoch.empty() ? derive_epoch(clock.now_ms()) : options.epoch;
    impl_->image.freshness_policy = options.freshness;
    impl_->image.thermal_policy = options.thermal;
    impl_->durable = false;
  }

  // The next ordinal continues past everything already committed, so an
  // ordinal is never reused across a restart.
  std::uint64_t highest = 0;
  for (const Observation& observation : impl_->image.evidence) {
    highest = std::max(highest, observation.committed_ordinal);
  }
  impl_->next_ordinal = highest + 1;
  impl_->open = true;
  return Status::success();
}

Status Engine::close() {
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  if (!impl_->open) {
    return Status::failure(Code::StoreClosed, "engine_not_open", "this engine is not open");
  }
  impl_->store.close();
  impl_->open = false;
  impl_->durable = false;
  impl_->options = EngineOptions{};
  impl_->clock = nullptr;
  return Status::success();
}

bool Engine::is_open() const noexcept {
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  return impl_->open;
}

namespace {

/// Every field a record of one kind must leave empty. Validation refuses a
/// record that populates the payload of another kind, because a producer that
/// fills both is describing two different things and the reader cannot tell
/// which one was meant.
Status require_shape(const Observation& observation) {
  switch (observation.kind) {
    case ObservationKind::Measurement:
      if (observation.state != LifecycleState::Unknown) {
        return Status::failure(Code::MalformedInput, "measurement_carries_state",
                               "a measurement record must not carry a lifecycle state");
      }
      break;
    case ObservationKind::EquipmentState:
      if (observation.measured.dimension != Dimension::None) {
        return Status::failure(Code::MalformedInput, "state_carries_measured_value",
                               "an equipment-state record must not carry a measured value");
      }
      break;
    case ObservationKind::Capability:
      if (observation.state != LifecycleState::Unknown) {
        return Status::failure(Code::MalformedInput, "capability_carries_state",
                               "a capability record must not carry a lifecycle state");
      }
      if (observation.declared_capacity < 0) {
        return Status::failure(Code::UnsupportedValue, "negative_capability",
                               "a declared capability must not be negative");
      }
      break;
    case ObservationKind::ReserveClaim:
      if (observation.declared_reserve.dimension == Dimension::None) {
        return Status::failure(Code::UnsupportedValue, "reserve_without_dimension",
                               "a reserve claim must state the dimension of the reserve it declares");
      }
      break;
    case ObservationKind::Constraint:
      if (!dimension_of(observation.constraint_kind).has_value()) {
        return Status::failure(Code::UnsupportedValue, "constraint_without_dimension",
                               "a constraint kind must have a dimension");
      }
      break;
    case ObservationKind::Failure:
      break;
  }
  return Status::success();
}

}  // namespace

Result<IngestReport> Engine::ingest(const std::vector<IngestRecord>& records) {
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  if (!impl_->open) {
    return Error(Code::StoreClosed, "engine_not_open", "this engine is not open");
  }
  if (records.empty()) {
    // An empty batch is not a commit. Returning a patch would imply a revision
    // advanced, and a revision that advances without a record is a revision
    // that no producer can explain.
    IngestReport report;
    report.patch.revision = impl_->image.revision;
    report.patch.sequence = impl_->image.sequence;
    report.patch.generation = impl_->image.generation;
    return report;
  }
  if (records.size() > impl_->options.store.limits.max_records) {
    return Error(limit_error("max_records", impl_->options.store.limits.max_records,
                                          records.size()));
  }

  ObservationImage candidate = impl_->image;
  IngestReport report;
  TimestampMs now = impl_->clock != nullptr ? impl_->clock->now_ms() : 0;

  std::vector<IngestRecord> canonical;
  canonical.reserve(records.size());
  for (const IngestRecord& record : records) {
    IngestRecord copy = record;
    canonicalise(copy);
    canonical.push_back(std::move(copy));
  }

  for (const IngestRecord& record : canonical) {
    const Status valid = validate_record(record, impl_->options.store.limits, candidate.evidence.size());
    if (!valid.ok()) {
      IngestReport::Rejection rejection;
      rejection.record_seq = record.record_seq;
      rejection.reason = valid.reason();
      rejection.detail = valid.detail();
      report.rejections.push_back(std::move(rejection));
      continue;
    }
    if (record.kind == RecordKind::AddObservation) {
      const Status shape = require_shape(record.observation);
      if (!shape.ok()) {
        IngestReport::Rejection rejection;
        rejection.record_seq = record.record_seq;
        rejection.reason = shape.reason();
        rejection.detail = shape.detail();
        report.rejections.push_back(std::move(rejection));
        continue;
      }
      // The candidate image carries the keys applied before this batch and the
      // keys applied earlier in it, so a producer that repeats a record inside
      // one call is recognised as a duplicate exactly as one that repeats it
      // across calls.
      if (std::find(candidate.applied_keys.begin(), candidate.applied_keys.end(),
                    key_of(record.observation)) != candidate.applied_keys.end()) {
        report.duplicates.push_back(key_of(record.observation));
        impl_->counters.increment(impl_->counters.duplicates_rejected);
        continue;
      }
    }

    switch (record.kind) {
      case RecordKind::AdoptStructure: {
        candidate.structure = record.structure;
        candidate.structure.reindex();
        candidate.generation = record.generation;
        report.patch.structure_replaced = true;
        impl_->counters.increment(impl_->counters.structure_generations);
        // A structure change makes every declaration stated against the previous
        // structure stale. The evidence is retained, because history is not
        // deleted by a topology change, and it stops being current, because it
        // describes a plant that is no longer held.
        break;
      }
      case RecordKind::AddObservation: {
        Observation observation = record.observation;
        observation.received_at_ms =
            observation.received_at_ms == 0 ? record.received_at_ms : observation.received_at_ms;
        if (observation.epoch.empty()) {
          observation.epoch = record.epoch.empty() ? candidate.epoch : record.epoch;
        }
        // Supersede: a later record from the same sensor about the same subject
        // and kind replaces the earlier one as the current value. The earlier
        // record is dropped from the working set rather than kept beside the
        // newer one, because two current values from one sensor is exactly the
        // conflict the reducer would report, and it would be a conflict this
        // runtime created itself.
        candidate.evidence.erase(
            std::remove_if(candidate.evidence.begin(), candidate.evidence.end(),
                           [&observation](const Observation& existing) {
                             return existing.subject == observation.subject &&
                                    existing.sensor == observation.sensor &&
                                    existing.kind == observation.kind;
                           }),
            candidate.evidence.end());
        observation.internal_sequence = impl_->next_internal_sequence++;
        observation.committed_ordinal = impl_->next_ordinal++;
        candidate.evidence.push_back(std::move(observation));
        candidate.applied_keys.push_back(key_of(record.observation));
        ++report.patch.evidence_added;
        break;
      }
      case RecordKind::RetireEvidence: {
        bool retired_any = false;
        for (Observation& observation : candidate.evidence) {
          if (observation.sensor != record.retire_sensor) {
            continue;
          }
          if (record.retire_subject.has_value() && observation.subject != record.retire_subject.value()) {
            continue;
          }
          if (axis_of(observation.kind) != record.retire_axis) {
            continue;
          }
          observation.origin = EvidenceOrigin::Recovered;
          retired_any = true;
        }
        if (retired_any) {
          candidate.retired_sensors.emplace_back(record.retire_sensor, candidate.revision);
          ++report.patch.evidence_retired;
          impl_->counters.increment(impl_->counters.evidence_retired);
        }
        break;
      }
      case RecordKind::ParsePolicy: {
        candidate.freshness_policy = record.freshness_policy;
        report.patch.policy_changed = true;
        break;
      }
      case RecordKind::ThermalPolicyUpdate: {
        candidate.thermal_policy = record.thermal_policy;
        report.patch.policy_changed = true;
        break;
      }
      case RecordKind::RegisterDeliveryPoint: {
        DeliveryPoint point = record.delivery_point;
        candidate.delivery_points.erase(
            std::remove_if(candidate.delivery_points.begin(), candidate.delivery_points.end(),
                           [&point](const DeliveryPoint& existing) { return existing.id == point.id; }),
            candidate.delivery_points.end());
        candidate.delivery_points.push_back(std::move(point));
        break;
      }
      case RecordKind::ForgetDeliveryPoint: {
        candidate.delivery_points.erase(
            std::remove_if(candidate.delivery_points.begin(), candidate.delivery_points.end(),
                           [&record](const DeliveryPoint& existing) {
                             return existing.id == record.forget_point;
                           }),
            candidate.delivery_points.end());
        break;
      }
    }
    ++report.patch.records_applied;
  }

  if (report.patch.records_applied == 0) {
    impl_->counters.records_rejected.fetch_add(report.rejections.size(), std::memory_order_relaxed);
    return report;
  }

  const Result<Revision> next_revision = candidate.revision.next();
  if (!next_revision) {
    return Error(next_revision.error().code, next_revision.error().reason,
                              next_revision.error().detail);
  }
  candidate.revision = next_revision.value();
  if (candidate.sequence < impl_->image.sequence) {
    return Error(Code::RevisionRegression, "sequence_regression",
                              "the batch would move the record sequence backwards");
  }
  candidate.as_of_ms = now;
  candidate.recovered = false;

  for (DeliveryPoint& point : candidate.delivery_points) {
    if (point.committed_revision.empty()) {
      point.committed_revision = candidate.revision;
    }
  }
  for (Observation& observation : candidate.evidence) {
    if (observation.committed_revision.empty()) {
      observation.committed_revision = candidate.revision;
    }
  }

  report.patch.revision = candidate.revision;
  report.patch.sequence = candidate.sequence;
  report.patch.generation = candidate.generation;
  report.patch.digest = image_digest(candidate);

  if (impl_->durable) {
    StoreManifest manifest;
    const Status committed = impl_->store.commit(candidate, manifest);
    if (!committed.ok()) {
      // The durable commit is the commit point. A failed write leaves the
      // in-memory state exactly as it was, so a caller that retries the batch
      // retries against the same starting revision.
      return Error(committed.code(), committed.reason(), committed.detail());
    }
    impl_->counters.increment(impl_->counters.snapshot_writes);
    // The store reports the exact size of what it encoded, so the byte counter
    // is the file size rather than an estimate of it.
    impl_->counters.snapshot_bytes_written.fetch_add(impl_->store.last_encode_bytes(),
                                                     std::memory_order_relaxed);
  }

  impl_->image = std::move(candidate);
  impl_->history.clear();
  for (const IngestRecord& record : canonical) {
    if (record.kind != RecordKind::AddObservation) {
      continue;
    }
    Observation observation;
    for (const Observation& candidate_observation : impl_->image.evidence) {
      if (candidate_observation.record_seq == record.record_seq && candidate_observation.epoch == record.epoch) {
        observation = candidate_observation;
        break;
      }
    }
    if (!observation.subject.id.empty()) {
      impl_->record_history(record, observation, now);
    }
  }

  if (impl_->recovered_state) {
    report.refreshed_after_recovery = true;
    impl_->recovered_state = false;
  }
  impl_->counters.increment(impl_->counters.commits);
  impl_->counters.records_applied.fetch_add(report.patch.records_applied, std::memory_order_relaxed);
  impl_->counters.records_rejected.fetch_add(report.rejections.size(), std::memory_order_relaxed);
  return report;
}

Result<ObservationReport> Engine::observe(const ObserveRequest& request) {
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  if (!impl_->open) {
    return Error(Code::StoreClosed, "engine_not_open", "this engine is not open");
  }
  const TimestampMs now = impl_->clock != nullptr ? impl_->clock->now_ms() : impl_->image.as_of_ms;
  const AnalysisContext context = impl_->context(now);

  ObservationReport report;
  report.kind = request.kind;
  report.revision = impl_->image.revision;
  report.sequence = impl_->image.sequence;
  report.epoch = impl_->image.epoch;
  report.generation = impl_->image.generation;
  report.as_of_ms = now;
  report.recovered = impl_->image.recovered;

  switch (request.kind) {
    case QueryKind::Image: {
      report.sections.image = true;
      report.image = impl_->image;
      report.freshness = impl_->image.evidence.empty() ? Freshness::Unknown : Freshness::Fresh;
      break;
    }
    case QueryKind::Delivery: {
      report.sections.delivery = true;
      report.delivery = analyse_delivery(context, request.filter);
      report.freshness = report.delivery.freshness;
      break;
    }
    case QueryKind::Constraints: {
      report.sections.constraints = true;
      report.constraints = analyse_constraints(context, request.filter);
      report.freshness = report.constraints.freshness;
      break;
    }
    case QueryKind::Failures: {
      report.sections.failures = true;
      report.failures = analyse_failures(context, request.filter);
      report.freshness = report.failures.freshness;
      break;
    }
    case QueryKind::Reserve: {
      report.sections.reserve = true;
      report.reserve = analyse_reserve(context, request.filter);
      report.freshness = report.reserve.freshness;
      break;
    }
    case QueryKind::Divergence: {
      report.sections.divergence = true;
      report.divergence = analyse_divergence(context, request.filter);
      report.freshness = report.divergence.freshness;
      break;
    }
    case QueryKind::Coverage: {
      report.sections.coverage = true;
      report.coverage = analyse_coverage(context, request.filter);
      report.freshness = report.coverage.freshness;
      break;
    }
    case QueryKind::Dependencies: {
      report.sections.dependencies = true;
      SubjectRef root;
      if (request.filter.root.has_value()) {
        root = request.filter.root.value();
      } else if (request.filter.subject.has_value()) {
        root = request.filter.subject.value();
      } else if (request.filter.loop.has_value()) {
        root = SubjectRef(SubjectKind::Loop, rebind<StrongId>(request.filter.loop.value()));
      } else if (request.filter.zone.has_value()) {
        root = SubjectRef(SubjectKind::Zone, rebind<StrongId>(request.filter.zone.value()));
      }
      if (root.id.empty()) {
        Indeterminacy indeterminacy;
        indeterminacy.code = Code::InvalidArgument;
        indeterminacy.reason = "traversal_root_required";
        indeterminacy.detail =
            "a dependency query needs a root, given as subject, root, loop or zone";
        report.indeterminacies.push_back(std::move(indeterminacy));
        report.freshness = Freshness::Unknown;
        break;
      }
      const Result<DependencyTraversal> traversed =
          analyse_dependencies(context, root, request.filter.traversal, impl_->image.generation,
                               impl_->image.revision);
      if (!traversed) {
        Indeterminacy indeterminacy;
        indeterminacy.code = traversed.error().code;
        indeterminacy.reason = traversed.error().reason;
        indeterminacy.detail = traversed.error().detail;
        report.indeterminacies.push_back(std::move(indeterminacy));
        report.freshness = Freshness::Unknown;
        break;
      }
      report.dependencies = traversed.value();
      report.freshness = Freshness::Fresh;
      break;
    }
    case QueryKind::History: {
      report.sections.history = true;
      report.history = impl_->history;
      std::sort(report.history.begin(), report.history.end(),
                [](const HistoryEntry& a, const HistoryEntry& b) {
                  if (a.revision != b.revision) {
                    return a.revision < b.revision;
                  }
                  return a.ordinal < b.ordinal;
                });
      const std::size_t limit = request.filter.limit == 0 ? impl_->options.store.limits.max_report_rows
                                                          : request.filter.limit;
      if (report.history.size() > limit) {
        report.history.erase(report.history.begin(),
                             report.history.begin() + static_cast<std::ptrdiff_t>(report.history.size() - limit));
      }
      report.freshness = Freshness::Fresh;
      break;
    }
  }

  impl_->counters.increment(impl_->counters.queries);
  return report;
}

Result<ObservationImage> Engine::image() {
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  if (!impl_->open) {
    return Error(Code::StoreClosed, "engine_not_open", "this engine is not open");
  }
  return impl_->image;
}

Stats Engine::stats() const {
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  return impl_->counters.snapshot(impl_->image.revision, impl_->image.sequence, impl_->image.epoch,
                                  impl_->image.generation);
}

Result<StoreManifest> Engine::manifest() const {
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  if (!impl_->open) {
    return Error(Code::StoreClosed, "engine_not_open", "this engine is not open");
  }
  if (!impl_->durable) {
    return Error(Code::NotFound, "engine_not_durable",
                               "this engine has no state directory and holds no manifest");
  }
  return impl_->store.manifest();
}

Status Engine::reopen() {
  std::lock_guard<std::recursive_mutex> guard(impl_->mutex);
  if (!impl_->open) {
    return Status::failure(Code::StoreClosed, "engine_not_open", "this engine is not open");
  }
  if (!impl_->durable) {
    return Status::failure(Code::LifecycleRefused, "engine_not_durable",
                           "this engine has no state directory to reopen from");
  }
  ObservationImage reloaded;
  StoreManifest manifest;
  const Status status = impl_->store.reload(reloaded, manifest);
  if (!status.ok()) {
    return status;
  }
  for (Observation& observation : reloaded.evidence) {
    observation.origin = EvidenceOrigin::Recovered;
  }
  reloaded.recovered = true;
  impl_->image = std::move(reloaded);
  impl_->recovered_state = true;
  impl_->history.clear();
  std::uint64_t highest = 0;
  for (const Observation& observation : impl_->image.evidence) {
    highest = std::max(highest, observation.committed_ordinal);
  }
  impl_->next_ordinal = highest + 1;
  impl_->counters.increment(impl_->counters.reopen_count);
  return Status::success();
}

IngestRecord make_measurement(RecordSeq sequence, EpochId epoch, MeasurementId measurement,
                              std::string sensor_id, Quantity value, TimestampMs observed_at_ms) {
  IngestRecord record;
  record.kind = RecordKind::AddObservation;
  record.record_seq = sequence;
  record.epoch = epoch;
  record.received_at_ms = observed_at_ms;
  record.observation.kind = ObservationKind::Measurement;
  record.observation.subject = SubjectRef(SubjectKind::Measurement, rebind<StrongId>(measurement));
  record.observation.sensor = SensorId(StrongId::from_validated(std::move(sensor_id)));
  record.observation.measured = value;
  record.observation.observed_at_ms = observed_at_ms;
  record.observation.received_at_ms = observed_at_ms;
  record.observation.record_seq = sequence;
  record.observation.epoch = epoch;
  record.observation.authority.domain = AuthorityDomain::FacilityTelemetry;
  record.observation.authority.authority = std::string(producer_identity());
  return record;
}

IngestRecord make_zone_measurement(RecordSeq sequence, EpochId epoch, ZoneId zone,
                                   MeasurementId measurement, std::string sensor_id, Quantity value,
                                   TimestampMs observed_at_ms) {
  (void)zone;
  return make_measurement(sequence, epoch, measurement, std::move(sensor_id), value, observed_at_ms);
}

IngestRecord make_structure(GenerationId generation, PlantModel structure, std::string witness) {
  IngestRecord record;
  record.kind = RecordKind::AdoptStructure;
  record.generation = generation;
  record.generation_witness = std::move(witness);
  record.structure = std::move(structure);
  record.structure.reindex();
  return record;
}

}  // namespace dccp::cooling_observatory