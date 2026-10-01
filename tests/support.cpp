// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace cotest {

PlantModel synthetic_plant() {
  PlantModel model;
  model.add_facility(FacilityId(StrongId::from_validated("dc1")), "dc1");

  CoolingPlant plant;
  plant.id = PlantId(StrongId::from_validated("plant.a"));
  plant.facility = FacilityId(StrongId::from_validated("dc1"));
  plant.label = "plant_a";
  model.add_plant(plant);

  Loop primary;
  primary.id = LoopId(StrongId::from_validated("loop.primary"));
  primary.plant = plant.id;
  primary.secondary = false;
  primary.label = "primary";
  model.add_loop(primary);

  Loop secondary;
  secondary.id = LoopId(StrongId::from_validated("loop.secondary"));
  secondary.plant = plant.id;
  secondary.secondary = true;
  secondary.label = "secondary";
  model.add_loop(secondary);

  auto component = [&model](ComponentKind kind, const char* id, const char* loop) {
    PlantComponent entry;
    entry.kind = kind;
    entry.id = StrongId::from_validated(id);
    entry.loop = LoopId(StrongId::from_validated(loop));
    model.add_component(entry);
  };
  component(ComponentKind::Pump, "pump.p1", "loop.primary");
  component(ComponentKind::Pump, "pump.p2", "loop.primary");
  component(ComponentKind::Chiller, "chiller.c1", "loop.primary");
  component(ComponentKind::Cdu, "cdu.d1", "loop.secondary");
  component(ComponentKind::Crah, "crah.h1", "loop.secondary");
  component(ComponentKind::Crah, "crah.h2", "loop.secondary");

  auto zone = [&model](const char* id, std::int64_t load_watts) {
    ThermalZone entry;
    entry.id = ZoneId(StrongId::from_validated(id));
    entry.facility = FacilityId(StrongId::from_validated("dc1"));
    entry.declared_load = Maybe<Quantity>::of(Quantity::power(load_watts));
    model.add_zone(entry);
  };
  zone("zone.a", 180'000);
  zone("zone.b", 120'000);

  auto link = [&model](SubjectKind from_kind, const char* from, SubjectKind to_kind, const char* to,
                       LinkRelation relation) {
    PlantLink entry;
    entry.from = SubjectRef(from_kind, StrongId::from_validated(from));
    entry.to = SubjectRef(to_kind, StrongId::from_validated(to));
    entry.relation = relation;
    model.add_link(entry);
  };
  link(SubjectKind::Plant, "plant.a", SubjectKind::Loop, "loop.primary", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.primary", SubjectKind::Loop, "loop.secondary", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.primary", SubjectKind::Chiller, "chiller.c1", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.primary", SubjectKind::Pump, "pump.p1", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.primary", SubjectKind::Pump, "pump.p2", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.secondary", SubjectKind::Cdu, "cdu.d1", LinkRelation::Supply);
  link(SubjectKind::Cdu, "cdu.d1", SubjectKind::Crah, "crah.h1", LinkRelation::Supply);
  link(SubjectKind::Cdu, "cdu.d1", SubjectKind::Crah, "crah.h2", LinkRelation::Supply);
  link(SubjectKind::Crah, "crah.h1", SubjectKind::Zone, "zone.a", LinkRelation::Supply);
  link(SubjectKind::Crah, "crah.h2", SubjectKind::Zone, "zone.b", LinkRelation::Supply);

  model.reindex();
  return model;
}

namespace {

std::string unique_suffix() {
  static std::uint64_t counter = 0;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::ostringstream out;
  out << now << '-' << (++counter);
  return out.str();
}

}  // namespace

TempDir::TempDir(const std::string& label) {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  const std::string root = error ? std::string(".") : base.string();
  path_ = root + "/cooling-observatory-tests/" + label + '-' + unique_suffix();
  std::filesystem::create_directories(path_, error);
}

TempDir::~TempDir() {
  std::error_code error;
  // The fixture removes only the directory it created, and only from under the
  // suite's own root, so a test can never delete anything it did not make.
  if (path_.find("cooling-observatory-tests") == std::string::npos) {
    return;
  }
  std::filesystem::remove_all(path_, error);
}

std::string TempDir::file(const std::string& name) const { return path_ + "/" + name; }

bool TempDir::exists() const {
  std::error_code error;
  return std::filesystem::exists(path_, error);
}

void write_bytes(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void truncate_file(const std::string& path, std::size_t length) {
  std::string bytes = read_bytes(path);
  if (bytes.size() > length) {
    bytes.resize(length);
  }
  write_bytes(path, bytes);
}

std::size_t file_size(const std::string& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::size_t>(size);
}

Fixture::Fixture(const std::string& label, bool durable)
    : directory(label), opened(false) {
  EngineOptions options;
  if (durable) {
    options.state_directory = directory.path();
  }
  const Status status = engine.open(options, clock);
  opened = status.ok();
}

Fixture::~Fixture() = default;

void open_fixture(Fixture& fixture, bool adopt_plant) {
  CO_REQUIRE(fixture.opened);
  if (!adopt_plant) {
    return;
  }
  const std::vector<IngestRecord> records{
      make_structure(GenerationId(1), synthetic_plant(), "test_fixture_rev_1")};
  const auto report = fixture.engine.ingest(records);
  CO_REQUIRE_OK(report);
  require_no_rejections(report.value(), "adopt structure");
}

MeasurementId zone_role(const char* zone_id, const char* role) {
  const ZoneId zone(StrongId::from_validated(zone_id));
  const std::string_view name(role);
  if (name == "flow") {
    return zone_flow(zone);
  }
  if (name == "supply_temp") {
    return zone_supply_temperature(zone);
  }
  if (name == "return_temp") {
    return zone_return_temperature(zone);
  }
  if (name == "diff_pressure") {
    return zone_differential_pressure(zone);
  }
  if (name == "airflow") {
    return zone_airflow(zone);
  }
  // A fully qualified identity is accepted as itself, so a test can name the
  // exact measurement a delivery point resolves to without restating the rule
  // that builds it.
  if (StrongId::valid(role) && name.find('.') != std::string_view::npos) {
    return MeasurementId(StrongId::from_validated(role));
  }
  throw Failure(std::string("unknown zone role: ") + std::string(role));
}

IngestRecord measurement(std::uint64_t sequence, EpochId epoch, const char* measurement_id,
                         const char* sensor, Quantity value, TimestampMs observed_at) {
  IngestRecord record = make_measurement(RecordSeq(sequence), epoch,
                                         MeasurementId(StrongId::from_validated(measurement_id)),
                                         sensor, value, observed_at);
  record.observation.generation = GenerationId(1);
  return record;
}

IngestRecord zone_measurement(std::uint64_t sequence, EpochId epoch, const char* zone_id,
                              const char* role, const char* sensor, Quantity value,
                              TimestampMs observed_at) {
  return measurement(sequence, epoch, zone_role(zone_id, role).str().c_str(), sensor, value,
                     observed_at);
}

IngestRecord zone_flow_measurement(std::uint64_t sequence, EpochId epoch, const char* zone_id,
                                   const char* sensor, Quantity value, TimestampMs observed_at) {
  return zone_measurement(sequence, epoch, zone_id, "flow", sensor, value, observed_at);
}

namespace {

/// Fill in the fields every producer-side declaration shares: identity,
/// sequence, epoch, the structure generation it was stated against, and the
/// receiving instant.
///
/// The generation is the one the fixtures adopt. A declaration that named no
/// generation would describe no plant in particular, and the freshness rules
/// would correctly refuse it as current.
IngestRecord declaration(std::uint64_t sequence, EpochId epoch, ObservationKind kind,
                         SubjectRef subject, AuthorityDomain domain, const char* authority,
                         TimestampMs observed_at) {
  IngestRecord record;
  record.kind = RecordKind::AddObservation;
  record.record_seq = RecordSeq(sequence);
  record.epoch = epoch;
  record.received_at_ms = observed_at;
  record.observation.kind = kind;
  record.observation.subject = std::move(subject);
  record.observation.observed_at_ms = observed_at;
  record.observation.received_at_ms = observed_at;
  record.observation.record_seq = RecordSeq(sequence);
  record.observation.epoch = epoch;
  record.observation.generation = GenerationId(1);
  record.observation.authority.domain = domain;
  record.observation.authority.authority = authority;
  return record;
}

}  // namespace

IngestRecord capability(std::uint64_t sequence, EpochId epoch, SubjectKind kind, const char* element,
                        Quantity capacity, std::int64_t derate_ppm, TimestampMs observed_at) {
  IngestRecord record =
      declaration(sequence, epoch, ObservationKind::Capability,
                  SubjectRef(kind, StrongId::from_validated(element)), AuthorityDomain::CoolingCapacity,
                  "dccp-cooling-capacity-accounting/1.0.0", observed_at);
  record.observation.capability_dimension = capacity.dimension;
  record.observation.declared_capacity = capacity.value;
  record.observation.derate_ppm = derate_ppm;
  return record;
}

IngestRecord equipment_state(std::uint64_t sequence, EpochId epoch, SubjectKind kind,
                             const char* element, LifecycleState state, TimestampMs observed_at) {
  IngestRecord record =
      declaration(sequence, epoch, ObservationKind::EquipmentState,
                  SubjectRef(kind, StrongId::from_validated(element)), AuthorityDomain::CoolingControl,
                  "dccp-cooling-control/1.0.0", observed_at);
  record.observation.state = state;
  return record;
}

IngestRecord constraint(std::uint64_t sequence, EpochId epoch, SubjectKind kind, const char* element,
                        ConstraintKind constraint_kind, ConstraintDirection direction, Quantity limit,
                        const char* origin_element, TimestampMs observed_at) {
  IngestRecord record =
      declaration(sequence, epoch, ObservationKind::Constraint,
                  SubjectRef(kind, StrongId::from_validated(element)), AuthorityDomain::CoolingFailure,
                  "dccp-cooling-failure-manager/1.0.0", observed_at);
  record.observation.constraint_kind = constraint_kind;
  record.observation.direction = direction;
  record.observation.constraint_state = ConstraintState::Active;
  record.observation.limit = limit;
  if (origin_element != nullptr) {
    record.observation.origin_element = Maybe<SubjectRef>::of(
        SubjectRef(kind, StrongId::from_validated(origin_element)));
  }
  return record;
}

IngestRecord failure(std::uint64_t sequence, EpochId epoch, SubjectKind kind, const char* element,
                     FailureKind failure_kind, FailureImpact impact, TimestampMs observed_at) {
  IngestRecord record =
      declaration(sequence, epoch, ObservationKind::Failure,
                  SubjectRef(kind, StrongId::from_validated(element)), AuthorityDomain::CoolingFailure,
                  "dccp-cooling-failure-manager/1.0.0", observed_at);
  record.observation.failure_kind = failure_kind;
  record.observation.severity = FailureSeverity::Major;
  record.observation.impact = impact;
  return record;
}

IngestRecord reserve_claim(std::uint64_t sequence, EpochId epoch, SubjectKind kind, const char* scope,
                           Quantity reserve, const std::vector<const char*>& assumes,
                           TimestampMs observed_at) {
  IngestRecord record =
      declaration(sequence, epoch, ObservationKind::ReserveClaim,
                  SubjectRef(kind, StrongId::from_validated(scope)), AuthorityDomain::CoolingCapacity,
                  "dccp-cooling-capacity-accounting/1.0.0", observed_at);
  record.observation.declared_reserve = reserve;
  for (const char* assumption : assumes) {
    record.observation.assumes_available.push_back(StrongId::from_validated(assumption));
  }
  return record;
}

std::vector<IngestRecord> baseline_records(EpochId epoch, TimestampMs now) {
  std::vector<IngestRecord> records;
  records.push_back(capability(1, epoch, SubjectKind::Pump, "pump.p1", Quantity::flow(40'000'000), 0,
                               now));
  records.push_back(capability(2, epoch, SubjectKind::Pump, "pump.p2", Quantity::flow(40'000'000), 0,
                               now));
  records.push_back(equipment_state(3, epoch, SubjectKind::Pump, "pump.p1", LifecycleState::Running,
                                    now));
  records.push_back(equipment_state(4, epoch, SubjectKind::Pump, "pump.p2", LifecycleState::Running,
                                    now));
  records.push_back(zone_measurement(5, epoch, "zone.a", "flow", "sensor.a.flow",
                                     Quantity::flow(30'000'000), now));
  records.push_back(zone_measurement(6, epoch, "zone.a", "supply_temp", "sensor.a.supply",
                                     Quantity::temperature(18'000), now));
  records.push_back(zone_measurement(7, epoch, "zone.a", "return_temp", "sensor.a.return",
                                     Quantity::temperature(24'000), now));
  records.push_back(zone_measurement(8, epoch, "zone.a", "diff_pressure", "sensor.a.dp",
                                     Quantity::pressure(120'000), now));
  records.push_back(zone_measurement(9, epoch, "zone.b", "flow", "sensor.b.flow",
                                     Quantity::flow(15'000'000), now));
  records.push_back(zone_measurement(10, epoch, "zone.b", "supply_temp", "sensor.b.supply",
                                     Quantity::temperature(19'000), now));
  records.push_back(zone_measurement(11, epoch, "zone.b", "return_temp", "sensor.b.return",
                                     Quantity::temperature(24'000), now));
  records.push_back(zone_measurement(12, epoch, "zone.b", "diff_pressure", "sensor.b.dp",
                                     Quantity::pressure(90'000), now));
  return records;
}

void require_no_rejections(const IngestReport& report, const char* what) {
  if (!report.rejections.empty()) {
    const IngestReport::Rejection& first = report.rejections.front();
    record_failure(std::string(what) + ": a record was rejected: " + first.reason + ": " +
                       first.detail,
                   __FILE__, __LINE__);
  }
}

ObservationReport query(Engine& engine, QueryKind kind) {
  ObserveRequest request;
  request.kind = kind;
  const auto report = engine.observe(request);
  if (!report) {
    record_failure(std::string("query failed: ") + std::string(to_token(report.error().code)) + ": " +
                       report.error().reason + ": " + report.error().detail,
                   __FILE__, __LINE__);
    throw Failure("query failed");
  }
  return report.value();
}

ObservationReport query(Engine& engine, QueryKind kind, const QueryFilter& filter) {
  ObserveRequest request;
  request.kind = kind;
  request.filter = filter;
  const auto report = engine.observe(request);
  if (!report) {
    record_failure(std::string("query failed: ") + std::string(to_token(report.error().code)) + ": " +
                       report.error().reason + ": " + report.error().detail,
                   __FILE__, __LINE__);
    throw Failure("query failed");
  }
  return report.value();
}

const DeliveryObservation& delivery_for(const DeliveryReport& report, const char* zone_id) {
  const std::string wanted = std::string("dp.") + zone_id;
  for (const DeliveryObservation& observation : report.points) {
    if (observation.point.id.view() == wanted) {
      return observation;
    }
  }
  record_failure(std::string("no delivery observation for ") + zone_id, __FILE__, __LINE__);
  throw Failure("delivery observation not found");
}

const DivergenceFinding& divergence_for(const DivergenceReport& report, const char* point_id) {
  for (const DivergenceFinding& finding : report.findings) {
    if (finding.point.view() == point_id) {
      return finding;
    }
  }
  record_failure(std::string("no divergence finding for ") + point_id, __FILE__, __LINE__);
  throw Failure("divergence finding not found");
}

namespace {

void collect_reasons(const ObservationReport& report, std::vector<std::string>& out) {
  for (const Indeterminacy& indeterminacy : report.indeterminacies) {
    out.push_back(indeterminacy.reason);
  }
  for (const DeliveryObservation& observation : report.delivery.points) {
    for (const Indeterminacy& indeterminacy : observation.indeterminacies) {
      out.push_back(indeterminacy.reason);
    }
  }
  for (const ConstraintAttribution& attribution : report.constraints.constraints) {
    for (const Indeterminacy& indeterminacy : attribution.indeterminacies) {
      out.push_back(indeterminacy.reason);
    }
  }
  for (const FailureAssessment& assessment : report.failures.failures) {
    for (const Indeterminacy& indeterminacy : assessment.indeterminacies) {
      out.push_back(indeterminacy.reason);
    }
  }
  for (const ReserveObservation& observation : report.reserve.scopes) {
    for (const Indeterminacy& indeterminacy : observation.indeterminacies) {
      out.push_back(indeterminacy.reason);
    }
  }
  for (const DivergenceFinding& finding : report.divergence.findings) {
    for (const Indeterminacy& indeterminacy : finding.indeterminacies) {
      out.push_back(indeterminacy.reason);
    }
    for (const Contradiction& contradiction : finding.contradictions) {
      out.push_back(std::string(to_token(contradiction.kind)));
    }
  }
  for (const CoverageGap& gap : report.coverage.gaps) {
    out.push_back(gap.reason);
  }
}

}  // namespace

bool mentions_reason(const ObservationReport& report, const std::string& reason) {
  std::vector<std::string> reasons;
  collect_reasons(report, reasons);
  return std::find(reasons.begin(), reasons.end(), reason) != reasons.end();
}

}  // namespace cotest