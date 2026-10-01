# Composition with adjacent DCCP authorities

Cooling Observatory is one runtime in the Data Center Control Plane. It composes
with its neighbours through data and through the public C++ API, and it acquires
none of their authority by doing so.

## The pattern

Every piece of consumed evidence carries an authority reference:

```cpp
struct AuthorityRef {
  AuthorityDomain domain;      // who produced it
  std::string authority;       // e.g. "dccp-cooling-topology/1.0.0"
  std::uint64_t generation;    // that authority's generation counter
  std::string digest;          // optional canonical digest of the referenced content
};
```

The reference is recorded verbatim. It is never interpreted as permission, never
used to authorise anything, and never rewritten. Freshness of a referenced
generation is owned by the authority that produced it: this runtime can observe
that a reference is old and cannot make it new.

## Cooling Topology

Cooling Topology owns the generation-bound structural graph. This runtime consumes
one generation at a time through `RecordKind::AdoptStructure`, which carries the
generation number it was adopted from. Holding a structure is not owning it:

- every traversal, constraint attribution and delivery path cites the generation
  and the engine revision it was computed against;
- adopting a new generation makes declarations stated against the previous one
  stale, and does not delete them;
- a link whose endpoint is absent from the adopted generation is reported as a
  dangling edge rather than repaired or ignored, so a partially adopted generation
  is visible.

The interchange form accepts the same vocabulary — facilities, plants, loops,
pumps, valves, chillers, CDUs, CRAH/CRAC units, manifolds, branches, zones and
typed directed links — so a producer can translate a topology generation into
records without a lossy mapping.

## Cooling Capacity Accounting

Capacity is consumed as declarations: `observation capability` carries a declared
capacity in its own dimension, a derate in parts per million, and a flag saying
whether the declaration assumes redundancy elsewhere.

This runtime does not add those declarations into a capacity figure and does not
apportion them. It uses them for two things, both stated in the answer:

- **Reserve**, where declared capability of the equipment in a scope, restricted
  to elements whose reported condition currently permits delivery, is compared with
  measured delivery to produce *evidenced* headroom — reported next to, never
  merged with, *claimed* reserve.
- **Divergence**, where the smallest current declaration on a delivery path is the
  expectation a measured flow is compared against.

A declaration stated against a generation this runtime no longer holds is stale,
is reported with its reason, and supports no figure.

## Failure authority

Failures and constraints are consumed, never declared. `observation failure`
carries the failure kind, the declared severity, the declared impact and any
residual delivery the observer measured. The observatory's contribution is the
*effect* verdict:

| Verdict | Meaning |
| --- | --- |
| `confirmed` | Measured delivery is gone or reduced as claimed |
| `contradicted` | Measured delivery is at or above declared while the claim says otherwise |
| `residual` | The failure is real and delivery measurably continues |
| `unconfirmed` | The claim is current and no delivery evidence shows an effect |
| `indeterminate` | No current delivery evidence either way |

Declaring an incident, raising or clearing an alarm, and deciding what to do about
a failure all belong to the failure authority. Every assessment records the
authority that declared it.

## Facility telemetry

Measurements are consumed with their sensor identity, quality, unit text,
observation instant and receiving instant. Quality is respected: a sensor
reporting its own value as bad or unavailable is silence, not a zero.

Observations of one quantity from two sensors are not merged. They are either
reduced to an agreed value, or reported as conflicting with both sensors and both
values named.

## Thermal and power observation

Thermal and electrical observation runtimes supply measurements in the same
vocabulary. The axes they feed are the delivery and condition axes; a declaration
they make about state is a condition observation and is subject to the same age
bound.

## Workload placement

Declared zone load is consumed for one purpose: comparing it with measured heat
removal. The two are never merged — a declaration is reported as declared, a
measurement as measured, and the difference between them carries the freshness of
both.

## Local observation

`AuthorityDomain::LocalObservation` is the only domain this runtime originates
evidence in, and it originates none in the current release: every fact it holds
came from a producer. The domain exists so that a future derived figure can be
labelled as this runtime's own claim rather than as somebody else's.

## Downstream consumers

The installed package exports one target, `dccp::cooling_observatory`, and
depends on no other package. A consumer composes with this runtime the same way
this runtime composes with its neighbours: through typed public API values, with
provenance attached. `tests/downstream/` is a worked example.
