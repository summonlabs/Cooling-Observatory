# Interchange format and answer grammar

## Why text

A cooling observation feed is produced by instruments, gateways and scripts that a
human has to be able to read, diff and hand-write. The format is therefore one
statement per line: a key and a value separated by whitespace, `#` comments, blank
lines ignored, no quoting rules, no nesting.

## Records

A record begins with `record <kind>` and continues until the next `record` line.
The kinds are separate verbs because they carry different authority:

| Kind | Meaning |
| --- | --- |
| `adopt_structure` | Replace the held structure and advance the adopted generation |
| `add_observation` | Append one fact |
| `retire_evidence` | Stop treating one sensor's evidence as current, without asserting a new value |
| `parse_policy` | Replace the freshness policy |
| `thermal_policy_update` | Replace the volumetric heat capacities |
| `register_delivery_point` | Declare how a point is instrumented |
| `forget_delivery_point` | Withdraw a delivery-point declaration |

Folding these into one "update" call is how a consumer ends up accidentally
replacing a whole generation, which is why they are distinct.

### Common fields

`seq` (the producer's record sequence, the idempotency key), `epoch`, `generation`
(the structure generation the record is stated against), `received_at`,
`authority` (`domain producer`), `authority_generation`, `authority_digest`.

### Structure

```
record adopt_structure
generation 7
witness as_built_rev_7
facility dc1
plant plant.a dc1 plant_a
loop loop.primary plant.a primary
component pump pump.p1 loop.primary primary_pump
component crah crah.h1 loop.secondary crah_one
zone zone.a dc1 hall_a 180kW 18degC
link loop:loop.primary pump:pump.p1 supply
```

A `zone` line is `id facility [label] [load] [max_supply_temp]`; a field written
as `-` means absent. A `link` is `from to relation` where an endpoint is
`kind:id`. Relations: `supply`, `return`, `air_supply`, `air_return`,
`thermal_coupling`, `electrical_feed`, `control_binding`. Only the first four
carry delivery; a traversal follows those and reports the rest.

### Observations

`root record` lines carry `observation <kind>`:

| `observation` | Additional fields |
| --- | --- |
| `measurement` | `value` (with unit), `quality`, `observed_at` |
| `equipment_state` | `state` (`off`, `starting`, `running`, `degraded`, `stopping`, `faulted`, `maintenance`, `unknown`) |
| `capability` | `capacity` (a quantity; its dimension is the capability's), `derate` (a percentage), `depends_on_redundancy` |
| `reserve_claim` | `reserve` (a quantity), `assumes` (repeatable, an element identity) |
| `constraint` | `constraint_kind`, `direction`, `constraint_state`, `limit`, `origin_element` |
| `failure` | `failure_kind`, `severity`, `impact`, `residual` |

A record that fills the fields of another kind is refused rather than quietly
ignored: filling both means the producer and the consumer disagree about what the
record means.

## Requests

```
query <kind>
point <id>
loop <id>
zone <id>
plant <id>
subject <kind:id>
root <kind:id>
direction upstream|downstream
all_relations true
max_depth 32
include_stale true
include_consistent false
include_covered true
require_evidenced true
minimum_severity major
tolerance 25000
limit 100
```

Kinds: `image`, `delivery`, `constraints`, `failures`, `reserve`, `divergence`,
`coverage`, `dependencies`, `history`. An unknown field is refused with
`unknown_token`, so a typo in a script is a diagnostic rather than a silently
ignored filter.

## Answers

Text rendering is line-oriented and sectioned:

```
query delivery
revision 2
record_sequence 12
epoch 1800000000000
generation 1
as_of 2027-01-15T08:00:00.000Z
freshness fresh
recovered false

-- delivery --
points 1
unevidenced_points 0
total_heat_removal 752W fresh
point dp.zone.a zone=zone.a loop=loop.secondary
  flow 30000000ul/s fresh
  differential_pressure unknown unknown
  temperature_difference 6000mK fresh
  heat_removal 752W fresh
```

Values render canonically: `30000000ul/s`, `-1200Pa`, `12500mK`, `752W`,
`12.5%`. A value that is absent renders as `unknown` and is never rendered as a
zero. JSON output uses a fixed key order, integers for numbers, and the same
spellings, so both surfaces are byte-stable and diffable.

## The identity grammar

An identity is 1 to 96 bytes: an ASCII letter or underscore, then letters, digits,
`_`, `.`, `:`, `-` or `/`. Nothing else is accepted, so an identity can never
contain whitespace, a quote, a control character or a backslash, and can never be
mistaken for a path or a format string.

Measurement identities are conventional rather than configured, so a producer can
construct one this runtime will resolve:

| Role | Identity |
| --- | --- |
| flow | `<zone-id>.flow` |
| supply temperature | `<zone-id>.supply_temp` |
| return temperature | `<zone-id>.return_temp` |
| differential pressure | `<zone-id>.diff_pressure` |
| airflow | `<zone-id>.airflow` |

A delivery point that is declared explicitly names its measurement identities
directly and does not rely on the convention.
