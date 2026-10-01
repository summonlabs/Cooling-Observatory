# Persistence and recovery

## What is durable

The engine's state directory holds two files:

| File | Purpose |
| --- | --- |
| `cooling-observatory.state` | One generation of committed state |
| `cooling-observatory.lock` | The writer lock, held for as long as a store is open |

The state file is the durable record. Nothing else is authoritative, and nothing
is appended: each commit writes a whole new generation and replaces the previous
one atomically. A reader therefore never has to reconcile a log with a snapshot,
and a process that dies mid-commit leaves a state file that is either entirely the
old generation or entirely the new one.

## Format

A fixed 64-byte header followed by a payload:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Magic `COBS` |
| 4 | 2 | Schema version |
| 6 | 2 | Header flags, written as zero and refused if set |
| 8 | 8 | Payload length in bytes |
| 16 | 4 | CRC-32C of the payload |
| 20 | 8 | Revision |
| 28 | 8 | Highest producer record sequence |
| 36 | 8 | Epoch |
| 44 | 8 | Adopted generation |
| 52 | 8 | Commit instant, milliseconds since the Unix epoch |
| 60 | 4 | Evidence count |

The fields sum to exactly 64 bytes, which the encoder asserts. The header repeats
the revision, sequence, epoch, generation, instant and evidence count, and the
decoder requires them to agree with the payload: a file whose frame and contents
disagree is refused rather than half-believed, because the two were written from
different states.

The payload is a sequence of little-endian primitives with every variable-length
field length-prefixed, so no field can be reinterpreted as the start of the next.
The decoder requires the payload to be consumed exactly: trailing bytes are a
corruption, not a hint.

The digest (`image_digest`) covers the revision, sequence, epoch, generation, the
structural digest, every retained observation's identifying fields and the applied
record keys. It does not cover transient fields, so two engines holding the same
evidence at the same revision digest identically whatever order they were fed in.

## Schema policy

The schema version is checked exactly. A file written by a different schema is
refused with `incompatible_schema` and a message naming both versions; it is never
partially decoded. Header flags are written as zero and any set flag is refused,
so a future field cannot be silently ignored by an older build.

## Commit point

A commit is:

1. encode the candidate state into bytes, refusing anything above the configured
   size limit;
2. write those bytes to `cooling-observatory.state.tmp` in the same directory;
3. flush the file to stable storage (unless the caller disabled syncing);
4. replace the live name with the temporary one in a single directory operation
   (`MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`, or `rename`);
5. only then update the in-memory state and the manifest.

**The commit point is step 4.** Before it, the live name refers to the previous
generation in full. After it, it refers to the new generation in full. There is no
interval in which an observer can see a mixture, and a process killed at any point
leaves a name that refers to a complete generation.

A failure at any step returns an error and leaves the in-memory state unchanged, so
a caller that retries a batch retries it against the same starting revision.

## Recovery

Three failure shapes are distinguished, and none of them is repaired by guessing:

| What is found | Outcome |
| --- | --- |
| No state file | A store that has never committed; the engine starts empty and this is not an error |
| A file that fails magic, length, CRC, schema, flags or header/payload agreement | `corrupt_snapshot`, and the store **refuses to open** |
| A residue of a temporary file from an interrupted commit | Ignored; the live name is what counts, and it refers to a complete generation |

Refusing to open is deliberate. Starting from empty state would turn a corrupt
facility into a facility with no equipment, and every later answer would be
confidently wrong. Nothing is deleted, so an operator can inspect or salvage the
file; restoring it restores service, which the adversarial suite proves by
truncating, corrupting and then restoring a real state file.

Recovery is conservative in one more way: a state that *does* load has all of its
evidence marked `recovered`. Recovered evidence is readable, appears in history,
and is never current. The moment new evidence for those sensors arrives, the
recovered copies are superseded and the answer becomes fresh again. The engine
reports `recovered` on its image and `refreshed_after_recovery` on the first
commit after a restart so a consumer can see the transition.

## Single writer

The store opens the lock file with a share mode of zero on Windows, or `flock`
with `LOCK_EX | LOCK_NB` on POSIX. Both are kernel-enforced for the lifetime of
the handle. A second process is refused with `store_busy` and a message saying
another process holds the lock; the lock is released when the holder exits,
including when it is killed, which the multiprocess suite proves with a child that
is terminated and a parent that then opens the same directory successfully.

## Idempotency

Each observation carries a producer record sequence. The set of applied sequences
is part of the state and therefore part of the same durable commit as the
mutation, which is what makes a retry across a crash safe:

- a record whose sequence was applied before this batch is a duplicate;
- a record whose sequence was applied earlier in the same batch is a duplicate;
- a duplicate is reported in the ingest report and applied once.

Applying records and recording that they were applied are one commit; there is no
window in which a mutation is durable but its idempotency identity is not.

## Bounded behaviour

The state's encoded size is checked against `max_snapshot_bytes` before anything
is written, so a facility that outgrows the configured ceiling is a reported
refusal rather than a partially written file. The decoder enforces the same limit
on read, and rejects implausible collection counts before allocating. Evidence for
a subject from one sensor of one kind is superseded rather than accumulated, so the
retained set is bounded by the plant and its instruments rather than by how long
the process has been running.
