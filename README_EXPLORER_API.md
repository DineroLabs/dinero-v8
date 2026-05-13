# 🚀 Dinero Explorer API v1

**Fast, production-ready blockchain explorer API for Dinero cryptocurrency.**

## ✨ Features

- **🔥 Lightning Fast**: Sub-second responses with SQLite indexing
- **📊 Complete Coverage**: Blocks, transactions, addresses, mempool, stats
- **🔄 Real-time Updates**: WebSocket streaming for live blockchain data
- **📱 Web-Ready**: CORS enabled, perfect for web applications
- **⚡ Cursor Pagination**: No offset explosions, handles millions of records
- **🎯 Type-Safe**: Exact JSON contracts prevent API drift
- **🔧 Drop-in Integration**: 3 lines of code to add to existing daemon

## 🚀 Quick Start

### 1. Integration (3 lines of code)

```cpp
// In your daemon's main.cpp:
explorer_initialize(datadir.c_str());                    // Initialize
explorer_handle_http_request(method, path, query, body); // Handle requests  
explorer_shutdown();                                     // Cleanup
```

### 2. Build & Run

```bash
# Build with Explorer API
./scripts/build_with_explorer.sh

# Start daemon
./build-explorer/bin/dinerod -datadir=./data -rpcport=20998

# Test immediately
curl -s localhost:20998/api/v1/health | jq
```

### 3. Explore the API

```bash
# Chain data
curl -s localhost:20998/api/v1/chain/tip | jq
curl -s localhost:20998/api/v1/block/height/1 | jq

# Address lookup (replace with actual address)
curl -s localhost:20998/api/v1/address/din1... | jq

# Real-time WebSocket
websocat ws://localhost:20998/ws/explorer
```

## 📊 API Endpoints

### Chain Data
- `GET /api/v1/chain/tip` - Current blockchain tip
- `GET /api/v1/block/{hash}?verbosity=0|1|2` - Block by hash
- `GET /api/v1/block/height/{height}` - Block by height  
- `GET /api/v1/blocks?from_height=0&limit=10` - Block list

### Transactions
- `GET /api/v1/tx/{txid}` - Transaction details
- `GET /api/v1/tx/{txid}/hex` - Raw transaction hex
- `GET /api/v1/tx/{txid}/proof` - Merkle proof

### Addresses (Bech32 din...)
- `GET /api/v1/address/{address}` - Address summary
- `GET /api/v1/address/{address}/utxos` - Address UTXOs
- `GET /api/v1/address/{address}/txs` - Transaction history

### Scripthash (Electrum-compatible)
- `GET /api/v1/scripthash/{hex}/utxos` - UTXOs by scripthash
- `GET /api/v1/scripthash/{hex}/history` - History by scripthash

### Mempool
- `GET /api/v1/mempool` - Mempool summary
- `GET /api/v1/mempool/txids` - Mempool transaction IDs
- `GET /api/v1/mempool/tx/{txid}` - Mempool transaction

### Statistics
- `GET /api/v1/stats/supply` - Supply statistics
- `GET /api/v1/stats/difficulty` - Difficulty info
- `GET /api/v1/stats/blocks/24h` - 24-hour block stats

### Utilities
- `GET /api/v1/search?q=<query>` - Search blocks/txs/addresses
- `GET /api/v1/health` - API health check

### WebSocket (Real-time)
- `ws://localhost:20998/ws/explorer` - Live blockchain updates

## 📋 Example Responses

### Chain Tip
```json
{
  "height": 12345,
  "hash": "a415b97b505c865e0e857261a7c744c9497086bf2450740056d51b7c241f1418",
  "time": 1700000093
}
```

### Block (verbosity=2)
```json
{
  "hash": "a415...",
  "height": 1,
  "time": 1700000093,
  "tx": [{
    "txid": "acbc4b85...",
    "vin": [{"coinbase": "...", "sequence": 4294967295}],
    "vout": [{
      "n": 0,
      "value": 2000000.0,
      "scriptPubKey": {
        "hex": "0014f5d4a415975f8cc990de60e476616103377423a7",
        "type": "witness_v0_keyhash"
      }
    }]
  }]
}
```

### Address Summary
```json
{
  "address": "din1...",
  "scripthash": "e3ab...",
  "received": 2000000.0,
  "sent": 0.0,
  "balance": 2000000.0,
  "tx_count": 1
}
```

### WebSocket Messages
```json
{"type":"newBlock","height":123,"hash":"...","time":1700000153}
{"type":"newTx","txid":"...","fee_rate":1.5}
{"type":"reorg","from":10,"to":9,"detached":["..."],"attached":["..."]}
```

## 🏗️ Architecture

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   HTTP Client   │───▶│  Explorer API    │───▶│ SQLite Index    │
│  (curl, web)    │    │   (REST + WS)    │    │  (Fast Lookup)  │
└─────────────────┘    └──────────────────┘    └─────────────────┘
                                │                        │
                                ▼                        ▼
                       ┌──────────────────┐    ┌─────────────────┐
                       │  WebSocket Bus   │    │ Blockchain DB   │
                       │  (Real-time)     │    │   (SQLite)      │
                       └──────────────────┘    └─────────────────┘
```

## ⚡ Performance

- **Sub-second responses** with proper SQLite indexing
- **Cursor pagination** prevents offset explosions
- **Smart caching**: 5s for tip data, 3600s for historical
- **Optimized queries** with prepared statements
- **Handles millions** of transactions efficiently

## 🔧 Integration Details

### Required Dependencies
- SQLite3 (for fast indexing)
- JsonCpp (for JSON handling)  
- OpenSSL (for scripthash calculation)

### CMake Integration
```cmake
# Add to your CMakeLists.txt
target_link_libraries(dinerod PRIVATE dinero_explorer)
```

### Indexing Hooks
```cpp
// Call these when processing blocks/transactions
explorer_on_new_block(height, hash, data, timestamp);
explorer_on_new_transaction(txid, hex, height, index);
explorer_on_address_activity(scripthash, txid, vout, value, height, is_spent);
```

### Database Schema
The Explorer API creates optimized SQLite tables:
- `blocks` - Block headers and metadata
- `tx` - Transaction index  
- `addr_utxo` - UTXO tracking by scripthash
- `addr_hist` - Transaction history by scripthash
- `addr_stats` - Cached address statistics
- `mempool` - Mempool transactions

## 📚 Documentation

- **[Quick Integration Guide](docs/QUICK_INTEGRATION_GUIDE.md)** - Get started in 5 minutes
- **[SQLite Requirements](docs/explorer_requirements.md)** - Database schema and indexing
- **[OpenAPI Specification](docs/explorer_openapi_complete.yaml)** - Complete API documentation
- **[Integration Example](examples/daemon_integration_example.cpp)** - Full daemon example

## 🧪 Testing

```bash
# Run comprehensive API tests
./scripts/test_explorer_api.sh

# Expected output:
✅ Health: {"ok":true,"height":1,"db":"ok"}
✅ Tip: {"height":1,"hash":"a415...","time":1700000093}
✅ Block: {"hash":"a415...","height":1,"tx":["acbc4b..."]}
✅ CORS: Access-Control-Allow-Origin: *
✅ Cache: Cache-Control: public,max-age=3600
```

## 🚨 Production Notes

1. **Database Size**: ~1GB per 1M transactions for explorer index
2. **Performance Impact**: ~10-20% overhead during block processing
3. **Caching**: Historical data cached 1 hour, tip data 5 seconds
4. **Rate Limiting**: Add rate limiting for public deployments
5. **CORS**: Enabled by default for web applications

## 🔍 Troubleshooting

| Issue | Solution |
|-------|----------|
| "Explorer not initialized" | Check `explorer_initialize()` returned true |
| Empty API responses | Verify HTTP server calls explorer handler |
| Missing transaction data | Ensure indexing hooks are called during block processing |
| Slow responses | Check SQLite indexes are created properly |
| WebSocket not working | Verify WebSocket server is running on correct port |

## 🎯 Use Cases

- **Block Explorers**: Complete blockchain browsing
- **Wallet Backends**: Address/UTXO lookups  
- **Analytics**: Blockchain statistics and monitoring
- **DeFi Applications**: Real-time transaction tracking
- **Mobile Apps**: Lightweight blockchain queries

## 📈 Roadmap

- ✅ **v1.0**: Core REST API with SQLite indexing
- 🔄 **v1.1**: Enhanced WebSocket streaming
- 📋 **v1.2**: Advanced analytics endpoints
- 🔍 **v1.3**: Full-text search capabilities
- 📊 **v2.0**: GraphQL interface

## 🤝 Contributing

The Explorer API is designed to be:
- **Stable**: Locked JSON contracts prevent breaking changes
- **Fast**: Optimized for production workloads  
- **Complete**: Covers all blockchain explorer needs
- **Extensible**: Easy to add new endpoints

## 📄 License

Same license as the main Dinero project.

---

**🚀 Ready to explore the Dinero blockchain with lightning-fast API responses!**
