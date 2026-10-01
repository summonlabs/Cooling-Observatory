# Contributing to Cooling Observatory

Cooling Observatory is part of Data Center Control Plane (DCCP) and is maintained
by Summon Software Labs.

## Licensing of contributions

This project is licensed under the Apache License, Version 2.0 (see `LICENSE`).

By submitting a contribution you agree that it is licensed under the terms of that
license, as described in section 5 of the license text. There is **no Contributor
License Agreement** to sign and no copyright assignment is required: you keep the
copyright in your contribution and grant the project the license described in
`LICENSE`.

Please do not add co-author trailers or attribution lines naming tools, assistants
or intermediate processes. Commit authorship is the responsibility of the human
contributor.

## What belongs in this repository

Cooling Observatory owns observation, explanation, historical inspection,
constraint localisation and divergence analysis for cooling delivery.

It does **not** own cooling topology, cooling capacity accounting, incident state,
recovery authorisation or any actuator. A change that has this runtime commanding
equipment, deciding what a plant can deliver, declaring an incident, or promoting
another authority's evidence to current is out of scope by construction, not by
convention.

## Before you start

- Read `README.md` for the boundary and the evidence model.
- Read `docs/semantics.md` before changing anything about freshness, authority or
  belief. Those rules are the contract; where the code and the document disagree,
  the code is the defect.
- Read `docs/concurrency.md` before touching the engine, the store or anything
  that runs under the engine lock.

## Building and testing

```powershell
pwsh -File scripts/build.ps1 -BuildType Release -Test
pwsh -File scripts/build.ps1 -BuildType Debug -Test
pwsh -File scripts/build.ps1 -BuildDirectory build/asan -Asan -Test
```

or, with a toolchain already in the environment:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

First-party code is built with `/W4 /WX` on MSVC and
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` elsewhere.
A change that adds a first-party warning is not ready.

## Code quality expectations

- **No floating point in the library.** Quantities are exact fixed-point integers.
  If a computation needs a wider intermediate, form it exactly and report the
  failure at the step that overflowed, naming the step.
- **No unchecked arithmetic on physical values.** Use the helpers in `checked.hpp`.
- **No silent defaults.** Absence is `Maybe`, failure is `Result`, refusal is
  `Status` with a stable code, a machine-readable reason token and a detail that
  names the input. Never report an absent value as zero, and never report a zero
  as absent.
- **No new lock.** There is one mutex in this runtime, deliberately. If you think
  you need a second, the design is wrong; explain the problem in an issue first.
- **No callback under a lock.** Analysis takes a const context and returns by
  value.
- **Bounded resources.** Every new collection is bounded by a number in `Limits`,
  and a breach is a reported outcome, not a truncation.
- **Deterministic output.** Every collection that reaches an answer is sorted by a
  total order, and rendering has one spelling per value.
- **No test timeouts.** A hang is a defect to diagnose. Do not add a timeout to
  make a suite pass.

## Tests

A change is expected to come with tests that would fail without it. The suites
are one executable per area, so a failure names the area it came from:

| Suite | Add to it when you change |
| --- | --- |
| `unit_foundations` | Arithmetic, units, identities, digests, timestamps, limits, the parser |
| `unit_evidence` | Freshness, reduction, authority, provenance |
| `unit_plant` | Structure, canonicalisation, validation, traversal |
| `integration_engine` | Lifecycle, commits, ingestion, policies, queries |
| `reference_arithmetic` | Any published figure: extend the independent reference too |
| `adversarial_evidence` | A new way for evidence to be wrong |
| `adversarial_persistence` | Format, recovery, locking |
| `concurrency_threads` | Anything that touches the lock |
| `restart_recovery` | Durability and the recovered rule |
| `process_isolation` | Cross-process behaviour and the CLI |
| `property_determinism` | Ordering, batching and determinism |

If your change alters a **published figure**, add it to `reference_arithmetic`
with a reference implementation written from the specification. A reference that
calls the code under test proves nothing.

## Proving persistence and packaging claims

```sh
cmake --install build/release --prefix /tmp/co-prefix
cmake -DCO_WORK_DIR=/tmp/co-downstream -DCO_PREFIX=/tmp/co-prefix \
      -P tests/downstream/run_downstream_proof.cmake
```

If your change touches the installed interface, run this. It configures, builds
and runs an independent consumer against the installed package.

## Honesty about evidence

The repository distinguishes **REAL**, **SYNTHETIC** and **UNSUPPORTED** claims,
and the distinction is part of the deliverable, not a footnote. Persistence,
filesystem behaviour, locking, process death and restart, packaging,
installation and downstream consumption are REAL when claimed, because the tests
exercise them. Facility hardware, BMS/DCIM integration, multi-node behaviour and
production telemetry are SYNTHETIC or UNSUPPORTED, and must be labelled as such
wherever they appear — including in benchmarks.

Do not add a claim to the README that a test does not support.

## Reporting a defect

A useful report says what you did, what you expected, what happened, and includes
the exact command and the full output. If an answer was wrong, include the answer:
the revision, epoch and generation it reports, and the evidence references it
carries, are usually enough to reproduce it.
