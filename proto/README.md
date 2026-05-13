# DineroCoin gRPC API

**Purpose**: Inter-daemon communication protocol between `dinerod` (core blockchain) and `lightningd` (Lightning Network)

**Status**: Phase 1 Complete
**Version**: 1.0

---

## Overview

This directory contains the Protocol Buffer (protobuf) definitions for the gRPC API that enables communication between DineroCoin daemons.

### Architecture

```
┌──────────────┐         gRPC/TCP          ┌──────────────┐
│   dinerod    │◄────── :50051 ─────────►│  lightningd  │
│              │                          │              │
│ gRPC Server  │   Blockchain queries     │ gRPC Client  │
│              │   Transaction broadcast  │              │
│              │   Event streaming        │              │
└──────────────┘                          └──────────────┘
```

---

## Services

### 1. Blockchain Service

**Purpose**: Query blockchain state (blocks, transactions, UTXOs)

**Methods**:
- `GetBlock` - Retrieve block by height or hash
- `GetBlockHeight` - Get current chain height
- `GetBlockHash` - Get block hash at height
- `GetConfirmationCount` - Check transaction confirmations
- `GetUTXO` - Retrieve unspent output
- `GetUTXOs` - Batch UTXO retrieval
- `GetTransaction` - Get raw transaction

**Lightning Use Case**: Channel funding verification, confirmation tracking

---

### 2. Mempool Service

**Purpose**: Broadcast transactions and query mempool state

**Methods**:
- `BroadcastTransaction` - Send transaction to network
- `EstimateFee` - Get current fee estimates
- `IsInMempool` - Check if transaction is pending
- `GetMempoolTransaction` - Get mempool tx details
- `GetMempoolInfo` - Mempool statistics

**Lightning Use Case**: Broadcast commitment/HTLC timeout transactions, fee estimation for on-chain operations

---

### 3. Events Service

**Purpose**: Real-time event streaming (server-side push)

**Methods**:
- `SubscribeBlocks` - Stream new blocks as they're mined
- `SubscribeTransactions` - Stream relevant transactions
- `SubscribeReorgs` - Blockchain reorganization events
- `SubscribeMempoolEvents` - Mempool add/remove events

**Lightning Use Case**: 
- Detect channel funding confirmations
- Monitor for breach (old commitment broadcasts)
- Handle reorgs (critical for HTLC safety)

---

### 4. Wallet Service (Optional)

**Purpose**: Basic wallet operations

**Methods**:
- `GetNewAddress` - Generate new address
- `SignRawTransaction` - Sign transactions
- `GetBalance` - Query wallet balance

**Lightning Use Case**: On-chain transaction signing, cooperative closes

---

## Message Types

### Block Data
- `GetBlockRequest` / `GetBlockResponse`
- `BlockEvent` (streaming)

### Transaction Data
- `TxIdRequest`
- `TransactionResponse`
- `TransactionEvent` (streaming)

### UTXO Data
- `OutPointRequest` / `UTXOResponse`
- `OutPointsRequest` / `UTXOsResponse` (batch)

### Mempool
- `RawTxRequest` / `TxBroadcastResponse`
- `FeeEstimateRequest` / `FeeEstimateResponse`
- `MempoolInfoResponse`

### Events
- `BlockEvent` - New block notification
- `TransactionEvent` - Transaction state change
- `ReorgEvent` - Chain reorganization
- `MempoolEvent` - Mempool updates

---

## Compilation

### Requirements
- `protoc` (Protocol Buffers compiler)
- `grpc_cpp_plugin` (gRPC C++ code generator)

### Generate C++ Code

```bash
# Install dependencies (macOS)
brew install protobuf grpc

# Generate C++ code
protoc --cpp_out=. \
       --grpc_out=. \
       --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
       dinerod.proto

# Output files:
# - dinerod.pb.h       (protobuf messages)
# - dinerod.pb.cc      (protobuf implementation)
# - dinerod.grpc.pb.h  (gRPC service definitions)
# - dinerod.grpc.pb.cc (gRPC service implementation)
```

### CMake Integration

```cmake
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

add_library(dinerod_proto
  proto/dinerod.proto
)

target_link_libraries(dinerod_proto
  protobuf::libprotobuf
  gRPC::grpc++
)

protobuf_generate(TARGET dinerod_proto LANGUAGE cpp)
protobuf_generate(TARGET dinerod_proto LANGUAGE grpc
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
)
```

---

## Usage Examples

### Server (dinerod)

```cpp
#include "proto/dinerod.grpc.pb.h"
#include <grpcpp/grpcpp.h>

using grpc::Server;
using grpc::ServerBuilder;

class BlockchainServiceImpl final : public dinerod::Blockchain::Service {
  grpc::Status GetBlock(grpc::ServerContext* context,
                        const dinerod::GetBlockRequest* request,
                        dinerod::GetBlockResponse* response) override {
    // Implementation
    return grpc::Status::OK;
  }
};

int main() {
  BlockchainServiceImpl service;

  ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:50051", grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on 127.0.0.1:50051" << std::endl;
  server->Wait();
}
```

### Client (lightningd)

```cpp
#include "proto/dinerod.grpc.pb.h"
#include <grpcpp/grpcpp.h>

using grpc::Channel;
using grpc::ClientContext;

class DinerodClient {
public:
  DinerodClient(std::shared_ptr<Channel> channel)
    : stub_(dinerod::Blockchain::NewStub(channel)) {}

  uint64_t GetBlockHeight() {
    dinerod::EmptyRequest request;
    dinerod::BlockHeightResponse response;
    ClientContext context;

    grpc::Status status = stub_->GetBlockHeight(&context, request, &response);
    if (!status.ok()) {
      return 0;
    }
    return response.height();
  }

private:
  std::unique_ptr<dinerod::Blockchain::Stub> stub_;
};

int main() {
  auto channel = grpc::CreateChannel(
    "127.0.0.1:50051",
    grpc::InsecureChannelCredentials()
  );

  DinerodClient client(channel);
  uint64_t height = client.GetBlockHeight();
  std::cout << "Block height: " << height << std::endl;
}
```

### Event Streaming

```cpp
// Server: Stream new blocks
grpc::Status SubscribeBlocks(ServerContext* context,
                              const dinerod::EmptyRequest* request,
                              grpc::ServerWriter<dinerod::BlockEvent>* writer) override {
  while (true) {
    Block block = waitForNewBlock();  // Blocking call

    dinerod::BlockEvent event;
    event.set_hash(block.hash);
    event.set_height(block.height);
    
    if (!writer->Write(event)) {
      break;  // Client disconnected
    }
  }
  return grpc::Status::OK;
}

// Client: Subscribe to blocks
void SubscribeToBlocks() {
  dinerod::EmptyRequest request;
  ClientContext context;

  std::unique_ptr<grpc::ClientReader<dinerod::BlockEvent>> reader(
    stub_->SubscribeBlocks(&context, request)
  );

  dinerod::BlockEvent event;
  while (reader->Read(&event)) {
    std::cout << "New block: " << event.height() << std::endl;
    handleNewBlock(event);
  }
}
```

---

## Performance Considerations

### Latency
- **Unix Domain Sockets**: ~50μs (recommended for local daemons)
- **TCP Localhost**: ~100μs
- **Remote TCP**: Network-dependent

### Configuration
```cpp
// Use Unix socket instead of TCP for lower latency
builder.AddListeningPort("unix:///tmp/dinerod.sock", 
                          grpc::InsecureServerCredentials());
```

### Batching
For operations that query multiple items, use batch methods:
- `GetUTXOs` instead of multiple `GetUTXO` calls

---

## Error Handling

### gRPC Status Codes
```cpp
if (!status.ok()) {
  switch (status.error_code()) {
    case grpc::StatusCode::NOT_FOUND:
      // Block/transaction not found
      break;
    case grpc::StatusCode::INVALID_ARGUMENT:
      // Bad request parameters
      break;
    case grpc::StatusCode::UNAVAILABLE:
      // dinerod offline or unreachable
      break;
    default:
      // Other error
      break;
  }
}
```

### Reconnection
```cpp
class DinerodClient {
  void ensureConnected() {
    if (channel_->GetState(false) != GRPC_CHANNEL_READY) {
      channel_ = grpc::CreateChannel(...);
      stub_ = dinerod::Blockchain::NewStub(channel_);
    }
  }
};
```

---

## Testing

### Manual Testing with grpcurl

```bash
# Install grpcurl
brew install grpcurl

# List services
grpcurl -plaintext localhost:50051 list

# List methods
grpcurl -plaintext localhost:50051 list dinerod.Blockchain

# Call method
grpcurl -plaintext -d '{}' localhost:50051 dinerod.Blockchain/GetBlockHeight

# Stream blocks
grpcurl -plaintext -d '{}' localhost:50051 dinerod.Events/SubscribeBlocks
```

### Unit Testing

```cpp
#include <gtest/gtest.h>
#include "proto/dinerod.grpc.pb.h"

TEST(BlockchainService, GetBlock) {
  // Mock or test service implementation
  BlockchainServiceImpl service;
  
  dinerod::GetBlockRequest request;
  request.set_height(0);  // Genesis block
  
  dinerod::GetBlockResponse response;
  grpc::ServerContext context;
  
  grpc::Status status = service.GetBlock(&context, &request, &response);
  
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(response.found());
  ASSERT_EQ(response.height(), 0);
}
```

---

## Security

### Current: Insecure (Development Only)
```cpp
grpc::InsecureServerCredentials()  // No TLS, no auth
```

### Production: TLS + mTLS

```cpp
// Server
grpc::SslServerCredentialsOptions ssl_opts;
ssl_opts.pem_root_certs = ReadFile("ca.pem");
ssl_opts.pem_key_cert_pairs.push_back({
  ReadFile("server-key.pem"),
  ReadFile("server-cert.pem")
});

auto creds = grpc::SslServerCredentials(ssl_opts);
builder.AddListeningPort("0.0.0.0:50051", creds);

// Client
grpc::SslCredentialsOptions ssl_opts;
ssl_opts.pem_root_certs = ReadFile("ca.pem");
ssl_opts.pem_private_key = ReadFile("client-key.pem");
ssl_opts.pem_cert_chain = ReadFile("client-cert.pem");

auto creds = grpc::SslCredentials(ssl_opts);
auto channel = grpc::CreateChannel("dinerod.example.com:50051", creds);
```

---

## API Versioning

### Version 1.0 (Current)
- All services marked stable
- Breaking changes require major version bump

### Future Versions
- Version in package name: `package dinerod.v2;`
- Server supports multiple versions
- Clients specify version in connection

---

## References

- [gRPC Documentation](https://grpc.io/docs/)
- [Protocol Buffers Guide](https://developers.google.com/protocol-buffers)
- [gRPC C++ Tutorial](https://grpc.io/docs/languages/cpp/quickstart/)
- [Bitcoin Core ZMQ](https://github.com/bitcoin/bitcoin/blob/master/doc/zmq.md) (inspiration)

---

**Maintainer**: DineroCoin Core Team
**Last Updated**: 2025-12-24
**Status**: Phase 1 Complete ✅
