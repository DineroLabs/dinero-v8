# Orchard history download: several body requests in flight

Status: **design for review**. Nothing in this note is implemented yet.
Scope: `include/p2p/orchard_phone_history.h`, in `OrchardPhoneHistoryDownload`, and its tests.
Reviewers: the swap session (owns the file), then codex-chainstate (trust rule), then the owner.
Target: v8.2.4, if the gates in section 9 pass on the exact release commit.

## 1. Why

On a first run from a snapshot, a wallet cannot be created until the Orchard wallet history has been downloaded and verified. That history runs from activation (150,000) to the tip, about 12,000 blocks.

The download does this one block body at a time. It sends one `getdata`, waits for the reply, verifies the body, appends it, and only then asks for the next one.

Measured on 2026-11-21, on a fresh datadir from the H161,527 snapshot, with commit `8feb792cb` (history starting during catch-up):
- **One peer serves all history**: 451 requests to 92.118.190.62, no timeouts.
- **Catch-up blocks come from all three peers**: 5, 9 and 9 in the same window.
- **Rate**: about 333 history blocks a minute.
- **Round trip**: median 125 ms, 90th percentile 268 ms, one outlier at 8.6 s.
- **Ceiling**: one request at a time cannot exceed about 480 a minute, whichever peer is asked.
- **Where the time goes**: samples of the worker thread (bob-952 at the tip; bob-catchup during catch-up) show 91 to 96% waiting for a reply and 3 to 9% verifying.
- **The old timing** (run2, before `8feb792cb`): the history step took 18.5 min of a roughly 40 min first run.

**Targets to test, not promises:**
- the history step at the tip goes from about 18.5 min to a few minutes;
- the first run goes from about 40 min to roughly 22 to 26 min;
- history makes useful progress during catch-up.

What limits these: peer capacity, lock contention during catch-up, and verification speed.

**What this change does not address:**
- block catch-up speed;
- the Orchard catalog download (71 MB; about 10 min fresh, about 80 min on a far-behind copy);
- snapshot age.

## 2. The current single-request state, and what replaces it

Today, the request state is:
- `flight_`: the peer and session of the one outstanding request;
- `wanted_`: the expected block hash;
- `packet_`, `body_received_`: one received body;
- `awaiting`, `sent`, `peer`: worker-local.

`Enqueue()` accepts a body only when four things hold:
- peer and session match `flight_`;
- the header hash equals `wanted_`;
- no body is queued yet;
- `!body_received_`.

These fields are replaced by one window, owned under `mutex_`:

```
struct Flight {           // one outstanding request
    uint32_t height;      // the height requested
    uint256  hash;        // the selected chain's block at that height when requested
    std::string address;  // peer address
    uint64_t session;     // peer connection session
    Clock::time_point deadline;   // sent + 30 s (the current timeout)
};
std::map<uint32_t,Flight> inflight_;   // keyed by height
struct Received {           // a body that arrived (or was read from the cache), not yet appended
    Bytes bytes;
    std::string address;    // the peer that sent it (empty for a cached body)
    uint64_t session = 0;
    bool cached = false;
};
std::map<uint32_t,Received> received_;   // reorder buffer, keyed by height
size_t received_bytes_ = 0;              // bytes in received_
```

**Window size:**
- `kWindow = 8` requests in flight, defined as a `constexpr`.
- Why 8: with a median round trip of 125 ms and verification around 10 to 20 ms per body, 8 in flight lifts the round-trip ceiling to about 3,800 a minute. That's roughly where verification becomes the limit.
- It's small enough that the reorder buffer stays tiny.
- The live run will try 4, 8 and 16 before the number is fixed.

**Reorder-buffer bound:**
- Count: at most `kWindow` entries. A reply is accepted only for a height in `inflight_`, so the buffer can never exceed the window.
- Bytes: `OrchardBlockCandidate::MaxWireSize()` is `MAX_BLOCK_WEIGHT` = 4,000,000 bytes, so the theoretical ceiling is 32 MB at a window of 8 and 64 MB at 16. A fixed cap `kReceivedBytesCap = 32 MiB` applies to `received_bytes_`, and no new request is sent while it's reached, **except for `next`**, the lowest height not yet appended. The body for `next` is always requested and always held, even when the buffer is full. Without that exception, bodies above `next` could fill the buffer (eight maximum-size bodies at a window of 16), `next` could never be held, and nothing could drain: a permanent stall with no timeout to break it. The memory bound is therefore the cap plus one body (`MaxWireSize()`). Ordinary history bodies are a few hundred bytes (about 480 measured), so the cap never binds in practice. It exists so memory use has a hard bound regardless of window size.

**Wire budget, precisely:**
- Today the worker charges `window_bytes` when it takes the packet, including abandoned requests and invalid bodies.
- With a window, the charge moves into `Enqueue()`, under `mutex_`: once per body that matches a `Flight` (hash, peer, session), whether it's then buffered or later found stale by the worker.
- Unsolicited and wrong-peer bodies are dropped uncharged, as today. So "charged once" has exactly one meaning.
- The soft overshoot of the per-minute budget grows from one body to up to `kWindow` bodies, at most `kWindow × MaxWireSize()`. That's acceptable against a 64 MiB/min soft budget, and the byte cap above bounds memory.

## 3. Accepting a reply (`Enqueue`)

**Where replies arrive.**
- `P2PService::HandleP2PMessage` hands a `block` message to this download before block acceptance, at `p2p_service.cpp` ~2971, after the wallet-funding download. A history body therefore never goes through block acceptance or the activation pass.
- `HandleP2PMessage` runs on the peer's handler thread, one per peer (see the samples on #957), so a history reply from a peer is handled after any catch-up block that peer sent before it.
- Pipelining keeps more requests queued at the serving peer, but doesn't remove that per-peer ordering. The live trial measures how much catch-up traffic still costs.
- `Enqueue` keeps today's rule: it decides under `mutex_` and does no I/O, callbacks, cache access or header lookups under it.

A reply is accepted only if a `Flight` exists with:
- `header.GetHash() == flight.hash`;
- `peer.address == flight.address`;
- `peer.session == flight.session`;
- no body already in `received_` for that height.

It is **never accepted by height alone**.

**On accept:**
- move the bytes into `received_[height]`;
- erase the `Flight`;
- add to `received_bytes_`;
- notify the worker.

**A height the cache already filled:** a cached body enters `received_` with no `Flight`. So a network reply for that height matches no `Flight` and is dropped, and cached and network copies can never both be accepted.

**Otherwise drop it, without charging it twice and without appending it twice.** This covers:
- an unsolicited body;
- a duplicate;
- a late reply for a height that was already re-requested or appended;
- a reply from a peer other than the one asked;
- a reply for a block no longer on the selected chain.

## 4. The worker loop

Each iteration:

1. **Wait** on `cv_` until a body for `next` is in `received_`, a flight deadline passes, or `ActiveWait` (50 ms) elapses. Stop as today.
2. **Target and retarget**, unchanged except as in section 6.
3. **Append in strict height order.** While `received_` holds `next`:
   - decode it, leaving it in `received_`;
   - check its header hash against the selected chain's block at `next` (`GetAncestorHashByHash`, as today);
   - run `AppendForSelectedHistory`;
   - **only once the append has succeeded**: cache-put (for a network body), corpus-append, erase `received_[next]`, subtract its bytes, and `++next`.

   Nothing is skipped. A gap stops appending until it's filled. Bodies above a gap stay buffered.

   **A body that can't be appended yet is not lost.** `AppendForSelectedHistory` returns empty when the target isn't selected at that instant (#952's retry). The body then stays in `received_`, the worker pauses as today, and the next iteration retries the same body. A pause mid-window therefore never turns buffered bodies into re-downloads or a gap.

   **A body that fails decode, the hash check or the proof check:**
   - throws, as today;
   - the cooldown goes to the peer that **sent that body** (`received_[next].address` and `session`), not to some other peer;
   - then `discard()` empties the whole window, the buffer and the verified prefix (`verified_height = 0`). That's today's conservative behaviour, kept unchanged for v8.2.4.

   With up to `kWindow` peers' bodies buffered, one bad peer is now more likely to cost the whole verified prefix. Changing that to "drop the bad body, cool that peer, re-request, keep the prefix" would be a trust-rule change. It's not part of this change, and is for codex-chainstate to rule on separately.
4. **Deadlines** (section 5).
5. **Fill the window** for heights in `[next, next + kWindow)` that aren't in flight and aren't buffered, lowest height first:
   - **Cache first**: if `Cached()` has the body, put it straight into `received_` as a cached entry, with no request. So the cache path still sends nothing.
   - **Pick a peer: stick to the one that answers.** The window goes to one serving peer, chosen as today: the first eligible, connected peer that isn't cooling down (`NODE_NETWORK`, address non-empty and at most 256 bytes, session non-zero). Fail over to the next eligible peer only when the serving peer times out or disconnects (section 5).

     Not round-robin, because:
     - only one peer has ever been seen serving history;
     - it's unproven that every peer keeps pre-base bodies;
     - a `notfound` reply never reaches this download (`HandleP2PMessage` routes it only to `ChainstateService::HandleNotFoundFromPeer`), so a peer that lacks the bodies just stays silent for 30 s.

     Round-robin would send part of every window into that timeout. The gain comes from more requests in flight, not from more peers.
   - **Clamp**: never request a height above `selected->height`.
   - **Budget**: stop filling when the per-minute wire budget is reached.
   - **Send**: `getdata` for one block, in today's wire format. Record the `Flight` with deadline `now + 30 s`. If `send_` fails, treat it as that peer failing (section 5).

**Readiness publication, unchanged:**
- When `next > selected->height`, the existing checks run: `AtSelectedTarget()`, then the final catalog equal to the selected target's catalog, then `target_() == selected`.
- Only then are `ready_` and the checkpoint published.

## 5. A silent, slow or disconnected peer

**Deadline passed** (`now >= deadline`):
- erase that `Flight`;
- put that peer on the existing 1-minute cooldown;
- the height becomes unrequested, and is re-requested in the next fill, to a different peer if one is eligible (the cooldown excludes the silent one);
- a late reply from the silent peer then matches no `Flight` and is dropped (section 3).

**Peer disconnected** (no longer in `peers_()` with the same session): every `Flight` for it is erased, and those heights are re-requested elsewhere. This is today's `fail_peer`, generalised.

**A timeout takes the whole window with it.** With one serving peer, a timeout puts that peer on cooldown, so none of its other outstanding flights will be answered usefully. On the first timeout:
- erase **all** of that peer's flights at once;
- re-request every one of those heights from the next eligible peer, or wait out the cooldown if there is none;
- never let each flight run to its own 30 s. Otherwise one silent moment would cost up to `kWindow` staggered timeouts.

Bodies already buffered from that peer are kept and revalidated by hash. Any late replies match no `Flight` and are dropped.

**Every eligible peer cooling down:** nothing is sent. Flights still outstanding keep their deadlines. That's today's behaviour.

**Bounded:** at most `kWindow` flights exist at any time. Re-requesting reuses the same slots, never adds to them.

**Head-of-line: a known limit for v8.2.4.** The deadline stays **30 s** for every flight. A silent serving peer still costs 30 s before the window fails over, as it does today, while up to `kWindow − 1` bodies sit buffered above `next`. Those bodies are kept: they came from the same peer, and they're revalidated by hash before they're appended.

Two shorter-deadline options were considered and rejected for v8.2.4:
- **A 10 s deadline for the lowest height:** with one serving peer, a single slow reply (an 8.6 s outlier was measured on a loaded Mac) would put the only answering peer on a 1-minute cooldown. That's a 60 s stall instead of 30 s.
- **Requesting `next` from a second peer:** it needs two flights per height.

Both are for after the release, once there is evidence about which peers actually keep pre-base bodies.

## 6. Reorganisation and retarget (#952 handling)

`RetargetSelectedHistory` is unchanged and returns one of three results:

- **`Retained`** (same base; the verified prefix is still on the chain):
  - for every in-flight or buffered height above the verified prefix, **cached entries included**, recompute the selected chain's hash at that height;
  - drop any whose hash no longer matches, whether in flight, buffered from the network, or read from the cache;
  - a dropped cached entry is looked up in the cache again by the new hash, and requested from a peer only if the cache doesn't have it;
  - those heights are re-requested by hash in the next fill;
  - entries that still match are kept.

  This generalises today's single-request `next_on_branch` check.
- **`PrefixChanged`** / `discard()`: clear `inflight_`, `received_` and `received_bytes_`, along with what `discard()` already resets.
- **`Unselected`** / `pause()` (the target isn't selected at this instant, including `AppendForSelectedHistory` returning empty):
  - **keep `inflight_`** (decided at review of bf272e50c);
  - **keep `received_`**;
  - keep the checkpoint, as today.

  Requests in flight are kept for the same reason buffered bodies are. A reply is matched by hash, peer and session, and revalidated before it's appended. A retarget drops what a reorganisation made stale. The 30 s deadline still applies. Clearing them would have handed up to `kWindow` replies per pause to the normal block path, and requested the same heights again.

  On the next iteration, each buffered body is revalidated against the selected chain's hash at its height before it's appended (section 4 step 3), and a body that no longer matches is dropped and re-requested. This is what keeps section 4's promise that a pause never turns buffered bodies, or requests still in flight, into re-downloads.

Because a reply is accepted only by the expected hash for the current selected chain, a reply for a block that a reorganisation replaced can never be appended.

## 7. Invariants that do not change (stated so tests can hold them)

- Nothing is published as ready or as a checkpoint until the replay is at the selected target, with the final catalog equal to that target's catalog.
- The checkpoint is withdrawn only on discard. Pause and pure extensions keep it.
- Bodies are appended strictly in height order, each checked against the selected chain's hash at its height. None is skipped or appended twice.
- The cache path sends no request for a body it holds.
- The wire budget and the 30 s per-request timeout keep their meaning. Only the number of simultaneous requests changes.
- `Ready()`, `ReadyCheckpoint()`, `ReadyCheckpointFor()`, `Observe()` and the constructor signature stay the same. Callers in `p2p_service.cpp` and chainstate are untouched.

## 8. Tests that fail without the new handling

Each test is to be shown failing with the relevant handling removed:

| Test | Shows |
|---|---|
| out-of-order replies | bodies for `next+3`, `next+1`, `next`, `next+2` are appended in order, and the history is ready with the correct catalog |
| a peer that never answers | its heights are re-requested to another peer after the deadline, its late reply is dropped, and the history still completes |
| a duplicate reply | the second copy is dropped, nothing is appended twice, the budget is charged once |
| reorg with several requests in flight, above the verified prefix (`Retained`) | buffered and in-flight bodies for replaced blocks are dropped and re-requested by the new hash, and a reply for a replaced block is never appended |
| reorg below the verified prefix (`PrefixChanged`) | window and buffer are emptied, the download restarts, and no stale body is appended |
| unsolicited or wrong-peer reply | dropped |
| window bound | never more than `kWindow` flights, and the buffer stays within the count bound and the 32 MiB byte cap |
| append retried after a pause | a body whose `AppendForSelectedHistory` returns empty stays buffered, and is appended on the next iteration without being re-downloaded |
| bad body from one peer | the cooldown goes to the peer that sent it, and the window, buffer and prefix are discarded (today's behaviour) |
| silent serving peer | after 30 s the window fails over to the next eligible peer, buffered bodies from the silent peer are kept and revalidated by hash, and its late replies are dropped |
| pause keeps the buffer | after a pause, buffered bodies and requests in flight are kept, a reply arriving during the pause is accepted, and nothing is requested again |
| full buffer never blocks `next` | with the buffer filled by bodies above `next`, the body for `next` is still requested and held, and the history completes |
| window = 1 regression | every existing `OrchardPhoneHistory.*` test also passes with the window set to 1 (identical to today's behaviour) and to 8 |
| cached and network copies | a network reply for a height the cache filled is dropped, and a cached body for a replaced block is dropped and looked up by the new hash |
| budget charged once | matching bodies are charged once in `Enqueue`, and unsolicited ones aren't charged |

**Unchanged and required to pass:**
- all existing `OrchardPhoneHistory.*` tests, including `ReorgAboveVerifiedPrefixKeepsReplayAndReplacesRequest`, `ABlockAfterVerificationKeepsTheCheckpoint…` and `VerifierStartsForATargetTheTipHasAlreadyPassed`;
- the full `test_assumeutxo_replay`.

## 9. Gates, in order

1. Unit and integration tests above, plus the full suite.
2. Review by the swap session and codex-chainstate.
3. **A fresh buyer-wallet test on the exact release commit** (run by the swap session): the coin is confirmed without a restart, and still confirmed after a locked restart.
4. A fresh first-run timing from the H161,527 snapshot, compared with run2 (about 40 min) and with the `8feb792cb`-only run (in progress). This tests the targets in section 1. A faster timing alone is **not** sufficient to ship.
5. CI on the exact commit.

## 10. Open points to settle before coding

- **The serving side's limits: settled by code reading** (the swap session, at this head and at tag v8.2.3).
  - The history request is a plain `getdata` with one `MSG_BLOCK` item.
  - On the server, the per-peer budget (10 s window, 500 block / 600 total, disconnect after 10 violations) applies only to `MSG_UTREEXO_*`.
  - `MSG_BLOCK` goes straight to `BlockRelayManager::HandleGetData`, which has no rate limit or scoring. The only cap is `MAX_GETDATA_ITEMS = 200` per message.
  - `src/daemon/peer_connection.cpp`'s 100 a minute isn't on this path, consistent with the 333 a minute we observed.
  - **Not checked:** generic per-connection message-rate or send-queue limits below that layer. The live 4/8/16 trial must watch for disconnects of the three peers.
- **One `getdata` per height: agreed.** It keeps the wire format, lets each height go to a different peer, and makes the `Flight` ↔ reply mapping exact.
- **Window size: agreed.** Try 4, 8 and 16 live, and take the smallest that reaches the verification-bound rate. A smaller window means less damage from one bad or silent peer (sections 4 and 5).
  - The window is a constructor parameter with a default (`kDefaultWindow`). Tests construct with 1 and with 8.
  - **Field fallback without a rebuild:** an undocumented node option, `orchard_history_window`, read once when the download is constructed and clamped to 1..16, default `kDefaultWindow`.
    - It isn't shown in the app.
    - Support can set it to 1, today's behaviour, if a release upsets some peer or network.
    - If codex-chainstate objects to a new configuration key this late, it's dropped and the default is fixed.
- **What the live trial measures besides time to wallet:** the catch-up rate (blocks per minute) must not drop, since more inbound messages share the per-peer receive path. Record the load average with every run.
