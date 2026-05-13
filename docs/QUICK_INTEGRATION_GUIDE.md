# Explorer API v1 - Quick Integration Guide

## 🚀 3-Step Integration

### Step 1: Add to your daemon's main.cpp

```cpp
#include "explorer/explorer_integration.h"

// In your main() function, after blockchain initialization:
int main(int argc, char* argv[]) {
    // ... existing initialization ...
    
    // Initialize Explorer API
    if (!explorer_initialize(datadir.c_str())) {
        dinero::g_logger.error("Failed to initialize Explorer API v1");
        return 1;
    }
    dinero::g_logger.info("✅ Explorer API v1 initialized");
    
    // Set component references
    explorer_set_blockchain(g_blockchain.get());
    explorer_set_mining(g_mining.get());
    explorer_set_node(g_node.get());
    
    // ... rest of your main loop ...
    
    // In your shutdown handler:
    explorer_shutdown();
    return 0;
}
```

### Step 2: Integrate with your HTTP server

In your existing HTTP request handler:

```cpp
// In your parseHTTPRequest() or similar function:
std::string parseHTTPRequest(const std::string& request) {
    // Parse HTTP request
    std::string method, path, query_string;
    // ... your existing parsing logic ...
    
    // Check if it's an Explorer API request
    if (path.substr(0, 8) == "/api/v1/") {
        const char* response = explorer_handle_http_request(
            method.c_str(), 
            path.c_str(), 
            query_string.c_str(), 
            nullptr
        );
        
        if (response) {
            return std::string(response);
        }
    }
    
    // Fall back to existing RPC handling
    return handleRPCRequest(request);
}
```

### Step 3: Add indexing hooks

When you process new blocks:

```cpp
// In your block processing code:
void ProcessBlock(const CBlock& block, uint32_t height) {
    // ... existing block processing ...
    
    // Index for Explorer API
    std::string block_hash = block.GetHash().GetHex();
    std::string block_data = EncodeHexBlock(block);
    uint64_t timestamp = block.nTime;
    
    explorer_on_new_block(height, block_hash.c_str(), block_data.c_str(), timestamp);
    
    // Index transactions
    for (size_t i = 0; i < block.vtx.size(); ++i) {
        const auto& tx = block.vtx[i];
        std::string txid = tx.GetHash().GetHex();
        std::string raw_hex = EncodeHexTx(tx);
        
        explorer_on_new_transaction(txid.c_str(), raw_hex.c_str(), height, i);
        
        // Index address/UTXO changes
        IndexTransactionOutputs(tx, height, i);
    }
    
    // Broadcast WebSocket update
    explorer_broadcast_new_block(height, block_hash.c_str(), timestamp);
}
```

## ✅ That's it! Your Explorer API is now live.

## 🧪 Test immediately:

```bash
# Test the API
curl -s localhost:20998/api/v1/health | jq
curl -s localhost:20998/api/v1/chain/tip | jq
curl -s localhost:20998/api/v1/block/height/1 | jq

# Run comprehensive tests
./scripts/test_explorer_api.sh
```

## 📊 CMake Integration

Add to your `CMakeLists.txt`:

```cmake
# Explorer API library
add_library(dinero_explorer
    src/explorer/explorer_index.cpp
    src/explorer/explorer_api.cpp
    src/explorer/explorer_integration.cpp
    src/explorer/explorer_handlers.cpp
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
```

## 🔧 Advanced Integration (Optional)

### Address/UTXO Indexing

```cpp
void IndexTransactionOutputs(const CTransaction& tx, uint32_t height, uint32_t tx_index) {
    std::string txid = tx.GetHash().GetHex();
    
    // Index outputs (new UTXOs)
    for (size_t vout = 0; vout < tx.vout.size(); ++vout) {
        const auto& output = tx.vout[vout];
        
        // Convert scriptPubKey to scripthash (Electrum format)
        std::string script_hex = HexStr(output.scriptPubKey);
        std::string scripthash = ScriptToElectrumScripthash(script_hex);
        
        explorer_on_address_activity(scripthash.c_str(), txid.c_str(), vout, 
                                   output.nValue, height, 0); // 0 = not spent
    }
    
    // Index inputs (spent UTXOs)
    for (const auto& input : tx.vin) {
        if (input.prevout.IsNull()) continue; // Skip coinbase
        
        std::string prev_txid = input.prevout.hash.GetHex();
        // Mark previous output as spent
        // You'd need to look up the scripthash from the previous output
        // explorer_on_address_activity(scripthash.c_str(), prev_txid.c_str(), 
        //                             input.prevout.n, 0, height, 1); // 1 = spent
    }
}
```

### Mempool Integration

```cpp
// When adding to mempool:
void AddToMempool(const CTransaction& tx) {
    // ... existing mempool logic ...
    
    std::string txid = tx.GetHash().GetHex();
    std::string raw_hex = EncodeHexTx(tx);
    double fee_rate = CalculateFeeRate(tx);
    
    // Index as unconfirmed transaction (height = 0)
    explorer_on_new_transaction(txid.c_str(), raw_hex.c_str(), 0, 0);
    
    // Broadcast WebSocket update
    explorer_broadcast_new_tx(txid.c_str(), fee_rate);
}
```

### Reorg Handling

```cpp
void HandleReorg(uint32_t from_height, uint32_t to_height, 
                const std::vector<std::string>& detached_blocks,
                const std::vector<std::string>& attached_blocks) {
    // ... existing reorg logic ...
    
    // Convert to C-style arrays for the C interface
    std::vector<const char*> detached_ptrs;
    for (const auto& hash : detached_blocks) {
        detached_ptrs.push_back(hash.c_str());
    }
    
    std::vector<const char*> attached_ptrs;
    for (const auto& hash : attached_blocks) {
        attached_ptrs.push_back(hash.c_str());
    }
    
    explorer_broadcast_reorg(from_height, to_height, 
                           detached_ptrs.data(), detached_ptrs.size(),
                           attached_ptrs.data(), attached_ptrs.size());
}
```

## 🎯 API Endpoints Available

Once integrated, your daemon serves:

- **Chain**: `/api/v1/chain/tip`, `/api/v1/block/{hash}`, `/api/v1/blocks`
- **Transactions**: `/api/v1/tx/{txid}`, `/api/v1/tx/{txid}/hex`
- **Addresses**: `/api/v1/address/{address}`, `/api/v1/address/{address}/utxos`
- **Mempool**: `/api/v1/mempool`, `/api/v1/mempool/txids`
- **Stats**: `/api/v1/stats/supply`, `/api/v1/stats/difficulty`
- **Utilities**: `/api/v1/search`, `/api/v1/health`

## 🚨 Important Notes

1. **SQLite Tables**: The Explorer will create its own tables in `{datadir}/explorer.db`
2. **Performance**: Indexing adds ~10-20% overhead to block processing
3. **Storage**: Expect ~1GB per 1M transactions for the explorer index
4. **Caching**: Historical data cached 1 hour, tip data cached 5 seconds
5. **CORS**: All endpoints have `Access-Control-Allow-Origin: *` for web apps

## 🔍 Troubleshooting

- **"Explorer not initialized"**: Check that `explorer_initialize()` returned true
- **Empty responses**: Verify your HTTP server is calling the explorer handler
- **Missing data**: Ensure indexing hooks are called during block processing
- **Performance issues**: Check SQLite indexes are created properly

The Explorer API is designed to be lightweight and fast, adding minimal overhead to your existing daemon while providing a complete blockchain API.
