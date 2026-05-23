# RELAY_HINTS Lifecycle & Bounded Gossip — Design

**Date:** 2026-05-23
**Author:** Dinero Labs (with Claude Opus 4.7)
**Status:** Spec (pending implementation plan)
**Branch this targets:** `dinero-main` (post PR #127 merge)
**Related:** [project_relay_broadcast_gap.md], [project_decentralization_vision.md]

---

## Background

The RELAY_HINTS subsystem lets home nodes discover each other through fleet relays for NAT-traversal P2P. Today's implementation has three structural gaps that emerged during the 2026-05-23 transient-peer investigation:

1. **No TTL on receiver cache.** `relay_hints_by_target_` retains entries forever. Stale node IDs (e.g., the phantom `fd4fc04d…` observed in production) propagate to every new peer and are dialed indefinitely. Header comment at `p2p_manager.h:818` explicitly flags: *"No explicit TTL today — slice 4b+ will add one."*

2. **Hints sent only at handshake.** `SendRelayHintsIfApplicable()` fires once per peer-handshake. If a hint is dropped (today: only by overwrite when a duplicate arrives — which doesn't happen if the source is gone) it never heals.

3. **No fleet-relay directory expiry.** A node that registers with a fleet relay then disconnects (crash, NAT timeout, network flap) stays in the relay's directory forever, propagating as a phantom to every future peer.

Combined effect: Mac+Dell behind same NAT, both with the v8 binary, cannot reliably find each other through fleet relays. Both end up dialing a phantom node ID that no one is listening on. Even when a circuit does briefly establish, the connection collapses within minutes (separate bug 3c, ngtcp2 ERR_IDLE_CLOSE — out of scope for this spec).

## Goal

Make RELAY_HINTS a reliable, decentralizable NAT-traversal discovery layer:

- Phantoms self-evict from receiver caches within minutes.
- Lost hints heal automatically within one refresh period.
- Fleet relays drop dead registrants quickly.
- Home nodes can propagate discovery information without making the fleet a permanent gatekeeper — while keeping stale-hint flooding bounded by cryptographic and rate limits.

## Non-goals

- Replace fleet relays entirely with peer-to-peer relay discovery. (Vision direction, not this spec.)
- Solve sub-bug 3c (ngtcp2 ERR_IDLE_CLOSE on inbound QuicSessions). Tracked separately.
- Change the relay circuit data plane (RELAY_DATA, RELAY_CONNECT). Only the discovery layer.

## Architecture overview

Three coordinated mechanisms, staged into three implementation phases:

```
   ┌──────────────────────────────────────────────────────────┐
   │                  Phase 1a — Lifecycle                    │
   │  ┌────────────────────┐    ┌────────────────────────┐    │
   │  │ Receiver cache TTL │    │ Sender periodic re-send│    │
   │  │ + failure counter  │    │      (5 min cadence)   │    │
   │  └────────────────────┘    └────────────────────────┘    │
   │  ┌────────────────────┐    ┌────────────────────────┐    │
   │  │ Fleet directory    │    │ Registrant reconnect   │    │
   │  │ grace (90s)        │    │ re-arms registration   │    │
   │  └────────────────────┘    └────────────────────────┘    │
   │                                                          │
   │  Wire-format unchanged. Self-advertise + relay-push.     │
   │  NO third-party gossip yet.                              │
   └──────────────────────────────────────────────────────────┘
                              │
                              ▼
   ┌──────────────────────────────────────────────────────────┐
   │                  Phase 1b — Signing                      │
   │  RELAY_HINTS v2: add expires_at + ed25519 signature      │
   │  (signed by target node's existing identity key).        │
   │  v1 still accepted, marked low-trust, never forwarded.   │
   └──────────────────────────────────────────────────────────┘
                              │
                              ▼
   ┌──────────────────────────────────────────────────────────┐
   │                  Phase 1c — Bounded Gossip               │
   │  Forward signed v2 hints to other peers under strict     │
   │  rules: dedup, caps, expires-honored, endpoints-frozen.  │
   │  Source-tagged: Self > RelayPush > Gossip preference.    │
   └──────────────────────────────────────────────────────────┘
```

Each phase ships as a separate PR. Phase 1a alone fixes the user-visible symptom (Mac+Dell find each other within 5min; phantoms evict in ≤15min). Phases 1b+1c deliver the decentralization payoff and ride on top of 1a.

---

## Phase 1a — Lifecycle (no wire changes)

### Receiver cache (`relay_hints_by_target_`)

Add the following fields to `RelayHintRecord` (declared in `p2p_manager.h` near line 819):

```cpp
struct RelayHintRecord {
    // existing
    std::array<uint8_t, 20> target_node_id{};
    dinero::p2p::NetworkType net{NetworkType::IPV4};
    std::vector<uint8_t> relay_addr;
    uint16_t relay_port{0};
    std::chrono::steady_clock::time_point learned_at;
    // new in Phase 1a
    int consecutive_dial_failures{0};
};
```

Eviction triggers:
- `now - learned_at > kHintTtl` where `kHintTtl = std::chrono::minutes(15)`
- `consecutive_dial_failures >= kHintMaxFailures` where `kHintMaxFailures = 3`

Refresh: on every incoming RELAY_HINTS that names the same `(target_node_id, relay_addr, relay_port)` triple, set `learned_at = now()` and `consecutive_dial_failures = 0`.

Failure counting: when the orchestrator's RELAY_CONNECT callback reports `ok=false` OR when the post-orchestrator QUIC handshake on the resulting circuit times out, increment `consecutive_dial_failures` for the hint that produced this dial.

Eviction execution: a new `SweepRelayHintsCache()` runs from `keepalive_loop` (30s cadence — already exists). Walks `relay_hints_by_target_` once, removes expired entries, removes entries over failure threshold, logs the eviction reason.

### Sender propagation — periodic re-send

`SendRelayHintsIfApplicable()` already exists and fires at handshake. Add a new periodic loop:

```cpp
void P2PManager::PeriodicRelayHintsResend() {
    // Walk connected_peers_, for each NODE_DINERO_V2 peer that is NOT
    // one of our configured relayregister= endpoints, re-send our own
    // RELAY_HINTS (target=self) with current relay-list snapshot.
    // Cadence: every kHintResendPeriod = std::chrono::minutes(5)
}
```

Driver: a new dedicated thread (or merged into `keepalive_loop` if its 30s cadence is acceptable — pick whichever is simpler in review).

### Fleet relay directory expiry

The fleet relay maintains an "I am willing to relay for X" registry. Today: registrations persist forever. Add:

- Track each registrant's connected `PeerInfo*` (most likely already known via the source peer that sent RELAY_REGISTER).
- On the registrant's peer-disconnect event, start a 90s grace timer with the entry retained but flagged `grace_pending=true`.
- If the registrant reconnects + re-sends RELAY_REGISTER inside 90s, clear the flag.
- If 90s elapse without reconnect, evict the registration and (Phase 1c) send a directory-change RELAY_HINTS to connected peers.

Implementation lives wherever RELAY_REGISTER is currently handled (search `HandleRelayRegister` or similar).

### Registrant reconnect

Verify that `SendRelayRegisterIfConfigured()` (or equivalent) is called on every successful (re-)handshake to a configured `relayregister=` endpoint, not just on startup. Add a regression test that simulates disconnect→reconnect and asserts the second RELAY_REGISTER is sent.

### Observability (Phase 1a)

Per-hint log lines tagged by source. Even though Phase 1a only has Self and RelayPush sources (no Gossip yet), the source tag is wired now so 1c plugs in cleanly:

```
[hint] received target=931db4ff... source=self signed=0 endpoints=1 expires_in=Nm
[hint] evicted target=fd4fc04d... reason=expired age=15m1s
[hint] evicted target=fd4fc04d... reason=failures count=3
```

Counters (cheap atomics, exported via getnetworkinfo extension):
- `hints_received_self`
- `hints_received_relay`
- `hints_evicted_expired`
- `hints_evicted_failure`

---

## Phase 1b — Signing (RELAY_HINTS v2 wire format)

### Message format change

Add two fields per hint entry:

| Field | Size | Semantics |
|---|---|---|
| `expires_at` | 4 bytes LE uint32 | Unix-seconds when this hint is no longer valid. Set by target node at sign time. Receivers ignore hints where `expires_at - now > 1 hour` (clamp ceiling). |
| `signature` | 64 bytes | ed25519 over `domain_sep ‖ target_node_id ‖ expires_at_le4 ‖ endpoints_canonical`. Signing key = the target node's existing identity key (reuse — no new keypair). |

Domain separator: `"DIN-RELAY-HINT-V2\x00"` (constant byte string, prevents cross-protocol signature reuse).

Endpoints canonical encoding: same byte layout as in the existing RELAY_HINTS message body (so the signature commits to the exact bytes peers see). Order is the encoding order.

### Wire-compat with v1

- v1 message tag (existing opcode) continues to be accepted. v1 hints get `signer_verified=false`.
- New v2 message tag (different opcode). v2 hints get `signer_verified=verify(signature)`. Failed verification = drop, don't even cache.
- Receiver cache treats v1 hints as low-trust: cached but excluded from third-party gossip (Phase 1c). Same TTL and failure-threshold apply.
- Sender behavior: a v8 node that knows the peer supports v2 (advertised via a new service bit in the version handshake) sends v2 only. To peers without the bit, sends v1 as before.

### Receiver-side changes

`RelayHintRecord` gains:
```cpp
bool signer_verified{false};
std::chrono::system_clock::time_point expires_at_system{};
```

TTL becomes `min(kHintTtl, expires_at_system - now)`. So a target advertising a short expiry overrides our local 15min cap downward; a target advertising a longer expiry is clamped down to 15min.

### Sender-side changes

`SendRelayHintsIfApplicable()` and `PeriodicRelayHintsResend()` both sign the outgoing hint with the target's (== our own) identity key. The dineroid handshake already uses ed25519 (`node_identity_->sign(...)`), so this reuses existing infrastructure.

`expires_at` is set to `now + 15min` at the moment of signing. If the periodic loop re-signs every 5min, peers see a fresh signature with a fresh expires_at within their TTL window — natural alignment.

---

## Phase 1c — Bounded gossip

### Forwarding rules

When a node receives a valid v2 RELAY_HINTS for any target X (not just self), it MAY forward the message to other peers. Forwarding is opt-in and bounded:

1. **Eligibility:** only v2 hints with `signer_verified=true` AND `now < expires_at_system` are eligible.
2. **Forward bytes as-is.** Do NOT re-sign. Do NOT modify `expires_at`. Do NOT modify endpoints. Forwarding mutates nothing.
3. **Per-source dedup:** maintain `forwarded_hints_by_target_` map keyed by `(target_node_id, signer_node_id)`. For each key, forward at most once per `kHintGossipDedupWindow = std::chrono::minutes(5)`.
4. **Per-peer rate cap:** forward at most `kHintGossipPerPeerRate = 8` third-party hints per minute per outgoing peer. Excess is dropped silently.
5. **Source-tag on receive:** if the inner signer is not the immediate sender, mark `source=Gossip` on the cached record.

### Cache caps

To prevent gossip-amplification storms from filling memory:

- `kHintMaxRecordsPerTarget = 4` — drop oldest if a 5th hint arrives for the same target_node_id.
- `kHintMaxThirdPartyRecords = 256` — global cap on `source=Gossip` records. LRU eviction over this.

### Orchestrator preference

When `OrchestrateRelayDials()` picks a hint to use for a given target, source preference order:

1. Most recent `Self` (target advertised it themselves via a direct edge to us)
2. Most recent `RelayPush` (fleet relay pushed it from its directory)
3. Most recent `Gossip` (third-party forwarded)

Within a tier, freshest `learned_at` wins.

### Observability (Phase 1c additions)

Per-source counters added:
- `hints_received_gossip`
- `hints_forward_sent`
- `hints_forward_skipped_dedup`
- `hints_forward_skipped_ratecap`
- `hints_forward_skipped_unsigned`
- `hints_evicted_cap` (when third-party cap forces LRU eviction)

---

## Data flow walkthrough — the phantom case

**Today:** node X registers with fleet relay LA, then crashes. LA never drops X. Mac handshakes with LA, gets X in RELAY_HINTS. Mac caches X forever. Mac dials X repeatedly via LA. Every dial fails (X is dead). The cache entry persists. The orchestrator keeps using slots on it.

**Phase 1a:** Same start, but:
- LA drops X 90s after X's TCP disconnect.
- Even if Mac learned X before that, Mac's cache entry expires in 15min.
- Even before TTL, 3 consecutive failed dials evict X from Mac's cache (≈ 3×30s orchestrator cadence ≈ 90s).
- Phantom propagation stops at the source within 90s; receiver-side cleanup within ≤15min.

**Phase 1b:** X's old hints already in flight cannot be regenerated by anyone but X (signature). So even if a buggy relay tries to advertise X, peers reject it.

**Phase 1c:** When Mac learns Dell exists (via LA's directory push or Dell's self-advertise to MO that Mac learned), Mac can re-forward Dell's hint to peers Mac knows that don't see LA or MO. The reach of hints exceeds the reach of any single fleet relay.

---

## Error handling

| Situation | Behavior |
|---|---|
| RELAY_HINTS message malformed | Drop, log at warning level, do not disconnect peer |
| v2 signature invalid | Drop, increment `hints_dropped_bad_sig` counter, do not disconnect peer (could be legit-but-buggy peer) |
| Receiver cache full (cap hit) | LRU evict oldest gossip-source record. Self and RelayPush are never evicted by cap pressure. |
| Forward dedup table memory growth | Cap dedup table at 1024 entries, LRU evict. Worst case re-forward sooner than 5min — not harmful. |
| Periodic re-send loop exception | Log + continue. Next iteration retries. |
| Fleet relay directory hits cap (out of scope here — relay-side memory mgmt) | Will be defined when fleet relay scaling is independently tackled |
| Clock skew (`expires_at` from future) | Clamp `expires_at_system` to `now + kHintTtl` on receive |

---

## Testing strategy

### Phase 1a (no wire change — unit + integration possible immediately)

Unit tests in `tests/network/relay_hints_test.cpp` (new file):
- TTL eviction: insert hint, advance steady_clock by 16min, run sweep, assert removed.
- Failure-counter eviction: insert hint, simulate 3 RELAY_CONNECT failures, assert removed.
- Refresh on duplicate: insert hint, advance 10min, receive duplicate, advance 10min, assert still present (refreshed).
- Periodic re-send: regtest 3-node topology, assert each node sends RELAY_HINTS every 5min ±1min.
- Directory grace: registrant disconnect → 60s wait → reconnect → assert still in directory; same flow with 120s wait → assert dropped.
- Reconnect re-register: simulate handshake-disconnect-reconnect, assert RELAY_REGISTER fires twice.

### Phase 1b (wire format)

- Signature verification: hand-craft valid + invalid signatures, assert verify true/false.
- v1↔v2 compatibility: peer A (v1-only) ↔ peer B (v2-aware); assert each receives the other's hints; assert B does not forward A's v1 hint.
- expires_at clamping: target advertises `expires_at = now + 7days`; receiver clamps to `now + 15min`.
- Domain separator: signature with wrong domain_sep fails verify.

### Phase 1c (gossip)

- Forward dedup: receive same hint from two peers within 5min; assert forwarded once.
- Per-peer rate cap: try to forward 100 hints to peer P in 1min; assert only 8 went out.
- Source preference: cache has Self+RelayPush+Gossip for same target; orchestrator picks Self.
- Cap enforcement: load 257 third-party hints, assert LRU eviction of oldest.

### Integration test — 3-node regtest

Spin up 3 dinero-regtest nodes (A, B, C). Configure A as a relay, B and C as registrants. Assert:
1. Within 5min, B and C learn about each other through A's directory push.
2. Disconnect B for 2min. Within 90s of disconnect, A drops B from directory.
3. Reconnect B. Within one re-send period (5min), C re-learns B.

### Production canary (per `feedback_canary_soak_discipline`)

After each phase deploys to VA, run a 60+ minute single-node soak before fleet rollout. Watch for:
- `hints_evicted_expired` > 0 (TTL working)
- `hints_received_self` > 0 (re-send working)
- No memory growth in cache size over the soak
- Established relay-virtual circuits live ≥ 5min (would require 3c fix; absence is not a 3b failure)

---

## Migration & rollout

- **Phase 1a:** behavior-only change, no protocol bump. Deploy to fleet first (low risk, contained to cache management). Once stable, ships in rc16 (or later).
- **Phase 1b:** wire format change. Requires version bit in handshake. Deploy to fleet first; fleet falls back to v1 for peers without the bit. Once >90% of mainnet population is on v2-capable binary, switch fleet to v2-only.
- **Phase 1c:** behavior change (gossip enable). Default off via runtime flag for one release cycle (early observability). Enable by default after one rc cycle of soak.

---

## Open questions (to surface during planning)

- Exact `kHintTtl` value: 15min vs 10min vs 20min. Tied to `kHintResendPeriod`. Keep ratio ≥ 3:1 against re-send.
- Whether to use the existing `keepalive_loop` thread or spin a dedicated `hints_loop_` thread. Trade-off: cohesion vs separation.
- Whether v1 hints should be accepted at all post-Phase-1b or only during a deprecation window.
- Whether fleet-relay directory should publish a delta `RELAY_HINTS_REMOVE` on grace-timer expiry, vs relying purely on receivers' TTL.

---

## Related artifacts

- Existing code: `src/daemon/p2p_manager.cpp` (`OrchestrateRelayDials`, `SendRelayHintsIfApplicable`, ingest at ~line 5258, hint table struct at `p2p_manager.h:819+`)
- Memory: `project_relay_broadcast_gap.md` (root-cause + sub-bug 3a/3b/3c breakdown), `project_decentralization_vision.md` (why this matters)
- PR #127 (in flight, separate from this spec): bugs 1+2+3a + diagnostic logging
