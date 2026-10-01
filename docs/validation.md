# Validation record

Everything below was run on the development machine. Each row states what was
executed and what it proves. Nothing here is projected, estimated or intended:
where a claim was not exercised, it says **UNSUPPORTED**.

## Environment

| | |
| --- | --- |
| Operating system | Windows 11 (10.0.26100) |
| Compiler | MSVC 19.44.35207 (Visual Studio 2022 Build Tools) |
| CMake | 4.3.2 |
| Ninja | 1.13.2 |
| Architecture | x64 |
| Configurations | Release (`/O2`, `/W4 /WX`), Debug (`/RTC1`, iterator debugging), AddressSanitizer |
| Static analysis | MSVC `/analyze` with `/WX` |

## Suites

Each suite is one executable. There are no test timeouts anywhere: a hang is a
defect to diagnose, so a deadlock manifests as a suite that never finishes.

| Suite | Tests | Result | Proves |
| --- | ---: | --- | --- |
| `unit_foundations` | 23 | pass | Checked arithmetic at every boundary (`min`, `max`, the one overflowing quotient, the two representable `min` subtractions), CRC-32C and SHA-256 against published vectors, quantity round-trips and unit conversion including rounding edges, identity and timestamp grammar, the limit set, the interchange parser's refusals |
| `unit_evidence` | 21 | pass | The freshness lattice and its join, per-axis age bounds, future timestamps, quality-reported silence, epoch and generation rules, group reduction, conflict instead of averaging, recovered never current, evidence references |
| `unit_plant` | 18 | pass | Canonical ordering and dedup, digest stability and unambiguity, structural validation, adjacency ranges, traversal with node and depth budgets, cycles, dangling edges, loads as leaves |
| `integration_engine` | 25 | pass | Lifecycle, commit point, idempotency, supersession, sensor retirement, policy and thermal-policy changes, refusals that do not advance the revision, query determinism |
| `reference_arithmetic` | 11 | pass | Heat removal, derates, reserve headroom, traversal and freshness checked against independently written decimal references, including the boundary where the exact product leaves the representable range |
| `adversarial_evidence` | 20 | pass | Sensor silence, staleness, cross-generation declarations, duplicate records, contradictory flow and pressure, failed equipment with residual delivery, unattributable constraints, numeric extremes, malformed input |
| `adversarial_persistence` | 15 | pass | Round-trip, single-bit corruption, torn tail, interior corruption under a repaired checksum, schema and flag refusal, size limits, writer exclusivity |
| `concurrency_threads` | 6 | pass | Eight concurrent readers, serialized writers with exact revision accounting, readers during commits, open/close churn, visibility of uncommitted batches, 400 queries racing `close` |
| `restart_recovery` | 10 | pass | Reopen fidelity, recovered-is-not-current, refresh after restart, idempotency across restart, a child process reopening, a killed writer, explicit reopen |
| `process_isolation` | 8 | pass | Kernel-enforced single writer across real processes, lock hand-off, a refused contender, the CLI as an external program, a 530-character state path |
| `property_determinism` | 7 | pass | Generated facilities and evidence: identical answers under different arrival orders and batch boundaries, repeatable digests and rendering |

**Total: 164 tests, 164 passing**, in Release, in Debug, and under
AddressSanitizer.

```
Release          100% tests passed, 0 tests failed out of 11
Debug            100% tests passed, 0 tests failed out of 11
AddressSanitizer 100% tests passed, 0 tests failed out of 11
```

## Warnings, sanitizers and analysis

- Release and Debug build with **zero first-party warnings** under `/W4 /WX`
  (plus `/permissive-`, `/utf-8`, `/Zc:__cplusplus`).
- **AddressSanitizer** (`/fsanitize=address`): the whole suite passes with no
  report of any kind. This is what makes the two memory-safety defects listed in
  the README findings rather than suspicions: both were found by ASan, and both
  were fixed before release.
- **Static analysis** (`/analyze /WX` over all 109 targets): **zero findings**.
  Two real robustness issues and one false positive were found and addressed: a
  stack buffer whose bound the analyser could not discharge, a 64 KiB stack frame
  in the CLI, and an indexed table construction.

## Benchmarks

`benchmarks/observatory_bench` measures completed work: how many records were
applied, how many queries were answered, how many rows came back, how many
durable commits happened and how many bytes were written. It prints both a
per-query and a per-row cost so neither can be mistaken for the other.

```
& build\release\benchmarks\observatory_bench.exe 64 4 20 durable <state-dir>
```

```
cooling_observatory 1.0.0 benchmark
proof: SYNTHETIC (generated facility and telemetry, single process, no hardware)
zones 64 pumps 4 rounds 20 mode durable

phase,work,elapsed_ms,per_query_us,per_row_us
build_structure,202 components,0.185,,
adopt_structure,1 record,2.243,,
ingest,5280 records,114.950,,21.771
delivery,20 queries 1280 rows,15.542,777.100,12.142
divergence,20 queries 1280 rows,86.347,4317.350,67.459
constraints,20 queries 0 rows,19.197,959.830,0.000
failures,20 queries 0 rows,14.223,711.175,0.000
reserve,20 queries 1320 rows,31.139,1556.925,23.590
coverage,20 queries 9460 rows,26.242,1312.110,2.774
encode_snapshot,117206 bytes,0.363,,

completed work: commits 21 records 5281 evidence_retired 0 queries 120
durable commits: 21 bytes 1953934
final revision 21 generation 1
```

**SYNTHETIC.** The facility and every measurement in this benchmark are
generated by the benchmark itself. There is no hardware, no BMS, no DCIM, no
network, no multi-node arrangement and no real telemetry anywhere in it. The
figures describe this library's own cost on generated input, and nothing about a
real cooling plant.

**REAL.** What the benchmark does prove is that the work was completed: 5280
records were applied across 21 commits, 120 queries were answered, 21 durable
commits wrote 1,953,934 bytes, and the final revision is 21. Those counters are
the runtime's own, reported by the runtime, and they are what makes the timings
mean something.

## Persistence and recovery

| Claim | How it is proved |
| --- | --- |
| The commit point is atomic | `restart_recovery` and `adversarial_persistence`: a state file is committed, the engine is dropped, a new engine reads it; and a commit that fails leaves the previous revision in place |
| A torn tail is refused, not repaired | `adversarial_persistence` truncates a real state file at many lengths and requires a refusal with a reason, and requires the store to refuse to open |
| Interior corruption is refused | A byte in the middle is flipped and the checksum repaired so only the header/payload agreement can catch it |
| A corrupt store does not start empty | The engine refuses to open rather than reporting a facility with no equipment, and works again once the file is restored |
| Single writer is kernel-enforced | `process_isolation`: a child process is refused with `store_busy` while the parent holds the store; a killed writer's lock is released by the kernel |
| Idempotency survives a restart | `restart_recovery`: a batch replayed after a reopen is recognised as duplicates, including duplicates inside one batch |
| Recovered evidence is never current | `restart_recovery` and every durable fixture: after a reopen the delivery figures are `recovered`, and become fresh again when new evidence arrives |

## Packaging and downstream use

```powershell
pwsh -File scripts/build.ps1 -BuildType Release -InstallPrefix <clean-prefix>
cmake -DCO_WORK_DIR=<scratch> -DCO_PREFIX=<clean-prefix> -P tests/downstream/run_downstream_proof.cmake
```

Real output:

```
-- downstream proof: configuring against <clean-prefix>
-- downstream proof: configure succeeded
-- downstream proof: build succeeded
-- downstream proof: downstream: package 1.0.0, library 1.0.0, producer dccp-cooling-observatory/1.0.0
downstream: all checks passed
-- downstream proof: passed
```

The consumer is a separate CMake project that:
- configures with a clean `CMAKE_PREFIX_PATH` and `find_package(cooling_observatory 1.0 REQUIRED)`;
- links only `dccp::cooling_observatory`, built with the same warning policy;
- builds its own synthetic facility from public headers, ingests it, and answers
  delivery, coverage and full-image queries;
- checks the derived heat removal (30 L/s across 6 K is 752 W), the adopted
  generation, refusal-free ingestion, deterministic rendering, and that a second
  engine reopens the state and sees three recovered observations;
- compares the version the package reported at configure time with
  `version_string()` at run time.

The installed tree contains headers, the static library, the `ccoctl` tool and the
CMake package files only. The installed `ccoctl` was run against a real state
directory as an external program:

```
$ <prefix>\bin\ccoctl.exe version
dccp-cooling-observatory/1.0.0 schema 1/1
$ <prefix>\bin\ccoctl.exe ingest --state <dir> --input records.txt
records_applied 2
records_rejected 0
duplicates 0
revision 1
generation 1
digest 7b241751afaf1dcd4fbb79c804a7ae98ce9c6ee09a8116b89334f0c224678a63
$ <prefix>\bin\ccoctl.exe delivery --state <dir>
query delivery
revision 1
freshness recovered
...
```

## The examples

Five runnable programs, each of which states the fact it demonstrates:

| Example | Demonstrates |
| --- | --- |
| `delivery_and_reserve` | Delivery, and a declared zone with no evidence reported as unevidenced rather than as zero headroom |
| `silence_and_stale` | A silent sensor as unknown, a stale flow still reported but labelled stale, and the delivery path printed |
| `failures_with_residual_delivery` | A declared failure whose effect is residual because delivery continues, and a failure naming an element the structure does not contain |
| `divergence_and_contradiction` | Under-delivery, an inverted temperature pair reported next to the figure, pressure without flow, and flow while stopped |
| `persistence_and_restart` | A durable commit, a restart, recovered evidence, and a duplicate record applied once |

## Claims and their status

**REAL** — proved by execution on this machine:

- Checked fixed-point arithmetic, unit conversion and the published derivations.
- The durable format, its integrity checks, the atomic commit point, torn-tail and
  interior-corruption refusal, and recovery.
- Kernel-enforced single writer across real processes, and lock release on process
  death.
- Restart, reopen and the recovered-evidence rule.
- Concurrency behaviour of the engine under real threads.
- The CLI as an external program against real state directories.
- Install, export and out-of-tree `find_package` consumption.
- Paths beyond the classic Windows limit for absolute paths.

**SYNTHETIC** — implemented and exercised, but with generated input:

- Every facility, topology, measurement set and failure declaration used in tests,
  examples and benchmarks.
- All benchmark timings, which describe this library on generated telemetry.

**UNSUPPORTED** — not claimed, not tested, and not to be inferred:

- Any behaviour against real cooling hardware, a BMS, a DCIM, a SCADA system or a
  live plant network.
- Multi-node or clustered operation, failover and leader election.
- Electrical, power-chain or generator behaviour.
- Protocol conformance to BACnet, Modbus, SNMP or any other field protocol.
- Any performance, scale or latency claim beyond the generated benchmark above.
- Any capacity, incident, recovery or control conclusion: those belong to the
  adjacent authorities this runtime composes with and does not speak for.
