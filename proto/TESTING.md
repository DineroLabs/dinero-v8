# gRPC Server Testing Guide

## Overview

The gRPC server is now fully integrated into dinerod and will start automatically when the daemon launches.

## Integration Details

- **Service Location**: `src/grpc/grpc_server.cpp`
- **Default Port**: `50051` (configurable via `--grpcport=PORT`)
- **Lifecycle**: Started in `DaemonApp::Start()`, stopped in `DaemonApp::Stop()`
- **Dependencies**: ChainDB, Mempool, FeeEstimator

## Starting the Daemon

The gRPC server starts automatically with dinerod:

```bash
# Start with default port (50051)
./bin/dinerod

# Start with custom port
./bin/dinerod --grpcport=50052
```

Expected output:
```
[DaemonApp] Initializing gRPC server for inter-daemon communication...
[DaemonApp] ✅ gRPC server listening on 127.0.0.1:50051
[DaemonApp]    Services: dinerod.Blockchain, dinerod.Mempool
```

## Testing with grpcurl

### Installation

```bash
# macOS
brew install grpcurl

# Linux
go install github.com/fullstorydev/grpcurl/cmd/grpcurl@latest
```

### List Available Services

```bash
grpcurl -plaintext localhost:50051 list
```

Expected output:
```
dinerod.Blockchain
dinerod.Mempool
grpc.reflection.v1alpha.ServerReflection
```

### List Methods for a Service

```bash
# Blockchain service methods
grpcurl -plaintext localhost:50051 list dinerod.Blockchain

# Mempool service methods
grpcurl -plaintext localhost:50051 list dinerod.Mempool
```

### Test Blockchain Service

```bash
# Get current block height
grpcurl -plaintext -d '{}' localhost:50051 dinerod.Blockchain/GetBlockHeight

# Get block hash at height 0 (genesis)
grpcurl -plaintext -d '{"height": 0}' localhost:50051 dinerod.Blockchain/GetBlockHash

# Get block by height
grpcurl -plaintext -d '{"height": 0}' localhost:50051 dinerod.Blockchain/GetBlock
```

### Test Mempool Service

```bash
# Get mempool info
grpcurl -plaintext -d '{}' localhost:50051 dinerod.Mempool/GetMempoolInfo

# Estimate fee for 6 block confirmation
grpcurl -plaintext -d '{"target_blocks": 6}' localhost:50051 dinerod.Mempool/EstimateFee

# Check if transaction is in mempool (example txid - will return false if not found)
grpcurl -plaintext -d '{
  "txid": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
}' localhost:50051 dinerod.Mempool/IsInMempool
```

### Broadcast Transaction Example

```bash
# Broadcast raw transaction (hex-encoded, base64 wrapped for protobuf)
grpcurl -plaintext -d '{
  "raw_tx": "<base64-encoded-transaction>"
}' localhost:50051 dinerod.Mempool/BroadcastTransaction
```

## Lightning Integration (Future)

Once `lightningd` is decoupled, it will connect to this gRPC server:

```cpp
// In lightningd
auto channel = grpc::CreateChannel("127.0.0.1:50051", grpc::InsecureChannelCredentials());
auto blockchain_stub = dinerod::Blockchain::NewStub(channel);
auto mempool_stub = dinerod::Mempool::NewStub(channel);

// Query blockchain
dinerod::EmptyRequest request;
dinerod::BlockHeightResponse response;
grpc::ClientContext context;

auto status = blockchain_stub->GetBlockHeight(&context, request, &response);
if (status.ok()) {
    std::cout << "Current height: " << response.height() << std::endl;
}
```

## Configuration Options

Add to command-line or config file:

```bash
# Disable gRPC server (not recommended if running Lightning)
# Currently always enabled - add flag in future if needed

# Change gRPC port
--grpcport=50052
```

## Troubleshooting

### Port Already in Use

```
[DaemonApp] ⚠️  Failed to start gRPC server (non-fatal)
```

Solution: Change the port with `--grpcport=DIFFERENT_PORT`

### Services Not Available

```
[DaemonApp] ⚠️  Core services not initialized, gRPC server disabled
```

Solution: Check that ChainDB and Mempool services started successfully

### No gRPC Output

If you don't see gRPC startup messages, check:
1. Build includes gRPC: `nm bin/dinerod | grep GrpcServer`
2. Required services initialized: Check for ChainDB and Mempool in logs

## Implementation Status

- ✅ **Phase 2 Complete**: gRPC server fully integrated into dinerod
- ✅ **BlockchainService**: All 7 methods implemented (GetBlock, GetBlockHeight, GetBlockHash, GetConfirmationCount, GetUTXO, GetUTXOs, GetTransaction)
- ✅ **MempoolService**: All 5 methods implemented (BroadcastTransaction, EstimateFee, IsInMempool, GetMempoolTransaction, GetMempoolInfo)
- ✅ **Lifecycle Management**: Automatic startup/shutdown with daemon
- ⏳ **Next Phase**: Decouple Lightning into standalone daemon using gRPC API

## Security Notes

**IMPORTANT**: The current implementation uses `InsecureServerCredentials` for local-only communication.

For production deployments:
1. Add TLS/mTLS authentication (see TODO in `grpc_server.cpp:45`)
2. Implement access control and authentication
3. Consider binding to Unix domain socket instead of TCP for local-only access
4. Add rate limiting and DoS protection

## Proto Files

- **API Definition**: `proto/dinerod.proto`
- **Documentation**: `proto/README.md`
- **Generated Code**: Automatically generated during build
