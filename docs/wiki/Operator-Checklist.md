# Operator Checklist

Use this checklist before calling a node production-ready.

## Node Identity

- Hostname and public IP are known.
- Node role is clear: seed, RPC, bridge, explorer backend, miner, or pool.
- Release version is recorded.

## Network

- P2P port `20999` is reachable when the node should accept peers.
- RPC is not publicly exposed without TLS/auth/proxy controls.
- DNS points to the intended host.
- TLS certificates are valid for public HTTPS/RPC endpoints.

## Chain Health

- Height matches the rest of the fleet.
- Best hash matches the rest of the fleet.
- Peer count is healthy.
- Disk usage has enough runway.
- Logs are persisted to disk or another durable sink.

## Bridge/Proof Serving

For bridge nodes:

- Historical blocks are available if the node is expected to serve from genesis.
- Bridge/proof service is advertised and reachable.
- Mobile clients can request and receive Utreexo block/proof payloads.
