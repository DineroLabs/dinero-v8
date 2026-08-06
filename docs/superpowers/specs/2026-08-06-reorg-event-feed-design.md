# Node-side Reorg Event Feed (Sub-project A) — Design

**Status:** proposed
**Scope:** a minimal, read-only record of chain reorganisations inside the daemon, exposed
over RPC. Sub-project B (the fleet watcher) is a separate spec and a separate PR. Neither
blocks the other.

## Goal

Let an external consumer learn that reorganisations happened, how many, and how deep — including
ones too brief to be visible to a poller between cycles.

## Why this is small

The fleet watcher already answers "is the fleet diverging right now" by comparing nodes from
outside. What it cannot see is a reorg that begins and resolves between two polls: from outside,
the chain simply looks fine at both ends. That gap is this feed's entire justification, and it
sets the scope — anything the watcher can already do belongs in the watcher.

This is deliberately sequenced second. The watcher ships against RPCs that exist today, so it
keeps working if this is delayed, and running it produces the evidence for tuning this feed's
ring size and confirming the gap is real rather than assumed.

## Non-goals

- **No persistence.** The record is in-memory and process-lifetime. A restart loses it, and the
  boot identifier is how a consumer knows that happened.
- **No alerting, no thresholds, no notification.** Those live in the watcher.
- **No fork geometry.** No fork-point hash, no old/new tip, no per-block paths. Fact and depth
  answer "did reorgs happen, how often, how deep", which is what alerting needs. Hashes are the
  fields most likely to be wrong under restart-replay edge cases, and per-block paths would make
  an event unbounded in size inside a consensus-critical process.
- **No influence on consensus.** This observes a decision already made and must never change it.
- **Does not revive or depend on the unregistered consensus RPC handlers** (see #526), or on the
  deleted metrics prototype (#528).

## Recording

A single call at the live reorg site in `src/daemon/services/chainstate_service.cpp`, where
`disconnect_path` and `connect_path` have both been built and the existing
`"[ActivateBestChain] REORG DETECTED"` warning is emitted.

**Keyed on `!disconnect_path.empty()` alone.** A connect-only advance is an ordinary new block,
not a reorganisation. The existing log line fires on `disconnect || connect`, which is why it
cannot be reused as the trigger: counting ordinary blocks as reorgs would make every downstream
rate meaningless.

The recorder must:

- **never throw** — a failure to record an observation must not disturb chain activation;
- **never block** — no I/O, no allocation-heavy work, no lock held across anything slow;
- **never affect the outcome** — it runs after the decision, reads only what is already in hand.

If recording cannot be made to satisfy all three, it does not go in. Observability is not worth
a consensus risk.

### Event

| field | meaning |
|---|---|
| `seq` | monotonic within the process, starting at 1 |
| `timestamp` | UTC, when the reorg was activated |
| `disconnected` | blocks removed from the active chain |
| `connected` | blocks added |

## Exposure

One method: **`reorg.status`**, returning the whole ring plus two scalars.

```json
{
  "boot_id": "…",
  "total": 12,
  "events": [
    {"seq": 11, "timestamp": "2026-08-06T04:12:07Z", "disconnected": 1, "connected": 2},
    {"seq": 12, "timestamp": "2026-08-06T04:12:41Z", "disconnected": 3, "connected": 4}
  ]
}
```

- `total` is the process-lifetime count, **not** the ring length. A consumer that sees `total`
  advance by more than the events it can account for knows the ring overflowed and can say so,
  rather than silently under-reporting. This is the only overflow signal, and it is why the
  counter exists separately from the ring.
- `boot_id` is the daemon's restart identity. When it changes, `seq` and `total` have reset and a
  consumer must not treat the discontinuity as data loss.
- The ring holds **64** events. Returning it whole keeps the node stateless and makes a missed
  poll cost nothing; at four fields per event this is a few KB.

Returning the whole ring is deliberate. A since-cursor would be smaller but requires the consumer
to persist a cursor per node, and a wrong cursor skips events silently — the failure mode this
feed exists to prevent.

## Registration

`reorg.status` is registered in **`register_daemon_status_rpc_methods()`**, the same function
that registers `safemode.status`, reached from `RegisterDiagnosticsRPC(ctx)` at
`src/rpc/rpc_init.cpp:32`.

This is a requirement, not a convenience. That path is empirically live — `safemode.status`
answers on production nodes. Introducing a new registration function is precisely how
`getchaintips`, `getchainwork` and `getreorgstatus` became unreachable: the functions exist,
compile, and are called from nowhere (#526).

## Testing

Three gates. **Compilation is not one of them** — every dead subsystem found in this repository
compiled cleanly.

1. **A real reorg produces a real event.** An integration test starts the daemon, causes an
   actual reorganisation using the existing harness in `tests/integration/`
   (`build_reorg_guard.sh` and the reorg soak tests already do this), calls `reorg.status`, and
   asserts the event's `disconnected`/`connected` depths match what was forced.

2. **A restart records nothing.** Restart the daemon and assert `total` is 0 with an empty ring.
   `ChainstateService` exposes no initial-block-download flag, so whether activation replays
   through the reorg path on startup cannot be settled by reading the code — this test settles
   it. If it fails, the recorder needs a replay guard, and that is a finding worth having before
   the feed is trusted.

3. **The method actually responds.** A check that `reorg.status` returns a result rather than
   `-32601`. Three separate subsystems in this repository are written, compiled, and unreachable;
   this is the cheapest possible defence against becoming the fourth.

Additionally: overflow behaviour is unit-tested — after more than 64 recorded reorgs, the ring
holds the newest 64 while `total` continues to climb.

## Interaction with the fleet watcher

The watcher polls `reorg.status` alongside its existing calls and records the result. It dedupes
by `(boot_id, seq)`, and reports a gap when `total` outruns the events it can see.

The watcher's rules are unchanged by this spec. Deciding whether a recorded reorg is worth paging
about — depth thresholds, rates, correlation across nodes — is watcher work, informed by what
this feed actually reports in practice. That decision is deliberately deferred until there is
data rather than made now from assumption.

## Open question, to be answered by data rather than guessed

The ring size of 64 and the absence of any rate limit are starting values. Once the watcher has
been running against a real fleet, the observed reorg rate should confirm or correct them. If
reorgs prove frequent enough to overflow a 64-entry ring between 60-second polls, that is itself
a finding worth acting on.
