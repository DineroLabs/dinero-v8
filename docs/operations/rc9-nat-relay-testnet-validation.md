# RC9 NAT Relay Testnet Validation

Status: pre-gate-flip runbook.

This document is the operational gate between the completed NAT relay
implementation and enabling the mainnet relay path for rc9.

As of `dinero-main` commit `9c827931`, the implementation and privileged
Linux netns validation are complete. `QuicTransport::MainnetRelayReady()`
still returns `false` by design. Do not flip it until this runbook passes on
testnet with real STUN and relay infrastructure.

## What Is Already Proven

- Encrypted QUIC relay implementation builds with vendored ngtcp2/OpenSSL
  3.5.6 when `DINERO_ENABLE_QUIC=ON`.
- Linux netns harness proves origin -> relay -> target dial-through through
  hostile NAT namespaces.
- Relay registry catch-up is covered: a late or reconnecting origin receives
  the relay registry without waiting for the periodic refresh.
- Relay hint handling is IPv4 and IPv6 aware.
- Mainnet remains gated.

Reference validation:

```bash
DINERO_RUN_NETNS_NAT=1 \
  ctest --test-dir build-release -R RelayNatNetnsHarness --output-on-failure
```

Expected result:

```text
RelayNatNetnsHarness ............. Passed
RELAY_NAT_NETNS_DIAL_THROUGH=PASS
```

## Testnet Topology

Use at least three nodes:

| Role | Network position | Required properties |
| ---- | ---------------- | ------------------- |
| Relay R | Public VPS | Public TCP P2P port, UDP allowed for QUIC/STUN, `externalip` set |
| Target T | Behind hostile NAT | Outbound-only, configured with `relayregister=<R>` |
| Origin O | Separate network | Connects to R after T registers, proves registry catch-up |

Recommended minimum infrastructure before the test:

- `stun1.dinerolabs.org:3478`
- `stun2.dinerolabs.org:3478`
- `stun3.dinerolabs.org:3478`
- one public relay node for this testnet run

Do not populate `MAINNET_RELAY_PEERS` during this validation. That is a
separate mainnet launch step after testnet passes.

## Build Gate

Build from the exact candidate commit on all test nodes.

```bash
git fetch origin dinero-main
git checkout <candidate-commit>
git submodule update --init third_party/ngtcp2

cmake -S . -B build-rc9-quic \
  -DCMAKE_BUILD_TYPE=Release \
  -DDINERO_USE_VENDORED_DEPS=ON \
  -DDINERO_ENABLE_QUIC=ON

cmake --build build-rc9-quic --target dinerod -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
cmake --build build-rc9-quic \
  --target dinero_quic_relay_test_targets \
  -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

ctest --test-dir build-rc9-quic \
  -R '^(QuicDependency|QuicCryptoCapability|QuicTransport|QuicSession|RelayVirtualTransport)$' \
  --output-on-failure
```

Pass criteria:

- `dinerod` builds.
- focused QUIC/relay tests pass.
- `dinerod --version` reports the candidate commit.

## Relay Node R

Start R on the public testnet relay host.

```bash
./build-rc9-quic/dinerod \
  --testnet \
  --datadir=/var/lib/dinero-testnet-relay \
  --rpcport=<relay-rpc-port> \
  --p2pport=<relay-p2p-port> \
  --externalip=<relay-public-host-or-ip>:<relay-p2p-port> \
  --portmap=auto \
  --p2p.stun.enabled=1
```

Firewall requirements:

- TCP `<relay-p2p-port>` open inbound.
- UDP `<relay-p2p-port>` open inbound if QUIC relay is enabled on the host.
- UDP 3478 open on STUN hosts, not necessarily on R unless R also runs STUN.

Relay pass checks:

```bash
dinero-cli -testnet -rpcport=<relay-rpc-port> getnetworkinfo
dinero-cli -testnet -rpcport=<relay-rpc-port> getpeerinfo
```

Expected:

- `network` is `testnet`.
- `listen` is `true`.
- `localaddresses` includes `<relay-public-host-or-ip>:<relay-p2p-port>`.
- `stun.message` is either `ok` or an explained non-fatal fallback while
  `externalip` still provides the advertised relay endpoint.

## Target Node T

Start T from behind the hostile NAT.

```bash
./build-rc9-quic/dinerod \
  --testnet \
  --datadir=/var/lib/dinero-testnet-target \
  --rpcport=<target-rpc-port> \
  --p2pport=<target-p2p-port> \
  --relayregister=<relay-public-host-or-ip>:<relay-p2p-port> \
  --p2p.stun.enabled=1
```

Then force an immediate direct connection to R if seed discovery has not
already connected it:

```bash
dinero-cli -testnet -rpcport=<target-rpc-port> \
  addnode "<relay-public-host-or-ip>:<relay-p2p-port>" onetry
```

Target pass checks:

```bash
dinero-cli -testnet -rpcport=<target-rpc-port> getpeerinfo
```

Expected logs on T:

```text
[P2P] relay-register: sent to <relay-public-host-or-ip>:<relay-p2p-port>
```

Expected logs on R:

```text
[P2P] relayreg: registered
```

## Origin Node O

Start O from a different network after T has registered with R.

```bash
./build-rc9-quic/dinerod \
  --testnet \
  --datadir=/var/lib/dinero-testnet-origin \
  --rpcport=<origin-rpc-port> \
  --p2pport=<origin-p2p-port> \
  --p2p.stun.enabled=1

dinero-cli -testnet -rpcport=<origin-rpc-port> \
  addnode "<relay-public-host-or-ip>:<relay-p2p-port>" onetry
```

Origin pass checks:

```bash
dinero-cli -testnet -rpcport=<origin-rpc-port> getpeerinfo
dinero-cli -testnet -rpcport=<target-rpc-port> getpeerinfo
```

Expected logs on R:

```text
[P2P] relay-hints: advertised registered target
[P2P] relaycon: opened circuit
```

Expected logs on O:

```text
[P2P] relay-hints: ingested
[P2P] relay-orchestrator: opened circuit
```

Expected logs on T:

```text
[P2P] relay-data: created inbound virtual peer
```

Expected RPC shape:

```bash
dinero-cli -testnet -rpcport=<origin-rpc-port> getpeerinfo \
  | jq '[((.result // .)[]?) | select((.inbound == false) and
      ((.addr // "") | startswith("relay:")))] | length'

dinero-cli -testnet -rpcport=<target-rpc-port> getpeerinfo \
  | jq '[((.result // .)[]?) | select((.inbound == true) and
      ((.addr // "") | startswith("relay:in:")))] | length'
```

Both counts must be at least `1`.

The read-only probe script runs the same checks once all three nodes are up:

```bash
RELAY_CLI_ARGS="-testnet -rpcport=<relay-rpc-port>" \
TARGET_CLI_ARGS="-testnet -rpcport=<target-rpc-port>" \
ORIGIN_CLI_ARGS="-testnet -rpcport=<origin-rpc-port>" \
RELAY_ENDPOINT="<relay-public-host-or-ip>:<relay-p2p-port>" \
  scripts/deploy/rc9_nat_relay_testnet_probe.sh
```

Log checks are optional but recommended for the final gate decision:

```bash
RELAY_LOG=/var/lib/dinero-testnet-relay/debug.log \
TARGET_LOG=/var/lib/dinero-testnet-target/debug.log \
ORIGIN_LOG=/var/lib/dinero-testnet-origin/debug.log \
RELAY_CLI_ARGS="-testnet -rpcport=<relay-rpc-port>" \
TARGET_CLI_ARGS="-testnet -rpcport=<target-rpc-port>" \
ORIGIN_CLI_ARGS="-testnet -rpcport=<origin-rpc-port>" \
RELAY_ENDPOINT="<relay-public-host-or-ip>:<relay-p2p-port>" \
  scripts/deploy/rc9_nat_relay_testnet_probe.sh
```

## Catch-Up Regression Check

This check specifically protects the PR #106 review fix: late or reconnecting
origins must receive the registry immediately.

1. With T still registered to R, stop O.
2. Wait until R no longer lists O in `getpeerinfo`.
3. Restart O.
4. Connect O to R again with `addnode ... onetry`.

Pass criteria:

- O ingests relay hints without waiting for the one-hour registry refresh.
- R opens a new circuit.
- O shows an outbound `relay:` peer.
- T shows an inbound `relay:in:` peer.

## Failure Policy

Do not flip `MainnetRelayReady()` if any of these fail:

- QUIC build or focused tests fail.
- Dell/Linux netns harness fails on the candidate commit.
- R cannot advertise a reachable relay endpoint.
- T cannot register with R.
- O cannot receive the registry catch-up on connect.
- O cannot establish a virtual relay peer to T.
- T does not report an inbound virtual relay peer.

If STUN fails but `externalip` is correctly configured and relay dial-through
still passes, the testnet relay validation may continue. Record the STUN
failure separately as ops debt before rc9 infrastructure launch.

## Gate Flip Criteria

Only after this document passes on testnet:

1. Open a small PR flipping `QuicTransport::MainnetRelayReady()` from `false`
   to `true`.
2. Populate `MAINNET_RELAY_PEERS` or the equivalent launch config with deployed
   relay endpoints.
3. Re-run:

```bash
cmake --build build-rc9-quic --target dinerod -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
ctest --test-dir build-rc9-quic \
  -R '^(QuicDependency|QuicCryptoCapability|QuicTransport|QuicSession|RelayVirtualTransport)$' \
  --output-on-failure
DINERO_RUN_NETNS_NAT=1 \
  ctest --test-dir build-rc9-quic -R RelayNatNetnsHarness --output-on-failure
```

The gate flip PR should contain no unrelated cleanup.
