# Reachability Plan 2b — Dial-back Verification, IPv6, NAT Measurement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A node gossips its own address only after other nodes have proven they can
dial it. Nodes listen on, dial and gossip IPv6. Each node measures its NAT mapping
behavior on the P2P UDP port and reports it. That gives the network honest address
data, a second inbound path wherever the router allows IPv6, and the per-node data
Plan 4's punching decisions and Plan 2c's health page need.

**Architecture:** Five parts.
1. **Peer keys and the address manager become IPv6-safe.** Peer keys get brackets, and
   the IPv6 validity, routability and netgroup rules are added.
2. **Sockets become dual-stack**, using separate v4 and v6 listeners for portability.
3. **A new pure-logic `SelfReachability` component** decides which candidate addresses
   are verified. It is driven by a new `reachchk`/`reachres` message pair:
   - The verifier dials the requester's *observed source IP* at the requested port.
   - It completes a transient handshake and reports success only if the dialed node
     proves the requester's `node_id`.
   - It reuses the feeler machinery.
4. **Gossip and reachability use only verified addresses.**
5. **STUN runs on Plan 2a's `QuicEndpoint` socket.** Responses are demultiplexed by
   RFC 7983 first-byte rules, collected from 2 or more servers, and classified.

**Tech Stack:** C++20, existing P2PManager / addrman / StunClient / UdpSocket, the
`QuicEndpoint` from Plan 2a, gtest with `DINERO_TEST_BUILD` hooks
(`p2p_manager.h:796-864`), and bash netns integration on DineroTX.

**Spec:** `docs/superpowers/specs/2026-10-25-zero-touch-p2p-reachability-design.md`,
sections B.2 (IPv6 listen), C (dial-back) and E (NAT-type measurement); acceptance
test 5 ("No node advertises an address that failed dial-back").

**This is plan 2b of 7.** It depends on Plan 2a (`QuicEndpoint`, `NODE_P2P_QUIC`,
`transport` in getpeerinfo). Tasks 1-4 don't need 2a and can start immediately;
Task 5 needs 2a merged.

## Global Constraints

- **New service bit:** `ServiceFlags::NODE_DIALBACK = 1ULL << 31`, meaning "answers
  `reachchk`". Bit 30 is `NODE_P2P_QUIC` (Plan 2a) and bit 29 is
  `NODE_COMPACT_TIMING_V1`.
- **New commands** in `MessageCommands` (`include/network/types.h:22-117`):
  `REACH_CHECK = "reachchk"` and `REACH_RESULT = "reachres"`. Sent only to peers with
  `NODE_DIALBACK` and `identity_proven`.
- **`reachchk` payload:** `nonce` (u64), `port` (u16), `transport` (u8: 0 = tcp,
  1 = quic). **There is no target IP field.** The verifier always dials the
  requester's observed source IP.
- **`reachres` payload:** `nonce` (u64), `result` (u8: 0 ok, 1 refused-rate,
  2 connect-failed, 3 identity-mismatch, 4 unsupported-transport), and `dialed`
  (addrv2 encoding of the ip:port it dialed).
- **Verifier limits:**
  - at most 2 `reachchk` per requesting peer per 10 min;
  - at most 30 per minute globally;
  - at most 4 dial-backs in flight.

  Over a limit it replies `result=1`, and never dials.
- **Verified means:** 2 or more `result=0` from verifiers in distinct netgroups within
  10 min. Netgroups are /16 for IPv4 and /32 for IPv6, as returned by the addrman group
  function after Task 2.
  - A verified address is re-checked every 30 min, and on a local-address or
    network change.
  - Two consecutive failed rounds make it unverified.
  - Regtest only: `p2p.dialback_min_groups` (default 2) may be set to 1 for
    single-host tests.
- **Gossip rule:**
  - learned (peer-voted, STUN or mapping) addresses are gossiped only while verified;
  - operator `externalip=` addresses are always gossiped (operator assertion), but are
    also verified and their state reported.
  - The fleet's SJ/NA/EU1 sit in three distinct /16s (173.249, 172.93, 92.118), so they
    can verify each other.
- **IPv6:**
  - peer key `"[addr]:port"`; per-IP inbound limit applied per /64 (12 per /64);
  - IPv6 bans match `/128`, and also CIDR via `CidrMatches` extended to 128 bits;
  - non-routable v6 (`::1`, `fe80::/10`, `fc00::/7`, `2001:db8::/32`) is never
    advertised or dialed outside regtest; `::ffff:0:0/96` is treated as IPv4.
- **NAT classification values:** `"endpoint_independent"`, `"endpoint_dependent"`,
  `"unknown"`, plus a separate bool `port_preserving`. It needs responses from 2 or
  more STUN servers on different IPs, all sent from the same socket.
- **No consensus changes. Old peers keep working:** they never see `reachchk`, because
  of the service-bit gate.
- Builds with `DINERO_ENABLE_QUIC=OFF`: Task 5 degrades to `nat.mapping="unknown"`,
  and dial-back with `transport=quic` returns `4`.
- **Owner verification rules:** confirm tests ran, neuter-check them, no
  `assert()`-only gates, `touch` and rebuild before reruns.
- **Commits as Claude**, with trailer
  `Co-Authored-By: Claude Opus 5 (1M context) <team@dinerolabs.org>`.
- **Branch:** `feat/reachability-dialback-ipv6`. Tasks 1-4 are cut from
  `feat/reachability-build-gate`; rebase onto `feat/reachability-quic-peer` before
  Task 5. Build and test on the Linux builder (`nice -n 19`) and on DineroTX for netns.

## Review Focus

1. **A malicious peer uses `reachchk` to make us connect somewhere else** (reflection
   or port-scanning a third party). It is impossible by construction, because the
   target IP is always the requester's own socket source. Pin it anyway in Task 4,
   `DialBack.VerifierDialsOnlyRequesterSourceIp`.
2. **A dialed address reaches a different node.** This is the owner's Mac advertising
   an address that actually lands on the Dell. The result must be `3`
   (identity-mismatch), never ok. Pinned in Task 4,
   `DialBack.SharedNatWrongNodeIsIdentityMismatch`.
3. **Every verifier sits in one /16** (a cloud provider, or the owner's house). The
   address must not become verified with only one group. Pinned in Task 3,
   `SelfReachability.SameGroupVerifiersDoNotVerify`.
4. **An IPv6 peer key round-trip through ban, dedup and the per-IP limit**, e.g.
   `[2600:1700::2d]:20999`. It must never be split on the wrong colon. Pinned in
   Task 1, `PeerKey.Ipv6RoundTripsThroughEveryParser`.
5. **STUN responses and QUIC packets on the same UDP socket.** A QUIC packet must never
   be parsed as STUN, nor the reverse. A forged STUN response with an unknown
   transaction ID must be ignored. Pinned in Task 5,
   `StunDemux.QuicAndStunNeverCrossRoute` and `StunDemux.UnknownTxnIgnored`.

---

### Task 1: IPv6-safe peer keys and endpoint parsing

**Files:**
- Modify: `src/daemon/p2p_manager.h`
  - `PeerInfo::to_string()` ~206-211: bracket when the address contains `:`
- Modify: `src/daemon/p2p_manager.cpp`
  - `AddressKey` ~244 (bracket v6)
  - `ParseIPv4HostPort` ~2334: route through the bracket-aware `SplitHostPort` ~367
  - `CidrMatches` ~425: add a v6 path with a prefix up to 128
- Modify: `src/daemon/services/p2p_service.cpp` (`ParseEndpoint` ~148)
- Modify: `src/rpc/methods_network_context.cpp` (`ParseNodeEndpoint` ~38)
- Create: `tests/p2p/test_peer_key_ipv6.cpp`. Register it in `tests/CMakeLists.txt`
  next to `P2PManager_TS1_Integration` (~3567), with the same real-`p2p_manager.cpp`
  link pattern.

**Interfaces:**
- Produces: one canonical formatter and one parser for every endpoint string. Later
  tasks use these:
  - `std::string FormatEndpoint(const std::string& host, uint16_t port)` returns
    `"1.2.3.4:20999"` or `"[2600::1]:20999"`.
  - `bool SplitHostPort(const std::string& s, std::string& host, uint16_t& port)`
    (the existing bracket-aware one at ~367, exported in the header).

- [ ] **Step 1: Write failing tests:**
  - `PeerKey.Ipv6RoundTripsThroughEveryParser` (Review Focus 4): for
    `"[2600:1700:7de1:7170::2d]:20999"` and `"1.2.3.4:20999"`, each of `ParseEndpoint`,
    `ParseNodeEndpoint`, `ParseIPv4HostPort` (v4 only, so it returns false for v6
    without mangling) and `SplitHostPort` yields the original host and port, and
    `FormatEndpoint` reproduces the string.
  - `PeerKey.BanMatchesIpv6ExactAndCidr`: `ban 2600:1700::/32` blocks `[2600:1700::5]:1`,
    and doesn't block `[2601::5]:1`.
  - `PeerKey.PeerInfoToStringBracketsIpv6`.
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the new test plus
  `ctest -R '^(P2PManager_TS1_Integration|RelayVirtualTransport)$'`. Expected: all
  pass, so IPv4 behavior is unchanged.
- [ ] **Step 5: Commit** `p2p: bracketed IPv6 peer keys; one bracket-aware endpoint parser everywhere`

---

### Task 2: addrman and gossip accept IPv6

**Files:**
- Modify: `src/p2p/addrman.cpp`
  - `NetworkAddress::isValid` ~20-38 (accept `inet_pton(AF_INET6)`)
  - `isRoutable` ~41-55 (the v6 rules from Global Constraints)
  - `extractSubnet16` ~612-618: rename the concept to a netgroup, /32 for v6, and keep
    the old name as a wrapper if other callers depend on it
- Modify: `src/daemon/p2p_manager.cpp`
  - `IsAdvertisableAddress` ~330: allow routable v6
  - `handle_addrv2` ~3844-3850: store IPv6 entries instead of counting and dropping
    them; remove the "TODO Phase A2"
- Test: `tests/p2p/test_addrman_ipv6.cpp`, registered beside the existing addrman test.
  Find its target name in `tests/CMakeLists.txt`; reuse that target if it's a simple
  gtest binary.

**Interfaces:**
- Produces: `std::string AddrManNetGroup(const std::string& ip)`, a /16 key for v4
  and a /32 key for v6. Task 3 uses it for verifier diversity.

- [ ] **Step 1: Write failing tests:**
  - `AddrManIpv6.AcceptsRoutableRejectsLocal`: `2600:1700::2d` valid and routable;
    `::1`, `fe80::1`, `fd00::1`, `2001:db8::1` not routable.
  - `AddrManIpv6.MappedV4TreatedAsV4`.
  - `AddrManIpv6.NetGroupIs32ForV6And16ForV4`: `2600:1700:a::1` and `2600:1700:b::1`
    are the same group; `2600:1701::1` is a different one.
  - `AddrV2.Ipv6EntriesAreStoredNotDropped`: feed an addrv2 message carrying one IPV6
    entry via `test_process_message`; addrman then contains it.
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the new tests plus the existing addrman, `PeerDiscoveryG5Verification`
  (~tests/CMakeLists.txt:11543) and TS1 integration tests. Expected: all pass.
- [ ] **Step 5: Commit** `p2p: addrman stores, groups and gossips routable IPv6`

---

### Task 3: `SelfReachability` — the verification state machine (pure logic)

**Files:**
- Create: `include/p2p/self_reachability.h`, `src/p2p/self_reachability.cpp` (no
  sockets, no P2PManager; clock injected)
- Modify: `src/p2p/CMakeLists.txt`
- Create: `tests/p2p/test_self_reachability.cpp`, registered as `SelfReachability`
  (labels `p2p;reachability`)

**Interfaces:**
- Consumes: `AddrManNetGroup` (Task 2).
- Produces:

  ```cpp
  namespace dinero::p2p {
  enum class Transport : uint8_t { Tcp = 0, Quic = 1 };
  enum class CandidateSource { PeerVote, Stun, PortMapping, ExternalIp };
  enum class VerifyState { Unverified, Verifying, Verified };
  struct Candidate { std::string ip; uint16_t port; Transport transport; CandidateSource source; };
  struct CandidateStatus { Candidate c; VerifyState state; uint32_t ok_groups; std::chrono::system_clock::time_point verified_at; };
  struct Verifier { std::string peer_key; std::string ip; };   // connected, identity_proven, NODE_DIALBACK

  class SelfReachability {
  public:
      struct Params { uint32_t min_groups = 2; std::chrono::minutes window{10}, recheck{30}; uint32_t fail_rounds_to_drop = 2; };
      using Clock = std::function<std::chrono::steady_clock::time_point()>;
      SelfReachability(Params, Clock);
      void AddCandidate(const Candidate&);
      void RemoveCandidate(const Candidate&);
      // Which (candidate, verifier) checks to send now; never two verifiers from one group per round.
      std::vector<std::pair<Candidate, Verifier>> PlanChecks(const std::vector<Verifier>& available);
      void OnResult(const Candidate&, const Verifier&, uint8_t result_code);
      void OnNetworkChange();               // all Verified -> Verifying
      bool IsVerified(const Candidate&) const;
      std::vector<CandidateStatus> Status() const;
      bool AnyVerified(Transport) const;
  };
  }
  ```

- [ ] **Step 1: Write failing tests** (fake clock):
  - `SelfReachability.TwoDistinctGroupsVerify`: oks from `1.2.x.x` and `5.6.x.x` give
    Verified.
  - `SelfReachability.SameGroupVerifiersDoNotVerify` (Review Focus 3): two oks from
    `1.2.3.4` and `1.2.9.9` leave it Verifying.
  - `SelfReachability.OkOutsideWindowDoesNotCount`: the second ok arrives 11 min after
    the first.
  - `SelfReachability.RecheckAfter30MinAndDropAfterTwoFailedRounds`.
  - `SelfReachability.PlanChecksNeverPairsTwoVerifiersFromOneGroup`.
  - `SelfReachability.NetworkChangeRequeuesVerified`.
  - `SelfReachability.MinGroupsOneForRegtest`.
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R '^SelfReachability$'`. Expected: 7/7 pass.
- [ ] **Step 5: Commit** `p2p: SelfReachability verification state machine`

---

### Task 4: `reachchk`/`reachres` on the wire, and gossip only verified addresses

**Files:**
- Modify: `include/network/types.h` (commands plus `NODE_DIALBACK`)
- Modify: `src/daemon/p2p_manager.h`
  - `P2PMessage` factories `create_reach_check` / `create_reach_result`, in the
    `create_*` style ~213-320
  - members `std::unique_ptr<p2p::SelfReachability> self_reach_` and a
    `RelayRequestWindow`-style limiter (~1094-1098)
- Modify: `src/daemon/p2p_manager.cpp`:
  - **Dispatch:** new branches in `process_message` ~6625-6673.
  - **Verifier side:** apply the limits; pick the target as the requesting peer's
    socket source IP plus the requested port; dial via `connect_to_peer_impl` in a new
    transient `is_dialback` mode.
    - Separate from the single feeler slot; reuse the feeler's post-handshake cleanup
      at ~5576-5582.
    - Bypass the self-dial filter only for the dial-back target (~4807-4842).
    - Add an outcome callback invoked after `perform_handshake` with `their_node_id`
      and `identity_proven`.
    - Compare against the requester's `their_node_id`, using the pattern at ~928-938.
    - Reply `reachres`.
  - **Requester side:**
    - `record_self_address_report` ~4333 adds a `PeerVote` candidate, instead of
      calling `add_advertised_address(...learned=true)` directly;
    - externalip, port-mapping and STUN feed candidates too;
    - a periodic `PlanChecks` tick runs from `keepalive_loop` (~8022);
    - `reachres` goes to `OnResult`.
  - **Gossip:** `get_local_advertised_addresses` and `collect_advertisable_addresses`
    (~4677, ~5993-5999) return explicit addresses plus learned addresses that
    `self_reach_->IsVerified`.
  - **Reachability:** `IsDirectlyReachable` (`p2p_manager.h:332-336`) gains the
    verified input: directly reachable = verified (any transport) OR port mapping
    active and verified. The RPC and `MaybeAutoRegisterWithRelays` (~3961-3992) use
    the same inbound count (real sockets plus direct-QUIC, no relay-virtual), which
    fixes today's mismatch.
- Modify: `src/daemon/services/p2p_service.cpp` (advertise `NODE_DIALBACK` at
  ~2264-2290; config `p2p.dialback_min_groups`, regtest only)
- Modify: `src/rpc/methods_network_context.cpp`:
  - new `getnetworkinfo.self_reachability: [{address, transport, source, state, ok_groups, verified_at}]`;
  - `localaddresses` lists only advertised (explicit or verified) entries;
  - `direct_inbound_observed` excludes relay-virtual peers.
- Test: `tests/p2p/test_dialback.cpp` (two or three real P2PManagers on loopback, as
  in TS1 integration)

**Interfaces:**
- Consumes: Task 3 `SelfReachability`; Tasks 1-2 IPv6 parsing and netgroup; Plan 2a
  `connect_to_peer_quic` for `transport=quic`, when present.
- Produces: verified-address gossip and the `self_reachability` RPC, read by Plan 2c's
  crawler and Plan 3's relay role decision.

- [ ] **Step 1: Write failing tests** (regtest params, `p2p.dialback_min_groups=1`
  where noted):
  - `DialBack.ReachableNodeBecomesVerifiedAndIsGossiped`: A listens; B connects to A;
    A sends `reachchk(port=A.listen)`; B dials A and gets ok. With min_groups=1, A's
    candidate is Verified and A's addr gossip to a third peer contains it.
  - `DialBack.UnreachablePortIsNeverGossiped` (acceptance test 5): A requests a check
    for a port nothing listens on. The result is `2`, the address is never in gossip,
    and `localaddresses` excludes it.
  - `DialBack.SharedNatWrongNodeIsIdentityMismatch` (Review Focus 2): A requests a
    check for a port where node C (a different identity) listens on the same IP.
    The result is `3`, and the address stays Unverified.
  - `DialBack.VerifierDialsOnlyRequesterSourceIp` (Review Focus 1): use the test hook
    that records the dial target. For a `reachchk` from a peer whose socket source is
    127.0.0.1, the only dial is to 127.0.0.1:port. No payload field can change the IP:
    fuzz the payload with extra trailing bytes.
  - `DialBack.RateLimitRefusesWithoutDialing`: the third request in 10 min gets result
    `1`, with zero dials recorded.
  - `DialBack.OldPeerWithoutBitNeverReceivesReachchk`.
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the new tests plus Tasks 1-3 and
  `P2PManager_TS1_Integration|RelayVirtualTransport|PeerDiscoveryG5Verification`.
  Expected: all pass.
- [ ] **Step 5: Commit** `p2p: dial-back verification; gossip and reachability use verified addresses only`

---

### Task 5: Dual-stack sockets, IPv6 version addresses, and NAT measurement on the P2P UDP port

**Files:**
- Modify: `src/daemon/p2p_manager.cpp`:
  - **Listeners:** `create_listen_socket` ~7026 plus `listen_loop` ~4948 hold two
    listeners, v4 and v6, each with `IPV6_V6ONLY=1` for Windows parity. The v6
    listener is best-effort: log and continue v4-only if `bind` fails.
  - **Accept and `getpeername`** (~4972-4980, ~5373) use `sockaddr_storage` plus
    `inet_ntop`.
  - **`create_client_socket`** ~7135 plus the resolver ~4759 / ~505 resolve through
    `getaddrinfo` (`AF_UNSPEC`) and try each result.
  - **Version message:** `create_version` ~636-654 writes the real peer address (v4,
    or v6 in v6 form) and our `listen_port_`, not the hardcoded 20999.
    `parse_version_payload` ~5679-5689 accepts v6 addrRecv.
- Modify: `include/network/stun_client.h`, `src/network/stun_client.cpp`:
  - make the parser public: `static bool IsStunPacket(const uint8_t*, size_t)` (first
    byte 0x00 or 0x01, magic cookie at offset 4) and
    `std::optional<UdpAddr> ParseBindingResponse(...)`;
  - add a constructor taking an external `SendFn` (no own socket);
  - add `HandleDatagram(src, data)`;
  - keep **all** responses per round, not first-wins (~300).
- Modify: `include/network/quic_endpoint.h` / `.cpp` (Plan 2a). Route datagrams where
  `IsStunPacket` is true to a registered STUN handler *before* QUIC handling. QUIC
  packets always have bit 0x40 set (RFC 9000 fixed bit), so the two never overlap
  (RFC 7983).
- Create: `include/p2p/nat_classifier.h`, `src/p2p/nat_classifier.cpp`:
  `NatMapping Classify(uint16_t local_port, const std::vector<std::pair<UdpAddr /*server*/, UdpAddr /*reflexive*/>>&)`
  returns `{mapping, port_preserving}`.
- Modify: `src/daemon/services/p2p_service.cpp`
  - `StartStunDiscoveryIfEnabled` ~849-910 uses the endpoint socket when it is
    listening, falling back to its own ephemeral socket only when QUIC is off (then
    the result is `unknown`);
  - re-measure every 30 min and on network change;
  - feed the reflexive address as a `Stun`/`Quic` candidate into `self_reach_`.
- Modify: `src/rpc/methods_network_context.cpp`: add
  `getnetworkinfo.nat: {mapping, port_preserving, reflexive: [..], servers: N, measured_at}`
  and keep `stun{}` as is.
- Tests:
  - `tests/network/test_stun_parse.cpp` using the **RFC 5769** test vectors (§2.2 IPv4
    response, §2.3 IPv6 response)
  - `tests/p2p/test_nat_classifier.cpp`
  - `tests/network/test_quic_endpoint.cpp` (new demux cases)
  - `tests/p2p/test_peer_key_ipv6.cpp` (new socket case)

**Interfaces:**
- Consumes: Plan 2a `QuicEndpoint`; Task 3 `SelfReachability`; Task 1 `FormatEndpoint`.
- Produces: `getnetworkinfo.nat`, read by Plan 2c's health self-report and Plan 4's
  punch decision. Also `QuicEndpoint::SetStunHandler(std::function<void(const UdpAddr&, const uint8_t*, size_t)>)`.

- [ ] **Step 1: Write failing tests:**
  - `StunParse.Rfc5769Ipv4AndIpv6Vectors`: decode XOR-MAPPED-ADDRESS to
    `192.0.2.1:32853` and `[2001:db8:1234:5678:11:2233:4455:6677]:32853`.
  - `NatClassifier.SameReflexiveAcrossServersIsIndependent`,
    `NatClassifier.DifferentPortsIsDependent`, `NatClassifier.OneServerIsUnknown`,
    `NatClassifier.PortPreservingDetected`. Use the owner's measured case:
    local 45123 → 162.200.227.214:45123 from three servers is independent and
    preserving.
  - `StunDemux.QuicAndStunNeverCrossRoute` (Review Focus 5): a QUIC Initial goes only
    to QUIC; a STUN response goes only to the STUN handler.
  - `StunDemux.UnknownTxnIgnored`.
  - `PeerKey.Ipv6LoopbackDialHandshake`: two managers handshake over `[::1]`. Skip
    with GTEST_SKIP only if `::1` can't be bound, and log the skip.
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** all tests from Tasks 1-5 plus Plan 2a's `QuicEndpoint|QuicPeer`.
  Expected: all pass.
- [ ] **Step 5: Commit** `p2p: dual-stack listeners/dials, IPv6 version addrs, STUN on the P2P UDP socket with NAT classification`

---

### Task 6: End-to-end on DineroTX netns, plus the owner's real network

**Files:**
- Modify: `tests/integration/test_relay_nat_netns.sh`. Add a `DIALBACK=1` scenario:
  - put the public namespaces in distinct /16s (e.g. `10.11.0.0/16`, `10.22.0.0/16`)
    so `min_groups=2` is exercised for real;
  - the NAT'd node behind MASQUERADE gets peer votes but must **never** list its
    gateway address in `localaddresses` or gossip;
  - the two public nodes verify each other within 5 min;
  - an EIM NAT namespace reports `nat.mapping=endpoint_independent`;
  - an `iptables ... MASQUERADE --random` namespace (endpoint-dependent) reports
    `endpoint_dependent`.
- Modify: `tests/integration/CMakeLists.txt`, if a new test name is needed.

- [ ] **Step 1: Write the scenario.** It fails before Task 4, because the NAT'd node
  advertises its gateway address after 3 votes, which is today's behavior.
- [ ] **Step 2: Run it on DineroTX** (`ssh -i ~/.ssh/dinerotx_key root@64.44.157.100`,
  `DINERO_RUN_NETNS_NAT=1 DIALBACK=1`). Expected: PASS. Record the output.
- [ ] **Step 3: Owner's network, read-only check.** Run a branch `dinerod` on the Dell
  in regtest with peers on DineroTX and SJ (or two regtest nodes on those hosts), to
  observe `getnetworkinfo.nat`.
  - Expected: `mapping=endpoint_independent`, `port_preserving=true`, which matches the
    2026-10-25 manual STUN measurement.
  - Do not touch the router.
- [ ] **Step 4: Neuter check.** Revert the gossip filter (Task 4) in a scratch commit.
  The netns scenario must fail on "NAT'd node gossips gateway address". Then drop
  the scratch commit.
- [ ] **Step 5: Commit** `test: netns dial-back and NAT classification scenarios`

---

## After this plan

- **Plan 2c's crawler** applies the same dial-back rule from the outside. Its
  opt-in, Tor-routed self-reports carry `nat` and the `transport` counts.
- **Plan 3:** a node becomes a community relay only when
  `self_reach_->AnyVerified(...)`.
- **Plan 4** reads both peers' `nat.mapping`, and skips IPv4 punching when either side
  is `endpoint_dependent`.
- **UX status line** ("Your node is reachable — helping the network", etc.): Plan 5,
  driven by `self_reachability` and `relay.active`.
