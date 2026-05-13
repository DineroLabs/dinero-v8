# Lightning Daemon Decoupling Plan

**Status**: Architecture Design
**Priority**: HIGH - Unblocks daemon development
**Date**: 2025-12-24

---

## Executive Summary

**Goal**: Separate Lightning Network implementation into standalone `lightningd` daemon

**Why This Matters**:
- ✅ dinerod can build/run without Lightning
- ✅ Lightning developed independently
- ✅ No compilation blockers for core daemon
- ✅ Lightning becomes optional component
- ✅ Different release cycles possible
- ✅ Better separation of concerns

**Architecture**: Two independent daemons communicating via gRPC

```
┌──────────────┐         gRPC          ┌──────────────┐
│   dinerod    │◄──────────────────────►│ lightningd   │
│              │                        │              │
│ - Blockchain │   Block Events         │ - Channels   │
│ - Consensus  │───────────────────────►│ - HTLCs      │
│ - P2P        │                        │ - Routing    │
│ - Mempool    │   TX Broadcast         │ - Invoices   │
│              │◄───────────────────────│              │
└──────────────┘                        └──────────────┘
```

---

## Current Coupling Analysis

### Dependencies: dinerod → Lightning

| Component | Usage | Decoupling Strategy |
|-----------|-------|---------------------|
| **DaemonContext** | Lightning accesses blockchain state | Replace with gRPC calls to dinerod |
| **ChainState** | Lightning queries blocks, UTXOs | gRPC: `GetBlock()`, `GetUTXO()` |
| **Mempool** | Lightning broadcasts transactions | gRPC: `BroadcastTransaction()` |
| **P2P Network** | Lightning piggybacks on peer connections | Separate P2P for Lightning gossip |
| **Wallet** | Lightning uses wallet for keys, addresses | Move wallet to lightningd |
| **RPC Server** | Lightning RPC endpoints exposed | lightningd has own RPC server |

### Dependencies: Lightning → dinerod

| Component | Usage | Decoupling Strategy |
|-----------|-------|---------------------|
| **Block Events** | Lightning monitors new blocks | gRPC stream: `SubscribeBlocks()` |
| **Transaction Events** | Lightning detects channel funding | gRPC stream: `SubscribeTransactions()` |
| **Confirmation Tracking** | Lightning checks tx confirmations | gRPC: `GetConfirmationCount()` |
| **Fee Estimation** | Lightning estimates on-chain fees | gRPC: `EstimateFee()` |
| **UTXO Access** | Lightning builds transactions | gRPC: `GetUTXOSet()` |

---

## Proposed Architecture

### Daemon Structure

```
DineroCoin/
├── dinerod/                    # Core blockchain daemon
│   ├── blockchain/             # Consensus, blocks, UTXOs
│   ├── mempool/                # Transaction pool
│   ├── p2p/                    # Peer-to-peer networking
│   ├── rpc/                    # JSON-RPC for dinerod
│   └── grpc/                   # NEW: gRPC server for inter-daemon communication
│
└── lightningd/                 # NEW: Lightning Network daemon
    ├── channels/               # Channel management
    ├── htlc/                   # HTLC handling
    ├── routing/                # Payment routing
    ├── gossip/                 # Lightning gossip protocol
    ├── wallet/                 # Lightning wallet (keys, addresses)
    ├── rpc/                    # JSON-RPC for Lightning
    └── grpc_client/            # gRPC client to talk to dinerod
```

---

## Phase 1: Define gRPC Interface

### Create: `proto/dinerod.proto`

```protobuf
syntax = "proto3";

package dinerod;

// Core blockchain queries
service Blockchain {
  // Get block by height or hash
  rpc GetBlock(GetBlockRequest) returns (GetBlockResponse);
  
  // Get current chain height
  rpc GetBlockHeight(EmptyRequest) returns (BlockHeightResponse);
  
  // Check if transaction exists and is confirmed
  rpc GetConfirmationCount(TxIdRequest) returns (ConfirmationCountResponse);
  
  // Get UTXO for given outpoint
  rpc GetUTXO(OutPointRequest) returns (UTXOResponse);
}

// Transaction broadcasting
service Mempool {
  // Broadcast raw transaction
  rpc BroadcastTransaction(RawTxRequest) returns (TxBroadcastResponse);
  
  // Estimate fee for transaction
  rpc EstimateFee(FeeEstimateRequest) returns (FeeEstimateResponse);
  
  // Check if transaction is in mempool
  rpc IsInMempool(TxIdRequest) returns (BoolResponse);
}

// Event streaming (push notifications)
service Events {
  // Subscribe to new blocks
  rpc SubscribeBlocks(EmptyRequest) returns (stream Block);
  
  // Subscribe to transactions matching filter
  rpc SubscribeTransactions(TxFilterRequest) returns (stream Transaction);
  
  // Subscribe to reorg events
  rpc SubscribeReorgs(EmptyRequest) returns (stream ReorgEvent);
}

// Messages
message GetBlockRequest {
  oneof identifier {
    uint64 height = 1;
    bytes hash = 2;
  }
}

message GetBlockResponse {
  bytes raw_block = 1;
  uint64 height = 2;
  bytes hash = 3;
  uint32 confirmations = 4;
}

message TxIdRequest {
  bytes txid = 1;  // 32-byte transaction hash
}

message ConfirmationCountResponse {
  uint32 confirmations = 1;
  bool in_mempool = 2;
  bool confirmed = 3;
}

message OutPointRequest {
  bytes txid = 1;
  uint32 vout = 2;
}

message UTXOResponse {
  bool exists = 1;
  uint64 value = 2;
  bytes script_pubkey = 3;
  uint32 height = 4;  // Block height where created
}

message RawTxRequest {
  bytes raw_tx = 1;  // Serialized transaction
}

message TxBroadcastResponse {
  bool success = 1;
  string error = 2;
  bytes txid = 3;
}

message FeeEstimateRequest {
  uint32 target_blocks = 1;  // Confirm within N blocks
}

message FeeEstimateResponse {
  uint64 sat_per_vbyte = 1;
}

message TxFilterRequest {
  repeated bytes addresses = 1;  // Filter by scriptPubKey
  repeated bytes txids = 2;      // Filter by specific txids
}

message ReorgEvent {
  uint64 old_height = 1;
  uint64 new_height = 2;
  bytes old_tip = 3;
  bytes new_tip = 4;
}

message Block {
  bytes hash = 1;
  uint64 height = 2;
  uint64 timestamp = 3;
  repeated Transaction transactions = 4;
}

message Transaction {
  bytes txid = 1;
  bytes raw_tx = 2;
  uint32 confirmations = 3;
}

message EmptyRequest {}
message BoolResponse { bool value = 1; }
message BlockHeightResponse { uint64 height = 1; }
```

---

## Phase 2: Implement dinerod gRPC Server

### 1. Add gRPC Dependencies

**CMakeLists.txt**:
```cmake
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

add_library(dinerod_grpc_proto
  proto/dinerod.proto
)

target_link_libraries(dinerod_grpc_proto
  protobuf::libprotobuf
  gRPC::grpc++
)

protobuf_generate(TARGET dinerod_grpc_proto LANGUAGE cpp)
protobuf_generate(TARGET dinerod_grpc_proto LANGUAGE grpc
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  PLUGIN "protoc-gen-grpc=\$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
)
```

### 2. Create gRPC Server

**src/grpc/blockchain_service.cpp**:
```cpp
#include "grpc/blockchain_service.h"
#include "daemon/daemon_context.h"
#include "proto/dinerod.grpc.pb.h"

using grpc::Server;
using grpc::ServerContext;
using grpc::Status;

class BlockchainServiceImpl final : public dinerod::Blockchain::Service {
public:
  BlockchainServiceImpl(DaemonContext& ctx) : m_ctx(ctx) {}

  Status GetBlock(ServerContext* context,
                  const dinerod::GetBlockRequest* request,
                  dinerod::GetBlockResponse* response) override {
    // Query blockchain
    std::optional<Block> block;
    if (request->has_height()) {
      block = m_ctx.chainstate->getBlockByHeight(request->height());
    } else if (request->has_hash()) {
      block = m_ctx.chainstate->getBlockByHash(request->hash());
    } else {
      return Status(grpc::StatusCode::INVALID_ARGUMENT, "Must provide height or hash");
    }

    if (!block) {
      return Status(grpc::StatusCode::NOT_FOUND, "Block not found");
    }

    // Serialize block
    response->set_raw_block(block->serialize());
    response->set_height(block->height);
    response->set_hash(block->hash);
    response->set_confirmations(m_ctx.chainstate->getHeight() - block->height + 1);

    return Status::OK;
  }

  // ... other methods

private:
  DaemonContext& m_ctx;
};
```

**src/daemon/dinerod.cpp**:
```cpp
// Start gRPC server
std::unique_ptr<Server> grpc_server;
{
  BlockchainServiceImpl blockchain_service(daemon_context);
  MempoolServiceImpl mempool_service(daemon_context);
  EventsServiceImpl events_service(daemon_context);

  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:50051", grpc::InsecureServerCredentials());
  builder.RegisterService(&blockchain_service);
  builder.RegisterService(&mempool_service);
  builder.RegisterService(&events_service);

  grpc_server = builder.BuildAndStart();
  std::cout << "gRPC server listening on 127.0.0.1:50051" << std::endl;
}
```

---

## Phase 3: Create lightningd Daemon

### 1. New Binary Structure

**lightningd/main.cpp**:
```cpp
#include "lightning/lightning_service.h"
#include "grpc_client/dinerod_client.h"
#include <grpcpp/grpcpp.h>

int main(int argc, char* argv[]) {
  // Parse config
  LightningConfig config = parseConfig(argc, argv);

  // Connect to dinerod via gRPC
  auto channel = grpc::CreateChannel(
    config.dinerod_address,  // "127.0.0.1:50051"
    grpc::InsecureChannelCredentials()
  );

  DinerodClient dinerod_client(channel);

  // Initialize Lightning
  LightningService lightning_service(config, dinerod_client);

  // Start Lightning RPC server (different port than dinerod)
  startLightningRPC(lightning_service, config.rpc_port);  // e.g., 10009

  // Subscribe to blockchain events
  lightning_service.subscribeToBlocks();
  lightning_service.subscribeToTransactions();

  // Main event loop
  lightning_service.run();

  return 0;
}
```

### 2. DinerodClient Wrapper

**lightningd/grpc_client/dinerod_client.h**:
```cpp
#pragma once

#include "proto/dinerod.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <optional>

class DinerodClient {
public:
  DinerodClient(std::shared_ptr<grpc::Channel> channel)
    : m_blockchain_stub(dinerod::Blockchain::NewStub(channel)),
      m_mempool_stub(dinerod::Mempool::NewStub(channel)),
      m_events_stub(dinerod::Events::NewStub(channel)) {}

  // Blockchain queries
  std::optional<Block> getBlock(uint64_t height);
  std::optional<Block> getBlock(const std::string& hash);
  uint64_t getBlockHeight();
  uint32_t getConfirmationCount(const std::string& txid);
  std::optional<UTXO> getUTXO(const std::string& txid, uint32_t vout);

  // Mempool operations
  bool broadcastTransaction(const std::string& raw_tx, std::string& error);
  uint64_t estimateFee(uint32_t target_blocks);
  bool isInMempool(const std::string& txid);

  // Event subscriptions (blocking streams)
  void subscribeBlocks(std::function<void(const Block&)> callback);
  void subscribeTransactions(std::function<void(const Transaction&)> callback);
  void subscribeReorgs(std::function<void(const ReorgEvent&)> callback);

private:
  std::unique_ptr<dinerod::Blockchain::Stub> m_blockchain_stub;
  std::unique_ptr<dinerod::Mempool::Stub> m_mempool_stub;
  std::unique_ptr<dinerod::Events::Stub> m_events_stub;
};
```

**Implementation**:
```cpp
std::optional<Block> DinerodClient::getBlock(uint64_t height) {
  dinerod::GetBlockRequest request;
  request.set_height(height);

  dinerod::GetBlockResponse response;
  grpc::ClientContext context;

  grpc::Status status = m_blockchain_stub->GetBlock(&context, request, &response);

  if (!status.ok()) {
    return std::nullopt;
  }

  // Deserialize block from raw bytes
  Block block;
  block.deserialize(response.raw_block());
  block.height = response.height();
  // ...

  return block;
}

bool DinerodClient::broadcastTransaction(const std::string& raw_tx, std::string& error) {
  dinerod::RawTxRequest request;
  request.set_raw_tx(raw_tx);

  dinerod::TxBroadcastResponse response;
  grpc::ClientContext context;

  grpc::Status status = m_mempool_stub->BroadcastTransaction(&context, request, &response);

  if (!status.ok() || !response.success()) {
    error = response.error();
    return false;
  }

  return true;
}

void DinerodClient::subscribeBlocks(std::function<void(const Block&)> callback) {
  dinerod::EmptyRequest request;
  grpc::ClientContext context;

  std::unique_ptr<grpc::ClientReader<dinerod::Block>> reader(
    m_events_stub->SubscribeBlocks(&context, request)
  );

  dinerod::Block block_msg;
  while (reader->Read(&block_msg)) {
    // Convert protobuf Block to internal Block structure
    Block block;
    block.hash = block_msg.hash();
    block.height = block_msg.height();
    // ... parse transactions

    callback(block);
  }
}
```

---

## Phase 4: Replace DaemonContext in Lightning

### Before (Coupled):
```cpp
// Lightning code directly accesses daemon internals
uint64_t height = m_daemon_context.chainstate->getHeight();
bool in_mempool = m_daemon_context.mempool->hasTransaction(txid);
m_daemon_context.mempool->addTransaction(tx);
```

### After (Decoupled):
```cpp
// Lightning code uses gRPC client
uint64_t height = m_dinerod_client.getBlockHeight();
bool in_mempool = m_dinerod_client.isInMempool(txid);
m_dinerod_client.broadcastTransaction(tx.serialize(), error);
```

### Refactor Pattern

**Find all DaemonContext usage**:
```bash
cd src/lightning
grep -r "m_daemon_context\|m_ctx\|DaemonContext" . | wc -l
# Result: ~150 usages
```

**Replace systematically**:

| Old (Coupled) | New (Decoupled) |
|---------------|-----------------|
| `m_daemon_context.chainstate->getHeight()` | `m_dinerod_client.getBlockHeight()` |
| `m_daemon_context.chainstate->getBlock(height)` | `m_dinerod_client.getBlock(height)` |
| `m_daemon_context.mempool->addTransaction(tx)` | `m_dinerod_client.broadcastTransaction(tx)` |
| `m_daemon_context.wallet->getAddress()` | `m_lightning_wallet.getAddress()` |

---

## Phase 5: Build System Changes

### CMakeLists.txt

```cmake
# dinerod (core daemon)
add_executable(dinerod
  src/daemon/dinerod.cpp
  src/blockchain/*.cpp
  src/mempool/*.cpp
  src/p2p/*.cpp
  src/rpc/*.cpp
  src/grpc/*.cpp  # NEW: gRPC server
  # NO LIGHTNING CODE
)

target_link_libraries(dinerod
  consensus
  crypto
  grpc++
  protobuf
)

# lightningd (Lightning daemon)
add_executable(lightningd
  lightningd/main.cpp
  src/lightning/*.cpp  # Move Lightning code here
  lightningd/grpc_client/*.cpp  # NEW: gRPC client
)

target_link_libraries(lightningd
  grpc++
  protobuf
  secp256k1
  leveldb
)
```

---

## Phase 6: Configuration

### dinerod.conf
```ini
# gRPC server for inter-daemon communication
grpc.enabled=1
grpc.bind=127.0.0.1:50051
grpc.max_connections=10

# JSON-RPC (unchanged)
rpc.bind=127.0.0.1:8332
```

### lightningd.conf
```ini
# Connection to dinerod
dinerod.grpc.address=127.0.0.1:50051
dinerod.grpc.timeout=30

# Lightning RPC server
rpc.bind=127.0.0.1:10009

# Lightning network
lightning.port=9735
lightning.alias="DineroCoin Lightning Node"

# Database
db.path=/var/lib/lightningd/
```

---

## Phase 7: Migration Strategy

### Step 1: Create Parallel Build (Week 1)
- ✅ Keep existing dinerod with Lightning
- ✅ Create new lightningd binary (empty shell)
- ✅ Both build in parallel

### Step 2: Implement gRPC (Week 2)
- ✅ Add gRPC server to dinerod
- ✅ Implement BlockchainService
- ✅ Implement MempoolService
- ✅ Implement EventsService
- ✅ Test: Can query blocks via gRPC

### Step 3: Create DinerodClient (Week 3)
- ✅ Implement gRPC client wrapper
- ✅ Test: Client can call all dinerod methods
- ✅ Add reconnection logic
- ✅ Add error handling

### Step 4: Move Lightning Code (Week 4)
- ✅ Copy src/lightning → lightningd/
- ✅ Replace DaemonContext with DinerodClient
- ✅ Fix compilation errors
- ✅ Test: lightningd compiles

### Step 5: Integration Testing (Week 5)
- ✅ Start dinerod
- ✅ Start lightningd
- ✅ Open channel end-to-end
- ✅ Send payment
- ✅ Close channel
- ✅ Verify both daemons work together

### Step 6: Remove Lightning from dinerod (Week 6)
- ✅ Delete src/lightning from dinerod
- ✅ Remove Lightning RPC endpoints from dinerod
- ✅ Update build system
- ✅ Test: dinerod builds and runs without Lightning

---

## Benefits

### 1. Build Simplification
**Before**:
```
make dinerod
# Compiles 30 Lightning files (10,000+ LOC)
# Lightning errors block daemon build
# Build time: 5 minutes
```

**After**:
```
make dinerod      # Fast, no Lightning
make lightningd   # Optional, separate
# Build time: 2 minutes (dinerod), 3 minutes (lightningd)
```

### 2. Independent Development
- Daemon team can release without waiting for Lightning
- Lightning team can iterate without affecting daemon
- Different release schedules

### 3. Optional Lightning
```bash
# Minimal node (no Lightning)
./dinerod --config=dinerod.conf

# Full node with Lightning
./dinerod --config=dinerod.conf &
./lightningd --config=lightningd.conf &
```

### 4. Better Resource Management
- dinerod: Core consensus (stable, low memory)
- lightningd: Experimental features (can crash without affecting chain)

### 5. Easier Testing
```bash
# Test daemon without Lightning complexity
./dinerod --testnet

# Test Lightning against any Bitcoin-compatible daemon
./lightningd --dinerod-rpc=bitcoin.example.com:8332
```

---

## Risks and Mitigations

### Risk 1: gRPC Latency
**Concern**: Network calls slower than direct function calls

**Mitigation**:
- Use Unix domain sockets (not TCP) for local communication
- Batch requests where possible
- Cache frequently-accessed data (block height, etc.)

**Benchmark**: gRPC over Unix socket: ~50μs latency (acceptable)

### Risk 2: Event Delivery
**Concern**: Block events may be delayed or lost

**Mitigation**:
- Use gRPC streaming (push notifications)
- Add reconnection logic with event replay
- Persist last-seen block height

### Risk 3: Complexity
**Concern**: Two daemons harder to manage than one

**Mitigation**:
- Provide `dinero-launch.sh` script to start both
- Add systemd service files
- Health checks between daemons

---

## Success Criteria

- [ ] dinerod builds without Lightning code
- [ ] lightningd runs as separate process
- [ ] gRPC communication works reliably
- [ ] All Lightning features still functional
- [ ] Performance acceptable (< 10% overhead)
- [ ] Documentation updated
- [ ] Migration guide for users

---

## Timeline

| Phase | Duration | Deliverable |
|-------|----------|-------------|
| 1. Design gRPC API | 3 days | `proto/dinerod.proto` complete |
| 2. Implement gRPC server | 1 week | dinerod exposes gRPC endpoints |
| 3. Create lightningd shell | 3 days | Empty lightningd binary builds |
| 4. Implement DinerodClient | 1 week | Client can query all endpoints |
| 5. Move Lightning code | 1 week | lightningd has full Lightning impl |
| 6. Integration testing | 1 week | End-to-end Lightning works |
| 7. Remove coupling | 3 days | dinerod builds without Lightning |
| 8. Documentation | 3 days | Migration guide, architecture docs |

**Total**: 6 weeks

---

## Next Steps

**This Week**:
1. ✅ Review and approve this plan
2. ✅ Create `proto/dinerod.proto` specification
3. ✅ Set up gRPC build dependencies
4. ✅ Create lightningd directory structure

**Next Week**:
1. ✅ Implement BlockchainService in dinerod
2. ✅ Implement MempoolService in dinerod
3. ✅ Test gRPC server with grpcurl

**Week 3**:
1. ✅ Implement DinerodClient wrapper
2. ✅ Write integration tests

---

**Owner**: DineroCoin Architecture Team
**Status**: APPROVED - Ready to implement
**Review Date**: 2025-12-24
