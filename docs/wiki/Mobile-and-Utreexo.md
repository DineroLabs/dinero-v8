# Mobile and Utreexo

Dinero v8 supports a local-validation mobile path using Utreexo-style proof data.

The main distinction:

- Local validating Utreexo node: verifies headers, chainwork, block/proof data, and accumulator state locally.
- Remote RPC wallet: asks a remote server for wallet/chain state and treats that server as the authority.

These are different trust models and should not be described as the same product.

## Useful Sync Signals

When debugging mobile local-validation sync, capture:

- validated local block height
- best header height
- connected bridge peers
- whether `getdata` for Utreexo blocks is sent
- whether `utxoblk` messages are received
- whether blocks connect after receipt
- whether the accumulator/forest advances
- whether snapshot/bootstrap restore activated

## Bridge Nodes

Bridge/proof-serving nodes are full nodes that can serve Utreexo block/proof data to local-validating clients. For mobile reliability, clients should have more than one healthy bridge peer.
