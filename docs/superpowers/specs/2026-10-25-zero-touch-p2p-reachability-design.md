# Zero-touch P2P reachability and direct home-to-home connections

Date: 2026-10-25
Status: draft for owner review

## Intent

Dinero is a young network. Today it is effectively three fleet servers (SJ
173.249.200.59, NA 172.93.167.32, EU1 92.118.190.62) with home wallets dialing
out to them. If the three servers go down or get overloaded, the network goes
with them, and home miners get stuck or lose block races (the Dell sat on a dead
fork for 18 days in October 2026).

The owner wants **real decentralization**. An ordinary Mac or Windows user installs
`dinero-qt` at home, configures nothing, and their node:

- connects out **and** accepts connections in;
- finds and talks to **other home nodes**, not just the fleet;
- where the network allows it, talks to other home nodes **directly**, with no
  server in the middle. The owner called this "real achievement". It is a
  headline goal, not an optimization.

This has to work on routers like the owner's own (AT&T gateway: refuses UPnP and
NAT-PMP, firewalls inbound IPv6), not only on friendly ones.

## Current state (measured 2026-10-25)

- **The network is tiny.** Besides the fleet, SJ/NA/EU1 see the owner's own
  machines (home NAT 162.200.227.214: Mac 8.1.13, Dell, a second v8.1.12 node,
  probably Windows), the owner's GreenCloud Windows VPS 173.249.208.101 (stuck at
  height 48365), one outside node on an old build (192.99.34.91, `e5f9a4dd`, does
  not serve blocks) and a crawler (5.196.74.147, `dinerorelay:0.3.1`, owner unknown).
  SJ saw 9 distinct inbound IPs in 7 days.
- **Port mapping exists but silently disappears from builds.**
  - Plain `dinerod` defaults `--portmap` off (`src/daemon/services/p2p_service.cpp`
    ~1332).
  - `dinero-qt` passes `--portmap=auto` by default (`qt/src/mainwindow.cpp` ~13473)
    and pins `-addnode` to the three fleet IPs (~13479).
  - CMake `DINERO_ENABLE_PORTMAPPING` defaults ON, but a missing miniupnpc or
    libnatpmp is only a STATUS message (`cmake/PortMapping.cmake`).

  | Artifact | UPnP / NAT-PMP / QUIC compiled in |
  |---|---|
  | Released v8.1.12 Mac app | yes |
  | "8.1.13" app on the owner's Mac | **none of the three**. A local dev build from branch `claude/qt-i18n` (`6f4cb1258`, 2026-09-25), not a release-lane artifact. The macOS release lane already refuses builds without them (`packaging/mac/assert-v8-release-lane.sh`). |
  | Linux `.deb` / server (`linux-release.yml:136`) | port mapping OFF on purpose; **QUIC not enabled** (no `-DDINERO_ENABLE_QUIC=ON`, and the default is OFF, `cmake/BuildOptions.cmake:19`) |
  | Linux desktop (`desktop-release.yml:220`) | port mapping OFF, QUIC ON |
  | Windows (`windows-release.yml`, `build-server-installer.ps1`) | QUIC ON; port mapping probably absent (depends/ never built in the lane); unconfirmed |
  | Live fleet SJ/NA/EU1 (hand-built `272e01fa`) and the Dell (8.1.12 `.deb`) | fleet: UPnP/NAT-PMP yes, **QUIC no**; Dell: none. **No node has `relay.active`; the encrypted relay data plane is down network-wide.** |

  The Linux OFF is deliberate only because the libs once leaked in as unexpected
  shared-library dependencies (`linux-release.yml:10-16`). Static vendoring through
  `depends/` (as on macOS) satisfies that allow-list.
- **The owner's router refuses automatic opening.** v8.1.12 tested on the owner's
  network: NAT-PMP fails (-7, no reply), and UPnP is discovered but `AddPortMapping`
  fails ("Miniupnpc HTTP error"). The existing 20999 → Dell forward was set by hand.
- **Addresses are advertised without verification.** "Self-address learned from
  peers" (`P2PManager::record_self_address_report`, `p2p_manager.cpp` ~4231-4265)
  advertises after 3 distinct reporters, with no dial-back. The owner's Mac
  advertises 162.200.227.214:20999, which actually lands on the Dell.
- **The relay fallback exists but is centralized.**
  - `MainnetRelayReady()` returns true.
  - NAT'd nodes register (RELAYREG) with the hardcoded `kMainnetRelayPeers` (the
    three fleet IPs, `p2p_manager.cpp` ~3850) plus connected relay peers.
  - The relay role on home nodes is on only while mining (`p2p.relay=auto`,
    `docs/network-participation.md`).
  - RELAY_HINTS go only to a NAT'd node's direct peers.
  - Observed: the Dell's relay dials to the Mac (node_id `7053985632…`) via SJ/NA
    time out repeatedly.
- **IPv6.** Owner's Mac and Dell have global IPv6 (2600:1700:7de1:7170::/64). The
  router's IPv6 firewall drops unsolicited inbound (even SSH to the Dell). `dinerod`
  listens IPv4 only.
- **Tor works.** dinero-qt runs an embedded Tor onion service with no setup; the Mac
  has an active `.onion`.
- **STUN works, and the owner's NAT is punch-friendly.** Measured 2026-10-25: one UDP
  socket queried stun.l.google.com, stun1.l.google.com and stun.cloudflare.com. From
  both the Mac and the Dell, all three saw the **same** external ip:port, and the
  port equaled the local port (45123 → 162.200.227.214:45123). So the AT&T gateway
  is endpoint-independent and port-preserving, the best case for IPv4 punching. The
  node's reported 56713 is just its STUN client binding an ephemeral port.
- **No direct UDP/QUIC peer transport exists.** `UdpSocket` exists, but QUIC runs only
  inside relay circuits, fed by synthetic per-circuit addresses
  (`RelayQuicAddress(circuit_id, …)`, `p2p_manager.cpp` ~235, ~1398-1420). The relay
  itself rides the TCP peer connection. Ordinary peer connections are TCP only.
- **The UI already shows a status.** Qt shows DIRECT / BEHIND_RELAY / UNREACHABLE
  (`qt/src/identitysection.cpp`) and a "Reachable" row with the hint "Open port
  20999 inbound" (`qt/src/contributionsection.cpp`). `getnetworkinfo` exposes
  `reachable`, `direct_reachable`, `port_mapping.*`, `relay.*`, `stun.*`,
  `quic_transport.*`.

## Goals

1. **Every shipped build has the full reachability stack**, enforced by release CI.
2. **Zero-touch inbound reachability** for home nodes, on any router.
3. **Direct home-to-home connections** wherever the two NATs allow it, over IPv6 and
   IPv4, with a relay acting only as introducer.
4. **Relays come from the community**, not a hardcoded fleet list. Any directly
   reachable node can relay within a bandwidth cap.
5. **New users meet community nodes first**; the fleet becomes a fallback.
6. **Decentralization is measurable over time** on a public network-health page.
7. **Old, unhelpful builds stop clogging the network.**

## Non-goals

- No consensus changes. Everything here is P2P and transport.
- No changes to block validation, fork choice or the download scheduler beyond
  peer-selection preferences (item 7).
- Not replacing Tor. Tor stays as the last-resort inbound path and a privacy option.
- Not 100% direct connectivity. Carrier-grade NAT pairs keep using relays.

## Anti-goals (failure even if it "works")

- The user has to configure anything: router settings, ports or flags.
- A home user's upload is used beyond the cap, or relaying degrades their own sync
  or mining.
- Nodes advertise addresses nobody can dial (today's behavior).
- The fleet stays a single point of failure for reachability, for example if relays
  still resolve only to SJ/NA/EU1.
- The feature silently vanishes from a build again (the dev-build and Linux-lane
  failure mode).
- Relays can read or alter relayed traffic. Relayed P2P must stay end-to-end
  encrypted, as the existing QUIC relay data plane already is.
- RPC or anything other than the P2P port gets exposed by any of this.

## Constraints

- **Release train (owner decision): all of it ships in v8.2.0, and the 150,000
  activation height stays.** The owner chose not to split it, because the network is
  young and nearly every node today is the owner's or the fleet's.
  - **Hard deadline.** Measured 2026-10-25: about 2-3 min per block, and height
    139,041 at 21:28 UTC. So 150,000 lands around **Nov 9-17, 2026**. v8.2.0 must be
    released and installed on every known node (fleet, owner's machines, the outside
    node operators the owner can reach) before then.
  - The builder should plan backwards from that date, leaving time for
    re-qualification of the combined candidate, and schedule the riskiest pieces
    (UDP/QUIC transport, hole punching, relay identity binding) first.
  - This work is non-consensus. It must not change any consensus behavior already
    qualified for v8.2.0, and the consensus qualification evidence must be re-run on
    the final combined candidate.
  - *Risk the owner accepted:* if the transport/punching work isn't solid by release
    freeze, the release slips toward the activation height. Raise any slip with the
    owner immediately; don't silently cut scope.
- **Linux dependency allow-list.** Linux server builds must keep the declared
  dynamic-dependency contract, so miniupnpc, libnatpmp and ngtcp2 must be linked
  statically.
- **Old peers.** Peers that don't speak the new messages must keep working.
  Capability flags gate new behavior. Existing service bits: `NODE_RELAY = 1 << 26`,
  `NODE_DINERO_V2 = 1 << 27`, `NODE_BEHIND_RELAY = 1 << 28`.
- **Existing building blocks to reuse, not re-invent:**
  - `PortMapper`;
  - the STUN client (RFC 5389, existing DineroLabs plus public fallback servers);
  - the QUIC transport (vendored ngtcp2), the relay registry and RELAY_* messages;
  - `OrchestrateRelayDials`, AddrMan, addrv2;
  - the `dynamic_p2p` governor;
  - the netns NAT harness (`test_relay_nat_netns.sh`, currently regtest-only).

## Design

### A. Build-integrity gate (owner decision: implement)

- Release CI for **every** artifact (macOS arm64 and Intel, Windows, Linux desktop,
  Linux server/.deb, Docker) launches the built `dinerod` in regtest and asserts
  `getnetworkinfo` reports:
  - `port_mapping.upnp_compiled = true`
  - `port_mapping.natpmp_compiled = true`
  - `quic_transport.ngtcp2_available = true`

  Any false fails the release.
- `cmake/PortMapping.cmake` turns a missing lib into a hard error when
  `DINERO_ENABLE_PORTMAPPING=ON`, or when a new `DINERO_RELEASE_BUILD` flag is set.
  It must not quietly omit the lib.
- Linux and Windows build miniupnpc, libnatpmp and ngtcp2 statically through
  `depends/`, matching the macOS lane. The Linux allow-list stays unchanged.

### B. Connection ladder (zero-touch)

On start, and on network change, a node tries in order and keeps every path that
works:

1. **Router mapping:** UPnP IGD (IPv4 port map and IPv6 pinhole via
   WANIPv6FirewallControl), NAT-PMP, and **PCP** (RFC 6887), which is new.
2. **Native IPv6:** `dinerod` listens on `[::]:20999` as well as IPv4.
3. **Verified advertisement** (section C) before any address is gossiped.
4. **Relay registration** with community relays (section D) when no direct path is
   verified.
5. **Direct connection upgrade** via relay-introduced hole punching (section E) for
   home-to-home peers.
6. **Tor onion** stays as the existing last resort.

The plain `dinerod` default for `--portmap` becomes `auto` on desktop and server
builds. *Server default: my call.* Operators can still set it to `0`.

### C. Dial-back verification

- A node asks 2 or more peers on different /16s (prefer different ASNs when known)
  to dial a candidate address (IPv4 or IPv6, reported by peers, STUN or a mapping)
  and complete a version handshake proving it reached **this** node (match
  `node_id` / dineroid).
- The address is advertised only after a successful dial-back, and stays advertised
  only while periodic re-checks pass (suggested every 30 min, and on network change).
- It replaces "3 reporters ⇒ advertise". Reporter votes become candidates only.
- A new message pair (e.g. `reachcheck` / `reachresult`), gated by a capability
  bit, with rate limits so it can't be used to make nodes dial arbitrary targets.
  The target must be the requester's own observed source IP.

### D. Community relays

- A node becomes a relay automatically when it is **directly reachable (verified)**,
  regardless of mining. This changes the `p2p.relay=auto` rule. It advertises
  `NODE_RELAY` through addr gossip.
- **Default cap (my call; owner said "acceptable cap"):**
  - at most 8 relayed circuits;
  - at most 200 KB/s sustained relayed traffic;
  - at most 20 GB/month;
  - relaying pauses automatically while the node is syncing or under load.

  Settings shows a toggle and the usage. A builder may tune the numbers but must
  keep a hard cap.
- NAT'd nodes keep relay registrations on **2 relays from different /16s
  (different ASNs where known)**, preferring non-fleet relays. Fleet relays stay as
  a fallback list.
- RELAY_HINTS for a NAT'd node must reach beyond its direct peers, for example
  advertised as addrv2 entries with a "via relay X" hint. That way any node can dial
  any NAT'd node. *Mechanism left to builder.*
- Fix the observed failure first: relay dials to the Mac time out via SJ/NA. Root
  cause not yet investigated (the 8.1.13 Mac lacks QUIC, which may be all of it).
- **"Fleet" vs "community" (my call):** fleet = the hardcoded `kMainnetRelayPeers` plus
  the fixed-seed IPs in `chainparamsseeds.h`. Every other relay is community.
  "Prefer non-fleet" means pick from community relays first, fleet only as fallback.
- **Relay security is required, not just today's level (my call).**
  - Today the relay QUIC uses `verify_peer=false` with an ephemeral self-signed cert,
    and relies on the dineroid identity check inside the stream
    (`p2p_manager.h` ~1014-1024).
  - Once untrusted home nodes relay, a relay could terminate both TLS legs and
    forward the identity exchange.
  - Stage 3 must **bind each relayed QUIC session to the endpoints' node identity
    keys**, for example by signing the TLS exporter / handshake transcript with the
    dineroid key, or by deriving and pinning the cert from `node_id`.
  - Add a test with a malicious relay that tries to man-in-the-middle the session and
    must be detected.

### E. Direct connections: relay-introduced hole punching (owner: headline goal)

- **Prerequisite: a native UDP/QUIC peer transport.** This is new work and the
  largest single item. It needs:
  - a UDP socket on the P2P port (20999/udp, alongside 20999/tcp), shared by STUN,
    punching and QUIC sessions;
  - a QUIC-carried peer session that the rest of the stack treats exactly like a TCP
    peer (handshake, header and block sync, relay, addr, download scheduling).

  A punched connection is a **first-class sync peer**, not a side channel. *My call:*
  UDP on the P2P port number counts as "the P2P port" for the anti-goal below.
  Plain direct QUIC dialing (no punching) between reachable nodes comes for free and
  is part of the acceptance tests.
- When two nodes are connected through a relay, or one wants to dial a relayed
  node, the relay introduces them:
  - it sends each one the other's reflexive addresses (STUN-observed IPv4 ip:port
    plus IPv6 addresses) and a start time;
  - both send QUIC Initial/UDP probes at the same moment;
  - on success, the P2P session migrates to the direct path and the relay circuit
    closes.
- IPv6 first: no address translation, so only the stateful firewall has to be
  crossed, and simultaneous outbound UDP opens it on most home routers.
- IPv4: works when both NATs use endpoint-independent mapping. Each node measures
  its mapping behavior (compare reflexive addresses from two STUN servers) and
  reports it. Pairs where either side has endpoint-dependent (symmetric) mapping
  skip punching and stay relayed. *Port prediction left to builder; not required.*
- TCP simultaneous-open is out of scope. Punching is UDP/QUIC only.
- Status: `getnetworkinfo` and the UI show connections by path type (direct,
  punched, relayed, Tor).

### F. Community-first bootstrap

- A crawler (fleet-hosted at first) continuously enumerates the network and
  verifies reachability with the same dial-back rule as section C. It feeds:
  - the DNS seeds (seed / seed2 / seed3.dinerolabs.org), so they return verified
    community nodes plus fleet;
  - a generated `chainparamsseeds.h` refresh at each release.
- dinero-qt stops hard-pinning `-addnode` to the three fleet IPs. The fleet stays as
  fixed-seed fallback, used when AddrMan has fewer than N good community candidates.
  N is the builder's call.
- Find out who runs 5.196.74.147 (`dinerorelay:0.3.1`) and whether to reuse it.

### G. Network-health page

- A public page, fed by the crawler and by node self-reports. **Self-reports are
  opt-in and sent over Tor only (owner decision).**
  - Off by default, with a plain Settings toggle.
  - When on, the report goes through the embedded Tor client, so the home IP is
    never attached.
  - It contains only aggregate-safe fields: NAT mapping type, counts of connections
    by path type, version, and whether the node is relaying. No addresses, node IDs,
    balances or wallet data.
  - Without opt-ins, the page shows only what the crawler can observe.

  The page shows over time:
  - total nodes seen;
  - nodes directly reachable via IPv4 and via IPv6;
  - nodes reachable only via relay, and unreachable nodes;
  - the share of connections that are direct, punched or relayed;
  - the number of community relays;
  - versions in the wild.
- Hosting and format left to builder. A static page regenerated by the crawler is
  fine.

### H. Unhelpful peers and old builds

- Peer selection deprioritizes peers that repeatedly don't deliver requested
  blocks (extends #824 slow-peer demotion) and frees their slots for community
  candidates.
- The app shows an upgrade prompt when peers report a newer version (mechanism left
  to builder, e.g. from version messages or the release feed).

### UX

- One plain status line, no settings needed:
  - "Your node is reachable — helping the network"
  - "Reachable through a relay"
  - "Connecting directly to N home nodes"
  - "Not reachable yet — trying…"
- Replace the "Open port 20999 inbound" hint with that status. Manual router advice
  goes behind a "details" link only.
- Settings: a relay toggle and usage display, and the existing port-mapping toggle.

## Testing and acceptance

**Primary test bench: DineroTX** (64.44.157.100, Linux, the owner's soak/canary box;
owner choice). Use network namespaces with iptables/nft NAT to put nodes behind real
NATs:

- endpoint-independent mapping (home-router-like);
- endpoint-dependent / symmetric mapping (carrier-NAT-like);
- IPv6 with a stateful firewall (inbound dropped, outbound allowed);
- no UPnP/NAT-PMP/PCP responder (like the owner's router), plus one variant with a
  miniupnpd responder to exercise the mapping path.

**Community relay for the tests (my call).** A `dinerod` on DineroTX's public IP, in
the root namespace with no NAT. It is not on the fleet list, so it is a community
relay by the definition in section D. A second one on another provider (e.g. a cheap
VPS, or the owner's GreenCloud VPS once it is resynced) covers the "2 relays on
different /16s" rule.

**Dell (owner's home, behind the real AT&T gateway).** Block its inbound P2P port
with the local firewall (reversible). Do not touch the router. Start fresh with no
configuration.

**Acceptance (owner's words: "Reachable through a relay", and nothing to configure):**

1. The Dell, configured with nothing, shows "Reachable through a relay" within a few
   minutes, and a node on DineroTX connects to it through a community (non-fleet)
   relay and exchanges blocks.
2. A home-router-style NAT node on DineroTX and the Dell (behind the real AT&T NAT)
   establish a **direct** punched connection, verified as path type direct/punched
   with the relay circuit closed, and exchange blocks.
3. The same over IPv6 between the Dell and a firewall-only IPv6 namespace.
4. A symmetric-NAT namespace node falls back to relay automatically and stays
   connected.
5. No node advertises an address that failed dial-back.
6. Relay caps hold under load: a relay with more than 8 circuit requests refuses
   politely, and bandwidth stays within the cap.
7. The build gate fails a deliberately broken build (portmapping off) and passes a
   correct one.

**Automated:** extend `test_relay_nat_netns.sh` to mainnet-shape mode and add the
scenarios above to CI. Every behavior fix ships with a regression test that fails
without it (owner's verification rules: confirm it ran, neuter-check it, no
`assert()`-only gates).

## Delivery order (my call; all lands in v8.2.0 before the Nov 9-17 window)

Run these as parallel tracks where possible. The order shows dependencies, not
sequential calendar time.

1. **Build-integrity gate and static deps** (A). First, because it unblocks testing
   the rest on real builds.
2. **In parallel from day one:**
   - **2a.** Native UDP/QUIC peer transport (prerequisite of E). This is the longest
     pole, so start it immediately.
   - **2b.** Dial-back verification and IPv6 listen (C, B.2), plus NAT-type
     measurement.
   - **2c.** Crawler and seeds (F) and the health page (G).
3. **Community relays with caps and identity binding** (D), plus the relay-timeout
   root cause.
4. **Hole punching**, IPv6 then IPv4 (E), on top of 2a and 3.
5. **Alongside:** peer-quality and upgrade prompts (H), and PCP and UPnP IPv6
   pinholes (B.1).
6. **Combined-candidate re-qualification:** the consensus evidence re-run plus the
   acceptance tests above. Then release, and upgrade all known nodes before 150,000.

## Decided by the owner

- Zero-touch: "the app should handle it with no help from them".
- Implement all of items 1-7: build gate, real-network tests, community relays,
  community seeds, health page, IPv6, old-build handling.
- Home nodes may relay for others with an acceptable bandwidth cap.
- Direct home-to-home connections are a headline goal.
- Test bench: DineroTX. Real-home test: the Dell.
- Everything ships in v8.2.0; activation stays at 150,000; no split release.
- Health-page self-reports are opt-in and sent over Tor.

## Calls I made (owner to check)

- Default relay cap numbers (8 circuits, 200 KB/s, 20 GB/month).
- Relay role tied to verified direct reachability instead of mining.
- `--portmap=auto` as the default on server builds too.
- Dial-back needs 2 or more distinct /16 verifiers; re-check every 30 min.
- UDP/QUIC-only punching; no TCP simultaneous open; no IPv4 port prediction. The
  owner's NAT was measured as endpoint-independent and port-preserving, so
  acceptance test 2 is achievable without prediction.
- A new native UDP/QUIC peer transport on 20999/udp; punched peers are first-class
  sync peers.
- Relayed sessions must be cryptographically bound to node identity (MITM-proof
  against community relays).
- Fleet = `kMainnetRelayPeers` plus fixed seeds; everything else is community.
- The test community relay is a public-IP `dinerod` on DineroTX.
- Delivery order above.

## Left to the builder

- Wire formats and capability bit numbers for dial-back and introductions.
- How relay hints propagate beyond direct peers.
- Crawler implementation and health-page hosting.
- The AddrMan threshold N before falling back to fleet seeds.
- Upgrade-prompt mechanism.
