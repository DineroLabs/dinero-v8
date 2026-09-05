# Crash-safe duplicate-announcement suppression

**Status:** design only. Not implemented. Written after the first attempt broke
crash recovery.

## The problem, measured

Block base+1 is announced repeatedly by peers while it is not yet connected.
Same commit, same test, two machines:

| host | base+1 deliveries | `ConnectTip` successes |
|---|---|---|
| Dell (48 cores) | 2 | 1 |
| GitHub CI runner | **667** | 1 |

Connection is correct on both — exactly once. What repeats is *acceptance*:
parse, validate, store, fsync. From one fast host the issue looked nearly
theoretical; 667 says otherwise.

This is distinct from the livelock that PR #693 fixed. That was a
self-sustaining drain loop producing **83,738** deliveries of one height while
fetching zero pre-base bodies, and it is gone. What remains is ordinary peer
re-announcement of a block that has not yet connected.

## Why the first attempt was wrong

The known-body guard reused a stored flatfile position when block metadata
already carried `BLOCK_HAVE_DATA`, skipping the write. It failed
`CsnEpochResetCrashAtomicity`.

The mechanism matters, because it constrains every future design. That test
deliberately drives a block to **stored but not connected**, then restarts to
exercise stateless recovery. The guard keyed on *durable* state — precisely the
state the test creates — so on the second delivery it suppressed the only write
that phase would have made, and recovery took the ordinary connect path instead
of the stateless replay branch. The crash hook never fired.

**The rule that follows:** suppression must never key on durable state.
"Already stored" does not mean "already handled": a stored-but-unconnected
block legitimately needs reprocessing, and that is exactly what recovery
depends on.

## Design

Suppress on **in-memory, non-durable** state only.

```
BlockAcceptor:
    recently_handled_ : bounded LRU< block_hash -> AcceptResult >
    epoch_            : monotonic counter, bumped on ANY state change that
                        could alter a block's fate
```

Accept path:

1. compute `hash`
2. if `recently_handled_` holds `hash` **for the current `epoch_`** → return the
   cached result immediately, before parse/validate/store
3. otherwise process normally, then record `(hash, result, epoch_)`

Bump `epoch_` — invalidating every cached entry at once — on:

- active tip change (connect or disconnect)
- reorg / `invalidateblock` / `reconsiderblock`
- AssumeUTXO promotion, or exit from deferred mode
- shielded state rollback or realignment

An epoch counter rather than targeted eviction: reasoning about *which* entries
a reorg invalidates is where this class of bug lives, and being wrong there
suppresses a block that needed reprocessing. Invalidate everything; the cache
refills in microseconds.

### Why this is crash-safe

The cache is memory-only. A crash clears it, so recovery always reprocesses
from scratch — the exact property the known-body guard violated by consulting
durable metadata that survived the crash. `CsnEpochResetCrashAtomicity` drives
store-ahead → abort → restart; after the restart the cache is empty, the block
is reprocessed, and the stateless recovery branch runs as before.

### Relationship to single-flight

Already implemented in `AcceptBlockFromPeer`: one in-flight acceptance per
hash, RAII-released, which dedups **concurrent** deliveries. This proposal
covers **sequential** ones. They compose; neither replaces the other.

## What must be proven before it ships

1. `CsnEpochResetCrashAtomicity` passes — the specific test the first attempt broke.
2. `ShieldedNullifierCrashBoundary` passes, including the stamped-cache fixture.
3. Deliveries drop materially on the CI runner (667 is the baseline to beat),
   with `ConnectTip` successes still exactly 1.
4. A reorg immediately after a suppressed duplicate still reprocesses that
   block — the epoch bump is load-bearing and must be mutation-tested by
   removing it.
5. Restart clears the cache: a block suppressed before restart is reprocessed
   after it.
6. Memory is bounded under a sustained announcement flood.

## Open question for review

Whether suppression should apply while `assumeutxo_active && !forward_connect`.
In deferred mode base+1 can *never* connect until promotion, so every delivery
is knowably futile and suppression is most valuable exactly there — but that is
also the regime where the livelock lived, and where a wrong suppression is
hardest to observe.
