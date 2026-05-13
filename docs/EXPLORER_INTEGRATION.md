# Dinero Explorer API v1 - Integration Guide

## Overview

This guide shows how to integrate the Explorer API v1 with your existing Dinero daemon that uses SQLite storage.

## Architecture

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   HTTP Client   │───▶│  Explorer API    │───▶│ Explorer Index  │
│  (curl, web)    │    │   (REST + WS)    │    │   (SQLite)      │
└─────────────────┘    └──────────────────┘    └─────────────────┘
                                │                        │
                                ▼                        ▼
                       ┌──────────────────┐    ┌─────────────────┐
                       │  WebSocket Bus   │    │ Blockchain DB   │
                       │  (Real-time)     │    │   (SQLite)      │
                       └──────────────────┘    └─────────────────┘
```

## Quick Integration

### 1. Add to your daemon's main.cpp

```cpp
#include "explorer/explorer_server.h"

// In your main() function, after blockchain initialization:
if (!explorer_initialize(datadir.c_str())) {
    dinero::g_logger.error("Failed to initialize Explorer API");
    return 1;
}
dinero::g_logger.info("Explorer API v1 initialized");

// In your shutdown handler:
explorer_shutdown();
```

### 2. Integrate with your HTTP server

In your existing HTTP request handler:

```cpp
#include "explorer/explorer_server.h"

std::string handle_http_request(const std::string& method, const std::string& path, const std::string& query) {
    // Check if it's an Explorer API request
    if (path.substr(0, 8) == "/api/v1/") {
        const char* response = explorer_handle_request(method.c_str(), path.c_str(), query.c_str());
        if (response) {
            return std::string(response);
        }
    }
    
    // Fall back to existing RPC handling
    return handle_rpc_request(method, path, query);
}
```

### 3. Add indexing hooks to blockchain processing

When you process new blocks:

```cpp
// In your block processing code:
void ProcessBlock(const CBlock& block, uint32_t height) {
    // ... existing block processing ...
    
    // Index for Explorer API
    std::string block_hash = block.GetHash().GetHex();
    std::string block_data = EncodeHexBlock(block);
    uint64_t timestamp = block.nTime;
    
    explorer_index_block(height, block_hash.c_str(), block_data.c_str(), timestamp);
    
    // Index transactions
    for (size_t i = 0; i < block.vtx.size(); ++i) {
        const auto& tx = block.vtx[i];
        std::string txid = tx.GetHash().GetHex();
        std::string raw_hex = EncodeHexTx(tx);
        
        explorer_index_transaction(txid.c_str(), raw_hex.c_str(), height, i);
        
        // Index address/UTXO changes
        IndexTransactionOutputs(tx, height, i);
    }
    
    // Broadcast WebSocket update
    explorer_ws_new_block(height, block_hash.c_str(), timestamp);
}
```

### 4. Index address/UTXO changes

```cpp
void IndexTransactionOutputs(const CTransaction& tx, uint32_t height, uint32_t tx_index) {
    std::string txid = tx.GetHash().GetHex();
    
    // Index outputs (new UTXOs)
    for (size_t vout = 0; vout < tx.vout.size(); ++vout) {
        const auto& output = tx.vout[vout];
        
        // Convert scriptPubKey to scripthash
        std::string script_hex = HexStr(output.scriptPubKey);
        std::string scripthash = ExplorerIndex::ScriptToElectrumScripthash(script_hex);
        
        explorer_index_address(scripthash.c_str(), txid.c_str(), vout, 
                              output.nValue, height, false);
    }
    
    // Index inputs (spent UTXOs)
    for (const auto& input : tx.vin) {
        if (input.prevout.IsNull()) continue; // Skip coinbase
        
        // Mark previous output as spent
        std::string prev_txid = input.prevout.hash.GetHex();
        // You'd need to look up the previous output's scripthash
        // explorer_index_address(scripthash.c_str(), prev_txid.c_str(), 
        //                       input.prevout.n, 0, height, true);
    }
}
```

### 5. WebSocket integration

If you want real-time updates, integrate with your existing WebSocket system:

```cpp
// In your mempool processing:
void AddToMempool(const CTransaction& tx) {
    // ... existing mempool logic ...
    
    std::string txid = tx.GetHash().GetHex();
    double fee_rate = CalculateFeeRate(tx);
    
    explorer_ws_new_tx(txid.c_str(), fee_rate);
}

// In your reorg handling:
void HandleReorg(uint32_t from_height, uint32_t to_height, 
                const std::vector<std::string>& detached_blocks,
                const std::vector<std::string>& attached_blocks) {
    // ... existing reorg logic ...
    
    const char** detached = new const char*[detached_blocks.size()];
    for (size_t i = 0; i < detached_blocks.size(); ++i) {
        detached[i] = detached_blocks[i].c_str();
    }
    
    const char** attached = new const char*[attached_blocks.size()];
    for (size_t i = 0; i < attached_blocks.size(); ++i) {
        attached[i] = attached_blocks[i].c_str();
    }
    
    explorer_ws_reorg(from_height, to_height, 
                     detached, detached_blocks.size(),
                     attached, attached_blocks.size());
    
    delete[] detached;
    delete[] attached;
}
```

## CMake Integration

Add to your `CMakeLists.txt`:

```cmake
# Explorer API library
add_library(dinero_explorer
    src/explorer/explorer_index.cpp
    src/explorer/explorer_api.cpp
    src/explorer/explorer_server.cpp
    src/explorer/explorer_websocket.cpp
)

target_include_directories(dinero_explorer PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(dinero_explorer PUBLIC
    sqlite3
    jsoncpp_static
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Link to your daemon
target_link_libraries(dinerod PRIVATE dinero_explorer)

# Tests
if(BUILD_TESTING)
    add_executable(test_explorer_api tests/test_explorer_api.cpp)
    target_link_libraries(test_explorer_api PRIVATE 
        dinero_explorer 
        gtest 
        gtest_main
    )
    add_test(NAME ExplorerAPITest COMMAND test_explorer_api)
endif()
```

## API Endpoints

Once integrated, your daemon will serve:

### Chain Data
- `GET /api/v1/chain/tip` - Current blockchain tip
- `GET /api/v1/block/{hash}` - Block by hash
- `GET /api/v1/block/height/{height}` - Block by height
- `GET /api/v1/blocks?from_height=0&limit=10` - Block list

### Transactions
- `GET /api/v1/tx/{txid}` - Transaction details
- `GET /api/v1/tx/{txid}/hex` - Raw transaction hex

### Addresses
- `GET /api/v1/address/{address}` - Address summary
- `GET /api/v1/address/{address}/utxos` - Address UTXOs
- `GET /api/v1/address/{address}/txs` - Address transaction history

### Mempool
- `GET /api/v1/mempool` - Mempool summary
- `GET /api/v1/mempool/txids` - Mempool transaction IDs

### Stats
- `GET /api/v1/stats/supply` - Supply statistics
- `GET /api/v1/stats/difficulty` - Difficulty info

### Utilities
- `GET /api/v1/search?q=<query>` - Search blocks/txs/addresses
- `GET /api/v1/health` - API health check

### WebSocket
- `ws://localhost:20998/ws/explorer` - Real-time updates

## Testing

```bash
# Build with tests
cmake -DBUILD_TESTING=ON -S . -B build
cmake --build build

# Run Explorer API tests
./build/test_explorer_api

# Test the API endpoints
curl -s localhost:20998/api/v1/chain/tip | jq
curl -s localhost:20998/api/v1/health | jq
curl -s localhost:20998/api/v1/stats/supply | jq
```

## Performance Considerations

1. **Indexing**: The Explorer API creates additional SQLite tables for fast lookups
2. **Caching**: Historical data is cached with long TTL, tip data with short TTL
3. **Pagination**: All list endpoints use cursor-based pagination
4. **Rate Limiting**: Consider adding rate limiting for public deployments

## Database Schema

The Explorer API creates these additional tables in `explorer.db`:

```sql
-- Block summaries for fast listing
CREATE TABLE blocks (
    height INTEGER PRIMARY KEY,
    hash TEXT UNIQUE NOT NULL,
    timestamp INTEGER NOT NULL,
    tx_count INTEGER NOT NULL
);

-- Transaction index for fast lookups
CREATE TABLE transactions (
    txid TEXT PRIMARY KEY,
    height INTEGER NOT NULL,
    tx_index INTEGER NOT NULL,
    raw_hex TEXT NOT NULL
);

-- UTXO index for address queries
CREATE TABLE utxos (
    scripthash TEXT NOT NULL,
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    value INTEGER NOT NULL,
    height INTEGER NOT NULL,
    is_spent INTEGER DEFAULT 0
);

-- Address statistics cache
CREATE TABLE address_stats (
    scripthash TEXT PRIMARY KEY,
    received INTEGER DEFAULT 0,
    sent INTEGER DEFAULT 0,
    tx_count INTEGER DEFAULT 0
);
```

## Production Deployment

For production use:

1. **Enable rate limiting** in your HTTP server
2. **Add authentication** for sensitive endpoints if needed
3. **Monitor database size** - the explorer index will grow with blockchain size
4. **Set up log rotation** for explorer logs
5. **Consider read replicas** for high-traffic deployments

## Troubleshooting

- **Database locked**: Ensure only one daemon instance is running
- **Missing data**: Check that indexing hooks are properly integrated
- **WebSocket not working**: Verify WebSocket server is running on correct port
- **Performance issues**: Check SQLite indexes and consider database optimization

The Explorer API is designed to be lightweight and fast, leveraging your existing SQLite infrastructure for optimal performance.
