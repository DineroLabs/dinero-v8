# Reachability Plan 3 — Community Relays, Capped and Identity-Bound Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:**
- Any home node whose address is *verified* reachable relays for others automatically,
  within hard caps.
- NAT'd nodes keep two relays on different networks, preferring community relays over
  the fleet.
- Every NAT'd node can be found by any node, not just its direct peers.
- No relay can read, alter or impersonate relayed traffic.

**Architecture:** This reuses the existing relay machinery (`RelayRegistry`, circuits,
RELAY_* messages, the behind-throttle and service policy in `p2p_service.cpp`). The
changes:
1. **Who relays:** verified reachability turns the relay role on (in addition to
   mining).
2. **Default limits** become the owner's caps, with a persisted monthly quota.
3. **Registration** targets 2 diverse relays, confirmed by an explicit ack and kept
   alive with `relay_ping`.
4. **Hints** become signed by the target and are forwarded with dedup and a hop limit.
5. **Identity binding:** dineroid signs the QUIC TLS exporter value, so identity is bound
   to the session, and that binding is mandatory for relayed peers. The relay outbound
   identity bug is fixed.
6. **RELAY_CONNECT timeouts** get root-caused and the bookkeeping bugs found so far
   fixed.

**Tech Stack:** C++20, P2PManager / P2PService, `QuicSession` (ngtcp2 + OpenSSL 3.5.7,
`SSL_export_keying_material`), `NodeIdentity` (secp256k1 ECDSA, node_id = HASH160 of
the pubkey), gtest with `DINERO_TEST_BUILD` hooks (`p2p_manager.h:840-889`), the netns
harness on DineroTX, and fleet canaries.

**Spec:** `docs/superpowers/specs/2026-10-25-zero-touch-p2p-reachability-design.md`
§D (community relays, caps, two diverse relays, hint reach, relay security
"bind each relayed QUIC session to the endpoints' node identity keys") and acceptance
tests 1, 4 and 6.

**This is plan 3 of 7.** It needs:
- Plan 1 (fleet and release builds actually carry QUIC; the live fleet reports
  `ngtcp2_available=false` today);
- Plan 2b (`SelfReachability::AnyVerified`, netgroup).

Task 5 (identity binding) and Task 6 (timeouts) don't depend on 2b and can start now.

## Global Constraints

- **Default relay caps** (owner: "acceptable cap"). These replace today's defaults at
  `p2p_manager.h:428-434`:
  - at most **8** circuits;
  - at most **200 KB/s** sustained relayed traffic globally, with a per-circuit bucket
    of 64 KB/s and 256 KB burst (keep that);
  - at most **20 GB per calendar month**, persisted in `<datadir>/relay_quota`,
    replacing the 50 GB/24 h window at `p2p_manager.cpp:1224`;
  - 2 circuits per peer and a 1800 s circuit lifetime (keep);
  - **pause** (refuse new circuits; existing ones continue until lifetime) while the
    node is in IBD or more than 6 blocks behind its best header. That tightens the
    existing more-than-100-behind rule at `p2p_manager.cpp:1047`.
- **Relay role:** on when `SelfReachability::AnyVerified()` (Plan 2b) or mining is
  active.
  - Operator policy `off` still wins, and `custom` still wins with its own limits
    (`p2p_service.cpp:614-677`).
  - The Settings toggle maps to `network.setrelayservice` off/automatic.
- **Registration:**
  - target **2** relays (`kAutoRelayTargetCount`, `p2p_manager.h:1051`) in distinct
    netgroups (`AddrManNetGroup`, Plan 2b);
  - prefer community relays; fleet (`kMainnetRelayPeers` plus fixed seeds) only to fill;
  - a registration counts only after a **`relayrok` ack** (new command, sent by relays
    that accept);
  - liveness is a `relay_ping` every 30 s to each of our relays (the sender is missing
    today, `create_relay_ping` at `p2p_manager.cpp:832` has no callers);
  - three missed pongs mean the relay is dead, so pick a replacement.
- **Signed hints:** new command `relayhn2`, payload
  `{target_node_id20, target_pubkey33, relay_addr (addrv2), relay_port, expiry_u32, hop_u8, sig}`.
  - `sig` is the target's NodeIdentity signature over
    `"dinero-relay-hint-v2" || target_node_id || relay_addr || relay_port || expiry`.
  - **Receivers verify that the signature is valid and that `HASH160(pubkey) == node_id`
    before storing anything.**
  - They forward to their other V2 peers once per (target, relay, expiry) while
    `hop < 3`, with a token bucket of 50 forwards/min.
  - The relay re-advertises its registrants every 10 min, and the target re-signs every
    10 min with a 30 min expiry.
  - Legacy `relayhnt` keeps working for old peers.
- **Identity binding** for relayed (and direct-QUIC) sessions:
  - `QuicSession` exposes `std::optional<std::array<uint8_t,32>> ExportBinding()`, i.e.
    `SSL_export_keying_material(label="EXPORTER-dinero-p2p-identity", context empty, 32 bytes)`,
    available once the handshake is confirmed.
  - **dineroid v2** signs `nonce_LE8 || binding32`. It's sent when the session has a
    binding, flagged by a version byte appended to the existing dineroid payload.
  - For `via_relay` peers, a missing, invalid or mismatched v2 proof means disconnect
    (no "continuing legacy"). For TCP peers today's behavior is unchanged.
- **Outbound relay identity:** the derived node id must equal the dialed
  `target_node_id`, or disconnect and record the target's hint as bad.
- **No consensus changes.**
- **Owner verification rules:** confirm tests ran, neuter-check them, no
  `assert()`-only gates.
- **Commits as Claude**, with trailer
  `Co-Authored-By: Claude Opus 5 (1M context) <team@dinerolabs.org>`.
- **Branch:** `feat/reachability-community-relays`, on top of Plans 2a and 2b. Fleet and
  DineroTX deploys need owner approval and go one node at a time.

## Review Focus

1. **A malicious community relay terminates both QUIC legs and forwards the dineroid
   messages** (classic MITM). Both endpoints must detect it and disconnect. Pinned in
   Task 5, `RelayIdentityBinding.MitmRelayIsDetectedOnBothEnds`.
2. **A forged hint** claiming "node X is reachable via relay R", signed by someone other
   than X, or by X but replayed after expiry. It must be rejected, not stored, and not
   forwarded. Pinned in Task 4, `SignedHints.ForgedOrExpiredHintIsDroppedAndNotForwarded`.
3. **A hint-flood storm** (cycle in the peer graph, many targets). Forwarding must stay
   bounded by hop limit, dedup and the token bucket, never amplifying. Pinned in Task 4,
   `SignedHints.ForwardingIsBoundedInACycle`.
4. **A home relay whose owner starts syncing, or hits the monthly cap mid-month.** New
   circuits must be refused politely (existing ack code), existing ones allowed to
   finish, and the quota must survive restart. Pinned in Task 2,
   `RelayCaps.MonthlyQuotaPersistsAcrossRestartAndResetsOnNewMonth` and
   `RelayCaps.SyncingRelayRefusesNewCircuits`.
5. **A relay that silently ignores `RELAY_REGISTER`** (role off, old build). The NAT'd
   node must not believe it's registered and must pick another relay. Pinned in Task 3,
   `RelayRegistration.UnackedRegistrationIsReplaced`.

---

### Task 1: Verified reachability turns the relay role on

**Files:**
- Modify: `src/daemon/services/p2p_service.cpp`
  - `IsRelayRoleEnabled` ~614-630: add `|| p2p_mgr_->self_reachability().AnyVerified()`
    after the operator-policy checks
  - re-evaluate `NODE_RELAY` advertisement (~2287-2289) whenever verification changes,
    through a callback from Plan 2b's `SelfReachability`
- Modify: `docs/network-participation.md` (~52-141): the "auto" tier is now "verified
  reachable, or mining"; remove the "won't flip default until abuse protection"
  roadmap line, since Tasks 2-5 are that protection
- Test: `tests/p2p/test_relay_virtual_transport.cpp`, `RelayServicePolicy` group

**Interfaces:**
- Consumes: Plan 2b `SelfReachability::AnyVerified(Transport)` and a change callback.
- Produces: `bool P2PService::IsRelayRoleEnabled() const`, now reachability-aware.

- [ ] **Step 1: Write failing tests:**
  - `RelayServicePolicy.VerifiedReachableEnablesRelayInAuto`
  - `RelayServicePolicy.OperatorOffBeatsVerified`
  - `RelayServicePolicy.UnverifiedNonMiningStaysOff`
  - `RelayServicePolicy.ServiceBitFollowsVerification`
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R '^RelayVirtualTransport$'` (it contains the policy group).
  Expected: all pass.
- [ ] **Step 5: Commit** `relay: verified-reachable nodes relay automatically (operator off still wins)`

---

### Task 2: Owner's caps as defaults, monthly quota, pause while syncing

**Files:**
- Modify: `src/daemon/p2p_manager.h` (default limits ~428-434, per Global Constraints)
- Modify: `src/daemon/p2p_manager.cpp`
  - quota ~1224: a calendar-month window persisted to `<datadir>/relay_quota` as
    `{month: "YYYY-MM", bytes}`, written at most once a minute and at shutdown
  - behind-throttle ~1047 / ~1294: refuse new circuits when in IBD or more than 6
    blocks behind
- Modify: `src/daemon/services/p2p_service.cpp` (`ValidRelayServiceLimits` ~69: keep
  custom limits valid; defaults come from the header)
- Modify: `src/rpc/methods_network_context.cpp` (`relay`: add
  `quota_month_bytes`, `quota_month_limit`, `paused_reason`)
- Modify: `qt/src/` settings: add "Help other home nodes connect (relay, up to 20 GB
  a month)", default on (it maps to automatic), plus current-month usage from
  `relay.quota_month_bytes`
- Test: `tests/p2p/test_relay_virtual_transport.cpp` `RelayBehindThrottle` group, and a
  new `RelayCaps` group in the same file

- [ ] **Step 1: Write failing tests:**
  - `RelayCaps.NinthCircuitRefused`
  - `RelayCaps.GlobalRateStaysUnder200KBps`: push 2 MB through 3 circuits; elapsed ≥
    9.5 s, measured with the injected clock if the bucket takes one, otherwise real
    time with ±10%
  - `RelayCaps.MonthlyQuotaPersistsAcrossRestartAndResetsOnNewMonth` (Review Focus 4)
  - `RelayCaps.SyncingRelayRefusesNewCircuits` (Review Focus 4): 7 blocks behind means
    the connect is acked as "behind", and an existing circuit still carries data
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R '^RelayVirtualTransport$'`. Expected: all pass.
- [ ] **Step 5: Commit** `relay: owner caps by default (8 circuits, 200 KB/s, 20 GB/month persisted), pause while syncing`

---

### Task 3: Two diverse, confirmed, live relay registrations

**Files:**
- Modify: `include/network/types.h` (`RELAY_REGISTER_OK = "relayrok"`)
- Modify: `src/daemon/p2p_manager.h` (`kAutoRelayTargetCount = 2`, ~1051)
- Modify: `src/daemon/p2p_manager.cpp`:
  - `handle_relay_register` ~892: on accept, send `relayrok` `{ttl_u32}`; on refusal
    (role off, ~894) send nothing, so old behavior is preserved
  - `SendRelayRegisterIfConfigured` ~2758-2791: mark `is_our_relay` only on `relayrok`
    within 30 s; otherwise mark the relay `unacked` for 30 min and don't count it
  - `MaybeAutoRegisterWithRelays` ~4013-4097:
    - at most one relay per netgroup;
    - community `NODE_RELAY` candidates (addrman health order) first;
    - `kMainnetRelayPeers` only to fill;
    - drop the "reserve one fleet slot" rule (~4021)
  - keepalive tick ~8066-8070: send `create_relay_ping` every 30 s to each confirmed
    relay; 3 missed pongs (the handler is at ~1570; add the pong reply if it doesn't
    already reply) means the relay is dead, so deregister locally and re-plan
- Test: `RelayAutoRegister` group in `tests/p2p/test_relay_virtual_transport.cpp`
  (hook `test_maybe_auto_register_with_relays`)

- [ ] **Step 1: Write failing tests:**
  - `RelayRegistration.TargetsTwoRelaysInDistinctNetgroups`
  - `RelayRegistration.PrefersCommunityOverFleet`
  - `RelayRegistration.FleetFillsWhenNoCommunity` (today's network)
  - `RelayRegistration.UnackedRegistrationIsReplaced` (Review Focus 5)
  - `RelayRegistration.DeadRelayDetectedByMissedPingsAndReplaced`
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R '^RelayVirtualTransport$'` plus
  `test_relay_registry_grace`. Expected: all pass.
- [ ] **Step 5: Commit** `relay: two acked, pinged registrations on distinct netgroups, community first`

---

### Task 4: Signed, forwarded hints, so any node can find any NAT'd node

**Files:**
- Modify: `include/network/types.h` (`RELAY_HINTS_V2 = "relayhn2"`)
- Create: `include/p2p/relay_hint_v2.h`, `src/p2p/relay_hint_v2.cpp`, as pure functions:
  - `std::vector<uint8_t> SignHint(const NodeIdentity&, const HintFields&)`
  - `std::optional<HintFields> VerifyHint(span<const uint8_t>, uint32_t now)`
  - `class HintForwarder` (dedup set with expiry, hop limit 3, token bucket 50/min)
- Modify: `src/daemon/p2p_manager.cpp`:
  - the target re-signs its hints every 10 min (30 min expiry) and sends `relayhn2` to
    V2 peers and its relays
  - the relay re-advertises stored v2 hints every 10 min, fixing today's "only on first
    registration" (~1004, ~2504)
  - `handle_relay_hints` ~2613: on `relayhn2`, `VerifyHint`, then store (same TTL map,
    capped at 4 per target), then `HintForwarder`, then forward
  - `OrchestrateRelayDials` ~2102-2185 also uses v2-learned hints
- Test: create `tests/p2p/test_relay_hint_v2.cpp`, registered as `RelayHintV2`

**Interfaces:**
- Consumes: `NodeIdentity::sign_bytes`/`verify_bytes` (`node_identity.cpp:110-133`).
- Produces: `HintFields { node_id20, pubkey33, relay_addr, relay_port, expiry, hop }`.

- [ ] **Step 1: Write failing tests:**
  - `SignedHints.ValidHintVerifies`
  - `SignedHints.ForgedOrExpiredHintIsDroppedAndNotForwarded` (Review Focus 2): wrong
    key, node_id ≠ HASH160(pubkey), tampered relay_addr, expiry in the past
  - `SignedHints.ForwardingIsBoundedInACycle` (Review Focus 3): 5 managers in a ring;
    one hint reaches all of them; total forwards ≤ 5 × 1 with no repeats; hop limit
    honored
  - `SignedHints.ThirdHopNodeCanDialTarget`: A (NAT'd) registers with relay R; node D
    is 2 hops from R and not connected to R or A; within 30 s `OrchestrateRelayDials`
    on D opens a circuit to A
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R 'RelayHintV2|RelayVirtualTransport|test_relay_hints_eviction'`.
  Expected: all pass.
- [ ] **Step 5: Commit** `relay: target-signed hints, forwarded with dedup + hop limit, re-advertised every 10 min`

---

### Task 5: Bind identity to the QUIC session; fix the outbound relay identity check

**Files:**
- Modify: `include/network/quic_session.h`, `src/network/quic_session.cpp`
  - in `CaptureTlsSnapshots` ~866-881 / ~986-1000, call `SSL_export_keying_material`
    on the session thread's `SSL* ssl` (~177) and publish the result under the
    snapshot mutex
  - `ExportBinding()` returns it; the stub `Impl` (~1119) returns `nullopt`
- Modify: `src/daemon/p2p_manager.cpp`
  - dineroid build ~2858-2880 and verify ~3737-3754: if the peer `uses_quic_stream()`
    (Plan 2a helper), sign and verify `nonce || binding` with version byte `0x02`
  - for `via_relay` peers, any failure disconnects
  - at ~3753, for outbound `via_relay` peers, compare the derived id with the
    pre-filled `target_node_id` (~1797); on mismatch, disconnect and mark that hint bad
- Test: `tests/p2p/test_relay_identity_binding.cpp`, registered as
  `RelayIdentityBinding`, using `quic_loopback_pair.h`

**Interfaces:**
- Produces: `QuicSession::ExportBinding() -> std::optional<std::array<uint8_t,32>>`,
  which Plan 4 also uses for punched sessions.

- [ ] **Step 1: Write failing tests:**
  - `RelayIdentityBinding.BindingMatchesOnBothEndsOfOneSession`
  - `RelayIdentityBinding.MitmRelayIsDetectedOnBothEnds` (Review Focus 1):
    - A connects to M's server leg; M connects to B on a client leg;
    - M bridges the two decrypted streams byte for byte, including dineroid;
    - both A and B disconnect with an identity-binding failure within 10 s.
  - `RelayIdentityBinding.OutboundRelayPeerWithWrongIdentityIsDisconnected` (bug (e)):
    dial target X; the endpoint presents a valid identity Y ≠ X, so disconnect and
    mark the hint bad.
  - `RelayIdentityBinding.RelayPeerWithoutV2ProofIsDisconnected`
  - `RelayIdentityBinding.TcpPeerLegacyBehaviorUnchanged`
- [ ] **Step 2: Run them and see them fail.** In particular, the MITM test passes the
  handshake today, and that's the bug.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R 'RelayIdentityBinding|RelayVirtualTransport|QuicSession|QuicDirectPeer'`.
  Expected: all pass.
- [ ] **Step 5: Commit** `relay: bind dineroid to the QUIC TLS exporter; enforce expected identity on outbound relay peers`

---

### Task 6: RELAY_CONNECT timeouts — fix the bookkeeping, then root-cause on the fleet

**Files:**
- Modify: `src/daemon/p2p_manager.cpp`
  - `cleanup_peer` ~6130: erase `pending_connects_` entries whose relay is this peer,
    and fail them immediately with reason `relay_disconnected`
  - `SweepRelayConnectTimeouts` ~2290: run on a dedicated 2 s tick, not the 30 s
    keepalive, so the 10 s timeout means 10-12 s
  - relay side: log every RELAY_CONNECT drop reason, release-compat disconnect
    (~6626) and send failure (~6834-6847), at info level with the circuit id
  - the originator logs ack-drop reasons (~1693, ~1717)
- Test: the `RelayOrchestrator` group

- [ ] **Step 1: Write failing tests:**
  - `RelayOrchestrator.PendingConnectFailsImmediatelyWhenRelayDisconnects`
  - `RelayOrchestrator.TimeoutFiresWithin12Seconds`
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R '^RelayVirtualTransport$'`. Expected: pass.
- [ ] **Step 5: Commit** `relay: fail pending connects on relay disconnect, 2 s sweep, log every drop reason`
- [ ] **Step 6: Root-cause the live timeouts.** Needs owner approval for the canary.
  - Put a Plan-1-gated build of this branch on **DineroTX** as a public relay, and a
    second node behind the netns NAT there.
  - Reproduce a dial through DineroTX and through one fleet node that has been upgraded
    (one at a time).
  - Using the new drop-reason logs, record which path fails. These are the five
    candidates from the code map: silent release-compat disconnect, ack dropped for
    sender or length, dead half-open link, relay reader stalled by blocking forward
    sends, old binary.
  - If it is the blocking `send_to_peer(dest)` in `handle_relay_data` (~1218), move the
    forward onto the existing async outbox in a follow-up commit, with a test
    reproducing reader starvation (`RelayOrchestrator.ForwardDoesNotBlockRelayReader`).

---

### Task 7: End to end — community relay, MITM-proof, on real networks

**Files:**
- Modify: `tests/integration/test_relay_nat_netns.sh`, adding scenarios:
  - `COMMUNITY_RELAY=1`: the public relay is a verified home-style node, not a
    pinned/fleet relay; two NAT'd nodes in distinct netgroups connect through it
  - `TWO_RELAYS=1`: kill one relay; the NAT'd node stays reachable via the second
    within 90 s, and re-registers a replacement
  - `CAPS=1`: the 9th circuit is refused; throughput stays ≤ 200 KB/s ±10%

- [ ] **Step 1: Write the scenarios.** They fail before Tasks 1-4.
- [ ] **Step 2: Run them on DineroTX.** Expected: PASS. Record the output.
- [ ] **Step 3: Acceptance test 1 (owner's).**
  - The Dell, with inbound P2P blocked by its local firewall (reversible:
    `ufw deny 20999`, undone after) and no configuration.
  - Expected: it shows `relay.registered` to the DineroTX community relay (not a fleet
    node), and a node on DineroTX dials it through the relay and exchanges blocks.
  - Record `getnetworkinfo` / `getpeerinfo` from both sides. Restore the firewall.
- [ ] **Step 4: Neuter checks.**
  - Disable binding enforcement in a scratch commit: the MITM unit test must fail.
  - Revert the netgroup filter: `TWO_RELAYS` must show both relays in one /16 and
    fail its assertion.
  - Drop both scratch commits.
- [ ] **Step 5: Commit** `test: community relay, two-relay failover and caps scenarios`

---

## After this plan

- **Plan 4** sends punch introductions over the relay circuit that Tasks 3-5 make
  reliable and authenticated, and reuses `ExportBinding()` for punched sessions.
- **Plan 2c's crawler** counts `NODE_RELAY` community relays for the health page.
- **Fleet rollout of relay-capable builds:** Plan 6 (the rollout plan), one node at a
  time.
