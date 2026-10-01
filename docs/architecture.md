# Architecture

## What the runtime is for

Cooling Observatory is a read model over cooling evidence. Producers push
generation-stamped facts into it; consumers ask it questions; it answers with the
evidence behind each answer attached. It owns no actuator, no topology, no
capacity ledger and no incident state.

## Data flow

```
  producers                                consumers
  ---------                                ---------
  topology generation  ─┐                  ┌─ delivery    what is arriving
  facility telemetry    │                  ├─ constraints where it is limited
  capacity declarations ├─ ingest ──┐      ├─ failures    what is claimed and whether
  failure declarations  │           │      │              measurement confirms it
  reserve claims       ─┘           ▼      ├─ reserve     claimed versus evidenced
                              ┌──────────┐ ├─ divergence  expectation versus measurement
                              │  Engine  │─┤
                              │          │ ├─ coverage    what is not known
                              │ one lock │ ├─ dependencies the path and what hangs off it
                              │ one rev  │ └─ history     what was committed, in order
                              └────┬─────┘
                                   │ commit
                              ┌────▼─────┐
                              │  Store   │ versioned file, CRC-checked,
                              │          │ atomically replaced, single writer
                              └──────────┘
```

## Layers

### Foundations

`error.hpp`, `checked.hpp`, `units.hpp`, `identity.hpp`, `checksum.hpp`,
`clock.hpp`, `limits.hpp`.

Every physical computation runs through checked signed-64-bit arithmetic with
128-bit intermediates where a product can exceed the range. Quantities are exact
fixed-point integers at a per-dimension base scale; floating point appears
nowhere, because a value that is compared, summed and published must be
reproducible bit for bit. Identities are validated once and every later use is a
copy of an already-valid value. CRC-32C and SHA-256 are implemented here rather
than taken from a platform library, so the durable format and the canonical
digest do not vary with the platform.

The engine never reads the ambient clock. Every decision that depends on "now"
goes through the `Clock` interface, so a test, a replay or a downstream authority
can pin time and get a bit-identical answer.

### Semantics

`semantics.hpp`, `evidence.hpp`.

The vocabulary of belief: authority domains, evidence axes, the freshness
lattice, lifecycle states, provenance, and the reduction that turns several
observations of one thing into a single answer. This layer decides what counts as
current; nothing above it re-decides.

### Model

`plant.hpp`, `delivery.hpp`.

The observed structure — facilities, plants, loops, pumps, valves, chillers,
CDUs, CRAH/CRAC units, manifolds, branches, thermal zones — with a compact
per-subject adjacency index, canonical ordering and a length-prefixed canonical
digest. A delivery point names the measurement identities that speak for it.

### Analysis

`dependency.hpp`, `constraint.hpp`, `failure.hpp`, `reserve.hpp`,
`divergence.hpp`, `coverage.hpp`.

Each analysis takes the same read-only context: an image, a limit set and an
instant. None mutates, none locks, and all return by value, which is what makes it
impossible for analysis to run under a lock or to observe a half-applied mutation.

- **Dependency** walks the structure breadth first, bounded, deterministic, and
  explicit about truncation and dangling edges.
- **Constraint attribution** is the localisation step: a constraint is *direct*
  when it names an element the adopted structure contains, *inherited* when one
  upstream element explains it, *ambiguous* when several could, and
  *unattributed* when none can. It never guesses.
- **Failure assessment** compares a declaration with measured delivery and
  reports *confirmed*, *contradicted*, *residual*, *unconfirmed* or
  *indeterminate*. Declaring failure state is not this runtime's authority.
- **Reserve** keeps *claimed* and *evidenced* apart, reports the smaller of the
  two as *binding*, and names every assumption it could not verify.
- **Divergence** compares measurement with the declared expectation, and reports
  contradictions that make the comparison meaningless (flow without pressure,
  pressure without flow, delivery while stopped, an inverted temperature pair,
  removal above capability, physically impossible values).
- **Coverage** reports per subject and per axis what has no current evidence, and
  the two findings that matter most: zones with no fresh delivery evidence and
  zones whose declared load has no measured removal behind it.

### Runtime

`engine.hpp`, `ingest.hpp`, `query.hpp`, `persistence.hpp`, `textproto.hpp`,
`render.hpp`, `stats.hpp`.

The engine owns one state directory, one committed revision, one adopted
generation and one epoch. A batch of records is applied all-or-nothing onto a
candidate copy of the committed state; only when the whole batch is accepted is
the candidate committed, and only when the durable write succeeds does the
in-memory state change. A query runs against the committed state under the same
lock and returns everything it used.

## Determinism

Two properties are enforced by construction rather than by convention:

1. Every collection that reaches an answer is sorted by a total order over
   validated identities, so the order in which evidence arrived cannot change the
   answer, only the revision at which it was computed.
2. Rendering emits sections in a fixed order and spells every value one way, so
   the same answer always produces the same bytes.

The `property_determinism` suite asserts both over generated facilities and
generated evidence, including different batch boundaries and arrival orders.

## Coupling

The library depends on no other package and exports no target from an adjacent
authority. Composition with Cooling Topology, Cooling Capacity Accounting, the
failure authority and facility telemetry happens through data: an authority
reference carries the domain, the producer and the generation, and the library
records it verbatim without interpreting it as permission.
