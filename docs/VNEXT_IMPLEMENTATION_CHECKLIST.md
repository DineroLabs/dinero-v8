# dinerod vNext Implementation Checklist

## Phase 1: RPC Foundation & Observability

### 🆕 NEW FILES (Add)

#### RPC Registry & Core Methods
- [ ] `include/rpc/rpc_registry.h` - Central RPC method registration
- [ ] `src/rpc/rpc_registry.cpp` - RpcRegistry implementation
- [ ] `src/rpc/methods_core.cpp` - help, getnetworkinfo, getmempoolinfo
- [ ] `src/rpc/methods_mining.cpp` - getblocktemplate, submitblock (stub)

#### HTTP Server & Routes  
- [ ] `include/http/http_server.h` - Boost.Beast HTTP server
- [ ] `src/http/http_server.cpp` - HTTP server implementation
- [ ] `src/http/http_routes.cpp` - /healthz, /metrics, /rpc routes

#### Node Interface & Status
- [ ] `include/node/node.h` - Node facade with subsystem interfaces
- [ ] `include/node/node_status.h` - Health status structures
- [ ] `src/node/node.cpp` - Node implementation with health reporting

#### Configuration & Schema
- [ ] `config/nodeinfo.schema.v1.json` - Deterministic config schema
- [ ] `include/config/nodeinfo_v1.h` - nodeinfo v1 parser
- [ ] `src/config/nodeinfo_v1.cpp` - nodeinfo v1 implementation

#### Logging & Observability
- [ ] `include/logging/structured_logger.h` - JSON logging with trace IDs
- [ ] `src/logging/structured_logger.cpp` - Structured logging implementation
- [ ] `include/metrics/prometheus.h` - Prometheus metrics interface
- [ ] `src/metrics/prometheus.cpp` - Metrics collection & formatting

#### Build & Testing
- [ ] `cmake/Sanitizers.cmake` - ASan/UBSan options for development
- [ ] `tools/smoke.sh` - Smoke test script for Phase 1 endpoints
- [ ] `tests/rpc/test_rpc_basic.cpp` - Unit tests for core RPC methods
- [ ] `tests/http/test_health_metrics.cpp` - Health/metrics endpoint tests

### 🔄 MODIFIED FILES (Upgrade)

#### Main Entry Point
- [ ] `src/main.cpp` - TODO: Wire RpcRegistry, HttpServer, structured logging
- [ ] `src/daemon/daemon.cpp` - TODO: Replace WebSocket RPC with HTTP RPC

#### CMake Build System
- [ ] `CMakeLists.txt` - TODO: Add Boost.Beast, nlohmann/json, RocksDB dependencies
- [ ] `cmake/FindRocksDB.cmake` - TODO: RocksDB detection for Phase 4

#### CLI Integration (Already Done)
- [x] `src/cli/main_new.cpp` - Schema validation, health endpoint integration
- [x] `src/cli/schema_validation.cpp` - RPC schema validation
- [x] `src/cli/health_client.cpp` - Health endpoint client

## Phase 2: Mempool Policy & Mining

### 🆕 NEW FILES (Add)

#### Mempool Policy
- [ ] `include/policy/policy.h` - Standardness, size/weight/sigops checks
- [ ] `src/policy/policy.cpp` - Policy validation implementation
- [ ] `include/policy/rbf.h` - Replace-by-fee logic
- [ ] `src/policy/rbf.cpp` - RBF implementation
- [ ] `include/policy/fees.h` - Fee estimation interface
- [ ] `src/policy/fees.cpp` - EWMA bucket fee estimator

#### Mempool Management
- [ ] `include/mempool/mempool.h` - Policy-aware mempool
- [ ] `src/mempool/mempool.cpp` - Mempool with CPFP-aware eviction
- [ ] `include/mempool/entry.h` - Mempool entry with policy metadata
- [ ] `src/mempool/entry.cpp` - Mempool entry implementation

#### Mining Pipeline
- [ ] `include/mining/block_assembler.h` - Policy-driven block assembly
- [ ] `src/mining/block_assembler.cpp` - BlockAssembler implementation
- [ ] `include/mining/miner.h` - Mining work loop (separate from assembly)
- [ ] `src/mining/miner.cpp` - Miner implementation

### 🔄 MODIFIED FILES (Upgrade)

#### RPC Methods (Expand)
- [ ] `src/rpc/methods_mining.cpp` - TODO: Full getblocktemplate with policy
- [ ] `src/rpc/methods_mempool.cpp` - TODO: Add mempool policy RPCs

#### Node Interface
- [ ] `src/node/node.cpp` - TODO: Wire mempool, fee estimator, block assembler

## Phase 3: P2P Sync & Networking

### 🆕 NEW FILES (Add)

#### P2P Engine
- [ ] `include/p2p/p2p_manager.h` - P2P connection management
- [ ] `src/p2p/p2p_manager.cpp` - P2P manager implementation
- [ ] `include/p2p/peer.h` - Peer connection with scoring
- [ ] `src/p2p/peer.cpp` - Peer implementation with banscore

#### Sync Protocol
- [ ] `include/p2p/headers_sync.h` - Headers-first sync protocol
- [ ] `src/p2p/headers_sync.cpp` - Headers sync implementation
- [ ] `include/p2p/compact_blocks.h` - BIP152-style compact blocks
- [ ] `src/p2p/compact_blocks.cpp` - Compact blocks implementation

#### Address Management
- [ ] `include/p2p/addrman.h` - Address manager with DNS/static seeds
- [ ] `src/p2p/addrman.cpp` - Address manager implementation
- [ ] `include/p2p/seeds.h` - DNS and static seed definitions
- [ ] `src/p2p/seeds.cpp` - Seed node management

### 🔄 MODIFIED FILES (Upgrade)

#### Network Layer
- [ ] `src/net/net.cpp` - TODO: Replace ad-hoc networking with P2P manager
- [ ] `src/net/protocol.cpp` - TODO: Add compact blocks protocol support

## Phase 4: Storage, Wallets & Security

### 🆕 NEW FILES (Add)

#### Storage Interface
- [ ] `include/storage/storage_interface.h` - IBlockIndex, IUTXOSet interfaces
- [ ] `include/storage/chain_db_rocks.h` - RocksDB chainstate backend
- [ ] `src/storage/chain_db_rocks.cpp` - RocksDB implementation
- [ ] `include/storage/pruning.h` - Pruning with target size
- [ ] `src/storage/pruning.cpp` - Pruning implementation
- [ ] `include/storage/snapshots.h` - Snapshot import/export
- [ ] `src/storage/snapshots.cpp` - Snapshot implementation

#### Descriptor Wallets
- [ ] `include/wallet/descriptor_wallet.h` - BIP84 descriptor wallets
- [ ] `src/wallet/descriptor_wallet.cpp` - Descriptor wallet implementation
- [ ] `include/wallet/psbt.h` - PSBT create/fund/sign/finalize
- [ ] `src/wallet/psbt.cpp` - PSBT implementation
- [ ] `include/wallet/labels.h` - Per-address and per-tx labels
- [ ] `src/wallet/labels.cpp` - Label management
- [ ] `include/wallet/multiwallet.h` - Multi-wallet directory management
- [ ] `src/wallet/multiwallet.cpp` - Multi-wallet implementation

#### Security & TLS
- [ ] `include/security/tls_modes.h` - TLS mode definitions (off/loopback/public)
- [ ] `src/security/tls_modes.cpp` - TLS mode implementation
- [ ] `include/security/cookie_auth.h` - Cookie rotation with grace window
- [ ] `src/security/cookie_auth.cpp` - Cookie authentication
- [ ] `include/security/dos_protection.h` - Request limits, slowloris protection
- [ ] `src/security/dos_protection.cpp` - DoS protection implementation

### 🔄 MODIFIED FILES (Upgrade)

#### Storage Migration
- [ ] `src/storage/chain_db_sqlite.cpp` - TODO: Keep for tests, add migration path
- [ ] `src/storage/utxo_db.cpp` - TODO: Abstract interface, RocksDB backend

#### Wallet Migration
- [ ] `src/wallet/wallet.cpp` - TODO: Migrate to descriptor-first model
- [ ] `src/wallet/hd_wallet.cpp` - TODO: Import legacy HD to descriptors

#### HTTP Server Security
- [ ] `src/http/http_server.cpp` - TODO: Add TLS support, request limits
- [ ] `src/http/http_routes.cpp` - TODO: Add authentication middleware

## 🗑️ DEPRECATIONS & REMOVALS

### Files to Remove/Replace
- [ ] `src/websocket/` - TODO: Remove WebSocket control RPCs (keep notifications)
- [ ] `src/rpc/rpc_old.cpp` - TODO: Remove ad-hoc RPC endpoints
- [ ] `src/logging/text_logger.cpp` - TODO: Replace with structured logging

### Code Patterns to Replace
- [ ] Hardcoded wallet names - TODO: Replace with multiwallet
- [ ] Magic values in config - TODO: Use nodeinfo.json v1 schema
- [ ] Ad-hoc error codes - TODO: Use JSON-RPC 2.0 standard codes
- [ ] CLI nodeinfo guessing - TODO: Use explicit nodeinfo.json

## Migration & Compatibility

### Configuration Migration
- [ ] `scripts/migrate-config.sh` - TODO: Generate nodeinfo.json from existing config
- [ ] `docs/MIGRATION_GUIDE.md` - TODO: Step-by-step migration instructions

### Storage Migration
- [ ] `tools/reindex-rocksdb.cpp` - TODO: SQLite → RocksDB migration tool
- [ ] `tools/snapshot-import.cpp` - TODO: Fast IBD via snapshot import

### Wallet Migration
- [ ] `tools/migrate-wallet.cpp` - TODO: Legacy HD → descriptor migration
- [ ] `tools/export-labels.cpp` - TODO: Preserve labels during migration

## Testing Strategy

### Unit Tests (Per Phase)
- [ ] `tests/rpc/` - RPC method coverage with schema validation
- [ ] `tests/http/` - Health/metrics endpoint tests
- [ ] `tests/policy/` - Mempool policy and fee estimation tests
- [ ] `tests/mining/` - BlockAssembler and mining pipeline tests
- [ ] `tests/p2p/` - Headers sync and compact blocks tests
- [ ] `tests/storage/` - RocksDB backend and pruning tests
- [ ] `tests/wallet/` - Descriptor wallets and PSBT tests
- [ ] `tests/security/` - TLS modes and DoS protection tests

### Integration Tests
- [ ] `tests/e2e/test_phase1_acceptance.cpp` - Phase 1 acceptance criteria
- [ ] `tests/e2e/test_phase2_mining.cpp` - Mining pipeline integration
- [ ] `tests/e2e/test_phase3_sync.cpp` - P2P sync integration
- [ ] `tests/e2e/test_phase4_storage.cpp` - Storage and wallet integration

### CI/CD Pipeline
- [x] `.github/workflows/vnext-smoke-tests.yml` - Automated testing pipeline
- [ ] `.github/workflows/migration-tests.yml` - Migration compatibility tests
- [ ] `.github/workflows/performance-tests.yml` - Performance regression tests

## Release Milestones

### Phase 1: dinerod-vNext-phase-1
- [ ] All Phase 1 NEW files implemented
- [ ] RPC methods return `rpc_schema: "din.rpc.v1"`
- [ ] `/healthz` and `/metrics` endpoints live
- [ ] JSON logging enabled by default
- [ ] CLI integration working with health endpoints

### Phase 2: dinerod-vNext-phase-2  
- [ ] Mempool policy and fee estimation working
- [ ] `getblocktemplate`/`submitblock` full implementation
- [ ] External miner compatibility verified

### Phase 3: dinerod-vNext-phase-3
- [ ] Headers-first sync completing on fresh nodes
- [ ] Compact blocks reducing bandwidth
- [ ] Peer banning and scoring functional

### Phase 4: dinerod-vNext-phase-4
- [ ] RocksDB storage stable across restarts
- [ ] Pruning working with target size
- [ ] Snapshot import reducing IBD time
- [ ] PSBT round-trip working with hardware wallets
- [ ] TLS public mode handshake working

## Implementation Priority

1. **Phase 1 Foundation** (Immediate)
   - RPC registry and core methods
   - Health/metrics endpoints  
   - Structured logging
   - CLI integration (already done)

2. **Phase 2 Policy** (Month 2)
   - Mempool policy framework
   - Fee estimation
   - Mining pipeline separation

3. **Phase 3 Networking** (Month 3)
   - P2P engine rewrite
   - Headers-first sync
   - Compact blocks

4. **Phase 4 Infrastructure** (Month 4)
   - RocksDB storage backend
   - Descriptor wallets
   - TLS security modes

This checklist provides concrete TODO anchors for each file and can be copied into GitHub issues/PRs for tracking implementation progress.
