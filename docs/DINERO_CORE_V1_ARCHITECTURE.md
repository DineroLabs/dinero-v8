# Dinero Core v1 Architecture

**Version**: 1.0.0  
**Date**: January 2025  
**Status**: Production-Ready Modular Architecture

---

## Overview

Dinero Core v1 represents a complete architectural evolution from a monolithic Bitcoin-like daemon to a **modular, service-oriented blockchain runtime**. This document describes the final architecture state after the Week 1-6 migration to eliminate global state and introduce pluggable consensus.

### Core Principles

1. **Zero Global State**: All services use dependency injection via `DaemonContext`
2. **Modular Consensus**: Consensus logic is abstracted behind `IConsensusEngine` interface
3. **Service Isolation**: Each service can be tested, mocked, or replaced independently
4. **Context-Driven**: All subsystems receive dependencies through `DaemonContext`
5. **Backward Compatible**: Legacy code paths maintained during transition

---

## Service Architecture

### Service Graph

```
DaemonApp
 ├── LoggerService (infrastructure)
 ├── ConfigService (infrastructure)
 ├── ChainstateService (data layer)
 │   ├── Blockchain (SQLite - RPC/legacy)
 │   └── ChainDB (RocksDB - primary storage)
 ├── MempoolService (data layer)
 │   └── Mempool (in-memory transaction pool)
 ├── WalletService (data layer)
 │   └── WalletManager (HD wallet, BIP39/BIP44)
 ├── P2PService (network layer)
 │   └── P2PManager (peer connections, block/transaction relay)
 ├── MiningService (application layer)
 │   ├── Mining (block creation, mining threads)
 │   └── Uses IConsensusEngine for block templates
 ├── RPCService (application layer)
 │   └── HttpRpcServer (JSON-RPC API)
 ├── MetricsService (application layer)
 │   └── MetricsRegistry (Prometheus/JSON export)
 └── IConsensusEngine (consensus layer) ✅ NEW
     └── PowConsensusEngine (default PoW implementation)
```

### Service Lifecycle

1. **Init Phase**: Services are created and initialized in dependency order
   - Infrastructure services first (Logger, Config)
   - Data layer next (Chainstate, Mempool, Wallet)
   - Network layer (P2P)
   - Application layer (Mining, RPC, Metrics)
   - **Consensus engine created after all services initialized**

2. **Start Phase**: Services are started in the same order
   - Each service can start background threads, open connections, etc.

3. **Run Phase**: Services operate independently
   - Services communicate via `DaemonContext` (no direct coupling)

4. **Stop Phase**: Services are stopped in reverse order
   - Ensures clean shutdown without dependency issues

---

## Consensus Layer

### Architecture

The consensus layer is the **most significant architectural addition** in v1. It abstracts all consensus logic behind a clean interface, enabling:

- **Modular consensus**: Swap PoW → PoS → Hybrid without touching other code
- **Testability**: Mock consensus engines for unit tests
- **Extensibility**: Add new consensus mechanisms without refactoring

### IConsensusEngine Interface

```cpp
class IConsensusEngine {
public:
    virtual ~IConsensusEngine() = default;
    
    // Get engine name (e.g., "PoW", "PoS", "Hybrid")
    virtual std::string GetName() const = 0;
    
    // Validate a block against consensus rules
    virtual bool ValidateBlock(const Block& block, const DaemonContext& ctx) = 0;
    
    // Create a new block template for mining/staking
    virtual Block CreateBlockTemplate(const DaemonContext& ctx) = 0;
    
    // Get current difficulty target (compact bits format)
    virtual uint32_t GetCurrentDifficulty(const DaemonContext& ctx) const = 0;
    
    // Check if block hash meets difficulty target
    virtual bool CheckProofOfWork(const std::string& block_hash, uint32_t target_bits) const = 0;
};
```

### PowConsensusEngine Implementation

The default PoW implementation wraps the existing `Mining` class:

- **Block Creation**: Uses `Mining::createBlockTemplate()` internally
- **Validation**: Uses `Mining::validateBlockTemplate()` + PoW hash check
- **Difficulty**: Uses `Mining::getDifficulty()` (ASERT DAA)
- **PoW Check**: Implements SHA-256d hash comparison

### Consensus Flow

```
DaemonApp::Init()
  ├─ Initialize all services
  └─ Create PowConsensusEngine:
      ├─ Get ChainDB from ChainstateService
      ├─ Get Mining from MiningService
      └─ Create engine → ctx_.consensus ✅

MiningService::createBlockTemplate(ctx)
  ├─ Check if ctx.consensus exists
  ├─ YES: Use ctx.consensus->CreateBlockTemplate(ctx) ✅
  └─ NO: Fallback to mining_->createBlockTemplate() (backward compatibility)

RPC Handler (generatetoaddress)
  ├─ Get MiningService from ctx.daemon->mining
  ├─ Call mining->createBlockTemplate(ctx)
  └─ Uses consensus engine automatically ✅
```

### Adding a New Consensus Engine

To add a new consensus engine (e.g., PoS):

1. **Create Implementation**:
   ```cpp
   class PosConsensusEngine : public IConsensusEngine {
       // Implement all virtual methods
   };
   ```

2. **Create Factory Function**:
   ```cpp
   std::unique_ptr<IConsensusEngine> CreatePosConsensusEngine(...) {
       return std::make_unique<PosConsensusEngine>(...);
   }
   ```

3. **Update DaemonApp**:
   ```cpp
   // In DaemonApp::Init(), replace:
   ctx_.consensus = CreatePowConsensusEngine(...);
   // With:
   ctx_.consensus = CreatePosConsensusEngine(...);
   ```

4. **No Other Changes Required**: MiningService, RPC handlers, and all other code automatically use the new engine.

---

## DaemonContext

### Structure

`DaemonContext` is the central dependency injection container:

```cpp
struct DaemonContext {
    // Core services (required)
    std::shared_ptr<LoggerService> logger;
    std::shared_ptr<ConfigService> config;
    std::shared_ptr<ChainstateService> chainstate;
    std::shared_ptr<MempoolService> mempool;
    std::shared_ptr<WalletService> wallet;
    std::shared_ptr<P2PService> p2p;
    std::shared_ptr<RPCService> rpc;
    std::shared_ptr<MiningService> mining;
    std::shared_ptr<MetricsService> metrics;
    
    // Phase 2: Consensus engine (modular consensus layer)
    std::shared_ptr<IConsensusEngine> consensus; ✅
    
    // Optional services
    std::shared_ptr<rpc::EventBus> event_bus;
    std::shared_ptr<bridge::FiatBridgeManager> fiat_bridge;
    std::shared_ptr<MarketplaceManager> marketplace;
    std::shared_ptr<p2p::EscrowManager> escrow;
};
```

### Usage Pattern

Services receive `DaemonContext` during initialization:

```cpp
bool MiningService::Init(DaemonContext& ctx) {
    logger_ = std::dynamic_pointer_cast<LoggerService>(ctx.logger);
    chainstate_ = std::dynamic_pointer_cast<ChainstateService>(ctx.chainstate);
    // ...
}
```

RPC handlers receive `ExecutionContext` (which contains `DaemonContext`):

```cpp
registry.registerHandler("generatetoaddress", [](const ExecutionContext& ctx, ...) {
    auto& mining = ctx.daemon->mining->get();
    Block block = mining.createBlockTemplate(*ctx.daemon); // Uses consensus engine
});
```

---

## Metrics System

### Architecture

Metrics are collected via `MetricsRegistry` and exposed via HTTP endpoint:

- **Prometheus Format**: `GET /metrics` returns Prometheus text format
- **JSON Format**: `GET /metrics?format=json` returns structured JSON

### Mining Metrics (with Labels)

All mining metrics support per-miner labels:

```cpp
metrics::LabelMap labels = {{"miner_id", miner_id_}};
MetricsRegistry::SetMiningHashrate(hashrate, labels);
MetricsRegistry::IncrementMiningBlocksFound(labels);
```

This enables:
- **Multi-miner tracking**: Each miner instance has unique metrics
- **Dashboard visualization**: Grafana can display per-miner statistics
- **Performance analysis**: Compare miner performance side-by-side

---

## Testing Strategy

### TestDaemonContext

`TestDaemonContext` provides isolated test environments:

```cpp
TestDaemonContext test_ctx;
auto ctx = test_ctx.make();
// ctx has Logger, Config, Chainstate, Wallet (all isolated)
```

### Mock Services

Services can be mocked for unit tests:

```cpp
class MockConsensusEngine : public IConsensusEngine {
    Block CreateBlockTemplate(const DaemonContext& ctx) override {
        return Block{}; // Return test block
    }
    // ...
};

DaemonContext test_ctx;
test_ctx.consensus = std::make_shared<MockConsensusEngine>();
```

### Integration Tests

Integration tests verify full service interaction:

- `test_consensus_integration`: Verifies consensus engine integration
- `test_block_assembler_smoke`: Verifies block creation pipeline

---

## Database Architecture

### Three-Layer Model

1. **RocksDB (ChainDB)**: Primary blockchain storage
   - Blocks, headers, UTXO set
   - Fast, persistent, production-grade

2. **SQLite (Blockchain)**: RPC/legacy compatibility layer
   - `block_index`, `chain_state`, `utxo`, `tx_index` tables
   - Used by RPC handlers for queries

3. **SQLite (Wallet)**: Wallet storage
   - HD wallet keys, addresses, transactions
   - BIP39/BIP44 compliant

---

## Extension Guidelines

### Adding a New Service

1. Create service class inheriting from `IService`
2. Implement `Init()`, `Start()`, `Stop()`, `Name()`
3. Add to `DaemonContext` struct
4. Create instance in `DaemonApp::Init()`
5. Add to service initialization loop

### Adding a New Consensus Engine

See "Adding a New Consensus Engine" section above.

### Adding New Metrics

1. Add metric to `MetricsRegistry` (with label support)
2. Update `ExportMetrics()` and `ExportMetricsJSON()`
3. Call from appropriate service (e.g., `MiningService::UpdateTelemetry()`)

---

## Migration History

### Week 1-2: Foundation
- Created `DaemonContext` and `IService` interface
- Migrated Logger and Config services

### Week 3-4: Core Services
- Migrated Chainstate, Mempool, Wallet, P2P services
- Removed bridge patterns for P2P and Wallet

### Week 5: Mining & Metrics
- Migrated MiningService
- Added MetricsService with label support
- Created IConsensusEngine interface

### Week 6: Final Integration
- Added consensus engine to DaemonContext
- MiningService uses consensus engine for block creation
- Removed all bridge patterns (except legacy stubs)

---

## Future Evolution

### Planned Enhancements

1. **Hybrid Consensus**: Mix PoW + PoS for energy efficiency
2. **Delegated Validators**: Stake-based block production
3. **Smart Contracts**: Add execution engine (EVM-compatible)
4. **Multi-Asset Support**: Native token issuance

### Architecture Readiness

The current architecture supports all planned enhancements:

- ✅ **Consensus swapping**: Already implemented
- ✅ **Service isolation**: Enables independent evolution
- ✅ **Context injection**: Supports new dependencies
- ✅ **Metrics**: Ready for new metric types

---

## Conclusion

Dinero Core v1 represents a **framework-grade blockchain runtime**:

- **Modular**: Services can be swapped, mocked, or extended
- **Testable**: Full context isolation enables unit testing
- **Extensible**: New consensus engines, services, and features can be added cleanly
- **Production-Ready**: Zero global state, clean shutdowns, multi-context capability

**This is not just a cryptocurrency daemon — it's a blockchain development platform.**

---

**Document Version**: 1.0.0  
**Last Updated**: January 2025  
**Maintainer**: Dinero Core Development Team

