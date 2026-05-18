# dinero_core Source Map

Generated from `origin/dinero-main` at `3e0f7c98`.

This map documents the remaining direct `dinero_core` source entries after the
safe mechanical source-list extraction series. Include counts are direct
`#include` hits from `src/`, `tests/`, and `include/` for the relevant public or
local header. They are not transitive dependency counts.

This map marks the end of safe mechanical source-list extraction. Further
movement should be design-led, not cleanup-led.

## Load-Bearing Centers

These files are depended on broadly enough that moving them can ripple across
the tree. They should stay visible until there is an explicit dependency plan.

| Entry | Evidence | Disposition |
| --- | ---: | --- |
| `src/common/logger.cpp` | `common/logger.h`: 312 refs | Core infrastructure; do not move casually. |
| `src/daemon/daemon_context.cpp` | `daemon/daemon_context.h`: 105 refs | Dependency container center; do not move casually. |
| `src/rpc/rpc_registry.cpp` | `rpc/rpc_registry.h`: 108 refs | RPC center; requires separate RPC plan. |
| `src/mempool/mempool.cpp` | `daemon/mempool.h`: 38 refs; `mempool/mempool.h`: 20 refs | Keep as mempool core. |
| `src/mining/block_assembler.cpp` | `mining/block_assembler.h`: 28 refs | Mining template core; touches RPC, daemon, miner, wallet hooks, and tests. |

## Public Service Hubs

These files are subsystem API surfaces. Moving them requires API stability
thinking, not just include accounting.

| Entry | Evidence | Disposition |
| --- | ---: | --- |
| `src/daemon/services/chainstate_service.cpp` | `daemon/services/chainstate_service.h`: 61 refs | Chainstate API surface; no mechanical peel. |
| `src/daemon/services/wallet_service.cpp` | `daemon/services/wallet_service.h`: 33 refs | Wallet service API surface. |
| `src/daemon/services/config_service.cpp` | `daemon/services/config_service.h`: 25 refs | Config API surface. |
| `src/daemon/services/mempool_service.cpp` | `daemon/services/mempool_service.h`: 24 refs | Mempool service API surface. |
| `src/daemon/services/p2p_service.cpp` | `daemon/services/p2p_service.h`: 21 refs | P2P service API surface. |
| `src/daemon/services/mining_service.cpp` | `daemon/services/mining_service.h`: 15 refs | Mining service hub. |
| `src/daemon/services/metrics_service.cpp` | `daemon/services/metrics_service.h`: 4 refs | Low fan-in, but still service-boundary code. |
| `src/daemon/services/rpc_service.cpp` | `daemon/services/rpc_service.h`: 2 refs | Low fan-in, but RPC orchestration service. |

## Validation-Adjacent Files

These files touch block/transaction acceptance, ingress, relay, or recovery
paths. Treat them as consensus-adjacent unless proven otherwise.

| Entry | Evidence | Disposition |
| --- | ---: | --- |
| `src/daemon/block_acceptor.cpp` | `daemon/block_acceptor.h`: 13 refs; `dinero/daemon/block_acceptor.h`: 5 refs | Validation center; do not move without a plan. |
| `src/daemon/block_relay_manager.cpp` | `daemon/block_relay_manager.h`: 9 refs | P2P/validation relay. |
| `src/daemon/tx_relay_manager.cpp` | `daemon/tx_relay_manager.h`: 6 refs | P2P/mempool relay. |
| `src/daemon/interfaces/ingress_types.cpp` | `daemon/interfaces/ingress_types.h`: 15 refs | Public ingress result types. |
| `src/daemon/mining_safety_gates.cpp` | `daemon/mining_safety_gates.h`: 5 refs | Mining safety path. |
| `src/daemon/header_metadata_recovery.cpp` | `daemon/header_metadata_recovery.h`: 3 refs | Recovery/storage/daemon-app adjacent. |
| `src/daemon/undo_rebuild_orchestrator.cpp` | `daemon/undo_rebuild_orchestrator.h`: 3 refs | Recovery/storage/daemon-app adjacent. |

## Consensus Cluster

Treat this as a separate future project. Do not treat these files as deferred
cleanup leftovers.

| Entry | Evidence |
| --- | ---: |
| `src/consensus/consensus_write_batch.cpp` | `consensus/consensus_write_batch.h`: 3 refs |
| `src/consensus/reindexer.cpp` | `consensus/reindexer.h`: 7 refs |
| `src/consensus/undo.cpp` | `consensus/undo.h`: 8 refs |
| `src/consensus/header_sync_manager.cpp` | `consensus/header_sync_manager.h`: 7 refs |
| `src/consensus/header_chain.cpp` | `consensus/header_chain.h`: 19 refs |
| `src/consensus/header_store.cpp` | `consensus/header_store.h`: 8 refs |
| `src/consensus/header_sync.cpp` | `consensus/header_sync.h`: 4 refs |
| `src/consensus/header_sync_p2p.cpp` | `consensus/header_sync_p2p.h`: 4 refs |
| `src/consensus/proof_gossip.cpp` | `consensus/proof_gossip.h`: 7 refs |
| `src/consensus/block_download_scheduler.cpp` | `consensus/block_download_scheduler.h`: 9 refs |
| `src/consensus/assume_utxo.cpp` | `consensus/assume_utxo.h`: 3 refs |
| `src/consensus/parallel_block_validator.cpp` | `consensus/parallel_block_validator.h`: 4 refs |
| `src/consensus/validation_worker_pool.cpp` | `consensus/validation_worker_pool.h`: 4 refs |
| `src/consensus/validation_queue.cpp` | `consensus/validation_queue.h`: 5 refs |
| `src/consensus/block_validation.cpp` | `consensus/block_validation.h`: 17 refs |
| `src/consensus/block_index.cpp` | `consensus/block_index.h`: 31 refs |
| `src/consensus/block_lifecycle.cpp` | `consensus/block_lifecycle.h`: 24 refs |
| `src/consensus/orphan_manager.cpp` | `consensus/orphan_manager.h`: 2 refs |
| `src/consensus/block_index_persistence.cpp` | `consensus/block_index_persistence.h`: 1 ref |
| `src/consensus/genesis_canonical.cpp` | `consensus/genesis_canonical.h`: 8 refs |
| `src/consensus/adapters/wallet_utxo_adapter.cpp` | `consensus/adapters/wallet_utxo_adapter.h`: 4 refs |

## Mining / Wallet / RPC Glue

These files cross subsystem boundaries. Moving them should follow a boundary
design, not another source-list cleanup pass.

| Entry | Evidence | Disposition |
| --- | ---: | --- |
| `src/mining/ct_selection_policy.cpp` | `mining/ct_selection_policy.h`: 8 refs | Crosses policy, mempool, and mining. |
| `src/mining/tx_inclusion_analyzer.cpp` | `mining/tx_inclusion_analyzer.h`: 7 refs | Crosses wallet fee-bump and RPC. |
| `src/mining/mining_manager_v2.cpp` | `mining/mining_manager_v2.h`: 6 refs | Service/RPC/mining hub. |
| `src/mining/miner.cpp` | `mining/miner.h`: 3 refs | Submission and proof-of-work path. |
| `src/mining/address_validator.cpp` | `mining/address_validator.h`: 11 refs | Crosses RPC, mining, and mempool. |
| `src/mining/payout_spec.cpp` | `mining/payout_spec.h`: 4 refs | Block-template API, not just utility code. |
| `src/wallet/psbt.cpp` | `wallet/psbt.h`: 20 refs | Wallet/RPC/public type surface. |
| `src/wallet/wallet_worker.cpp` | `wallet/wallet_worker.h`: 7 refs | Wallet execution glue. |
| `src/daemon/wallet_crypto.cpp` | `crypto/wallet_crypto.h`: 4 refs; local `wallet_crypto.h`: 2 refs | Wallet crypto glue. |

## Singleton / Low-Value Leftovers

These entries are not good candidates for mechanical extraction. Most are
singletons, narrow utilities, or compatibility shims. Extracting them into
one-file variables would invent structure rather than reveal it.

| Entry | Evidence | Disposition |
| --- | ---: | --- |
| `src/daemon/health.cpp` | `daemon/health.h`: 3 refs | Singleton decision logic. |
| `src/mining/midstate_cache.cpp` | `mining/midstate_cache.h`: 2 refs | Narrow utility; no cohort left. |
| `src/mining/mining_script_override.cpp` | `mining/mining_script_override.h`: 2 external refs | Global override shim; no cohort. |
| `src/daemon/hd_wallet_manager.cpp` | local header only | Singleton wallet glue. |
| `src/daemon/genesis_init.cpp` | `daemon/genesis_init.hpp`: 6 refs | Genesis singleton. |
| `src/daemon/transaction_pool.cpp` | `transaction_pool.h`: 3 refs; local wrapper: 1 ref | Singleton leftover. |
| `src/cli/version.cpp` | `cli/version.h`: 5 refs | Version singleton. |
| `src/daemon/lightning_stubs.cpp` | no public refs found | Stub file; target source remains explicit. |
| `src/common/json_logger.cpp` | `common/json_logger.h`: 3 refs | Logger-adjacent singleton. |
| `src/common/logger_router.cpp` | `common/logger_router.h`: 3 refs | Logger-adjacent singleton. |
| `src/common/config_manager.cpp` | `common/config_manager.h`: 7 refs | Config singleton. |
| `src/util/ser.cpp` | `util/ser.h`: 4 refs | Serialization utility singleton. |

## Already Extracted Cohorts

These source-list variables are the mechanically extracted cohorts that remain
inside the root `dinero_core` target.

| Variable | Cohort |
| --- | --- |
| `DINERO_CORE_IPC_ORACLE_SOURCES` | IPC oracle clients |
| `DINERO_CORE_STORAGE_GLUE_SOURCES` | Chainstate restart metadata persistence |
| `DINERO_CORE_METRICS_SOURCES` | Metrics sources |
| `DINERO_CORE_POLICY_SOURCES` | Policy sources |
| `DINERO_CORE_ADDRESS_SOURCES` | Address handling |
| `DINERO_CORE_PRIVACY_SOURCES` | Privacy sources |
| `DINERO_CORE_DATABASE_SOURCES` | Database utilities |
| `DINERO_CORE_CRYPTO_UTILITY_SOURCES` | Crypto utilities |
| `DINERO_CORE_WEBSOCKET_SOURCES` | WebSocket state/event helpers |
| `DINERO_CORE_P2P_SERVICE_SOURCES` | P2P service wrappers |
| `DINERO_CORE_MINING_TELEMETRY_SOURCES` | Mining telemetry |
| `DINERO_CORE_ENCODING_SOURCES` | Bech32/P2MR encoding |
| `DINERO_CORE_MINING_TEMPLATE_SUPPORT_SOURCES` | Mining template support |
| `DINERO_CORE_MEMPOOL_SUPPORT_SOURCES` | Mempool support |
| `DINERO_CORE_DAEMON_EDGE_SERVICE_SOURCES` | Daemon edge services |
| `DINERO_CORE_P2P_SOURCES` | P2P primitive layer, defined in `src/p2p/CMakeLists.txt` |

## Recommended Next Steps

1. Do not move load-bearing centers casually.
2. Treat the consensus cluster as a separate future project.
3. Create a separate RPC plan before moving `rpc_registry` or RPC-adjacent handlers.
4. Create a service-boundary plan before moving public service hubs.
5. Improve and land a fuller test workflow before moving `dinero_core` target ownership.
6. Only after the graph and tests support it, consider splitting `dinero_core` into smaller libraries.
