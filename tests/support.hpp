// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Fixtures shared by the suite: a synthetic facility, a deterministic clock, an
// engine that writes into a private temporary directory, and helpers that build
// records without repeating the boilerplate.

#ifndef COOLING_OBSERVATORY_TEST_SUPPORT_HPP
#define COOLING_OBSERVATORY_TEST_SUPPORT_HPP

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/engine.hpp"
#include "test_harness.hpp"

namespace cotest {

using namespace dccp::cooling_observatory;

/// A fixed instant the whole suite starts from. Every test that involves time
/// advances a clock explicitly, so no test depends on how long it took to run.
inline constexpr TimestampMs kStart = 1'800'000'000'000;

inline TimestampMs at(std::int64_t offset_ms) { return kStart + offset_ms; }

/// The synthetic facility: one plant, a primary and a secondary loop, two
/// primary pumps, a chiller, a CDU, two CRAH units and two zones.
PlantModel synthetic_plant();

/// A directory under the system temporary directory, unique to this process and
/// this test. Nothing in the suite writes inside the repository.
class TempDir {
 public:
  explicit TempDir(const std::string& label);
  ~TempDir();

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string file(const std::string& name) const;
  [[nodiscard]] bool exists() const;

 private:
  std::string path_{};
};

/// Write bytes to a file, creating parents. Used to plant fixtures and to
/// corrupt a durable state deliberately.
void write_bytes(const std::string& path, const std::string& bytes);

[[nodiscard]] std::string read_bytes(const std::string& path);

/// Truncate a file to a length, for the torn-tail proof.
void truncate_file(const std::string& path, std::size_t length);

[[nodiscard]] std::size_t file_size(const std::string& path);

/// An engine plus the clock it reads, so a test can move time without touching
/// the engine.
struct Fixture {
  explicit Fixture(const std::string& label, bool durable = true);
  ~Fixture();

  FixedClock clock{kStart};
  Engine engine{};
  TempDir directory;
  bool opened = false;
};

/// Open a fixture's engine, recording the outcome as a required condition.
void open_fixture(Fixture& fixture, bool adopt_plant = true);

/// Records for the synthetic facility, in the order a producer would send them.
[[nodiscard]] std::vector<IngestRecord> baseline_records(EpochId epoch, TimestampMs now);

[[nodiscard]] IngestRecord measurement(std::uint64_t sequence, EpochId epoch, const char* measurement_id,
                                       const char* sensor, Quantity value, TimestampMs observed_at);

[[nodiscard]] IngestRecord zone_measurement(std::uint64_t sequence, EpochId epoch, const char* zone_id,
                                            const char* role, const char* sensor, Quantity value,
                                            TimestampMs observed_at);

[[nodiscard]] IngestRecord zone_flow_measurement(std::uint64_t sequence, EpochId epoch,
                                                 const char* zone_id, const char* sensor,
                                                 Quantity value, TimestampMs observed_at);

[[nodiscard]] IngestRecord capability(std::uint64_t sequence, EpochId epoch, SubjectKind kind,
                                      const char* element, Quantity capacity,
                                      std::int64_t derate_ppm, TimestampMs observed_at);

[[nodiscard]] IngestRecord equipment_state(std::uint64_t sequence, EpochId epoch, SubjectKind kind,
                                           const char* element, LifecycleState state,
                                           TimestampMs observed_at);

[[nodiscard]] IngestRecord constraint(std::uint64_t sequence, EpochId epoch, SubjectKind kind,
                                      const char* element, ConstraintKind constraint_kind,
                                      ConstraintDirection direction, Quantity limit,
                                      const char* origin_element, TimestampMs observed_at);

[[nodiscard]] IngestRecord failure(std::uint64_t sequence, EpochId epoch, SubjectKind kind,
                                   const char* element, FailureKind failure_kind,
                                   FailureImpact impact, TimestampMs observed_at);

[[nodiscard]] IngestRecord reserve_claim(std::uint64_t sequence, EpochId epoch, SubjectKind kind,
                                         const char* scope, Quantity reserve,
                                         const std::vector<const char*>& assumes,
                                         TimestampMs observed_at);

/// Zone measurement roles by name, so a test states "flow" rather than the
/// identity-building rule.
[[nodiscard]] MeasurementId zone_role(const char* zone_id, const char* role);

/// The number of records that were rejected, as a required zero.
void require_no_rejections(const IngestReport& report, const char* what);

/// Run one query and require that it succeeded.
[[nodiscard]] ObservationReport query(Engine& engine, QueryKind kind);

/// Run one query with a filter.
[[nodiscard]] ObservationReport query(Engine& engine, QueryKind kind, const QueryFilter& filter);

/// Find a delivery observation for a zone, or fail the test.
[[nodiscard]] const DeliveryObservation& delivery_for(const DeliveryReport& report, const char* zone_id);

/// Find a divergence finding for a point, or fail the test.
[[nodiscard]] const DivergenceFinding& divergence_for(const DivergenceReport& report,
                                                      const char* point_id);

/// True when the report mentions a reason token anywhere an explanation lives.
[[nodiscard]] bool mentions_reason(const ObservationReport& report, const std::string& reason);

}  // namespace cotest

#endif  // COOLING_OBSERVATORY_TEST_SUPPORT_HPP
