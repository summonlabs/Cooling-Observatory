# Limits and what is not modelled

## Bounded resources

Every collection this library builds is bounded by a number in `Limits`. A cooling
facility can produce unbounded evidence, and an observatory that accepts unbounded
input is one that a single misbehaving collector can stop.

| Limit | Default | Bounds |
| --- | --- | --- |
| `max_facilities` | 16 | Sites |
| `max_plants` | 256 | Plants |
| `max_loops` | 4096 | Loops |
| `max_elements` | 65536 | Pumps, valves, chillers, CDUs, CRAH/CRAC, manifolds |
| `max_zones` | 16384 | Thermal zones |
| `max_links` | 262144 | Structural edges |
| `max_measurements` | 200000 | Retained observations |
| `max_records` | 4096 | Records accepted in one ingest call |
| `max_report_rows` | 65536 | Rows in one answer |
| `max_traversal_nodes` | 65536 | Nodes in one traversal |
| `max_traversal_depth` | 64 | Depth of one traversal (above 4096 is refused as a cycle) |
| `max_explanation_lines` | 512 | Lines of one rendered explanation |
| `max_evidence_refs` | 64 | Evidence references carried by one value |
| `max_snapshot_bytes` | 512 MiB | Durable state size |
| `max_identity_length` | 96 bytes | One identity |
| `max_tolerance` | 1e9 | Widest accepted equality tolerance |

A breach is a reported outcome with the limit named, never a silent truncation.
Structural limits must be positive and mutually satisfiable: a limit set in which
the measurement ceiling exceeds the byte ceiling is refused at validation, because
no snapshot could ever hold it.

## Numeric range

Quantities are signed 64-bit fixed point at a per-dimension base scale. Arithmetic
is checked: an overflowing sum, product, quotient or unit conversion returns no
value and a reason rather than wrapping. Where a physical computation needs a wider
intermediate — heat removal multiplies three quantities — the intermediate is
formed exactly and the failure is reported at the step that overflowed, with the
step named.

Measured values that land outside what the model can express are reported as
unknown with a reason, never clamped. A computed heat removal is reported at the
nearest watt because it is derived from measurements; unit conversion rounds to
the nearest base-scale step for the same reason; a quantity parsed from text
round-trips exactly when written in its canonical unit.

## What is deliberately not modelled

- **Thermodynamics beyond a volumetric heat capacity.** Heat removal is measured
  flow times measured temperature difference times a configured capacity. There is
  no enthalpy model, no phase change, no humidity, no altitude correction.
- **Pressure drop from geometry.** Differential pressure is an observation. This
  runtime never computes what it should be from pipe lengths, fittings or valve
  positions.
- **Flow networks.** Flow is measured, not solved. There is no continuity solver,
  no hydraulic balancing and no network simulation.
- **Capacity accounting.** Declared capability is consumed from the accounting
  authority. This runtime does not sum capabilities into a capacity figure, does
  not apportion capacity, and does not decide what a plant can deliver. Where it
  needs an expectation for divergence it uses the smallest current declaration on
  the delivery path, and says so.
- **Reliability modelling.** A declaration marked as depending on redundancy is
  recorded as depending on redundancy; the runtime does not evaluate whether the
  redundant element would actually start.
- **Incident state.** Failure state is consumed, never declared, and severity is
  echoed rather than assigned.
- **Control.** No command path exists. Nothing in this repository can move an
  actuator.
- **Time series.** Evidence is reduced to a current belief with a freshness, not
  stored as a series. History reports what was committed, not what was measured
  over time.
- **Forecasting.** Nothing is predicted.

## Known limitations

- Airflow and liquid flow share one dimension and one arithmetic; the medium only
  selects which heat capacity converts the flow into heat. Where a point is
  instrumented for both, the declared flow role is the one that speaks.
- A constraint whose kind has no measured counterpart at a point (a supply
  temperature bound, a valve position, a pump speed) is reported with its limit
  and without a measured margin, rather than against the wrong quantity.
- Coverage is per subject and per axis, and reports only what has no current
  evidence. It makes no claim about a covered subject.
- The writer lock is advisory on POSIX and kernel-enforced on Windows. A process
  that deliberately ignores it can still write the files.
- Extended-length paths on Windows are applied to absolute paths only; a very long
  relative path remains subject to the classic limit.
- A structure arriving in pieces is reported as dangling edges rather than
  refused, so a partially adopted generation is visible instead of fatal. Resolving
  it is the topology authority's job.
