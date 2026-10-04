# Cooling Observatory

Cooling delivery observation, constraint localisation and divergence analysis.

Cooling Observatory answers one question about a data-centre cooling plant, from
evidence it did not produce:

> Given the cooling evidence currently held, what cooling is actually being
> delivered, where are the flow, pressure, airflow and heat-removal constraints,
> how much reserve is evidenced rather than merely claimed, what failures and
> inefficiencies are visible, and what remains unknown?

It is a C++20 library with a command-line inspection tool, a durable and
integrity-checked store, and a text interchange format. It depends on nothing
outside the C++20 standard library and the operating-system interfaces it needs
for durable atomic file replacement, advisory file locking and process handling.

## What this repository is not

This runtime **observes**. It does not own the plant, and it does not acquire
another authority's ownership by being able to see that authority's evidence.

It does not:

- command pumps, valves, CDUs, chillers, CRAH or CRAC units, or any actuator;
- own Cooling Topology, or Cooling Capacity Accounting, or declare incident state;
- authorise recovery, failover, evacuation or any change to the plant;
- originate evidence in another authority's domain, or promote that authority's
  recovered evidence to current on its behalf.

Every fact it holds carries the authority it came from, the generation of that
authority's state it was stated against, and the epoch of the control-plane
incarnation that produced it. Holding a fact is not owning it, an observation is
not a command, an acknowledgement is not an effect, configured state is not
observed state, and recovered evidence is never fresh evidence.

## The four things it refuses to do

1. **Silence is not zero.** A sensor that reports nothing, reports its own value
   as bad, or does not exist produces *unknown*, never a zero and never health. A
   delivery point with no evidence is reported as unevidenced rather than as a
   point delivering nothing.
2. **Disagreement is not averaged.** Two current sensors that disagree produce
   *conflicting* with both values named. This runtime never reports a middle value
   that no instrument measured.
3. **A claim is not a measurement.** Declared capability, declared reserve and
   declared failures are recorded with their provenance and reported next to the
   measurements, never merged into them.
4. **A recovered value is not a current one.** State restored after a restart
   describes the world before the restart. It stays readable and stops being
   current until new evidence arrives.

## Architecture

```
                    evidence in                        answers out
   topology / telemetry / capacity / failure  ─┐   ┌─  delivery, constraints, failures,
   declarations, generation-stamped            │   │   reserve, divergence, coverage,
                                               ▼   │   dependencies, history
                                        ┌──────────────────┐
                                        │      Engine      │  one lock, one committed
                                        │  ingest  observe │  revision, one generation
                                        └────────┬─────────┘
                                                 │ commit (atomic replace)
                                        ┌────────▼─────────┐
                                        │      Store       │  versioned, CRC-checked,
                                        │  state directory │  single-writer locked
                                        └──────────────────┘
```

Layers, each in its own translation unit with its own header:

| Layer | Headers | Responsibility |
| --- | --- | --- |
| Foundations | `error`, `checked`, `units`, `identity`, `checksum`, `clock`, `limits` | Checked integer arithmetic, exact fixed-point quantities, validated identities, CRC-32C and SHA-256, canonical timestamps |
| Semantics | `semantics`, `evidence` | Authority, evidence axes, the freshness lattice, lifecycle, provenance, belief reduction |
| Model | `plant`, `delivery` | Loops, plants, CDUs, pumps, valves, chillers, CRAH/CRAC, manifolds, airflow zones, delivery points, thermal policy |
| Analysis | `dependency`, `constraint`, `failure`, `reserve`, `divergence`, `coverage` | Constraint localisation, effect confirmation, claimed versus evidenced reserve, divergence and contradiction, coverage gaps |
| Runtime | `engine`, `ingest`, `query`, `persistence`, `textproto`, `render`, `stats` | Lifecycle, commits, idempotent ingestion, durable state, the interchange format, deterministic rendering |

## State, evidence and authority model

### Authority

Evidence is stamped with the domain it came from: `cooling_topology`,
`cooling_capacity`, `cooling_failure`, `cooling_control`, `facility_telemetry`,
`workload_placement`, `thermal_observation`, `power_observation`, and
`local_observation` — the last being the only domain this runtime originates.
A reference to another authority's state names that authority, its generation and
an optional digest. Those references are recorded verbatim and are never
interpreted as permission.

### Evidence axes

Freshness is judged **per axis**, because the axes age at completely different
rates and collapsing them is how an observatory reports a day-old structure as a
live constraint:

| Axis | Default age bound | What it answers |
| --- | --- | --- |
| `structure` | 24 hours | Which entities exist and how they connect |
| `delivery` | 30 seconds | Measured flow, pressure, airflow, temperature |
| `condition` | 5 minutes | Reported equipment lifecycle state |
| `capability` | 1 hour | Declared capacity and derates |
| `reserve` | 5 minutes | Declared reserve claims |
| `fault` | 10 minutes | Declared failures and constraints |

The bounds are configuration. Two further rules decide currency and are also
configuration: evidence from a superseded **epoch** is not current, and a
declaration stated against a superseded structure **generation** is not current.

### The freshness lattice

`fresh` < `stale` < `recovered` < `conflicting` < `unknown`, worst-wins. A
value derived from several inputs is as fresh as its worst current input. When at
least one source is current the answer is described by those sources — a restored
observation of the same quantity does not make a fresh reading look uncertain —
and when nothing is current the worst of what was considered is reported.

### Generations, epochs and revisions

- **Generation** identifies one structural generation adopted from the topology
  authority. Adopting a new one makes declarations stated against the previous
  one stale without deleting them.
- **Epoch** identifies one control-plane incarnation. Evidence from a superseded
  epoch is retained for history and is never promoted to current.
- **Revision** is this runtime's own durable counter. It advances exactly once
  per commit, and a commit is one accepted batch of records.
- **Ordinal** is the position of one observation in the committed evidence, so an
  explanation can cite the exact record it used.

Every answer reports the revision, epoch and generation it was computed against,
and every judged value carries the evidence references behind it.

## Build

Requirements: CMake 3.20 or newer, a C++20 compiler (MSVC 19.3x, GCC 11+, or
Clang 14+), and Ninja or a Visual Studio generator.

On Windows, `scripts/build.ps1` locates the MSVC toolset with `vswhere`, sets the
environment for its own process only, and configures and builds:

```powershell
pwsh -File scripts/build.ps1 -BuildType Release     # build/release
pwsh -File scripts/build.ps1 -BuildType Debug       # build/debug, with /RTC1 and checked iterators
pwsh -File scripts/build.ps1 -BuildDirectory build/asan -Asan
pwsh -File scripts/build.ps1 -BuildType Release -Analyze
```

With the toolchain already in the environment, plain CMake works:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Options: `COOLING_OBSERVATORY_BUILD_TESTS`, `..._BUILD_TOOLS`,
`..._BUILD_EXAMPLES`, `..._BUILD_BENCHMARKS`,
`..._WARNINGS_AS_ERRORS` (default `ON`), `..._ENABLE_ASAN`,
`..._ENABLE_ANALYZE`, `..._BUILD_DOWNSTREAM_TESTS`.

First-party targets are built with `/W4 /WX` on MSVC and
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` elsewhere.
Both configurations build with zero first-party warnings.

## Install and downstream use

```sh
cmake --install build/release --prefix /some/clean/prefix
```

The package exports one target and is self-contained: it depends on no other
package and exports no target from an adjacent authority.

```cmake
find_package(cooling_observatory 1.0 REQUIRED)
target_link_libraries(app PRIVATE dccp::cooling_observatory)
```

`tests/downstream/` is an independent out-of-tree consumer that does exactly this
against an installed prefix, and `tests/downstream/run_downstream_proof.cmake`
configures, builds and runs it from scratch. See
[docs/validation.md](docs/validation.md) for the recorded result.

## API example

```cpp
#include "dccp/cooling_observatory/engine.hpp"

using namespace dccp::cooling_observatory;

SystemClock clock;
Engine engine;
EngineOptions options;
options.state_directory = "state";           // omit for an in-memory engine
if (const Status opened = engine.open(options, clock); !opened.ok()) {
  return 1;                                   // opened.reason() names the problem
}

const EpochId epoch = engine.image().value().epoch;

std::vector<IngestRecord> records;
records.push_back(make_structure(GenerationId(1), plant_model, "as_built_rev_7"));
records.push_back(make_zone_measurement(RecordSeq(1), epoch, zone, zone_flow(zone),
                                        "sensor.a.flow", Quantity::flow(30'000'000),
                                        clock.now_ms()));

if (const auto ingested = engine.ingest(records); !ingested) {
  return 1;                                   // nothing was committed
}

ObserveRequest request;
request.kind = QueryKind::Delivery;
if (const auto report = engine.observe(request); report) {
  for (const DeliveryObservation& point : report.value().delivery.points) {
    // point.flow.value is absent when the sensor is silent, and
    // point.flow.freshness says why: stale, conflicting, recovered or unknown.
    // point.flow.evidence names the exact records behind the answer.
  }
}
```

A value that is judged rather than simply present is a `Judged<Maybe<Quantity>>`:
the `Maybe` distinguishes "no value" from "a value of zero", and the freshness
says why. Reading a `Result` without checking it is refused at run time rather
than yielding a default that would enter a physical calculation.

### The interchange format

One statement per line, a key and a value separated by whitespace, `#` comments,
blank lines ignored. Records begin with `record <kind>`. The parser is total:
every malformed line produces a reason and a line number.

```
record adopt_structure
generation 7
witness as_built_rev_7
facility dc1
plant plant.a dc1 plant_a
loop loop.primary plant.a secondary
component pump pump.p1 loop.primary primary_pump
zone zone.a dc1 hall_a 180kW
link loop:loop.primary pump:pump.p1 supply

record add_observation
observation measurement
subject measurement:zone.a.flow
sensor sensor.a.flow
value 30000ml/s
observed_at 2027-01-15T08:00:00.000Z
seq 1
epoch 1800000000000

record add_observation
observation capability
subject pump:pump.p1
capacity 40l/s
derate 10%
seq 2
epoch 1800000000000
```

Accepted units include `ul/s`, `ml/s`, `l/s`, `l/min`, `l/h`, `m3/h`, `cfm`,
`Pa`, `kPa`, `psi`, `bar`, `mK`, `degC`, `degF`, `W`, `kW`, `J`, `kJ`, `MJ`,
`ml`, `l`, `m3`, `uHz`, `mHz`, `Hz`, `kHz` and percentages. Values are stored as
exact fixed-point integers at the dimension's base scale; floating point is not
used anywhere in this library.

### Command line

```
ccoctl image         [--state DIR] [--json]
ccoctl delivery      [--state DIR] [--zone Z] [--loop L] [--point P] [--json]
ccoctl constraints   [--state DIR] [--include-stale] [--json]
ccoctl failures      [--state DIR] [--include-stale] [--json]
ccoctl reserve       [--state DIR] [--require-evidenced] [--json]
ccoctl divergence    [--state DIR] [--tolerance PPM] [--json]
ccoctl coverage      [--state DIR] [--include-covered] [--json]
ccoctl dependencies  --root KIND:ID [--direction upstream|downstream] [--json]
ccoctl history       [--state DIR] [--json]
ccoctl ingest        --input FILE [--state DIR]
ccoctl observe       --input FILE [--state DIR]
ccoctl stats         [--state DIR]
ccoctl version
```

Exit codes: `0` answered and determined, `1` operation failed, `2` command line
not understood, `3` answered with part of the answer undetermined. Text and JSON
rendering are byte-stable for a given answer.

## Persistence, recovery and concurrency

**Format.** One generation per file, replaced atomically. A fixed 64-byte header
carries a magic value, the schema version, the payload length, a CRC-32C of the
payload, and the revision, sequence, epoch, generation, instant and evidence count
that the payload must agree with. The header's own summary is checked against the
payload, so a file whose frame and contents disagree is refused rather than
half-believed.

**Commit point.** Writing a temporary file in the same directory, flushing it to
stable storage, then replacing the live name. Before the replace the live name
refers to the previous state in full; after it, to the new state in full. There is
no interval in which it refers to a mixture, and a process that dies mid-commit
leaves either the old generation or the new one.

**Recovery is conservative.** A torn tail or an interior corruption is refused
with a reason, and the store refuses to open rather than starting from empty state
— a corrupt facility must not look like a facility with no equipment. Nothing is
deleted, so an operator can inspect what was refused. A state that does load is
marked recovered: its evidence is readable, is never current, and stops being
recovered the moment new evidence for those sensors arrives.

**Torn tails are impossible to promote.** The payload length and CRC decide what a
file is; there is no partial application path.

**Single writer.** The store takes a kernel-enforced exclusive lock on a lock file
in the state directory. A second process is refused with `store_busy`, and the
lock is released by the kernel when the holder exits, including when it is killed.

**Idempotency.** Each observation carries a producer record sequence. A sequence
already applied — before this batch or earlier in the same batch — is reported as
a duplicate and applied once. The applied keys are part of the same durable commit
as the mutation, so a retry across a restart is still recognised.

**Concurrency.** One engine owns exactly one mutex and there is no second lock to
order against it, so there is no lock-order question to get wrong. It is
recursive because a public entry point may call another. No callback, event or
observer runs under the lock: analysis takes a const context and returns by value.
Shutdown and close hold the same lock, so a query either completes against a
committed revision or is refused with `store_closed`. See
[docs/concurrency.md](docs/concurrency.md) for the audit.

## Honest limits

- **Everything about the plant is SYNTHETIC** unless a deployment supplies real
  telemetry. The tests, examples and benchmarks generate their facilities and
  measurements. No BMS, DCIM, electrical, multi-node, hardware or real cooling
  data was involved in any claim in this repository.
- The observatory does not model coolant thermodynamics beyond a volumetric heat
  capacity, does not compute pressure drop from geometry, and does not simulate
  flow networks. Heat removal is "measured flow x measured temperature difference
  x a configured volumetric heat capacity", and nothing more is claimed.
- Redundancy, derates and capacity are consumed from the accounting authority.
  Where this runtime needs an expectation of its own — divergence against a
  declared capability — it uses the smallest current declaration on the delivery
  path, and says so. It never sums capabilities into a capacity figure.
- Coverage is per subject and per axis. It reports what has no current evidence;
  it does not claim that a covered subject is healthy.
- Failure **state** is consumed, never declared. The observatory reports whether
  measured delivery confirms, contradicts or cannot speak to a declared failure.
- The lock is advisory by nature on POSIX (`flock`) and kernel-enforced on
  Windows. A process that ignores the lock can still write the files.
- Long paths are supported through the extended-length prefix on Windows for
  absolute paths; a very long *relative* path is not extended and remains subject
  to the classic limit.

## Validation

Measured on the development machine, Windows 11, MSVC 19.44, CMake 4.3, Ninja
1.13, Release and Debug. Full detail, including the exact commands, is in
[docs/validation.md](docs/validation.md).

| Suite | What it proves |
| --- | --- |
| `unit_foundations` | Checked arithmetic at every boundary, CRC-32C and SHA-256 test vectors, exact quantity round-trips and unit conversion, identity and timestamp grammar, the limit set, the interchange parser |
| `unit_evidence` | The freshness lattice, per-axis age bounds, epoch and generation rules, group reduction, conflict rather than averaging, recovered never current |
| `unit_plant` | Canonical ordering, digest stability and unambiguity, validation, adjacency, traversal with budget, cycles, dangling edges and loads |
| `integration_engine` | Lifecycle, the commit point, idempotency, supersession, retirement, policy changes, refusals that do not advance the revision, determinism |
| `reference_arithmetic` | Heat removal, derates, reserve headroom, traversal and freshness checked against independently written references |
| `adversarial_evidence` | Silence, staleness, cross-generation declarations, contradictions, residual delivery, unattributable constraints, numeric extremes, malformed input |
| `adversarial_persistence` | Round-trip, single-bit corruption, torn tail, interior corruption under a repaired checksum, schema and flag refusal, size limits, lock exclusivity |
| `concurrency_threads` | Concurrent readers, serialized writers, readers during a commit, open/close churn, visibility of uncommitted batches, close during a query |
| `restart_recovery` | Reopen fidelity, recovered-is-not-current, refresh after restart, idempotency across restart, child-process reopen, a killed writer |
| `process_isolation` | Kernel-enforced single writer across real processes, sequential lock hand-off, a refused contender, the CLI as an external program, long paths |
| `property_determinism` | Generated facilities and evidence: identical answers under different arrival orders and batch boundaries, repeatability, digest stability |

## Documented defects found and fixed

These are recorded because they are the reason the validation above is worth
anything; each was found by a test or a probe disagreeing with the code.

1. **128-bit negation destroyed its own magnitude.** A canonicalisation step in
   the wide-integer helper forced the high word to all ones, so every magnitude it
   returned looked like a huge negative number. Representable results — including
   `min * 1` — were reported as overflow. Fixed by removing the
   canonicalisation.
2. **Signed multiplication was checked on unsigned bit patterns.** `min * 1` has
   its top bit set, so a sign-extension test read a representable product as an
   overflow. Fixed by checking magnitudes and applying the sign afterwards.
3. **The product's sign was taken from the top bit of the unsigned product.**
   `min * 2` has a clear top bit and is still negative. Fixed by deriving the sign
   from the operands.
4. **Heat removal used the wrong unit exponent.** The stored scales multiply to
   1e-15, not 1e-9, so every figure was a million times too large. Fixed, and the
   reference implementation now derives the same exponent independently.
5. **Derived figures truncated instead of rounding.** `3.6 m3/h` parsed as one
   microlitre per second short of a litre per second. Fixed by publishing a
   rounding multiply-divide and using it for unit conversion, heat removal and
   derates.
6. **The durable header was 70 bytes where the decoder read 64.** The encoder
   wrote a reserved field the decoder never read, so no state could ever be
   committed. Fixed; the fields now sum to the header size exactly, with a check.
7. **The freshness accumulator started at the wrong end of the lattice.** Because
   `unknown` outranks `fresh`, joining it with anything yielded `unknown`, so
   stale-only groups reported as unknown and conflicting groups reported as
   unknown. Fixed by treating `unknown` as the identity of the join, and by
   reporting an answer's freshness from its current contributors when it has any.
8. **A capability of zero was reported for a scope whose only declaration was
   stale.** Silence presented as zero, in the one place this runtime is most
   careful. Fixed: no figure is reported and the reason is named.
9. **A declared measurement role lost to the fallback role.** A stale or
   conflicting liquid flow was reported as the airflow's silence. Fixed by letting
   the declared role decide which sensor speaks for a point.
10. **Distance measurement identity was built with a doubled prefix**
    (`zone.zone.a.flow`), which nothing else in the ecosystem writes. Fixed to
    `zone.a.flow`.
11. **A retired sensor made its delivery point vanish** instead of reporting an
    unknown reading. Fixed: withdrawal of standing is not absence of an
    instrument.
12. **Coverage judged each observation separately**, so two individually fresh but
    contradictory sensors were reported as a fresh reading. Fixed by judging each
    group with the same reduction the analyses use.
13. **A constraint naming an element the structure does not contain** was
    attached to whatever happened to sit upstream. Fixed: it is reported as
    unattributed and the missing element is named.
14. **Duplicates within one batch were applied more than once.** Idempotency only
    consulted the committed state. Fixed by checking the candidate state, which
    covers both earlier batches and earlier records in the same batch.
15. **Extended-length paths used forward slashes and preserved doubled
    separators**, both of which Win32 refuses once normalisation is disabled, and
    the recursion tried to create the extended form of a drive root. Fixed; a
    path beyond the classic limit now works from end to end.
16. **A failed heat-removal computation kept the freshness of its inputs**, so a
    figure that could not be computed was described as fresh. Fixed.

## Repository layout

```
include/dccp/cooling_observatory/   public headers, one per layer
src/                                implementation, one translation unit per area
tools/ccoctl/                       the inspection tool
tests/                              eleven suites, a reference model, a multiprocess harness
tests/downstream/                   out-of-tree find_package consumer and its proof script
examples/                           five runnable programs
benchmarks/                         completed-work benchmark
docs/                               architecture, semantics, persistence, concurrency,
                                    protocol, limits, compatibility, validation
scripts/build.ps1                   MSVC build driver
```

## Further reading

- [docs/architecture.md](docs/architecture.md) — layers, data flow, ownership
- [docs/semantics.md](docs/semantics.md) — authority, freshness, belief, refusal
- [docs/persistence.md](docs/persistence.md) — format, commit point, recovery
- [docs/concurrency.md](docs/concurrency.md) — the locking audit
- [docs/protocol.md](docs/protocol.md) — the interchange format and the answer grammar
- [docs/limits.md](docs/limits.md) — bounded resources and what is not modelled
- [docs/compatibility.md](docs/compatibility.md) — composition with adjacent DCCP authorities
- [docs/validation.md](docs/validation.md) — commands and results

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
