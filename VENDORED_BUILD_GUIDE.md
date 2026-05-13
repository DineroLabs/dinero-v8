# Vendored Build Quick Reference

## Build Commands

```bash
cd /Users/haydarevich/Documents/DineroCoin
rm -rf build-vendored
mkdir build-vendored
cd build-vendored
cmake .. -DCMAKE_BUILD_TYPE=Release -DUSE_VENDORED_ROCKSDB=ON -DENABLE_SANITIZERS=OFF
make -j$(sysctl -n hw.ncpu) dinerod
```

## Validation Checklist

```bash
# 1. Version check
./dinerod --version

# 2. Initialize test environment
mkdir -p /tmp/dinero_test
./dinerod --datadir=/tmp/dinero_test --rpcuser=test --rpcpassword=test123
# Press Ctrl+C after 10 seconds

# 3. Verify dependencies
otool -L ./dinerod
# Should only show: libSystem, Security.framework, libc++

# 4. Check RocksDB logs
cat /tmp/dinero_test/chaindb/LOG | grep -iE "(error|corruption|failed)" || echo "Clean"

# 5. Test restart
./dinerod --datadir=/tmp/dinero_test --rpcuser=test --rpcpassword=test123
# Should reopen existing DB without reinitializing
```

## Vendored Libraries

- jsoncpp 1.9.6 - `third_party/jsoncpp/`
- secp256k1 - `third_party/secp256k1/`
- RocksDB 9.1.1 - `third_party/rocksdb-9.1.1/`
- SQLite3 3.48.0 - `third_party/sqlite-amalgamation-3480000/`
- OpenSSL 3.3.2 - `third_party/openssl-3.3.2/`
- Boost 1.85.0 - `third_party/boost_1_85_0/`

## Known Configuration

- RocksDB compression: **kNoCompression** (LZ4/Snappy disabled)
- Build type: Release
- Sanitizers: Disabled
- Platform: macOS 12.0+ (portable)

## Deployment

Binary is fully self-contained. Copy to target system:

```bash
scp build-vendored/dinerod user@remote:/usr/local/bin/
```

No additional dependencies required on target system.

## Validation Status

Last validated: 2025-10-29  
Build: 7c898171  
Status: ✅ PRODUCTION READY

