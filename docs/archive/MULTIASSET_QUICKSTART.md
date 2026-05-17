# Multi-Asset Escrow - Quick Start Guide

## Installation (5 minutes)

### 1. Add to Build System

Edit `src/CMakeLists.txt` and add:

```cmake
# Multi-asset escrow
src/contracts/multiasset_escrow_contract.cpp
src/rpc/methods_multiasset.cpp
```

### 2. Register RPC Methods

Edit `src/daemon/daemon.cpp` (or wherever RPC methods are registered):

```cpp
#include "rpc/methods_multiasset.h"

// In initialization:
dinero::rpc::register_multiasset_methods();
```

### 3. Build

```bash
cd /Users/haydarevich/Documents/DineroCoin/build
make -j$(nproc)
```

### 4. Test

```bash
# Run unit tests
./tests/test_multiasset_escrow

# Start daemon
./dinerod --regtest --rpcport=19999 --datadir=/tmp/ma-test &

# Run manual tests
cd ..
./test_multiasset_manual.sh
```

## Quick Usage Examples

### Create Basic DIN Escrow

```bash
dinero-cli multiasset.createescrow '{
  "buyer_pubkey": "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
  "seller_pubkey": "02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5",
  "mediator_pubkey": "02f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9",
  "asset_id": "DIN",
  "amount": 10.5,
  "refund_blocks": 2880
}'
```

### Create USDT Escrow with EUR Conversion

```bash
dinero-cli multiasset.createescrow '{
  "buyer_pubkey": "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
  "seller_pubkey": "02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5",
  "mediator_pubkey": "02f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9",
  "asset_id": "USDT",
  "amount": 100.0,
  "refund_blocks": 2880,
  "release_asset": "EUR"
}'
```

### List All Contracts

```bash
dinero-cli multiasset.listcontracts
```

### Get Statistics

```bash
dinero-cli multiasset.stats
```

## Files Created

✅ `include/contracts/multiasset_escrow_contract.h`
✅ `src/contracts/multiasset_escrow_contract.cpp`
✅ `include/rpc/methods_multiasset.h`
✅ `src/rpc/methods_multiasset.cpp`
✅ `tests/test_multiasset_escrow.cpp`
✅ `test_multiasset_manual.sh`

## Documentation

📖 **Main README:** `MULTIASSET_ESCROW_README.md`
📖 **Build Guide:** `docs/MULTIASSET_BUILD_INTEGRATION.md`
📖 **Architecture:** `docs/MULTI_ASSET_ESCROW_ARCHITECTURE.md`
📖 **Implementation:** `MULTIASSET_IMPLEMENTATION_SUMMARY.md`

## Supported Assets

DIN, BTC, ETH, USDT, USDC, DAI, EUR, USD, GBP

## RPC Methods

- `multiasset.createescrow` - Create escrow
- `multiasset.releaseescrow` - Release with conversion
- `multiasset.refundescrow` - Refund
- `multiasset.getcontract` - Get details
- `multiasset.listcontracts` - List/filter
- `multiasset.getconversionroutes` - Show routes
- `multiasset.estimateconversion` - Estimate output
- `multiasset.stats` - Statistics
- `multiasset.supportedassets` - List assets

## Need Help?

1. Check `MULTIASSET_ESCROW_README.md` for detailed docs
2. See `docs/MULTIASSET_BUILD_INTEGRATION.md` for troubleshooting
3. Review test cases in `tests/test_multiasset_escrow.cpp`
