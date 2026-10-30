# Reachability Plan 2a — Native UDP/QUIC Peer Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two `dinerod` nodes can hold a normal, first-class P2P peer session over QUIC on
UDP 20999 (the same port number as TCP). That means version/verack, dineroid, header
and block sync, inv/getdata and addr, scheduled and accounted for exactly like a TCP
peer. Plan 4's hole punching then reuses this.

**Architecture:**
- **A new `QuicEndpoint`** owns one dual-stack `UdpSocket` bound to the P2P port. It
  routes datagrams by remote address to per-peer `QuicSession`s, accepts new inbound
  sessions only from QUIC Initial packets (rate-limited), and sends each session's
  output to its real remote address.
- **P2PManager** gains a third peer kind next to TCP and relay: a direct-QUIC
  `PeerInfo`.
  - It reuses the relay data plane's stream framing, inbox and reader loop.
  - It plugs in at the two existing choke points, `send_peer_message` and
    `receive_peer_message`, so the handshake and message loop are unchanged.
- **TCP stays the default dial transport.** QUIC is dialed when configured
  (`p2p.quic_connect`), and later by Plan 4.

**Tech Stack:** C++20, vendored ngtcp2 1.22.1 with the OpenSSL 3.5.7 `ossl` crypto
bridge, the existing `QuicSession` / `UdpSocket` / `QuicSessionOptions`, gtest (the QUIC
tests' style), and bash integration tests (regtest dinerods, Linux netns).

**Spec:** `docs/superpowers/specs/2026-10-25-zero-touch-p2p-reachability-design.md`,
section E "Prerequisite: a native UDP/QUIC peer transport", and the anti-goal
"nothing but the P2P port".

**This is plan 2a of 7.** Depends on Plan 1 (release builds carry QUIC). Plans 2b
(NAT measurement on the shared 20999 socket), 3 (relay identity binding) and 4
(punching) build on the `QuicEndpoint` defined here.

## Global Constraints

- **UDP port** = the node's P2P listen port (`listen_port_`, default 20999 mainnet). No
  other new port.
- **New service bit:** `ServiceFlags::NODE_P2P_QUIC = 1ULL << 30` in
  `include/network/types.h`. Bit 29 is `NODE_COMPACT_TIMING_V1`. It is advertised only
  when the endpoint is actually listening.
- **New config keys:**
  - `p2p.quic` (bool, default `true`): bind the UDP endpoint when QUIC is compiled in.
  - `p2p.quic_connect`: comma-separated `ip:port` list to dial over QUIC.
  - CLI aliases `quic` and `quicconnect` go in the alias table in
    `src/daemon/services/config_service.cpp` (~line 35).
- **ALPN** for direct peers is `"dinero-p2p/1"`. Relay circuits keep `"dinero-relay/1"`.
  A peer offering the wrong ALPN fails the handshake.
- **Peer key:** a direct-QUIC peer's `PeerInfo::to_string()` is the same `"ip:port"`
  form as TCP. The remote UDP port is the port. Existing ban, dedup and block-download
  keying keep working. At most one session per remote `ip:port` across both
  transports.
- **Same limits as TCP:**
  - inbound QUIC peers count toward the 125 inbound / 12-per-IP limits
    (`p2p_manager.h:914,918`);
  - outbound QUIC peers count toward the 8 outbound slots;
  - bans apply.
- **Unsolicited-Initial rate limit:** at most 20 new inbound sessions/s globally and
  4/s per source IP. Excess Initials are dropped silently. ngtcp2's built-in 3×
  anti-amplification stays on, so don't disable address validation.
- **No consensus changes. Relay behavior stays byte-for-byte compatible:** existing
  relay tests must still pass unchanged.
- Builds with `DINERO_ENABLE_QUIC=OFF` must still compile. Every new path is behind
  the existing `#if` guards, as `quic_session.cpp:1114` does.
- **Owner verification rules:** confirm each test ran; neuter-check it; no
  `assert()`-only gates; `touch` and rebuild before reruns.
- **Commits as Claude:** `git -c user.name=Claude -c user.email=team@dinerolabs.org commit`
  with trailer `Co-Authored-By: Claude Opus 5 (1M context) <team@dinerolabs.org>`.
- **Where to work:** branch `feat/reachability-quic-peer`, cut from
  `feat/reachability-build-gate`. Build and run the tests on the Linux builder (`ssh
  dinero-dell`, its own worktree, `nice -n 19`: the Dell's mainnet node is syncing) and
  on DineroTX for netns.

## Review Focus

1. **Spoofed or stray datagrams for an existing session from a different source
   address.** They must be dropped and never fed to that session (no session hijack,
   no reflection). Pinned in Task 2, `QuicEndpoint.DropsDatagramFromWrongSourceForSession`.
2. **A flood of fake QUIC Initials from one IP.** It must not create unbounded sessions
   or threads, and legitimate dials from other IPs still succeed. Pinned in Task 2,
   `QuicEndpoint.InitialFloodIsRateLimitedPerIp`.
3. **The same remote dials both TCP and QUIC at once** (or we dial it on both). There
   must be one peer at the end, not a duplicate that breaks block-download
   bookkeeping. Pinned in Task 4, `QuicPeer.DuplicateTransportToSameAddressIsRejected`.
4. **A QUIC peer that disappears without CONNECTION_CLOSE** (laptop sleep, NAT
   timeout). It must be cleaned up by idle timeout (60 s) or the idle reaper, its slot
   freed, and no thread leaked. Pinned in Task 4, `QuicPeer.SilentRemoteIsReapedAndSlotFreed`.
5. **A node built without QUIC or with `p2p.quic=0`.** It must not bind UDP, must not
   advertise `NODE_P2P_QUIC`, and `p2p.quic_connect` entries must be skipped with a
   logged warning, not crash. Pinned in Task 5,
   `QuicPeerConfig.DisabledEndpointDoesNotAdvertiseOrBind`.

---

### Task 1: QuicSession sends CONNECTION_CLOSE and takes an ALPN per role

**Files:**
- Modify: `src/network/quic_session.cpp` (`Impl::Stop` ~216-244, `Close()` ~1268)
- Modify: `include/network/quic_session.h` (doc comment ~106-107 now true)
- Test: `tests/network/test_quic_session.cpp` (uses `tests/network/quic_loopback_pair.h`)

**Interfaces:**
- Produces: `QuicSession::Close()` writes a CONNECTION_CLOSE (application error code
  0, reason `"closed"`) through the session's writer before tearing down the conn.
  `Stats()` (or `last_error()`) on the remote shows a peer-initiated close.
- `QuicSessionOptions::alpn` is honored on both client and server. This is existing
  behavior; pin it with a test.

- [ ] **Step 1: Write failing tests** in `tests/network/test_quic_session.cpp`:
  - `QuicSession.CloseNotifiesPeerPromptly`: complete a loopback handshake, call
    `a.Close()`, and assert that within 2 s `b` reports closed. Use whatever
    `Stats()`/`last_error()` exposes; add a `bool peer_closed()` accessor if nothing
    does. Today `b` only notices after the 60 s idle timeout.
  - `QuicSession.AlpnMismatchFailsHandshake`: client `alpn="dinero-p2p/1"`, server
    `alpn="dinero-relay/1"`; the handshake future resolves to failure within 5 s.
- [ ] **Step 2: Run them and see them fail** (Close test times out; ALPN test may
  already pass, which you record).
  Run: `ctest --test-dir <build> -R '^QuicSession$' --output-on-failure`
- [ ] **Step 3: Implement.** In the close path on the session thread, call
  `ngtcp2_conn_write_connection_close` (application error via
  `ngtcp2_ccerr_set_application_error`) into a buffer and send it through the writer,
  then delete the conn. In the receive path, treat `NGTCP2_ERR_DRAINING` /
  `NGTCP2_ERR_CLOSING` from `ngtcp2_conn_read_pkt` as peer-closed.
- [ ] **Step 4: Run** `ctest -R '^(QuicSession|QuicSessionStress|RelayVirtualTransport)$'`.
  Expected: all pass, and the relay tests are unchanged.
- [ ] **Step 5: Commit** `quic: send CONNECTION_CLOSE on Close() so peers learn of shutdown immediately`

---

### Task 2: `QuicEndpoint` — one UDP socket, many sessions

**Files:**
- Create: `include/network/quic_endpoint.h`, `src/network/quic_endpoint.cpp`
- Modify: `src/p2p/CMakeLists.txt` (~22-27, next to the other QUIC sources, under the
  same QUIC guard)
- Create: `tests/network/test_quic_endpoint.cpp`
- Modify: `tests/CMakeLists.txt` (register `QuicEndpoint` next to `QuicSession` ~3859,
  labels `p2p;quic;ngtcp2`)

**Interfaces:**
- Consumes: `UdpSocket` (`Bind`, `OnReceive`, `SendTo`, `Stop`), `UdpAddr`, `QuicSession`
  (`StartClient`, `StartServer`, `EnqueueIncomingPacket`), `QuicSessionOptions`.
- Produces:

  ```cpp
  namespace dinero::network {
  class QuicEndpoint {
  public:
      struct Limits { uint32_t initials_per_sec_global = 20; uint32_t initials_per_sec_per_ip = 4; };
      // Return false to refuse (bans, inbound caps). Called on the socket thread; must not block.
      using AcceptFn  = std::function<bool(const UdpAddr& remote)>;
      // Ownership of the new inbound session passes to the callee.
      using InboundFn = std::function<void(std::shared_ptr<QuicSession>, const UdpAddr& remote)>;

      bool Start(uint16_t port, QuicSessionOptions server_opts, AcceptFn, InboundFn, Limits = {});
      std::shared_ptr<QuicSession> Connect(const UdpAddr& remote, QuicSessionOptions client_opts);
      void Forget(const UdpAddr& remote);   // after the owner tears a session down
      void Stop();
      bool Listening() const;
      uint16_t Port() const;
      size_t SessionCount() const;
  };
  }
  ```

- Behavior:
  - Sessions are keyed by remote `UdpAddr` (family + ip + port).
  - A datagram from a known remote goes to that session.
  - A datagram from an unknown remote is accepted only if it parses as a QUIC
    long-header Initial (`ngtcp2_pkt_decode_version_cid` succeeds and the packet type
    is Initial), passes the rate limit and `AcceptFn`. It becomes a server session
    (`StartServer`, then the datagram is enqueued) and is handed to `InboundFn`.
  - Anything else is dropped and counted.
  - Each session's writer is `SendTo(remote, …)`.

- [ ] **Step 1: Write failing tests** in `tests/network/test_quic_endpoint.cpp`. Two
  endpoints on `127.0.0.1` with ports from bind(0). Use `GenerateRelayTlsKeypair` for
  certs, as `test_relay_tls_keypair.cpp:80` does.
  - `QuicEndpoint.ClientAndServerCompleteHandshakeAndExchangeStream`: `Connect`; server
    `InboundFn` fires once; both handshakes succeed within 5 s; a 64 KiB payload sent
    with `EnqueueOutgoingStream` arrives intact via `ReadDecryptedStream` on the other
    side.
  - `QuicEndpoint.DropsDatagramFromWrongSourceForSession` (Review Focus 1): after a
    session exists for A, send random bytes and a replayed packet to the server from a
    third socket. The session's received-bytes stat is unchanged, and no new session
    is created.
  - `QuicEndpoint.NonInitialFromUnknownSourceCreatesNoSession`: a short-header packet
    from a new port gives `SessionCount()` unchanged.
  - `QuicEndpoint.InitialFloodIsRateLimitedPerIp` (Review Focus 2): fire 200 crafted
    Initials from one socket within 1 s. `InboundFn` fires at most 4 times (+1
    tolerance) in that second, and a real `Connect` from a second socket still
    succeeds.
  - `QuicEndpoint.AcceptFnRefusalCreatesNoSession`: `AcceptFn` returns false, so
    `InboundFn` never fires and the client's handshake fails within 5 s.
- [ ] **Step 2: Run** `ctest -R '^QuicEndpoint$'`. Expected: fails to build (no header).
- [ ] **Step 3: Implement** `quic_endpoint.{h,cpp}`. A token bucket per IP and a global
  one; the map is guarded by a mutex; `Stop()` stops the socket, then `Close()`s every
  session.
- [ ] **Step 4: Run** `ctest -R '^QuicEndpoint$'`. Expected: 5/5 pass. Then confirm the
  `DINERO_ENABLE_QUIC=OFF` configure still builds `dinerod` (the endpoint compiles to a
  stub whose `Start` returns false).
- [ ] **Step 5: Commit** `quic: QuicEndpoint demultiplexes one UDP socket into per-peer sessions`

---

### Task 3: Direct-QUIC peers in P2PManager (data plane)

**Files:**
- Modify: `src/daemon/p2p_manager.h`
  - `PeerInfo` ~44-211: add `bool quic_direct = false;` and `UdpAddr quic_remote;`
  - add `bool uses_quic_stream() const { return quic_direct || (via_relay && via_relay->encrypted_quic); }`
    (use the relay struct's actual member name)
- Modify: `src/daemon/p2p_manager.cpp`. Every place that branches on the relay
  encrypted-QUIC condition must use `uses_quic_stream()`:
  - `send_peer_message` ~3397
  - `receive_peer_message` ~3424-3479
  - `send_to_peer` ~6851
  - `outbox_loop` ~7493
  - the QUIC-ready wait and reader spawn in `peer_handler_loop` ~5526-5557
  - `cleanup_peer` ~6172

  The relay-specific send (`send_relay_data_to_virtual_peer`, ~6936) becomes "frame
  with `FrameRelayQuicStreamPayload`, then `EnqueueOutgoingStream`" for both kinds.
  Only relay peers keep the RELAY_DATA wrapping in their session writer.
- Test: `tests/p2p/test_relay_virtual_transport.cpp` (unchanged; must still pass).
  New: `tests/p2p/test_quic_direct_peer.cpp`, registered next to
  `test_relay_virtual_transport` (~3713-3781), same `DINERO_TEST_BUILD` linking pattern.

**Interfaces:**
- Consumes: Task 2's `QuicSession` handed in by `QuicEndpoint`.
- Produces, for test and later-task use under `DINERO_TEST_BUILD`, following the
  existing `test_perform_handshake` style:
  - `P2PManager::install_direct_quic_peer(std::shared_ptr<QuicSession>, const UdpAddr& remote, bool outbound) -> std::string peer_key`
    creates the `PeerInfo` (`socket_fd=-1`, `quic_direct=true`, key `ip:port`), inserts
    it and starts `peer_handler_loop`.

- [ ] **Step 1: Write a failing test** `QuicDirectPeer.TwoManagersHandshakeOverQuicPair`.
  - Two `P2PManager` instances in regtest params with no listen socket.
  - Wire two `QuicSession`s together via `quic_loopback_pair.h` with
    `alpn="dinero-p2p/1"`.
  - `install_direct_quic_peer` on each, one side outbound.
  - Assert within 10 s that both report the peer `is_connected` with `identity_proven`
    true and `their_node_id` equal to the other manager's node id.
  - Send a `ping`/`pong` round trip through `send_to_peer`.
- [ ] **Step 2: Run it and see it fail** (no `install_direct_quic_peer`).
- [ ] **Step 3: Implement** `uses_quic_stream()`, the branch generalization and
  `install_direct_quic_peer`.
- [ ] **Step 4: Run** `ctest -R '^(QuicDirectPeer|RelayVirtualTransport|P2PManagerTs1Integration)$'`.
  Expected: all pass. Use the real CTest names from `tests/CMakeLists.txt`.
- [ ] **Step 5: Commit** `p2p: direct-QUIC peers share the relay QUIC data plane via uses_quic_stream()`

---

### Task 4: Dial, accept, limits, dedup and cleanup

**Files:**
- Modify: `src/daemon/p2p_manager.{h,cpp}`
  - member `std::unique_ptr<network::QuicEndpoint> quic_endpoint_`
  - start it in `start()` (~4366-4377) on `listen_port_` with server opts
    `alpn="dinero-p2p/1"` and `relay_tls_options_`'s cert/key (generated at ~4343-4364)
  - `AcceptFn` = ban check (`is_peer_banned`), the 125 inbound / 12-per-IP limits from
    `handle_incoming_connection` (~5323-5347), and the dedup rule below
  - `InboundFn` calls `install_direct_quic_peer(..., outbound=false)`
  - new `bool connect_to_peer_quic(const std::string& ip, uint16_t port)` mirrors
    `connect_to_peer_impl`'s self-loop, ban and dedup checks (~4784-4890), then
    `quic_endpoint_->Connect`, then `install_direct_quic_peer(..., true)`
  - `cleanup_peer` calls `quic_endpoint_->Forget(remote)` for direct-QUIC peers
  - outbound QUIC peers count toward `MAX_OUTBOUND_CONNECTIONS` in the
    `connection_manager_loop` count (~5024-5030)
- Test: `tests/p2p/test_quic_direct_peer.cpp`

**Interfaces:**
- Consumes: Task 2 `QuicEndpoint`; Task 3 `install_direct_quic_peer`.
- Produces: `connect_to_peer_quic(ip, port) -> bool`; `quic_endpoint_` for Tasks 5 and 6.

- [ ] **Step 1: Write failing tests** (two real managers, each with its endpoint on a
  loopback ephemeral port):
  - `QuicPeer.DialOverRealUdpReachesConnected`: A `connect_to_peer_quic("127.0.0.1",
    B.port)`; both sides connected and identity-proven within 10 s; B counts it
    inbound, A outbound.
  - `QuicPeer.DuplicateTransportToSameAddressIsRejected` (Review Focus 3): with a TCP
    peer to B already connected, `connect_to_peer_quic` to the same `ip:port` returns
    false. Conversely, B's `AcceptFn` refuses a QUIC Initial from an address that
    already has a connected peer. Exactly one peer remains.
  - `QuicPeer.InboundLimitAppliesToQuic`: set B's inbound cap to 1 via the existing
    test hook or constructor param. A second QUIC dial from another socket is refused.
  - `QuicPeer.SilentRemoteIsReapedAndSlotFreed` (Review Focus 4): after connect, `Stop()`
    A's endpoint without Close, which simulates the vanish. Within 70 s, B no longer
    lists the peer, `B.quic_endpoint_->SessionCount()==0`, and no handler thread
    remains for it (use the existing thread-count or handler-registry accessor used by
    `test_p2p_idle_peer_reap`).
  - `QuicPeer.BannedIpCannotConnectOverQuic`.
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement** as listed in Files.
- [ ] **Step 4: Run** `ctest -R '^(QuicPeer|QuicDirectPeer|QuicEndpoint|RelayVirtualTransport|P2PIdlePeerReap)$'`.
  Expected: all pass.
- [ ] **Step 5: Commit** `p2p: dial and accept direct QUIC peers on the P2P port with TCP-equal limits`

---

### Task 5: Config, service bit and visibility

**Files:**
- Modify: `include/network/types.h` (add `NODE_P2P_QUIC = 1ULL << 30`, with a comment in
  the style of the 26-28 block)
- Modify: `src/daemon/services/config_service.cpp` (aliases `quic` to `p2p.quic`, and
  `quicconnect` to `p2p.quic_connect`)
- Modify: `src/daemon/services/p2p_service.cpp`
  - read `p2p.quic` (default true) and `p2p.quic_connect` (comma-split like `p2p.connect`
    at ~1721-1731)
  - dial those at startup and keep them as persistent targets, as with `p2p.connect`
  - OR `NODE_P2P_QUIC` into our services at ~2264-2290 only if `quic_endpoint_->Listening()`
- Modify: `include/rpc/peer_json_utils.h` (`BuildPeerInfoJson` ~23-68: add
  `"transport": "tcp" | "relay" | "quic"`)
- Modify: `src/rpc/methods_network_context.cpp` (`quic_transport` ~319-333: add
  `"listening"` (bool), `"listen_port"`, `"sessions"`)
- Test: `tests/p2p/test_quic_direct_peer.cpp` and the existing peer-json test, if one
  covers `BuildPeerInfoJson`; otherwise add `tests/rpc/test_peer_json_transport.cpp`.

**Interfaces:**
- Consumes: Task 4 `quic_endpoint_`, `connect_to_peer_quic`.
- Produces: `getpeerinfo[].transport` and
  `getnetworkinfo.quic_transport.{listening,listen_port,sessions}`, which Task 6 and
  later plans read.

- [ ] **Step 1: Write failing tests:**
  - `PeerJson.TransportFieldReflectsPeerKind`: TCP gives `"tcp"`, relay gives
    `"relay"`, direct QUIC gives `"quic"`.
  - `QuicPeerConfig.DisabledEndpointDoesNotAdvertiseOrBind` (Review Focus 5): with
    `p2p.quic=false`, the local services lack `NODE_P2P_QUIC`, nothing is bound on UDP
    `listen_port` (bind a UDP socket to it in the test and expect success), and a
    `p2p.quic_connect` entry logs a warning and makes no session.
  - `QuicPeerConfig.EnabledEndpointAdvertisesServiceBit`.
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the Task 4 command plus `-R 'PeerJson|QuicPeerConfig'`. Expected:
  all pass.
- [ ] **Step 5: Commit** `p2p: p2p.quic / p2p.quic_connect, NODE_P2P_QUIC service bit, transport in getpeerinfo`

---

### Task 6: End-to-end — real daemons sync over QUIC only, then through a NAT

**Files:**
- Create: `tests/integration/test_direct_quic_sync.sh`. Follow
  `tests/integration/test_relay_nat_netns.sh`'s style for launching regtest
  `dinerod`s and mining.
- Modify: `tests/integration/CMakeLists.txt`. Register `DirectQuicSync` next to the netns
  test (~880-898), labels `integration;quic`.
- Modify: `tests/integration/test_relay_nat_netns.sh`. Add a `DIRECT_QUIC=1` scenario:
  the NAT'd node dials the public node with `--quicconnect`.

**Interfaces:**
- Consumes: Task 5 config keys and RPC fields.

- [ ] **Step 1: Write `test_direct_quic_sync.sh`.** It fails today because
  `--quicconnect` is unknown.
  - Start A (regtest, `--listen=1`, `--quic=1`).
  - Start B with `--listen=0 --quicconnect=127.0.0.1:<A port>` and no TCP connect, so
    B's only possible peer is QUIC.
  - Mine 20 blocks on A.
  - Assert:
    - B reaches height 20 within 60 s;
    - B's `getpeerinfo` has exactly one peer with `"transport":"quic"`;
    - A's `getpeerinfo` shows it inbound with `"transport":"quic"`;
    - A's `getnetworkinfo.quic_transport.listening == true`.
  - Exit non-zero on any failure; no `assert`-only checks.
- [ ] **Step 2: Run it** (`ctest -R DirectQuicSync`) and see it fail before Tasks 1-5
  land. Check out Task 5's parent commit to show it, or record the failure from before
  Task 5.
- [ ] **Step 3: Run it on the branch head.** Expected: PASS. Confirm in the ctest log
  that it ran (not skipped).
- [ ] **Step 4: Netns NAT scenario on DineroTX** (`ssh -i ~/.ssh/dinerotx_key
  root@64.44.157.100`, root needed, `DINERO_RUN_NETNS_NAT=1 DIRECT_QUIC=1`).
  - The NAT'd namespace node dials the public-namespace node over QUIC through
    MASQUERADE.
  - TCP inbound to the NAT'd node stays blocked (existing check).
  - Expected: the NAT'd node syncs over a `"transport":"quic"` peer.
  - Record the output in the report. This proves outbound QUIC works through a real
    NAT.
- [ ] **Step 5: Neuter check.** Rerun `DirectQuicSync` with `--quic=0` on A; B must
  stay at height 0 and the script must fail. Then restore.
- [ ] **Step 6: Commit** `test: regtest daemons sync over direct QUIC only, and through a netns NAT`

---

## After this plan

- **Plan 2b** moves STUN onto `QuicEndpoint`'s socket. Today STUN binds an ephemeral
  port (`stun_client.cpp:141-146`), so its reflexive address is not 20999's. NAT
  mapping type then gets measured per peer pair.
- **Plan 3** binds QUIC TLS to dineroid. Today `ExchangeDineroId` signs only the version
  nonce (`p2p_manager.cpp:3691-3762`), with no exporter/cert binding. Plan 3 must also
  fix the reported outbound relay check: `their_node_id` is overwritten without
  comparison to the expected target (`p2p_manager.cpp:1797, 3753`).
- **Plan 4** uses `QuicEndpoint::Connect` for punching (simultaneous Initials from both
  sides, coordinated by a relay).
- **Known limitation, unchanged here:** `QuicSession` is compiled only for the OSSL
  crypto backend (`quic_session.cpp:21,45`). A quictls build reports
  `crypto_available=true` but gets a stub. All release lanes use OpenSSL 3.5.7 (ossl),
  so this doesn't affect shipped builds.
