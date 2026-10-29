# Typed connected-block effects for mempool reconciliation

`ChainstateService` still connects and notifies historical `Block` values. A
mixed Orchard body has no valid conversion to that type. The ordinary mempool
nevertheless needs the exact confirmed transaction IDs and spent transparent
outpoints after a validated block, so that it can evict confirmed transactions
and conflicts and mark surviving accumulator proofs stale.

`ConnectedBlockEffects` carries only those identities. The historical
`Mempool::onBlockConnected(Block)` path now builds this summary and delegates to
one reconciliation implementation. `BuildOrchardConnectedBlockEffects` reads
the immutable `OrchardBlockCandidate` and includes both ordinary and Orchard
transparent inputs. A height-only disconnect entry point preserves the existing
proof-staleness behavior without inventing a historical `Block`.

The typed builder **does not validate a block**. The future service caller must
derive it from the exact body it has already validated and committed. Orchard
nullifier conflicts need a separate Orchard mempool index and are not resolved
by transparent outpoints. The existing wallet, oracle, relay and mining event
consumers also require typed routing before production connection can use this
path. Until then, live Orchard connection remains disabled.

The historical mempool regression checks shared-outpoint eviction, confirmed
removal, unrelated transaction survival, stale proofs and typed disconnect.
The Orchard reader regression compares extracted IDs and both prevouts against
the independent Python envelope vector. This component evidence does not
qualify a daemon block lifecycle or a release binary.
