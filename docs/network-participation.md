# Network participation in Dinero

Dinero is not a wallet that talks to a few servers. Every `dinero-qt` /
`dinerod` instance ships with a real embedded P2P node, and that node
participates in the network — regardless of whether the user is mining,
hosting a public relay, or simply running the GUI to check a balance.

This document defines the three independent tiers of network behaviour
the codebase is organized around. Reviewers and contributors should keep
these tiers conceptually separate when reading code, naming fields, and
writing user-facing copy.

## Tier 1 — Full P2P node (always on)

Every Dinero node:

- Connects to peers via outbound dialing and listens for direct inbound.
- Validates the chain — headers, blocks, transactions — under consensus.
- Relays valid blocks and transactions to its peers.
- Gossips peer addresses (BIP155 `addr_v2`).
- Attempts UPnP / NAT-PMP port mapping when enabled.
- Runs STUN to discover its public reachable address.
- Carries the `NODE_NETWORK` + `NODE_DINERO_V2` (`1 << 27`) service bits.

None of this is gated on mining, on the `p2p.relay=*` setting, or on
operator opt-in. If you launched the daemon, you are a P2P participant.

## Tier 2 — NAT-fallback target (default on, never depends on mining)

If direct inbound fails — the router won't open port 20999, CGNAT is in
the way, UPnP didn't help — the node automatically registers with
already-connected relay peers (`NODE_RELAY = 1 << 26`) so other peers can
dial it through them. This is the `MaybeAutoRegisterWithRelays` path.

A non-mining wallet behind NAT therefore stays reachable for inbound
peer connections via relay tunnels, just like a public-IP node. From the
rest-of-network perspective, the only difference is one extra hop.

The behavior turns off automatically if direct inbound is observed
(`advertised_addresses_` non-empty), or when an operator pins the
relay list explicitly via `relayregister=`. It does NOT depend on mining
state.

## Tier 3 — Public relay operator (opt-in)

A subset of nodes choose to *be* the relay — accept dial-through
RELAY_CONNECT messages from other NAT'd peers and forward their
encrypted (QUIC) traffic. This costs real bandwidth and exposes the node
to abuse vectors, so it stays opt-in.

The `p2p.relay` config flag controls Tier 3 only:

| `p2p.relay` value | Tier 3 behavior |
| --- | --- |
| `1` / `on` / `true` | Tier 3 on, always. Operator-pinned. |
| `0` / `off` / `no` | Tier 3 off, always. Hard opt-out. |
| `auto` (default, unset) | Tier 3 on while the daemon's auto-mode flag is engaged. |

The auto-mode flag (`P2PService::relay_active_`, exposed as
`getnetworkinfo.relay.active`) is currently engaged by exactly one
trigger: `mining.start` / `mining.stop` (a mining node moves more value,
so the incremental relay cost pays for itself). The lever is general,
not mining-specific — future triggers (operator dashboard opt-in,
spare-bandwidth heuristic, scheduled hours, etc.) can engage the same
flag via `P2PService::SetRelayActive(true)` without protocol or RPC
changes.

The dedicated RPC `mining.setrelayactive` lets external miners (the Qt
embedded miner, mining pools) drive the auto-mode flag directly,
because they bypass `mining.start` by talking to `getblocktemplate` /
`submitblock`. Despite the namespace, that RPC is the supported general
way to toggle the Tier 3 auto-mode lever from outside the daemon; new
non-mining callers should use it.

## What this means for naming

Tier 3 is the only tier that has a config flag. Tier 1 and Tier 2 are
unconditional and don't need one. As a result:

- "Mining relay" is not a thing. The relay role is general, not mining-
  bound. Code, comments, log lines, RPC fields, and UI strings should
  not imply otherwise. Where the historical name `mining_relay_active`
  remained in the codebase as of v8.0.0-rc12, it was a misleading legacy
  identifier for the Tier 3 auto-mode toggle — renamed in this commit's
  ancestor.
- "Network participation" describes Tier 1 (and is always on).
- "Reachability" / "NAT fallback" describes Tier 2 (and is always on).
- "Public relay role" / "relay operator" describes Tier 3 (and is the
  opt-in piece).

When in doubt: name fields by what they *do*, not by what triggered them.
`relay_active` (what state the relay role is in) is correct;
`mining_relay_active` (what triggered the state) is wrong.

## Why the encrypted relay data plane belongs here

The relay role only makes sense if the relay can carry inner P2P
traffic for other peers without seeing it in cleartext (otherwise the
relay sees every transaction your wallet broadcasts). That property is
delivered by the QUIC-encrypted relay data plane: each circuit is a
client-server QUIC session terminating at the originator and target,
keyed on the relay-issued `circuit_id`. The relay forwards opaque
encrypted datagrams; the dineroid identity exchange runs inside the
encrypted stream as the trust anchor.

This is why Tier 3 is safe to be default-on for any operator who
chooses to enable it, and why running a public relay does not turn a
node into a surveillance choke point.

## Roadmap to flipping Tier 3 default

Tier 3 will not be flipped to default-on (i.e., on for `auto` mode
without a mining-or-similar trigger) until abuse-protection lands:
per-source registration rate limits, per-source circuit caps, ban-score
for misbehaving relay clients, and the existing bandwidth caps
(`kRelayPerCircuitBurstBytes`, the 50 GB / 24h rolling quota, etc.)
covering enough of the threat surface.

Until that lands, the conservative default is: a node runs as a public
relay only when there's a positive trigger (mining, operator opt-in).
Tier 1 and Tier 2 — the parts that *make this a real P2P network* —
remain unconditional.
