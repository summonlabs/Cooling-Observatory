# Concurrency

This document is the audit the project asks for: ownership of every lock, every
path that takes one, and every place a lock could be held somewhere it must not
be.

## The design

Exactly one mutex exists in this runtime: `Engine::Impl::mutex`, a
`std::recursive_mutex`. It is private to the engine. The store, the analyses, the
renderers and the interchange parser hold no lock of their own and are not
thread-safe by themselves, which is why they are never called concurrently.

```
public entry point            lock                    work
Engine::open                  impl_->mutex            configure, open the store, load, mark recovered
Engine::close                 impl_->mutex            release the writer lock, drop state
Engine::ingest                impl_->mutex            validate, apply to a candidate, commit durably
Engine::observe               impl_->mutex            run one analysis against the committed state
Engine::image                 impl_->mutex            copy the committed state out
Engine::stats                 impl_->mutex            read counters
Engine::manifest              impl_->mutex            read the store manifest
Engine::reopen                impl_->mutex            re-read the durable state
```

## Why there is no lock-order question

Lock-order inversion needs two locks. There is one. The recursive choice is not a
convenience: `Engine::open` calls into the store's own bookkeeping and
`Engine::close` releases the store, and a non-recursive lock would turn those
internal calls into self-deadlocks. Because every one of those paths is a direct
call inside the same thread, the recursion depth is bounded by the call graph and
is never more than a couple of frames.

## Read-to-write upgrade

A read guard can never be held while a write is attempted, because there is no
read/write distinction to begin with and no path holds a guard across a call that
takes one. Every public entry point takes the lock once and releases it on return;
nothing calls another public entry point while holding a guard except the
recursive cases above.

## Locks and callbacks

Analysis takes a `const AnalysisContext&` — an image pointer, a limit-set pointer
and an instant — and returns its report by value. There is no observer, no
listener, no visitor and no callback anywhere in the library, so it is not
possible for user code to run while the lock is held, and not possible for user
code to re-enter the engine from inside it.

Values are copied out of the lock: `observe` builds an `ObservationReport` and
returns it; `image` returns an `ObservationImage` by value. A consumer reads its
copy without any engine involvement.

## Shutdown and join

The engine starts no threads and owns no workers, so there is no shutdown/join
hazard: `close` releases the writer lock and drops the state, and a concurrent
query either completes against a committed revision or is refused with
`store_closed`. There is no window in which state is released while a worker still
needs it, because there are no workers.

The multiprocess harness spawns real child processes, but the engine is not
involved in that: children are started by the test, not by the runtime.

## Cancellation and stale authority

There is no cancellation mechanism, and therefore no cancellation race. Stale
authority is handled as data rather than as a race: an observation from a
superseded epoch or stated against a superseded generation is classified as stale
by the same code path that classifies age, under the same lock that produced the
answer.

## Concurrent publication

The durable state is published by an atomic replace, so a concurrent reader — a
different process, since the lock prevents a second in-process writer — sees a
whole generation or the whole previous one. Backup software, an operator's
`copy`, or a second process reading the file without the lock cannot observe a
partial write.

## The interleaving that is actually possible

Because writers serialize on the engine lock, the observable behaviours are:

- **Reader during a commit**: the reader takes the lock, either before the commit
  (seeing the previous revision) or after (seeing the new one). Never a mixture.
- **Two writers**: fully serialized. Each batch is applied to the state left by the
  previous one, and the revision advances once per accepted batch.
- **Query during close**: either the query runs and close happens after, or close
  won and the query is refused with `store_closed`.
- **Two engines on one directory**: the second is refused with `store_busy`
  before it can touch anything.

## What is asserted

`concurrency_threads` exercises: eight threads querying one engine
simultaneously; six writers committing forty-eight batches while asserting the
revision advanced exactly once per batch; a reader running while a writer commits
forty batches, asserting every answer describes one whole committed revision;
repeated open/close of one directory; a reader that must never observe a value
before the commit that produced it; and 400 queries racing one `close`, asserting
each one either completed or was refused — never anything else.

`process_isolation` exercises the cross-process half: a child refused while the
parent holds the store, a holder that reports and then releases, and a contender
that is refused while a different process holds the lock.

None of these tests uses a timeout. A hang is a defect to be diagnosed, and the
suite is written so that a deadlock manifests as a suite that does not finish.
