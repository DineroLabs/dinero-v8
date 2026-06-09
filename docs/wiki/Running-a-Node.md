# Running a Node

A Dinero v8 node can be run as a local wallet backend, an operator node, a bridge/proof-serving node, or part of infrastructure such as an explorer or RPC fleet.

## Normal Operator Guidance

- Use the current v8 release candidate from the Releases page.
- Keep RPC behind TLS or a trusted local proxy.
- Do not expose unauthenticated RPC publicly.
- Monitor height, best hash, peer count, disk free space, and logs.
- Keep node roles explicit: seed, RPC backend, bridge/proof server, explorer backend, miner, or pool server.

## Useful Health Checks

- Current block height and best hash
- Peer count and whether outbound peers are stable
- Disk free space
- Whether RPC is reachable from the intended network only
- Whether P2P port `20999` is reachable where appropriate
- Whether bridge/proof service is serving clients if the node is meant to be a bridge

## Fresh Sync Expectations

Fresh-from-genesis validation is the strongest model, but it can take time and depends on healthy peers serving historical blocks. Snapshot/AssumeUTXO bootstrap is a faster operational path when configured and verified correctly.
