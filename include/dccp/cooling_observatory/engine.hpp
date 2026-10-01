// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_ENGINE_HPP
#define DCCP_COOLING_OBSERVATORY_ENGINE_HPP

#include <memory>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/clock.hpp"
#include "dccp/cooling_observatory/error.hpp"
#include "dccp/cooling_observatory/ingest.hpp"
#include "dccp/cooling_observatory/limits.hpp"
#include "dccp/cooling_observatory/persistence.hpp"
#include "dccp/cooling_observatory/query.hpp"
#include "dccp/cooling_observatory/stats.hpp"

namespace dccp::cooling_observatory {

/// How an engine is configured when it opens.
struct EngineOptions {
  /// The durable home of the state. Absent means a purely in-memory engine,
  /// which is useful for a one-shot analysis and is never durable.
  std::string state_directory{};
  /// The epoch this incarnation stamps on the evidence it originates. Zero
  /// means choose one, and the engine derives it from the clock so that two
  /// incarnations do not share an epoch by accident.
  EpochId epoch{};
  StoreOptions store{};
  FreshnessPolicy freshness{};
  ThermalPolicy thermal{};
};

/// The cooling observatory runtime.
///
/// One engine owns one state directory, one committed revision, one adopted
/// structure generation and one epoch. Every public entry point takes the
/// engine lock, works on the committed state, and releases it, so a query never
/// observes a half-applied mutation.
class Engine {
 public:
  Engine();
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /// Open or create an engine. When the state directory holds a state from a
  /// previous incarnation, this restores it and marks the restored evidence as
  /// recovered rather than current.
  [[nodiscard]] Status open(const EngineOptions& options, const Clock& clock);

  /// Close the engine and release the writer lock. Committing after close is
  /// refused rather than performed.
  [[nodiscard]] Status close();

  [[nodiscard]] bool is_open() const noexcept;

  /// Apply a batch of records. Either every record is applied and the batch is
  /// committed, or none is: a partially applied batch would leave a durable
  /// state that no producer ever asked for.
  [[nodiscard]] Result<IngestReport> ingest(const std::vector<IngestRecord>& records);

  /// Answer a query against the committed state.
  [[nodiscard]] Result<ObservationReport> observe(const ObserveRequest& request);

  /// The observable state.
  [[nodiscard]] Result<ObservationImage> image();

  /// Counters.
  [[nodiscard]] Stats stats() const;

  /// The store manifest, for a caller that wants to see what was recovered.
  [[nodiscard]] Result<StoreManifest> manifest() const;

  /// Re-read the durable state from disk into this engine. Used by the reopen
  /// proof: the engine drops what it holds and takes what the file says.
  [[nodiscard]] Status reopen();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Build a measurement record with the fields a producer must supply.
[[nodiscard]] IngestRecord make_measurement(RecordSeq sequence, EpochId epoch, MeasurementId measurement,
                                            std::string sensor_id, Quantity value,
                                            TimestampMs observed_at_ms);

/// Build a measurement record for one zone role defined by this library's
/// naming convention.
[[nodiscard]] IngestRecord make_zone_measurement(RecordSeq sequence, EpochId epoch, ZoneId zone,
                                                 MeasurementId measurement, std::string sensor_id,
                                                 Quantity value, TimestampMs observed_at_ms);

/// Build a record that adopts a structure as one generation.
[[nodiscard]] IngestRecord make_structure(GenerationId generation, PlantModel structure,
                                          std::string witness);

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_ENGINE_HPP
