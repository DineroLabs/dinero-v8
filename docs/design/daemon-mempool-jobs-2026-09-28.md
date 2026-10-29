# Retain mempool service ownership in daemon jobs

Actual CSN cached-payload serving and received-proof handling now retain a shared mempool service operation until their synchronous work finishes. Received-proof handling keeps one operation across the existing membership, submission, proof metadata, payload cache and relay calls. The same guard covers startup shielded-state wiring, disk load/save and both legacy reorg reconciliation paths. Shutdown closes new independent acquisition and waits for these guards through the existing service drain.

This change preserves existing transaction/proof validation, admission, callback order and persistence behavior. A service guard protects lifetime; it does not make the separate proof metadata/cache writes atomic, certify their selected-chain provenance, or turn best-effort mempool persistence into a durable pending owner. The existing pool lock still governs each pool operation.

Block relay, block assembler and optional gRPC retain separately installed raw pool references under their existing shutdown-order contract. Direct independent chainstate shutdown and asynchronous work outside these guarded call sites remain outside this change. Startup and daemon shutdown remain externally serialized. The production Orchard notification provider and Orchard admission/readmission are still absent; mainnet remains unset.

Qualification uses fresh backend-enabled and disabled daemon builds and unchanged service-owner component cases. Required daemon cases exercise real Init/Start/repeated Stop, compact-block download, covenant restart/reorg/reindex and CSN spend/reorg reconciliation. They establish their executed scenarios, not every received-proof callback branch or whole-node release readiness.
