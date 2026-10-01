# Semantics

This document is the contract. Where the code and this document disagree, the code
is the defect.

## Authority is not ownership

Evidence is stamped with the domain it came from. `local_observation` is the only
domain this runtime originates evidence in; everything else is consumed. A
reference to another authority's state is recorded verbatim and never interpreted
as permission: seeing a topology generation does not make this runtime the
topology authority, and carrying an incident reference does not make it the
incident-state authority.

Four rules follow, and they are the reason the model looks the way it does:

- **Observation is not ownership.** The runtime describes what it can see.
- **Acknowledgement is not effect.** A control plane reporting a pump as running
  is an observation about a report, not a statement that the pump is turning.
- **Configured state is not observed state.** Declared capability, declared
  reserve and declared failure are held as declarations.
- **Recovered evidence is not fresh evidence.** State restored after a restart
  describes the world before the restart.

## Evidence axes

Freshness is judged per axis, because the axes age at different rates:

| Axis | Default bound | Question |
| --- | --- | --- |
| `structure` | 24 hours | Which entities exist and how they connect |
| `delivery` | 30 seconds | What is measured |
| `condition` | 5 minutes | What lifecycle state is reported |
| `capability` | 1 hour | What capacity is declared |
| `reserve` | 5 minutes | What reserve is claimed |
| `fault` | 10 minutes | What failure or constraint is declared |

A record's kind determines its axis; a record that states a different axis is
refused, because a mismatch means producer and consumer disagree about what the
record means.

## The freshness lattice

```
fresh  <  stale  <  recovered  <  conflicting  <  unknown
```

Worst wins. The rules, in the order they are applied:

1. **Recovered first.** Evidence restored from durable state is `recovered`
   whatever else is true of it. No policy setting can promote it.
2. **Age.** Older than the axis bound — or stamped in the future, which is just as
   unusable — is `stale`. A producer's clock and this host's clock cannot bound
   each other, so neither is trusted over the other.
3. **Quality.** A sensor reporting its own value as bad or unavailable is
   reporting silence: `unknown`. Silence is never zero.
4. **Epoch.** Evidence from a superseded incarnation is `stale` unless the policy
   explicitly accepts other epochs.
5. **Generation.** A declaration stated against a structure this runtime no longer
   holds is `stale`.

### Reduction

Several observations of one subject reduce to a single answer:

- no evidence at all → `unknown`, no value;
- one or more current sources that agree → the value, `fresh`;
- current sources that disagree beyond tolerance → `conflicting`, no value, with
  every disagreeing sensor and value named;
- nothing current → the worst of what was considered (`stale`, `recovered`), no
  value.

When at least one source is current, the answer's freshness is derived from the
current sources only: a restored observation of the same quantity is superseded by
a fresh one and must not make the fresh reading look uncertain.

## Absence is a first-class outcome

`Maybe<T>` distinguishes "no value" from "a value of zero". This is not a
convenience: almost every mistake an observatory can make is reporting an absent
value as zero, or a zero as absent.

Where absence arises and how it is reported:

| Situation | Reported as |
| --- | --- |
| No evidence for a delivery point's roles | The point appears in `unevidenced_points` |
| Instrumented point whose sensors are silent | The point appears with `unknown` fields and the reason |
| No current capability for a scope | No capability figure; reason `capability_not_current` |
| No evidence for a subject on an axis | A coverage gap with reason `no_evidence` |
| A constraint whose element is absent | `unattributed`, with the missing element named |
| A failure whose effect cannot be seen | `indeterminate` |

## Refusal

Refusals are outcomes, not exceptions. A refused operation returns a stable code,
a machine-readable reason token and a human-readable detail, and it changes
nothing: a refused batch does not advance the revision, and a refused open does not
create a state file.

The codes distinguish what a caller must do differently:

- input problems (`invalid_argument`, `malformed_input`, `unknown_token`,
  `limit_exceeded`, `unsupported_value`);
- evidence problems (`missing_evidence`, `stale_evidence`,
  `conflicting_evidence`, `recovered_evidence_not_current`, `unknown_evidence`);
- lifecycle problems (`stale_epoch`, `stale_generation`, `unknown_generation`,
  `revision_regression`, `duplicate_record`, `lifecycle_refused`);
- persistence problems (`not_found`, `corrupt_snapshot`, `incompatible_schema`,
  `store_busy`, `store_closed`, `io_failure`, `lock_failure`).

Codes are part of the public contract: a caller may branch on them, and they are
rendered verbatim. New codes may be added; existing codes never change meaning.

## Explanations

Every answer carries the revision, epoch and generation it was computed against.
Every judged value carries the evidence references behind it, each naming the
durable revision, the producer's record sequence, the epoch, the generation, the
authority and the subject. An explanation that cannot name its inputs is not an
explanation, so the types make it awkward to produce one.
