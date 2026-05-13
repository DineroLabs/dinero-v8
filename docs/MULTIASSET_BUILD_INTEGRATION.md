# Multi-Asset Escrow - Build Integration Guide

## Files Created

### Header Files
- `include/contracts/multiasset_escrow_contract.h` - Core multi-asset escrow types and interfaces
- `include/rpc/methods_multiasset.h` - RPC method declarations

### Implementation Files
- `src/contracts/multiasset_escrow_contract.cpp` - Multi-asset escrow implementation
- `src/rpc/methods_multiasset.cpp` - RPC method implementations

### Test Files
- `tests/test_multiasset_escrow.cpp` - Comprehensive unit tests
- `test_multiasset_manual.sh` - Manual integration test script

## CMakeLists.txt Integration

### 1. Add to `src/CMakeLists.txt`

Find the section where contract sources are added and include:

```cmake
# Multi-asset escrow support
src/contracts/multiasset_escrow_contract.cpp
```

Find the section where RPC sources are added and include:

```cmake
# Multi-asset RPC methods
src/rpc/methods_multiasset.cpp
```

### 2. Add to `tests/CMakeLists.txt`

```cmake
# Multi-asset escrow tests
add_executable(test_multiasset_escrow
    test_multiasset_escrow.cpp
)

target_link_libraries(test_multiasset_escrow
    dinero_contracts
    dinero_bridge
    dinero_common
    gtest
    gtest_main
    pthread
)

add_test(NAME MultiAssetEscrowTests COMMAND test_multiasset_escrow)
```

## Daemon Initialization

### Register RPC methods in `src/daemon/daemon.cpp`

Add to the initialization section:

```cpp
#include "rpc/methods_multiasset.h"

// In the initialization function:
void initialize_rpc_methods() {
    // ... existing registrations ...

    // Multi-asset escrow methods
    dinero::rpc::register_multiasset_methods();
}
```

## Build Instructions

### Clean Build

```bash
cd /Users/haydarevich/Documents/DineroCoin
rm -rf build
mkdir build
cd build
cmake ..
make -j$(nproc)
```

### Incremental Build

```bash
cd /Users/haydarevich/Documents/DineroCoin/build
make -j$(nproc)
```

### Run Tests

```bash
cd /Users/haydarevich/Documents/DineroCoin/build
./tests/test_multiasset_escrow
```

### Run Manual Test

```bash
cd /Users/haydarevich/Documents/DineroCoin

# 1. Start regtest daemon
./build/dinerod --regtest --rpcport=19999 --datadir=/tmp/multiasset-test &

# 2. Wait for daemon to start
sleep 3

# 3. Run manual tests
chmod +x test_multiasset_manual.sh
./test_multiasset_manual.sh
```

## Dependencies

The multi-asset escrow implementation depends on:

- **Existing contracts system** (`contracts/escrow_contract.h`)
- **Bridge/routing system** (`bridge/fiat_bridge_manager.h`, `bridge/routing_engine.h`)
- **Common utilities** (`common/logger.h`)
- **JSON library** (already included in project)

All dependencies are already present in the DineroCoin codebase.

## Compilation Verification

After building, verify the following symbols are present:

```bash
# Check for multi-asset symbols
nm build/dinerod | grep -i multiasset

# Expected output should include:
# - MultiAssetEscrowBuilder
# - MultiAssetContractRegistry
# - BridgedEscrowManager
# - multiasset_createescrow
# - multiasset_releaseescrow
# etc.
```

## RPC Method Verification

After starting the daemon, verify RPC methods are registered:

```bash
./build/dinero-cli -rpcport=19999 help | grep multiasset
```

Expected output:
```
multiasset.createescrow
multiasset.releaseescrow
multiasset.refundescrow
multiasset.getcontract
multiasset.listcontracts
multiasset.getconversionroutes
multiasset.estimateconversion
multiasset.stats
multiasset.supportedassets
```

## Troubleshooting

### Issue: Undefined reference to bridge methods

**Solution:** Ensure `bridge/fiat_bridge_manager.cpp` is compiled and linked.

### Issue: JSON parsing errors

**Solution:** Verify `din_json.h` is included and JsonCpp is linked.

### Issue: Registry not persisting

**Solution:** The registry is in-memory only. For persistence, integrate with the existing contract persistence layer.

### Issue: No conversion routes available

**Solution:** Bridge providers must be registered. Check that `FiatBridgeManager` has active providers.

## Next Steps

1. **Integration with existing build system**
   - Add files to appropriate CMakeLists.txt
   - Verify compilation

2. **RPC registration**
   - Call `register_multiasset_methods()` during daemon init

3. **Testing**
   - Run unit tests
   - Run manual integration tests
   - Test with real bridge providers

4. **Documentation**
   - Add RPC method docs to user manual
   - Create usage examples
   - Document conversion flow

5. **Production readiness**
   - Add persistence layer
   - Implement actual provider swap execution
   - Add monitoring and logging
   - Security audit
