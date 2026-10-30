# Reachability Plan 4: Relay-Coordinated Hole Punching — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two nodes that are connected through a relay try to open a direct QUIC connection
to each other through their routers, then drop the relayed connection. If punching fails,
they stay relayed.

**Architecture:**
- **No relay changes.** Coordination runs end to end over the existing relayed session,
  which is already identity-proven and session-bound (Plan 3). The relay can't read or
  forge it, and relays running v8.2.0 need no update. Same idea as libp2p DCUtR.
- **Roles.** The side that dialed the relayed connection (the origin) is the
  **initiator** and the QUIC client. The other side is the **acceptor**:
  - it sends small throwaway UDP probes from the P2P port, to open its own router toward
    the initiator;
  - its existing `QuicEndpoint` then accepts the initiator's Initial.
  - Both sides calling `Connect` can't work. Each side would register a client session
    for the other's address, and the other side's Initial would land on that client
    session and be discarded (`QuicEndpoint::OnDatagram`).
- **Timing.** The initiator measures round-trip time with the offer/answer exchange. It
  then sends `punchgo` and dials RTT/2 later. The acceptor starts probing as soon as
  `punchgo` arrives, so its probes and the initiator's Initial cross at about the same
  time.
- **Replacement, not migration.** Once the direct session has proven the same node id
  with a session-bound `dineroid`, the relayed peer with that node id is disconnected.
  The relay's circuit then idle-closes within `kCircuitIdleTimeout` (5 min). No QUIC
  connection migration and no new close frame.
- **Pure logic in its own unit.** Address pairing, skip rules, timing and backoff live
  in a pure `dinero::p2p` component, so they are unit-tested without sockets, like
  `SelfReachability` and `NatClassifier`.

**Tech Stack:** C++20, the existing `QuicEndpoint`/`QuicSession` (vendored ngtcp2
1.22.1, OpenSSL 3.5.7), gtest, and bash/netns integration tests on DineroTX.

**Spec:** `docs/superpowers/specs/2026-10-25-zero-touch-p2p-reachability-design.md`,
section E. The owner superseded the 2026-10-27 deferral on 2026-10-29: qualify
this completed implementation with the relay stack for v8.2.0. Inclusion depends
on the combined-source tests, not on the earlier standalone branch results.

## Global Constraints

- **Release.** Target v8.2.0 after combined-source qualification (owner direction,
  2026-10-29). The earlier post-v8.2.0 deferral is superseded.
- **Branch.** Work is on `feat/reachability-hole-punch`, stacked on
  `feat/reachability-community-relays` (#853). Rebase when the stack moves.
- **Non-consensus.** No change to validation, mining or block rules.
- **Transport.** UDP/QUIC only. No TCP simultaneous open, and no IPv4 port prediction
  (spec E).
- **Order.** IPv6 first. IPv4 only when neither side's mapping is `endpoint_dependent`.
  `unknown` is attempted (spec E: "pairs where either side has endpoint-dependent
  (symmetric) mapping skip punching").
- **A punched connection is a first-class sync peer** (spec E). It goes through the same
  handshake, `dineroid` v2 and message loop as any direct QUIC peer.
- **Old peers see nothing new.** Punch messages go only to peers that advertise
  `NODE_HOLEPUNCH`. Peers without the bit never receive them.
- **Failure leaves the relayed connection untouched.**
- **Verification rules** (owner):
  - confirm every test ran;
  - neuter-check each fix: the test must fail with the fix disabled;
  - no `assert()`-only gates;
  - `touch` and rebuild before trusting a result;
  - every fix ships with a test that fails first.
- **Commit identity:** `git -c user.name=Claude -c user.email=team@dinerolabs.org commit`,
  with the trailer `Co-Authored-By: Claude Opus 5 (1M context) <team@dinerolabs.org>`.
- **Approval.** Fleet/DNS changes and router changes need owner approval. Dell tests run
  only with the owner present. Never reuse the Dell sudo password.

## Review Focus

1. **The initiator already has 8 outbound peers** (the usual case). Before this plan,
   `OrchestrateRelayDials` returned at `MAX_OUTBOUND_CONNECTIONS`, so a busy home node
   never formed the relayed link that punching starts from, and a punched dial could be
   refused for want of a slot. Expect the relayed dial and the punched dial both to go
   ahead within the home-peer budget (Task 4 tests).
2. **Both sides try to initiate.** Two nodes each dialed the other through a relay, so
   each holds an outbound relayed peer. Expect exactly one attempt: only the side with
   the lower node id initiates (Task 1 `RoleFor`, Task 5 test).
3. **Someone else answers at the punched address.** The direct session proves a
   different node id. Expect it to be disconnected and counted as `mismatch`, with the
   relayed peer kept (Task 6 test).
4. **A malicious relayed peer names a victim's address.** Its candidates are only its
   own claim, so it could try to get acceptors to probe someone else. Expect the
   acceptor side to apply the same in-flight cap and per-peer backoff, to probe at most
   3 targets per attempt, and to send a bounded total (at most 3 × 40 probes of 8 bytes
   per attempt) (Task 5 test).
5. **Stale candidates.** The address changed since the last STUN round, so the attempt
   times out. Expect `timeout` to be counted, the relayed peer kept, and no new attempt
   for that node for 30 min (Task 5 test).

Also pinned by tests, below the top five: the acceptor has no free inbound slot
(Task 5), and both nodes share one public IPv4 address, so the IPv4 pairing is skipped
(Task 1).

---

### Task 1: Pure punch planner

**Files:**
- Create: `include/p2p/hole_punch.h`
- Create: `src/p2p/hole_punch.cpp`
- Test: `tests/p2p/test_hole_punch.cpp` (add to `tests/CMakeLists.txt` next to
  `test_nat_classifier`, labels `p2p;quic`)

**Interfaces:**
- Produces (namespace `dinero::p2p`):
  - `struct PunchCandidates { std::vector<network::UdpAddr> v6; std::optional<network::UdpAddr> v4; NatMappingType v4_mapping{NatMappingType::Unknown}; };`
  - `std::vector<network::UdpAddr> ChoosePunchTargets(const PunchCandidates& local, const PunchCandidates& remote);`
    Returns the remote addresses to try, in order: every remote v6 address (only if
    `local.v6` is non-empty), then `remote.v4`, capped at `kPunchMaxTargets = 3`. `remote.v4` is included only if both
    sides have a v4, neither mapping is `EndpointDependent`, and the two v4 IPs differ.
  - `enum class PunchRole { None, Initiator, Acceptor };`
    `PunchRole RoleFor(bool we_dialed_relayed, bool they_dialed_relayed_too, const std::array<uint8_t,20>& ours, const std::array<uint8_t,20>& theirs);`
    - We dialed, they didn't: `Initiator`.
    - They dialed, we didn't: `Acceptor`.
    - Both dialed: the lower node id is the `Initiator`.
  - `class PunchScheduler` (clock injected, as in `SelfReachability`):
    - `bool MayStart(const std::array<uint8_t,20>& peer)`: false when 2 attempts are
      already in flight, or this peer failed less than 30 min ago.
    - `void Started(peer)`, `void Succeeded(peer)`, `void Failed(peer)`.
  - Constants: `kPunchAttemptTimeout = 10s`, `kPunchProbeInterval = 250ms`,
    `kPunchMaxInFlight = 2`, `kPunchFailureBackoff = 30min`, `kPunchMaxTargets = 3`.

- [ ] **Step 1: Write the failing tests**
  - `HolePunch.Ipv6FirstThenIpv4`: both sides have v6 and v4, both `EndpointIndependent`,
    so the result is `[remote v6..., remote v4]`.
  - `HolePunch.SymmetricEitherSideSkipsIpv4`: run it with the local side
    `EndpointDependent`, then with the remote side `EndpointDependent`. Each time the
    result has v6 only.
  - `HolePunch.UnknownMappingIsAttempted`: `Unknown` on both sides still includes v4.
  - `HolePunch.NoLocalV6SkipsRemoteV6`.
  - `HolePunch.TargetsCappedAtThree`: 5 remote v6 addresses plus a v4 give 3 targets.
  - `HolePunch.SamePublicIpv4SkipsIpv4` (Review Focus 5): the v4 IPs are equal (ports
    differ), so the result has no v4.
  - `HolePunch.RoleWhenBothDialed` (Review Focus 1): the node with the lower id is
    `Initiator` and the other is `Acceptor`. Check from both sides with the same two ids.
  - `HolePunch.SchedulerCapsInFlightAndBacksOffFailures`:
    - with 2 attempts started, a 3rd peer gets `MayStart` false;
    - a failed peer is refused at 29 min and allowed at 31 min;
    - a peer that succeeded is not backed off.

- [ ] **Step 2: Run them to verify they fail**
  Run: `cmake --build build --target test_hole_punch && ./build/test_hole_punch`
  Expected: the build fails because `hole_punch.h` doesn't exist.

- [ ] **Step 3: Implement `hole_punch.h/.cpp` as specified above**
  Compare IPs with the same key `QuicEndpoint` uses (`IpKey`, address bytes only).

- [ ] **Step 4: Run them to verify they pass**
  Run: `./build/test_hole_punch`. Expected: 8/8 PASS.

- [ ] **Step 5: Neuter-check, then commit**
  Remove the same-IP rule and confirm `SamePublicIpv4SkipsIpv4` fails. Remove the
  `EndpointDependent` rule and confirm `SymmetricEitherSideSkipsIpv4` fails. Restore
  both, then commit: `feat(p2p): hole punch planner (pairing, roles, backoff)`.

### Task 2: Wire messages and the service bit

**Files:**
- Modify: `include/network/types.h` (service flags at lines 134-157)
- Modify: `src/daemon/p2p_manager.h` / `src/daemon/p2p_manager.cpp`:
  `P2PMessage::create_*`, as for `create_relay_hints` (~line 859)
- Test: `tests/p2p/test_hole_punch.cpp`

**Interfaces:**
- Consumes: Task 1 `PunchCandidates`.
- Produces:
  - `constexpr uint64_t NODE_HOLEPUNCH = 1ULL << 32;` Service flags are `uint64_t` end
    to end (`PeerInfo::service_flags`). Check that version encode/decode keeps bit 32,
    and add a test.
  - `P2PMessage::create_punch_offer(uint64_t nonce, const PunchCandidates&)`. Command
    `punchoff`.
  - `P2PMessage::create_punch_answer(uint64_t nonce, const PunchCandidates&)`. Command
    `punchans`.
  - `P2PMessage::create_punch_go(uint64_t nonce)`. Command `punchgo`.
  - `std::optional<std::pair<uint64_t, PunchCandidates>> ParsePunchCandidates(const std::vector<uint8_t>&)`.
  - Payload format:
    - `nonce u64 LE`;
    - `v4_mapping u8` (0 unknown, 1 independent, 2 dependent);
    - `count u8` (at most 8);
    - then `count × (net u8 [BIP155 1=v4, 2=v6] | addr 4/16 | port u16 BE)`;
    - at most one v4 entry.
    - Anything else is rejected whole, including trailing bytes.

- [ ] **Step 1: Write the failing tests**
  - `HolePunchWire.RoundTripsV4AndV6`.
  - `HolePunchWire.RejectsMalformed`: each of these is rejected:
    - a truncated address;
    - `count` of 9;
    - two v4 entries;
    - an unknown net value;
    - a trailing byte.
  - `HolePunchWire.ServiceBit32SurvivesVersion`: encode and decode a version message
    with `NODE_HOLEPUNCH | NODE_P2P_QUIC`, and both bits survive.

- [ ] **Step 2: Run them to verify they fail** (undefined symbols).

- [ ] **Step 3: Implement the codec and the flag.**
  - Advertise `NODE_HOLEPUNCH` from `local_quic_service_bits()` only when the endpoint
    is listening.

- [ ] **Step 4: Run them to verify they pass, neuter-check the trailing-byte rule, then
  commit** `feat(p2p): punch offer/answer/go messages and NODE_HOLEPUNCH`.

### Task 3: Local candidates and NAT mapping in P2PManager

**Files:**
- Modify: `src/daemon/services/p2p_service.cpp` (~line 950, where `Classify` runs)
- Modify: `src/daemon/p2p_manager.h/.cpp`
- Test: `tests/p2p/test_hole_punch.cpp`, or a `PunchCandidates` group in
  `test_relay_virtual_transport.cpp` (it already has a `P2PManager` test harness)

**Interfaces:**
- Consumes: `stun_reflexive_` (set by `update_stun_candidates`, measured on the P2P
  port), `NatMapping` from `Classify`.
- Produces:
  - `void P2PManager::set_nat_mapping(dinero::p2p::NatMapping)`. P2PService calls it
    next to `nat_mapping_ = ...`.
  - `dinero::p2p::PunchCandidates P2PManager::LocalPunchCandidates() const`:
    - v6 = this host's routable (global unicast) IPv6 interface addresses, enumerated
      with the `getifaddrs` code in `include/network/local_interfaces.h`, each with the
      QUIC endpoint's port. Not `stun_reflexive_`: `StunClient` resolves each server to
      IPv4 first (`stun_client.cpp` ~line 83), so STUN almost never yields a v6
      address, and IPv6 has no translation to measure anyway;
    - v4 = the first routable IPv4 entry of `stun_reflexive_`;
    - `v4_mapping` = the last mapping set.
    - The result is empty when the QUIC endpoint is not listening.

- [ ] **Step 1: Write the failing tests**
  - `PunchCandidates.FromStunRoundAndMapping`: inject reflexive `203.0.113.5:20999` and
    `EndpointIndependent`, plus an injected interface list with `2001:db8::5`. Expect v4
    `203.0.113.5:20999`, v6 `[2001:db8::5]:<endpoint port>`, and the mapping.
  - `PunchCandidates.EmptyWithoutQuicEndpoint`.
  - `PunchCandidates.SkipsNonRoutable`: `10.0.0.5`, `fe80::1` and `fd00::1` are
    excluded.

- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement.**
  - Use `update_stun_candidates` for the v4 input.
  - Add one test hook, `test_set_local_interface_ips(std::vector<std::string>)`, that
    replaces the `getifaddrs` read in test builds only.
- [ ] **Step 4: Run them to verify they pass, then commit**
  `feat(p2p): local punch candidates from STUN and NAT mapping`.

### Task 4: Home-peer outbound budget

**Files:**
- Modify: `src/daemon/p2p_manager.h/.cpp`: `OrchestrateRelayDials` (~line 2697, the
  early return at ~line 2729) and `connect_to_peer_quic` (~line 8560).
- Test: `tests/p2p/test_relay_virtual_transport.cpp` (`RelayOrchestrator` group; it
  already builds 7-peer setups).

**Interfaces:**
- Produces:
  - `PeerInfo` fields:
    - `bool punched{false}`;
    - `std::optional<std::array<uint8_t,20>> punch_expected_id`, set on an outbound
      punched dial.
  - `static constexpr size_t kHomePeerSlots = 2;`
  - Outbound peers that are `via_relay` or `punched` count against `kHomePeerSlots`,
    not against `MAX_OUTBOUND_CONNECTIONS`, both in the orchestrator's count and in any
    other outbound-cap check they pass through.
  - `bool P2PManager::connect_to_peer_quic(const std::string& ip, uint16_t port, std::optional<std::array<uint8_t,20>> punch_expected_id = std::nullopt);`
    A dial with `punch_expected_id` set is a punched dial: it is counted in the
    home-peer budget, and it replaces the relayed peer.

- [ ] **Step 1: Write the failing tests**
  - `RelayOrchestrator.RelayedDialAtFullOutbound` (Review Focus 1): with 8 direct
    outbound peers and a signed hint for target T through a connected relay, a
    `relaycon` for T is sent.
  - `RelayOrchestrator.HomePeerBudgetCapsAtTwo`: 8 direct outbound peers, 2 relayed
    outbound peers and a hint for a 3rd target, so no `relaycon` is sent.
  - `RelayOrchestrator.DirectSlotsUnchangedByRelayedPeers`: 7 direct and 2 relayed, so
    the normal direct-dial pass still sees 1 free slot.
- [ ] **Step 2: Run them to verify they fail.** Expected: the first fails, because the
  orchestrator returns at 8.
- [ ] **Step 3: Implement the split count.**
- [ ] **Step 4: Run them to verify they pass.** Then rerun the whole
  `test_relay_virtual_transport` binary. Every existing `RelayOrchestrator` test must
  still pass, including the 7-peer `AcceptedColdDialDoesNotStarveTheConnectedRoute`.
- [ ] **Step 5: Neuter the split** (count relayed peers as direct again) and confirm
  `RelayedDialAtFullOutbound` fails. Restore it, then commit
  `feat(p2p): home-peer outbound budget for relayed and punched peers`.

### Task 5: The punch exchange (initiator and acceptor)

**Files:**
- Modify: `src/daemon/p2p_manager.h/.cpp`:
  - a message handler next to `relaycon`/`relayhn2` (~line 7710);
  - a tick on the keepalive loop next to `SweepIdleCircuits` (~line 9801).
- Test: new `tests/p2p/test_hole_punch_manager.cpp`. Use the two-manager fixture from
  `tests/p2p/test_quic_direct_peer.cpp` (`Node`, `SharedLoopback`) with real loopback
  `QuicEndpoint`s, plus `test_install_quic_relay_peer` for the relayed link.

**Interfaces:**
- Consumes:
  - Tasks 1-4 (`punched` and `punch_expected_id` come from Task 4);
  - `QuicEndpoint::SendDatagram(dest, data, len)` for probes;
  - Task 4 `connect_to_peer_quic(..., punch_expected_id)`;
  - `PeerInfo::via_relay`, `identity_proven`, `their_node_id`, `is_outbound`.
- Produces:
  - Per-peer punch state in P2PManager, under its own leaf mutex `punch_mutex_`:
    `nonce`, `role`, `offer_sent_at`, `deadline`, `targets`.
  - Test hooks:
    - `test_punch_tick()`;
    - `test_set_punch_clock(...)`, or reuse `clock_`;
    - `test_punch_stats()`.

**Flow:**
- **Trigger.** When a relayed peer is identity-proven, advertises `NODE_HOLEPUNCH`, has
  `RoleFor == Initiator`, and `MayStart` allows it, the initiator:
  - sends `punchoff` with `LocalPunchCandidates()`;
  - records `offer_sent_at`.
- **Acceptor on `punchoff`.** This is accepted only from a relayed, identity-proven peer
  whose role for us is `Acceptor`, and only when the acceptor's own `PunchScheduler`
  `MayStart` allows it (the same in-flight cap and per-peer backoff as the initiator).
  The acceptor answers `punchans` with its own candidates. A refused offer gets no
  answer.
- **Initiator on `punchans`.**
  - It computes `targets = ChoosePunchTargets(local, remote)`. If that's empty, it
    counts `skipped` and stops.
  - Otherwise it sets `rtt = now - offer_sent_at` and sends `punchgo`.
  - After `rtt/2` it dials `targets[0]` over QUIC with `punch_expected_id` set. On
    failure, it tries the next target, while the deadline allows.
- **Acceptor on `punchgo`.** Every `kPunchProbeInterval`, until the deadline, it sends
  an 8-byte probe (first byte `0x00`, the rest random) from the P2P port to each of
  the initiator's targets
  (`ChoosePunchTargets(local, remote)` computed from its own view).
  - The probe is shorter than 20 bytes, so `IsStunPacket` is false. It is not a long
    header, so `LooksLikeClientInitial` is false. The receiver drops it as an unknown
    datagram.
  - The probe exists only to open the acceptor's router.
- **Probe budget.** At most `kPunchMaxTargets` targets, one probe each per
  `kPunchProbeInterval`, until the deadline: at most 3 × 40 probes per attempt.
- **Deadline.** Both sides use `kPunchAttemptTimeout` from `punchgo`. If it expires
  without a punched peer, the attempt has failed (Task 6 decides success).
- **Messages ignored with a log line, no misbehavior:**
  - from non-relayed peers, or peers without the bit;
  - with the wrong nonce;
  - a duplicate `punchoff` while an attempt is in flight.

- [ ] **Step 1: Write the failing tests**
  - `HolePunchManager.InitiatorOffersAcceptorAnswersAndBothConnectDirect`:
    - nodes A and B are linked by a relayed peer (A dialed it);
    - both have loopback "reflexive" candidates;
    - after ticks, A has an outbound `quic_direct` peer to B with `punched=true`;
    - B's punch stats show probes sent (`probes_sent > 0`). Don't assert how A's
      endpoint counted them: once A's client session exists, they route into it
      instead of being dropped.
  - `HolePunchManager.OnlyLowerIdInitiatesWhenBothDialed` (Review Focus 1): two
    relayed links, one in each direction, give exactly one `punchoff` on the wire in
    total.
  - `HolePunchManager.NoBitNoMessages`: the peer lacks `NODE_HOLEPUNCH`, so it receives
    no `punch*`.
  - `HolePunchManager.StaleCandidateTimesOutAndBacksOff` (Review Focus 3):
    - B's answer names a dead port;
    - after 10 s the attempt is `timeout` and the relayed peer is still connected;
    - no new `punchoff` goes to B before 30 min.
  - `HolePunchManager.AcceptorWithoutInboundSlotFailsCleanly` (Review Focus 4): B's
    inbound slots are full, so the attempt fails, the relayed peer stays, and exactly
    one attempt is made.
  - `HolePunchManager.AcceptorBoundsProbesToClaimedTargets` (Review Focus 4):
    - a relayed peer offers 8 v6 candidates naming third-party addresses;
    - B probes at most 3 targets, and sends at most 120 probes in total for the attempt;
    - a second `punchoff` from the same peer within 30 min of a failure gets no answer.
  - `HolePunchManager.InitiatorAtFullOutboundStillPunches` (Review Focus 1): A has 8
    direct outbound peers, and the punch still completes.
  - `HolePunchManager.UnsolicitedPunchMessagesIgnored`: `punchoff` from a direct TCP
    peer, and `punchgo` with a wrong nonce, change no state.

- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement the flow.**
  - Respect the endpoint lock order (`p2p_manager.h` ~line 1136): never call into
    `quic_endpoint_` while holding `peers_mutex_`.
  - Take `punch_mutex_` only as a leaf.
- [ ] **Step 4: Run them to verify they pass.** Then rerun the whole
  `test_quic_direct_peer` and `test_relay_virtual_transport` binaries.
- [ ] **Step 5: Neuter-check** the role tie-break and the failure backoff, one at a
  time. Then commit `feat(p2p): relay-coordinated punch exchange`.

### Task 6: Prove, replace and report

**Files:**
- Modify: `src/daemon/p2p_manager.cpp`:
  - the `dineroid` check (~line 4448, next to the relayed-target check);
  - peer cleanup.
- Modify: `src/daemon/p2p_manager.h`: `PeerInfo::transport_name()` and a new
  `path_name()`.
- Modify: `include/rpc/peer_json_utils.h` (`getpeerinfo`) and
  `src/rpc/methods_network_context.cpp` (`getnetworkinfo`).
- Test: `tests/p2p/test_hole_punch_manager.cpp`, plus `PeerJson` in
  `test_quic_direct_peer.cpp`.

**Interfaces:**
- Consumes: Task 4 `punched` / `punch_expected_id`, and Task 5's punch state.
- Produces:
  - `PeerInfo::path_name()` returns `"punched"` when `punched`, `"relayed"` when
    `via_relay`, else `"direct"`. `getpeerinfo` gets a new `"path"` field, and
    `"transport"` is unchanged.
  - `getnetworkinfo.hole_punch` =
    `{attempts, succeeded, failed: {timeout, mismatch, refused}, skipped, by_family: {ipv4: {attempts, succeeded}, ipv6: {...}}}`.

**Rules:**
- **Initiator's punched session.** Its `dineroid` must be session-bound (v2). That is
  already enforced for every direct QUIC peer, since `bound = peer->uses_quic_stream()`
  covers `quic_direct` (`p2p_manager.cpp` ~line 4388), so no new binding wiring is
  needed. The derived id must equal `punch_expected_id`. Otherwise it is disconnected and counted
  as `mismatch`, and the relayed peer is kept. This mirrors the existing relayed-target
  check.
- **Acceptor's punched session.** An inbound `quic_direct` peer whose proven
  (session-bound) id matches an in-flight acceptor attempt is marked `punched`.
- **On success (either side):**
  - disconnect the relayed peer with the same node id;
  - `Succeeded(peer)` and clear the punch state.
  - The relay's circuit idle-closes on its own.

- [ ] **Step 1: Write the failing tests**
  - `HolePunchManager.SuccessReplacesRelayedPeer`: after the punch, A and B each have
    exactly one peer for the other, `path=="punched"`, and a `ping` round-trips over it.
  - `HolePunchManager.WrongIdentityAtPunchedAddressIsDropped` (Review Focus 2):
    - a third node C answers at the target address;
    - the punched session is disconnected and `failed.mismatch == 1`;
    - the relayed peer to B is still connected.
  - `HolePunchManager.NonBoundProofOnPunchedSessionIsRefused`: a v1-only `dineroid`
    (`test_set_dineroid_v1_only`) on the punched session is disconnected.
  - `PeerJson.PathFieldReflectsPunchedRelayedDirect`.
  - `HolePunchManager.NetworkInfoCountsOutcomes`: a success and a timeout show up in the
    counters, by family.

- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them to verify they pass.** Then run the whole relay, QUIC and
  identity-binding binaries (`test_relay_virtual_transport`, `test_quic_direct_peer`,
  `test_relay_identity_binding`).
- [ ] **Step 5: Neuter-check, then commit.**
  - Neuter the expected-id comparison: `WrongIdentity...` must fail.
  - Neuter the relayed-peer disconnect: `SuccessReplaces...` must fail.
  - Restore both, then commit `feat(p2p): punched peers replace relayed ones; path and counters`.

### Task 7: netns scenarios on DineroTX

**Files:**
- Modify: `tests/integration/test_relay_nat_netns.sh`. Reuse `configure_nat` (EIM by
  default, `--random` for endpoint-dependent) and the `COMMUNITY_RELAY` topology
  (public relay, dial-back).
- Modify: `tests/CMakeLists.txt` if the scenarios are registered there.

**Interfaces:**
- Consumes:
  - Task 6 `getpeerinfo.path` and `getnetworkinfo.hole_punch`;
  - the existing helpers `wait_relay_peerinfo`, `wait_networkinfo_jq`,
    `wait_peer_gone`.

- [ ] **Step 1: Write the scenarios** (each needs `DINERO_RUN_NETNS_NAT=1`, like the
  others):
  - **`PUNCH_V4=1`** (spec acceptance 2):
    - setup: E1 and E2, each behind its own EIM NAT with inbound blocked; one public
      relay R; E2 registers with R; E1 reaches E2 through R;
    - PASS: within 60 s both show a peer with `path=="punched"`, and the relayed peer
      to the other is gone;
    - PASS: E1 mines 10 blocks and E2 syncs them over the punched peer;
    - PASS: `hole_punch.by_family.ipv4.succeeded >= 1`.
  - **`PUNCH_SYMMETRIC=1`** (spec acceptance 4):
    - setup: the same, but E2 is behind `--random`;
    - PASS: no punched peer and `hole_punch.skipped >= 1`;
    - PASS: the relayed peer is still connected after 2 min and blocks still sync.
  - **`PUNCH_V6=1`** (spec acceptance 3):
    - new IPv6 topology: routable test prefixes on a shared WAN bridge, with
      firewall-only `ip6tables` in each node's namespace (`-m conntrack --ctstate NEW`
      drops inbound, established is allowed);
    - same PASS checks as `PUNCH_V4`, but on `by_family.ipv6`.
- [ ] **Step 2: Run them against the Task 6 build** (detached:
  `setsid nohup nice -n 10 bash run.sh`, as in the existing harness on DineroTX). Each
  must fail on a build with the punch trigger neutered, and pass on the real one.
- [ ] **Step 3: Rerun the existing five scenarios** (DEFAULT, DIRECT_QUIC, DIALBACK,
  COMMUNITY_RELAY, TWO_RELAYS). All must still pass.
- [ ] **Step 4: Commit** `test(netns): punch v4, v6 and symmetric fallback scenarios`.

### Task 8: Real-router acceptance (owner present)

**Files:** none (manual, results go in the ledger and the PR).

- [ ] **Step 1: Get the owner's go-ahead.** This needs the Dell behind the AT&T gateway
  and a NAT'd namespace node on DineroTX, both running the Task 6 build. No router
  changes and no fleet changes.
- [ ] **Step 2:**
  - Dell and the DineroTX NAT node connect through a community relay, then punch over
    IPv4;
  - check `getpeerinfo path=="punched"` on both, the relayed peer gone, and blocks
    exchanged;
  - repeat over IPv6 if the AT&T gateway's IPv6 firewall allows outbound-initiated UDP.
- [ ] **Step 3: Record what was observed**, including any failure and its counter, in
  the ledger and the PR body. Failures here are findings, not test edits.

## Departures from spec E (my calls; owner to check)

- **Coordination runs end to end.** The spec says the relay sends each side the other's
  reflexive addresses. Here, the two peers exchange them over the relayed session, so a
  relay can't lie about them, and relays need no update.
- **Replacement, not migration.** The spec says the session migrates to the direct
  path. Here, a new direct session is set up and proven, and then the relayed one is
  dropped.
- **An `unknown` mapping is attempted.** The spec only says to skip
  `endpoint_dependent`.
- **Path types reach the RPC only.** The spec also names the UI. That comes later.
- **A home-peer budget of 2 outbound slots** for relayed and punched peers, outside the
  8 direct ones (Task 4). The spec doesn't address it, but without it a busy home node
  never forms the relayed link that punching starts from.

## Left out on purpose

- **QUIC connection migration** of the relayed session. A fresh direct session replaces
  it, which is simpler and goes through the normal handshake.
- **An explicit circuit-close frame.** Relays idle-close circuits within 5 min, and
  adding one would need every relay to update.
- **Connection reversal.** That's the NAT'd target dialing a reachable origin directly.
  Punching covers it whenever the target's mapping is endpoint-independent.
- **Same-LAN discovery** (two nodes behind one router). Skipped by Review Focus 5.
- **UI** (path type in the Qt peer list). It comes later; RPC first.
