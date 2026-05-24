# Dinero Dynamic P2P v1 - Design

**Date:** 2026-05-24
**Author:** Dinero Labs (with Codex)
**Status:** Spec
**Branch this targets:** `dinero-main` after rc16 relay lifecycle work
**Related:** `docs/network-participation.md`, `docs/superpowers/specs/2026-05-23-relay-hints-lifecycle-design.md`

---

## Background

Dinero now has the core pieces needed for NAT-resilient peer-to-peer networking:

- `dinero-qt` and `dinerod` run real embedded P2P nodes.
- `addrman` stores known peer addresses and service flags.
- `NODE_RELAY` marks peers willing to act as encrypted circuit relays.
- NAT-blocked nodes auto-register with relay peers.
- relay hints let other nodes dial NAT-blocked targets through relays.
- `getnetworkinfo` exposes direct reachability, relay state, and hint counters.

That work answers the first reachability question:

> Can a NAT-blocked home node receive an inbound P2P connection?

Dynamic P2P answers the next decentralization question:

> Can the network discover, rank, rotate, and survive without treating LA / VA / MO / CN as "main nodes"?

The target state is not "four fleet nodes plus users." The target state is:

> Fleet nodes are bootstrap infrastructure. The actual network is every reachable `dinero-qt` / `dinerod` instance that discovers peers, validates blocks, relays traffic, and participates according to its capability.

This spec copies the useful part of Cardano's Dynamic P2P design - peer lifecycle management - without copying Cardano's ledger/on-chain registration model. Dinero remains a Bitcoin-style peer discovery network with `addrman`, DNS seeders, service flags, and bounded gossip.

## Goals

- Make the fleet bootstrap-only, not the center of the network.
- Let every node maintain a useful peer set without operator hand-tuning.
- Prefer diverse, successful, up-to-date peers.
- Discover and prefer good relay peers dynamically.
- Rotate weak peers out slowly and safely.
- Make relay-capable community nodes visible to NAT-blocked users.
- Expose simple status in `dinero-qt`: direct inbound, relay reachable, hot peers, relay peers, helping as relay.

## Non-goals

- Do not put peer or relay registration on-chain.
- Do not remove hardcoded seed peers in v1.
- Do not trust STUN as proof of direct reachability. STUN remains diagnostic / future hole-punch input.
- Do not turn every node into an uncapped public circuit relay. Tier 3 relay role stays controlled by `p2p.relay` and bandwidth caps.
- Do not rewrite P2P transport, QUIC, or relay data plane.

---

## Current fit in the codebase

Dynamic P2P is an orchestration layer over code that already exists.

### `AddressManager`

Current file: `include/p2p/addrman.h`, `src/p2p/addrman.cpp`

Already present:

- `NetworkAddress { ip, port, services, timestamp }`
- `AddressEntry` with `first_seen`, `last_seen`, `last_try`, `last_success`
- attempt and success accounting
- new/tried pools
- subnet diversity
- `getAddresses(count)`
- `getAddressesByService(service_bit, count)`, already used for `NODE_RELAY`
- duplicate re-advertisement updates service flags, so `NODE_RELAY` can propagate into addrman

Needed for Dynamic P2P:

- richer peer quality metadata
- relay usefulness metadata
- latency / freshness / failure reason accounting
- persistence/load path if not already wired in the running daemon
- stats exposed through `getnetworkinfo`

### `PeerScoringManager`

Current file: `include/p2p/peer_scoring.h`, `src/p2p/peer_scoring.cpp`

This is mostly a negative trust layer: misbehavior, bans, protocol violations, timeouts, spam. Keep it that way.

Dynamic P2P needs a separate positive usefulness model:

- "this peer helps me sync"
- "this peer delivers blocks quickly"
- "this relay successfully connects me to NAT-blocked targets"
- "this peer is stable but not especially useful"

Bad peers are banned by peer scoring. Weak peers are simply rotated out by the governor.

### `P2PManager`

Current file: `src/daemon/p2p_manager.cpp`, `src/daemon/p2p_manager.h`

Already present:

- `connection_manager_loop`
- seed-node bootstrap plus addrman candidates
- relay dialing and relay hints
- `MaybeAutoRegisterWithRelays`
- periodic keepalive loop
- `PeerInfo` fields for service flags, latency-ish counters, heights, bytes sent/received, relay metadata

Needed:

- delegate peer choice to a new governor instead of hand-rolling selection inside the connection loop
- feed connection outcomes back into addrman / peer quality
- add slow churn of weak outbound peers
- prefer discovered relay peers over bootstrap relays after enough evidence exists

### `P2PService` and RPC/UI status

Current files: `include/daemon/services/p2p_service.h`, `src/daemon/services/p2p_service.cpp`, `src/rpc/methods_network_context.cpp`

Already present:

- direct reachability status
- port mapping status
- STUN diagnostic status
- relay local/active/mode status
- relay hint counters
- relay directory counters

Needed:

- `dynamic_p2p` status object in `getnetworkinfo`
- Qt-friendly summary fields
- dry-run governor decisions for early rollout

---

## Architecture

### 1. Peer memory

`addrman` remains the long-lived address book. It should store or derive:

| Field | Purpose |
| --- | --- |
| address | IP/port or future network type |
| services | `NODE_NETWORK`, `NODE_DINERO_V2`, `NODE_RELAY`, etc. |
| source | seed, DNS seed, addr gossip, relay gossip, manual |
| first_seen / last_seen | freshness |
| last_try / last_success | connection history |
| last_failure | recent failure history |
| failure_reason | dial timeout, handshake timeout, stale chain, relay failure |
| latency_ms | peer quality |
| best_known_height | sync usefulness |
| block_usefulness | delivered useful headers/blocks |
| tx_usefulness | delivered accepted txs, if measured |
| relay_usefulness | relay registration/connect/handshake success |
| diversity_key | subnet now, ASN later |

The initial implementation should add only fields that are immediately used or exposed. Do not bulk-add metadata that no code consumes.

### 2. Peer quality model

Add a small positive scoring model, separate from ban scoring.

Suggested derived score:

```
quality =
  connection_success_weight
+ handshake_success_weight
+ freshness_weight
+ block_usefulness_weight
+ relay_usefulness_weight
- recent_failure_penalty
- latency_penalty
- fleet_overconcentration_penalty
```

This score is not consensus-critical. It is only a local node policy for selecting peers.

Rules:

- Score decay over time.
- Recent success matters more than old success.
- A peer with poor score is not automatically malicious.
- Misbehavior still flows through `PeerScoringManager`.
- Relay score is separate from block/tx score. A mediocre block peer may be a good relay and vice versa.

### 3. Peer governor

Introduce a new `PeerGovernor` component. It can live under `src/p2p/` or `src/daemon/` depending on dependency direction; avoid bloating `P2PManager`.

The governor maintains target sets:

| Set | Meaning | Example target |
| --- | --- | --- |
| cold | known, mostly untested addrman entries | many |
| warm | recently tested standby peers | 16 |
| hot | active peers used for block/tx/header relay | 8 outbound |
| relay candidates | `NODE_RELAY` peers worth registering with or dialing through | 8 known, 2-3 active |

The governor loop answers:

- Do I have enough hot outbound peers?
- Are my hot peers diverse?
- Are too many peers from the fleet?
- Which cold peer should I test next?
- Which weak hot peer should be demoted?
- Which warm peer should be promoted?
- If I am NAT-blocked, which relay peers should I register with?
- If a relay fails, when should I retry vs rotate?

The first implementation should run in dry-run mode:

- compute decisions
- expose them in `getnetworkinfo`
- log them at debug level
- do not actually disconnect or rotate peers yet

Only after dry-run looks sane should it take control of outbound churn.

### 4. Diversity policy

Dinero should not depend on any one operator, ASN, subnet, or relay.

v1 diversity rules:

- cap active peers per `/16` IPv4 subnet where possible
- do not allow LA / VA / MO / CN to dominate outbound slots when other good peers exist
- cap relay registrations per relay operator/source
- prefer a mix of seed-learned, addr-gossiped, and relay-gossiped peers
- keep at least one stable fleet/bootstrap peer until the discovered set is healthy

Future diversity rules:

- ASN-aware caps in DNS seeder / operator tooling
- country/region diversity if reliable data is available
- IPv6 and Tor/I2P buckets once those paths are fully supported

### 5. Relay-aware selection

NAT-blocked nodes should prefer dynamic relays over hardcoded relays once the dynamic set is healthy.

Selection order:

1. Known-good relay peers from addrman (`NODE_RELAY`, recent success)
2. Relay peers from DNS seeder relay pool
3. Relay peers from signed relay-hint gossip
4. Fleet bootstrap relays as fallback

Relay usefulness updates:

- registration success
- registration refresh success
- relay circuit open success
- end-to-end QUIC handshake success
- relay-backed peer lifetime
- bytes delivered without protocol errors
- failure count and recent failure reason

This directly reduces permanent reliance on VA / LA / MO / CN.

### 6. Gossip

Gossip is required for real P2P discovery. The safety rule is not "disable gossip." The safety rule is:

> Gossip only information that can expire, be verified, be deduplicated, and be rate-limited.

v1:

- continue `addr` / `addrv2` peer address gossip
- keep relay hints lifecycle from the existing relay-hints spec
- do not forward unsigned stale relay hints forever

v2:

- signed `RELAY_HINTS` with `expires_at`
- forward target-signed hints byte-for-byte
- dedup by `(target_node_id, relay_endpoint, expires_at, signature)`
- cap third-party forwards per peer per minute
- never extend another node's expiry
- evict on repeated dial failure

This is the path from fleet-assisted discovery to true network discovery.

### 7. DNS seeder support

DNS seeders should become bootstrap inputs, not authorities.

Add or extend pools:

- general healthy public peers
- relay-capable peers (`NODE_RELAY`)
- maybe mining/public peers later, if useful

Seeder selection should prefer:

- recent successful handshake
- current chain height near tip
- `NODE_DINERO_V2`
- `NODE_RELAY` for relay pool
- diversity by subnet/ASN

The daemon uses DNS seeders only to get initial candidates. The governor decides which peers stay.

### 8. User-facing status

`dinero-qt` should present this simply:

- Direct inbound: yes / no
- Relay reachable: yes / no
- Helping as relay: off / auto / on
- Hot peers: count
- Relay peers: count
- Network participation: limited / healthy / strong

Avoid exposing cold/warm/hot jargon unless the user opens an advanced panel.

Plain language:

- "Direct inbound unavailable; relay fallback active."
- "This node is helping relay the Dinero network."
- "Discovering more peers."
- "Using community relay peers."

---

## Implementation plan

### PR A - Spec only

Land this document. No behavior change.

### PR B - Dynamic P2P observability

Add read-only stats:

- addrman total/new/tried/relay candidates
- connected fleet peers vs discovered peers
- direct inbound observed
- relay reachable
- current hot-peer count (initially equivalent to connected outbound block peers)
- current relay candidate count

Expose under:

```json
"dynamic_p2p": {
  "enabled": false,
  "mode": "observe",
  "addrman": { ... },
  "peers": { ... },
  "relays": { ... }
}
```

### PR C - Peer quality model

Add a small, unit-tested `PeerQuality` model.

No network behavior change. Feed it synthetic events in tests:

- connection success
- connection failure
- handshake success
- stale height
- useful block
- relay success
- relay failure

### PR D - Governor dry-run

Add `PeerGovernor` in observe-only mode.

Inputs:

- addrman snapshot
- connected peer snapshot
- relay hint state
- direct reachability state

Outputs:

- recommended hot peers
- recommended warm test candidates
- recommended relay registrations
- peers it would demote

No disconnects. No automatic dialing changes.

### PR E - Slow outbound churn

Enable conservative action:

- only rotate one weak peer per interval
- never churn immediately after startup
- never drop all fleet peers at once
- keep minimum stable peer count
- log every decision

This is the first behavior-changing PR.

### PR F - Dynamic relay selection

NAT-blocked nodes register with best discovered relays first, fleet relays second.

Rules:

- keep 2-3 active relay registrations
- prefer successful recent relays
- decay failed relays
- cap same-subnet relays
- fall back to fleet bootstrap if dynamic set is empty

### PR G - Signed relay-hint gossip

Implement the signed/bounded gossip phase from the relay-hints lifecycle spec.

This is the step that makes relay discovery genuinely peer-to-peer.

### PR H - DNS seeder relay pool

Teach seeder tooling to emit:

- healthy public peer pool
- relay-capable peer pool

### PR I - Qt network participation UI

Expose the simple status in `dinero-qt`.

---

## Rollout discipline

Dynamic P2P must be rolled out cautiously:

1. Observe first.
2. Score second.
3. Dry-run governor third.
4. Slow churn fourth.
5. Dynamic relay selection fifth.
6. Signed gossip sixth.

Do not jump directly from "fleet bootstrap" to "full autonomous churn." Peer-selection bugs can look like network instability, sync stalls, or accidental centralization.

## Success criteria

This project is successful when:

- A fresh node can discover useful peers beyond LA / VA / MO / CN.
- NAT-blocked nodes can find relay peers without hardcoded fleet dependency.
- Fleet nodes can go offline without instantly killing peer discovery for already-running nodes.
- `dinero-qt` users can see that their node is participating.
- Relay-capable community nodes become useful without manual config.
- The network's peer graph becomes more diverse over time.

## Core principle

The goal is not fewer fleet nodes. The goal is fewer fleet assumptions.

Fleet infrastructure should help the network start, not define the network.

