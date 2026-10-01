// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/persistence.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "dccp/cooling_observatory/checksum.hpp"
#include "dccp/cooling_observatory/clock.hpp"
#include "dccp/cooling_observatory/version.hpp"
#include "file_ops.hpp"

namespace dccp::cooling_observatory {
namespace {

/// The live state file. One generation per file, replaced atomically, so the
/// name never refers to a partially written file.
constexpr std::string_view kStateFileName = "cooling-observatory.state";
/// The writer lock. Held for as long as the store is open.
constexpr std::string_view kLockFileName = "cooling-observatory.lock";

constexpr std::uint32_t kMagic = 0x434F4253u;  // "COBS"
constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kFrameOverhead = 8;

// ---------------------------------------------------------------------------
// Little-endian primitives. The durable format is explicitly little-endian so
// that a state file written on one machine decodes on another.
// ---------------------------------------------------------------------------

void put_u16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void put_u32(std::string& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
  }
}

void put_u64(std::string& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
  }
}

void put_i64(std::string& out, std::int64_t value) { put_u64(out, static_cast<std::uint64_t>(value)); }

void put_bool(std::string& out, bool value) { out.push_back(value ? '\x01' : '\x00'); }

/// Length-prefixed bytes. Every variable-length field is prefixed, so no field
/// can be reinterpreted as the start of the next one.
void put_bytes(std::string& out, std::string_view bytes) {
  put_u32(out, static_cast<std::uint32_t>(bytes.size()));
  out.append(bytes);
}

void put_string(std::string& out, std::string_view text) { put_bytes(out, text); }

void put_optional_quantity(std::string& out, const Maybe<Quantity>& value) {
  put_bool(out, value.has_value());
  if (value.has_value()) {
    put_u16(out, static_cast<std::uint16_t>(value.value().dimension));
    put_i64(out, value.value().value);
  }
}

void put_maybe_subject(std::string& out, const Maybe<SubjectRef>& value) {
  put_bool(out, value.has_value());
  if (value.has_value()) {
    put_u16(out, static_cast<std::uint16_t>(value.value().kind));
    put_string(out, value.value().id.view());
  }
}

class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }
  [[nodiscard]] const std::string& failure() const noexcept { return failure_; }

  std::uint16_t u16() {
    if (remaining() < 2) {
      fail("truncated_u16");
      return 0;
    }
    const std::uint16_t value = static_cast<std::uint16_t>(
        static_cast<unsigned char>(bytes_[position_]) |
        (static_cast<unsigned int>(static_cast<unsigned char>(bytes_[position_ + 1])) << 8));
    position_ += 2;
    return value;
  }

  std::uint32_t u32() {
    if (remaining() < 4) {
      fail("truncated_u32");
      return 0;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[position_ + i])) << (8 * i);
    }
    position_ += 4;
    return value;
  }

  std::uint64_t u64() {
    if (remaining() < 8) {
      fail("truncated_u64");
      return 0;
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[position_ + i])) << (8 * i);
    }
    position_ += 8;
    return value;
  }

  std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

  bool boolean() {
    if (remaining() < 1) {
      fail("truncated_bool");
      return false;
    }
    const unsigned char value = static_cast<unsigned char>(bytes_[position_]);
    position_ += 1;
    if (value > 1) {
      fail("invalid_bool");
      return false;
    }
    return value == 1;
  }

  std::string bytes() {
    const std::uint32_t length = u32();
    if (!ok_) {
      return {};
    }
    if (remaining() < length) {
      fail("truncated_bytes");
      return {};
    }
    std::string out(bytes_.substr(position_, length));
    position_ += length;
    return out;
  }

  bool skip(std::size_t count) {
    if (remaining() < count) {
      fail("truncated_skip");
      return false;
    }
    position_ += count;
    return true;
  }

 private:
  void fail(const char* reason) {
    if (ok_) {
      ok_ = false;
      failure_ = reason;
    }
  }

  std::string_view bytes_;
  std::size_t position_ = 0;
  bool ok_ = true;
  std::string failure_{};
};

void encode_observation(std::string& out, const Observation& observation) {
  put_u16(out, static_cast<std::uint16_t>(observation.kind));
  put_u16(out, static_cast<std::uint16_t>(observation.subject.kind));
  put_string(out, observation.subject.id.view());
  put_string(out, observation.sensor.view());
  put_u16(out, static_cast<std::uint16_t>(observation.authority.domain));
  put_string(out, observation.authority.authority);
  put_u64(out, observation.authority.generation);
  put_string(out, observation.authority.digest);
  put_u64(out, observation.epoch.value());
  put_i64(out, observation.observed_at_ms);
  put_i64(out, observation.received_at_ms);
  put_u16(out, static_cast<std::uint16_t>(observation.origin));
  put_u64(out, observation.record_seq.value());
  put_u64(out, observation.generation.value());
  put_u16(out, static_cast<std::uint16_t>(observation.measured.dimension));
  put_i64(out, observation.measured.value);
  put_u16(out, static_cast<std::uint16_t>(observation.quality));
  put_string(out, observation.unit_text);
  put_u16(out, static_cast<std::uint16_t>(observation.state));
  put_u16(out, static_cast<std::uint16_t>(observation.capability_dimension));
  put_i64(out, observation.declared_capacity);
  put_i64(out, observation.derate_ppm);
  put_bool(out, observation.depends_on_redundancy);
  put_u16(out, static_cast<std::uint16_t>(observation.declared_reserve.dimension));
  put_i64(out, observation.declared_reserve.value);
  put_u32(out, static_cast<std::uint32_t>(observation.assumes_available.size()));
  for (const StrongId& assumption : observation.assumes_available) {
    put_string(out, assumption.view());
  }
  put_u16(out, static_cast<std::uint16_t>(observation.constraint_kind));
  put_u16(out, static_cast<std::uint16_t>(observation.direction));
  put_u16(out, static_cast<std::uint16_t>(observation.constraint_state));
  put_u16(out, static_cast<std::uint16_t>(observation.limit.dimension));
  put_i64(out, observation.limit.value);
  put_maybe_subject(out, observation.origin_element);
  put_u16(out, static_cast<std::uint16_t>(observation.failure_kind));
  put_u16(out, static_cast<std::uint16_t>(observation.severity));
  put_u16(out, static_cast<std::uint16_t>(observation.impact));
  put_optional_quantity(out, observation.residual_delivery);
  put_string(out, observation.detail);
  put_u64(out, observation.committed_revision.value());
  put_u64(out, observation.committed_ordinal);
}

Result<Observation> decode_observation(Reader& reader) {
  Observation observation;
  observation.kind = static_cast<ObservationKind>(reader.u16());
  observation.subject.kind = static_cast<SubjectKind>(reader.u16());
  observation.subject.id = StrongId::from_validated(reader.bytes());
  observation.sensor = SensorId(StrongId::from_validated(reader.bytes()));
  observation.authority.domain = static_cast<AuthorityDomain>(reader.u16());
  observation.authority.authority = reader.bytes();
  observation.authority.generation = reader.u64();
  observation.authority.digest = reader.bytes();
  observation.epoch = EpochId(reader.u64());
  observation.observed_at_ms = reader.i64();
  observation.received_at_ms = reader.i64();
  observation.origin = static_cast<EvidenceOrigin>(reader.u16());
  observation.record_seq = RecordSeq(reader.u64());
  observation.generation = GenerationId(reader.u64());
  observation.measured.dimension = static_cast<Dimension>(reader.u16());
  observation.measured.value = reader.i64();
  observation.quality = static_cast<MeasurementQuality>(reader.u16());
  observation.unit_text = reader.bytes();
  observation.state = static_cast<LifecycleState>(reader.u16());
  observation.capability_dimension = static_cast<Dimension>(reader.u16());
  observation.declared_capacity = reader.i64();
  observation.derate_ppm = reader.i64();
  observation.depends_on_redundancy = reader.boolean();
  observation.declared_reserve.dimension = static_cast<Dimension>(reader.u16());
  observation.declared_reserve.value = reader.i64();
  const std::uint32_t assumption_count = reader.u32();
  if (!reader.ok() || assumption_count > 65536) {
    return fail(Code::CorruptSnapshot, "corrupt_assumption_count",
                "the assumption count in the snapshot is not plausible");
  }
  for (std::uint32_t i = 0; i < assumption_count; ++i) {
    observation.assumes_available.push_back(StrongId::from_validated(reader.bytes()));
  }
  observation.constraint_kind = static_cast<ConstraintKind>(reader.u16());
  observation.direction = static_cast<ConstraintDirection>(reader.u16());
  observation.constraint_state = static_cast<ConstraintState>(reader.u16());
  observation.limit.dimension = static_cast<Dimension>(reader.u16());
  observation.limit.value = reader.i64();
  const bool has_origin = reader.boolean();
  if (has_origin) {
    SubjectRef origin;
    origin.kind = static_cast<SubjectKind>(reader.u16());
    origin.id = StrongId::from_validated(reader.bytes());
    observation.origin_element = Maybe<SubjectRef>::of(origin);
  }
  observation.failure_kind = static_cast<FailureKind>(reader.u16());
  observation.severity = static_cast<FailureSeverity>(reader.u16());
  observation.impact = static_cast<FailureImpact>(reader.u16());
  const bool has_residual = reader.boolean();
  if (has_residual) {
    Quantity residual;
    residual.dimension = static_cast<Dimension>(reader.u16());
    residual.value = reader.i64();
    observation.residual_delivery = Maybe<Quantity>::of(residual);
  }
  observation.detail = reader.bytes();
  observation.committed_revision = Revision(reader.u64());
  observation.committed_ordinal = reader.u64();
  if (!reader.ok()) {
    return fail(Code::CorruptSnapshot, "corrupt_observation", reader.failure());
  }
  return observation;
}

void encode_delivery_point(std::string& out, const DeliveryPoint& point) {
  put_string(out, point.id.view());
  put_string(out, point.zone.view());
  put_string(out, point.loop.view());
  put_u16(out, static_cast<std::uint16_t>(point.medium));
  put_string(out, point.flow_measurement.view());
  put_string(out, point.supply_temperature.view());
  put_string(out, point.return_temperature.view());
  put_string(out, point.differential_pressure.view());
  put_string(out, point.loop_differential_pressure.view());
  put_string(out, point.airflow.view());
  put_optional_quantity(out, point.declared_load);
}

Result<DeliveryPoint> decode_delivery_point(Reader& reader) {
  DeliveryPoint point;
  point.id = StrongId::from_validated(reader.bytes());
  point.zone = ZoneId(StrongId::from_validated(reader.bytes()));
  point.loop = LoopId(StrongId::from_validated(reader.bytes()));
  point.medium = static_cast<CoolantMedium>(reader.u16());
  point.flow_measurement = MeasurementId(StrongId::from_validated(reader.bytes()));
  point.supply_temperature = MeasurementId(StrongId::from_validated(reader.bytes()));
  point.return_temperature = MeasurementId(StrongId::from_validated(reader.bytes()));
  point.differential_pressure = MeasurementId(StrongId::from_validated(reader.bytes()));
  point.loop_differential_pressure = MeasurementId(StrongId::from_validated(reader.bytes()));
  point.airflow = MeasurementId(StrongId::from_validated(reader.bytes()));
  const bool has_load = reader.boolean();
  if (has_load) {
    Quantity load;
    load.dimension = static_cast<Dimension>(reader.u16());
    load.value = reader.i64();
    point.declared_load = Maybe<Quantity>::of(load);
  }
  if (!reader.ok()) {
    return fail(Code::CorruptSnapshot, "corrupt_delivery_point", reader.failure());
  }
  return point;
}

void encode_structure(std::string& out, const PlantModel& model) {
  put_u32(out, static_cast<std::uint32_t>(model.facilities().size()));
  for (const FacilityId& id : model.facilities()) {
    put_string(out, id.view());
  }
  put_u32(out, static_cast<std::uint32_t>(model.plants().size()));
  for (const CoolingPlant& plant : model.plants()) {
    put_string(out, plant.id.view());
    put_string(out, plant.facility.view());
    put_string(out, plant.label);
  }
  put_u32(out, static_cast<std::uint32_t>(model.loops().size()));
  for (const Loop& loop : model.loops()) {
    put_string(out, loop.id.view());
    put_string(out, loop.plant.view());
    put_bool(out, loop.secondary);
    put_string(out, loop.label);
  }
  put_u32(out, static_cast<std::uint32_t>(model.components().size()));
  for (const PlantComponent& component : model.components()) {
    put_u16(out, static_cast<std::uint16_t>(component.kind));
    put_string(out, component.id.view());
    put_string(out, component.loop.view());
    put_string(out, component.label);
    put_u32(out, static_cast<std::uint32_t>(component.needs.size()));
    for (const NeedKind need : component.needs) {
      put_u16(out, static_cast<std::uint16_t>(need));
    }
  }
  put_u32(out, static_cast<std::uint32_t>(model.zones().size()));
  for (const ThermalZone& zone : model.zones()) {
    put_string(out, zone.id.view());
    put_string(out, zone.facility.view());
    put_string(out, zone.label);
    put_optional_quantity(out, zone.declared_load);
    put_optional_quantity(out, zone.max_supply_temperature);
  }
  put_u32(out, static_cast<std::uint32_t>(model.links().size()));
  for (const PlantLink& link : model.links()) {
    put_u16(out, static_cast<std::uint16_t>(link.from.kind));
    put_string(out, link.from.id.view());
    put_u16(out, static_cast<std::uint16_t>(link.to.kind));
    put_string(out, link.to.id.view());
    put_u16(out, static_cast<std::uint16_t>(link.relation));
  }
}

Result<PlantModel> decode_structure(Reader& reader) {
  PlantModel model;
  const std::uint32_t facility_count = reader.u32();
  if (!reader.ok() || facility_count > 1000000) {
    return fail(Code::CorruptSnapshot, "corrupt_facility_count", "implausible facility count");
  }
  for (std::uint32_t i = 0; i < facility_count; ++i) {
    model.add_facility(FacilityId(StrongId::from_validated(reader.bytes())), std::string());
  }
  const std::uint32_t plant_count = reader.u32();
  if (!reader.ok() || plant_count > 1000000) {
    return fail(Code::CorruptSnapshot, "corrupt_plant_count", "implausible plant count");
  }
  for (std::uint32_t i = 0; i < plant_count; ++i) {
    CoolingPlant plant;
    plant.id = PlantId(StrongId::from_validated(reader.bytes()));
    plant.facility = FacilityId(StrongId::from_validated(reader.bytes()));
    plant.label = reader.bytes();
    model.add_plant(std::move(plant));
  }
  const std::uint32_t loop_count = reader.u32();
  if (!reader.ok() || loop_count > 10000000) {
    return fail(Code::CorruptSnapshot, "corrupt_loop_count", "implausible loop count");
  }
  for (std::uint32_t i = 0; i < loop_count; ++i) {
    Loop loop;
    loop.id = LoopId(StrongId::from_validated(reader.bytes()));
    loop.plant = PlantId(StrongId::from_validated(reader.bytes()));
    loop.secondary = reader.boolean();
    loop.label = reader.bytes();
    model.add_loop(std::move(loop));
  }
  const std::uint32_t component_count = reader.u32();
  if (!reader.ok() || component_count > 10000000) {
    return fail(Code::CorruptSnapshot, "corrupt_component_count", "implausible component count");
  }
  for (std::uint32_t i = 0; i < component_count; ++i) {
    PlantComponent component;
    component.kind = static_cast<ComponentKind>(reader.u16());
    component.id = StrongId::from_validated(reader.bytes());
    component.loop = LoopId(StrongId::from_validated(reader.bytes()));
    component.label = reader.bytes();
    const std::uint32_t need_count = reader.u32();
    if (!reader.ok() || need_count > 64) {
      return fail(Code::CorruptSnapshot, "corrupt_need_count", "implausible need count");
    }
    for (std::uint32_t n = 0; n < need_count; ++n) {
      component.needs.push_back(static_cast<NeedKind>(reader.u16()));
    }
    model.add_component(std::move(component));
  }
  const std::uint32_t zone_count = reader.u32();
  if (!reader.ok() || zone_count > 10000000) {
    return fail(Code::CorruptSnapshot, "corrupt_zone_count", "implausible zone count");
  }
  for (std::uint32_t i = 0; i < zone_count; ++i) {
    ThermalZone zone;
    zone.id = ZoneId(StrongId::from_validated(reader.bytes()));
    zone.facility = FacilityId(StrongId::from_validated(reader.bytes()));
    zone.label = reader.bytes();
    if (reader.boolean()) {
      Quantity load;
      load.dimension = static_cast<Dimension>(reader.u16());
      load.value = reader.i64();
      zone.declared_load = Maybe<Quantity>::of(load);
    }
    if (reader.boolean()) {
      Quantity bound;
      bound.dimension = static_cast<Dimension>(reader.u16());
      bound.value = reader.i64();
      zone.max_supply_temperature = Maybe<Quantity>::of(bound);
    }
    model.add_zone(std::move(zone));
  }
  const std::uint32_t link_count = reader.u32();
  if (!reader.ok() || link_count > 100000000) {
    return fail(Code::CorruptSnapshot, "corrupt_link_count", "implausible link count");
  }
  for (std::uint32_t i = 0; i < link_count; ++i) {
    PlantLink link;
    link.from.kind = static_cast<SubjectKind>(reader.u16());
    link.from.id = StrongId::from_validated(reader.bytes());
    link.to.kind = static_cast<SubjectKind>(reader.u16());
    link.to.id = StrongId::from_validated(reader.bytes());
    link.relation = static_cast<LinkRelation>(reader.u16());
    model.add_link(std::move(link));
  }
  if (!reader.ok()) {
    return fail(Code::CorruptSnapshot, "corrupt_structure", reader.failure());
  }
  model.reindex();
  return model;
}

void encode_policy(std::string& out, const FreshnessPolicy& policy) {
  put_i64(out, policy.structure_max_age_ms);
  put_i64(out, policy.delivery_max_age_ms);
  put_i64(out, policy.condition_max_age_ms);
  put_i64(out, policy.capability_max_age_ms);
  put_i64(out, policy.reserve_max_age_ms);
  put_i64(out, policy.fault_max_age_ms);
  put_bool(out, policy.accept_other_epoch_as_current);
  put_bool(out, policy.accept_other_generation_as_current);
}

FreshnessPolicy decode_policy(Reader& reader) {
  FreshnessPolicy policy;
  policy.structure_max_age_ms = reader.i64();
  policy.delivery_max_age_ms = reader.i64();
  policy.condition_max_age_ms = reader.i64();
  policy.capability_max_age_ms = reader.i64();
  policy.reserve_max_age_ms = reader.i64();
  policy.fault_max_age_ms = reader.i64();
  policy.accept_other_epoch_as_current = reader.boolean();
  policy.accept_other_generation_as_current = reader.boolean();
  return policy;
}

void encode_thermal(std::string& out, const ThermalPolicy& policy) {
  put_i64(out, policy.coolant_heat_capacity_uj_per_l_k);
  put_i64(out, policy.air_heat_capacity_uj_per_l_k);
  put_bool(out, policy.clamp_negative_removal);
}

ThermalPolicy decode_thermal(Reader& reader) {
  ThermalPolicy policy;
  policy.coolant_heat_capacity_uj_per_l_k = reader.i64();
  policy.air_heat_capacity_uj_per_l_k = reader.i64();
  policy.clamp_negative_removal = reader.boolean();
  return policy;
}

}  // namespace

std::string image_digest(const ObservationImage& image) {
  // The digest covers what an answer depends on: the revision, the epoch, the
  // adopted generation and the retained evidence. It deliberately does not
  // cover transient fields, so two engines that hold the same evidence at the
  // same revision digest identically whatever order they were fed in.
  std::string canonical;
  canonical.reserve(256);
  canonical += "cooling-observatory/image/1;";
  canonical += std::to_string(image.revision.value());
  canonical += ';';
  canonical += std::to_string(image.sequence.value());
  canonical += ';';
  canonical += std::to_string(image.epoch.value());
  canonical += ';';
  canonical += std::to_string(image.generation.value());
  canonical += ';';
  canonical += image.structure.digest();
  canonical += ';';
  for (const Observation& observation : image.evidence) {
    canonical += std::to_string(static_cast<unsigned>(observation.kind));
    canonical += ':';
    canonical += std::to_string(static_cast<unsigned>(observation.subject.kind));
    canonical += ':';
    canonical += observation.subject.id.view();
    canonical += ':';
    canonical += observation.sensor.view();
    canonical += ':';
    canonical += std::to_string(observation.epoch.value());
    canonical += ':';
    canonical += std::to_string(observation.record_seq.value());
    canonical += ':';
    canonical += std::to_string(observation.observed_at_ms);
    canonical += ':';
    canonical += std::to_string(static_cast<unsigned>(observation.measured.dimension));
    canonical += ':';
    canonical += std::to_string(observation.measured.value);
    canonical += ':';
    canonical += std::to_string(observation.declared_capacity);
    canonical += ':';
    canonical += std::to_string(observation.limit.value);
    canonical += ':';
    canonical += std::to_string(observation.committed_revision.value());
    canonical += ':';
    canonical += std::to_string(observation.committed_ordinal);
    canonical += ';';
  }
  for (const RecordSeq& key : image.applied_keys) {
    canonical += std::to_string(key.value());
    canonical += ',';
  }
  return sha256_hex(canonical);
}

Result<std::string> encode_image(const ObservationImage& image, const Limits& limits) {
  std::string payload;
  payload.reserve(4096 + image.evidence.size() * 256);

  put_u64(payload, image.revision.value());
  put_u64(payload, image.sequence.value());
  put_u64(payload, image.epoch.value());
  put_u64(payload, image.generation.value());
  put_i64(payload, image.as_of_ms);
  put_bool(payload, image.recovered);

  encode_structure(payload, image.structure);
  encode_policy(payload, image.freshness_policy);
  encode_thermal(payload, image.thermal_policy);

  put_u32(payload, static_cast<std::uint32_t>(image.evidence.size()));
  for (const Observation& observation : image.evidence) {
    encode_observation(payload, observation);
  }

  put_u32(payload, static_cast<std::uint32_t>(image.delivery_points.size()));
  for (const DeliveryPoint& point : image.delivery_points) {
    encode_delivery_point(payload, point);
  }

  put_u32(payload, static_cast<std::uint32_t>(image.retired_sensors.size()));
  for (const auto& retirement : image.retired_sensors) {
    put_string(payload, retirement.first.view());
    put_u64(payload, retirement.second.value());
  }

  put_u32(payload, static_cast<std::uint32_t>(image.applied_keys.size()));
  for (const RecordSeq& key : image.applied_keys) {
    put_u64(payload, key.value());
  }

  if (payload.size() + kHeaderBytes > limits.max_snapshot_bytes) {
    return fail(Code::LimitExceeded, "snapshot_too_large",
                "the encoded state is " + std::to_string(payload.size() + kHeaderBytes) +
                    " bytes, above the configured limit of " + std::to_string(limits.max_snapshot_bytes));
  }

  std::string out;
  out.reserve(payload.size() + kHeaderBytes);
  put_u32(out, kMagic);
  put_u16(out, kSnapshotSchemaVersion);
  put_u16(out, 0);  // header flags, reserved and written as zero
  put_u64(out, static_cast<std::uint64_t>(payload.size()));
  put_u32(out, crc32c(payload));
  put_u64(out, image.revision.value());
  put_u64(out, image.sequence.value());
  put_u64(out, image.epoch.value());
  put_u64(out, image.generation.value());
  put_i64(out, image.as_of_ms);
  put_u32(out, static_cast<std::uint32_t>(image.evidence.size()));
  // The fields above sum to exactly kHeaderBytes, which is the point: the fixed
  // header is fully accounted for by values the decoder reads, with no padding
  // whose meaning a future reader would have to guess.
  if (out.size() != kHeaderBytes) {
    return fail(Code::InternalError, "header_size_mismatch",
                "the encoded header is not the size the decoder expects: " +
                    std::to_string(out.size()) + " bytes");
  }
  out.append(payload);
  return out;
}

Result<ObservationImage> decode_image(std::string_view bytes, std::size_t max_bytes) {
  if (bytes.size() > max_bytes) {
    return fail(Code::LimitExceeded, "snapshot_too_large", "the file exceeds the configured limit");
  }
  if (bytes.size() < kHeaderBytes) {
    return fail(Code::CorruptSnapshot, "snapshot_truncated_header",
                "the file is shorter than the fixed header");
  }
  Reader header(bytes.substr(0, kHeaderBytes));
  const std::uint32_t magic = header.u32();
  if (!header.ok() || magic != kMagic) {
    return fail(Code::CorruptSnapshot, "snapshot_bad_magic",
                "the file does not begin with the state-file signature");
  }
  const std::uint16_t schema = header.u16();
  const std::uint16_t flags = header.u16();
  const std::uint64_t payload_size = header.u64();
  const std::uint32_t payload_crc = header.u32();
  const std::uint64_t header_revision = header.u64();
  const std::uint64_t header_sequence = header.u64();
  const std::uint64_t header_epoch = header.u64();
  const std::uint64_t header_generation = header.u64();
  const std::int64_t header_time = header.i64();
  const std::uint32_t header_evidence_count = header.u32();

  if (!header.ok()) {
    return fail(Code::CorruptSnapshot, "snapshot_header_unreadable", header.failure());
  }
  if (schema != kSnapshotSchemaVersion) {
    return fail(Code::IncompatibleSchema, "snapshot_schema_unsupported",
                "the file declares schema " + std::to_string(schema) + " and this build reads " +
                    std::to_string(kSnapshotSchemaVersion));
  }
  if (flags != 0) {
    return fail(Code::IncompatibleSchema, "snapshot_flags_unsupported",
                "the file sets header flags this build does not define");
  }
  if (payload_size != bytes.size() - kHeaderBytes) {
    // The length is the frame's own statement about where it ends. A mismatch
    // means the tail is missing or extra, which is exactly the torn state a
    // crash during the write produces.
    return fail(Code::CorruptSnapshot, "snapshot_length_mismatch",
                "the header declares " + std::to_string(payload_size) + " payload bytes and the file has " +
                    std::to_string(bytes.size() - kHeaderBytes));
  }
  const std::string_view payload = bytes.substr(kHeaderBytes);
  const std::uint32_t actual_crc = crc32c(payload);
  if (actual_crc != payload_crc) {
    return fail(Code::CorruptSnapshot, "snapshot_checksum_mismatch",
                "the payload checksum does not match; the file is torn or was modified");
  }

  Reader reader(payload);
  ObservationImage image;
  image.revision = Revision(reader.u64());
  image.sequence = RecordSeq(reader.u64());
  image.epoch = EpochId(reader.u64());
  image.generation = GenerationId(reader.u64());
  image.as_of_ms = reader.i64();
  image.recovered = reader.boolean();
  if (!reader.ok()) {
    return fail(Code::CorruptSnapshot, "snapshot_preamble", reader.failure());
  }

  auto structure = decode_structure(reader);
  if (!structure) {
    return Error(structure.error().code, structure.error().reason,
                                  structure.error().detail);
  }
  image.structure = std::move(structure.value());
  image.freshness_policy = decode_policy(reader);
  image.thermal_policy = decode_thermal(reader);
  if (!reader.ok()) {
    return fail(Code::CorruptSnapshot, "snapshot_policy", reader.failure());
  }

  const std::uint32_t evidence_count = reader.u32();
  if (!reader.ok() || evidence_count > 100000000) {
    return fail(Code::CorruptSnapshot, "corrupt_evidence_count", "implausible evidence count");
  }
  image.evidence.reserve(evidence_count);
  for (std::uint32_t i = 0; i < evidence_count; ++i) {
    auto observation = decode_observation(reader);
    if (!observation) {
      return Error(observation.error().code, observation.error().reason,
                                    observation.error().detail);
    }
    image.evidence.push_back(std::move(observation.value()));
  }

  const std::uint32_t point_count = reader.u32();
  if (!reader.ok() || point_count > 10000000) {
    return fail(Code::CorruptSnapshot, "corrupt_point_count", "implausible delivery point count");
  }
  for (std::uint32_t i = 0; i < point_count; ++i) {
    auto point = decode_delivery_point(reader);
    if (!point) {
      return Error(point.error().code, point.error().reason, point.error().detail);
    }
    image.delivery_points.push_back(std::move(point.value()));
  }

  const std::uint32_t retirement_count = reader.u32();
  if (!reader.ok() || retirement_count > 1000000) {
    return fail(Code::CorruptSnapshot, "corrupt_retirement_count", "implausible retirement count");
  }
  for (std::uint32_t i = 0; i < retirement_count; ++i) {
    const SensorId sensor(StrongId::from_validated(reader.bytes()));
    const Revision revision(reader.u64());
    image.retired_sensors.emplace_back(sensor, revision);
  }

  const std::uint32_t key_count = reader.u32();
  if (!reader.ok() || key_count > 100000000) {
    return fail(Code::CorruptSnapshot, "corrupt_key_count", "implausible record key count");
  }
  image.applied_keys.reserve(key_count);
  for (std::uint32_t i = 0; i < key_count; ++i) {
    image.applied_keys.push_back(RecordSeq(reader.u64()));
  }

  if (!reader.ok()) {
    return fail(Code::CorruptSnapshot, "snapshot_trailing", reader.failure());
  }
  if (reader.remaining() != 0) {
    // The payload is self-delimiting: a decoder that finishes before the end
    // has found a file whose parts disagree about its length.
    return fail(Code::CorruptSnapshot, "snapshot_trailing_bytes",
                std::to_string(reader.remaining()) + " bytes remain after the last field");
  }

  // Conservation: the header's summary must agree with the payload it frames.
  // A disagreement means one of the two was written from a different state.
  if (image.revision.value() != header_revision || image.sequence.value() != header_sequence ||
      image.epoch.value() != header_epoch || image.generation.value() != header_generation ||
      image.as_of_ms != header_time ||
      static_cast<std::uint64_t>(image.evidence.size()) != header_evidence_count) {
    return fail(Code::CorruptSnapshot, "snapshot_header_payload_disagreement",
                "the header summary does not describe the payload");
  }

  image.structure.reindex();
  return image;
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

struct Store::Impl {
  StoreOptions options{};
  std::string directory{};
  ObservationImage image{};
  StoreManifest manifest{};
  bool open = false;
  LockHandle lock{};
  std::size_t last_encode_bytes = 0;
};

Store::Store() : impl_(std::make_unique<Impl>()) {}

Store::~Store() { close(); }

Status Store::open(const std::string& directory, const StoreOptions& options) {
  if (impl_->open) {
    return Status::failure(Code::LifecycleRefused, "store_already_open",
                           "this store is already open; close it before reopening");
  }
  const Status limits = validate(options.limits);
  if (!limits.ok()) {
    return limits;
  }
  const Status created = ensure_directory(directory);
  if (!created.ok()) {
    return created;
  }
  impl_->directory = directory;
  impl_->options = options;

  if (options.exclusive_writer) {
    auto locked = acquire_writer_lock(join_path(directory, std::string(kLockFileName)));
    if (!locked) {
      return Status::failure(locked.error().code, locked.error().reason, locked.error().detail);
    }
    impl_->lock = locked.value();
  }

  const std::string state_path = join_path(directory, std::string(kStateFileName));
  auto bytes = read_file(state_path, options.limits.max_snapshot_bytes);
  if (bytes.ok()) {
    auto decoded = decode_image(bytes.value(), options.limits.max_snapshot_bytes);
    if (decoded.ok()) {
      impl_->image = std::move(decoded.value());
      impl_->manifest.revision = impl_->image.revision;
      impl_->manifest.sequence = impl_->image.sequence;
      impl_->manifest.epoch = impl_->image.epoch;
      impl_->manifest.generation = impl_->image.generation;
      impl_->manifest.committed_at_ms = impl_->image.as_of_ms;
      impl_->manifest.evidence_count = impl_->image.evidence.size();
      impl_->manifest.digest = image_digest(impl_->image);
      impl_->manifest.schema_version = kSnapshotSchemaVersion;
      impl_->manifest.recovered = impl_->image.recovered;
      impl_->open = true;
      return Status::success();
    }
    // The live file is unusable. The store does not guess at a repair: it
    // reports the reason, keeps nothing from the damaged file, and leaves the
    // directory untouched so that an operator can inspect it. Refusing to open
    // is the conservative outcome; silently starting from empty state would
    // make a corrupt facility look like a facility with no equipment.
    release_writer_lock(impl_->lock);
    impl_->lock = LockHandle{};
    impl_->directory.clear();
    return Status::failure(decoded.error().code, decoded.error().reason,
                           std::string("the state file is not usable: ") + decoded.error().detail);
  }
  if (bytes.error().code != Code::NotFound) {
    release_writer_lock(impl_->lock);
    impl_->lock = LockHandle{};
    impl_->directory.clear();
    return Status::failure(bytes.error().code, bytes.error().reason, bytes.error().detail);
  }

  // No state yet. This store has never committed.
  impl_->image = ObservationImage{};
  impl_->manifest = StoreManifest{};
  impl_->manifest.schema_version = kSnapshotSchemaVersion;
  impl_->open = true;
  return Status::success();
}

Status Store::commit(const ObservationImage& image, StoreManifest& manifest_out) {
  if (!impl_->open) {
    return Status::failure(Code::StoreClosed, "store_not_open", "the store is not open");
  }
  auto encoded = encode_image(image, impl_->options.limits);
  if (!encoded) {
    return Status::failure(encoded.error().code, encoded.error().reason, encoded.error().detail);
  }
  const std::string& bytes = encoded.value();
  impl_->last_encode_bytes = bytes.size();

  const std::string state_path = join_path(impl_->directory, std::string(kStateFileName));
  const std::string temporary_path = join_path(impl_->directory, std::string(kStateFileName) + ".tmp");
  const Status written = write_file_atomic(temporary_path, state_path, bytes,
                                           impl_->options.sync_before_replace);
  if (!written.ok()) {
    return written;
  }

  impl_->image = image;
  impl_->manifest = StoreManifest{};
  impl_->manifest.revision = image.revision;
  impl_->manifest.sequence = image.sequence;
  impl_->manifest.epoch = image.epoch;
  impl_->manifest.generation = image.generation;
  impl_->manifest.committed_at_ms = image.as_of_ms;
  impl_->manifest.evidence_count = image.evidence.size();
  impl_->manifest.digest = image_digest(image);
  impl_->manifest.schema_version = kSnapshotSchemaVersion;
  impl_->manifest.recovered = image.recovered;
  manifest_out = impl_->manifest;
  return Status::success();
}

Status Store::reload(ObservationImage& image_out, StoreManifest& manifest_out) {
  if (!impl_->open) {
    return Status::failure(Code::StoreClosed, "store_not_open", "the store is not open");
  }
  const std::string state_path = join_path(impl_->directory, std::string(kStateFileName));
  auto bytes = read_file(state_path, impl_->options.limits.max_snapshot_bytes);
  if (!bytes) {
    return Status::failure(bytes.error().code, bytes.error().reason, bytes.error().detail);
  }
  auto decoded = decode_image(bytes.value(), impl_->options.limits.max_snapshot_bytes);
  if (!decoded) {
    return Status::failure(decoded.error().code, decoded.error().reason, decoded.error().detail);
  }
  image_out = std::move(decoded.value());
  impl_->manifest.revision = image_out.revision;
  impl_->manifest.sequence = image_out.sequence;
  impl_->manifest.epoch = image_out.epoch;
  impl_->manifest.generation = image_out.generation;
  impl_->manifest.committed_at_ms = image_out.as_of_ms;
  impl_->manifest.evidence_count = image_out.evidence.size();
  impl_->manifest.digest = image_digest(image_out);
  impl_->manifest.recovered = image_out.recovered;
  manifest_out = impl_->manifest;
  return Status::success();
}

const ObservationImage& Store::image() const { return impl_->image; }

const StoreManifest& Store::manifest() const { return impl_->manifest; }

bool Store::open_state() const noexcept { return impl_->open; }

const std::string& Store::directory() const noexcept { return impl_->directory; }

std::size_t Store::last_encode_bytes() const noexcept { return impl_->last_encode_bytes; }

void Store::close() {
  if (!impl_) {
    return;
  }
  release_writer_lock(impl_->lock);
  impl_->lock = LockHandle{};
  impl_->open = false;
  impl_->directory.clear();
  impl_->image = ObservationImage{};
}

}  // namespace dccp::cooling_observatory