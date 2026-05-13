# DineroCoin v1.1.0-rc1 Release Notes

**Release Date**: September 28, 2025  
**Release Type**: Release Candidate  
**Previous Version**: v1.0.4

## 🎯 Major Features

### Address API Hardening
- **Fixed Bech32 decoding**: Correct checksum exclusion (6-byte) in `convertbits` operation
- **Strict HRP validation**: Case-insensitive network-specific validation (regtest: "rdin", testnet: "tdin", mainnet: "din")
- **Mixed-case rejection**: Enforced case-sensitive address validation
- **Bech32m future-proofing**: Explicit rejection of `witver >= 1` with clear TODO for future support
- **Round-trip validation**: Consistent `getnewaddress` → `validateaddress` → `scriptPubKey` flow

### vNext-Only Runtime
- **Unified port strategy**: JSON-RPC and HTTP both on port 20999 (no separate RPC port)
- **Legacy RPC removal**: Complete elimination of `RPCServer`, `g_rpc_server`, and `din_ws` dependencies
- **Duplicate RPC fix**: Strict registry prevents duplicate method registrations
- **Clean startup**: vNext Phase 3 startup with proper initialization order

### End-to-End Validation
- **A→Z test suite**: Comprehensive build→mine→PSBT→broadcast validation
- **PSBT round-trip**: Create→fund→sign→finalize→extract→broadcast verified
- **UTXO integrity**: Placeholder detection and P2WPKH structure validation
- **Wallet-chainstate alignment**: Proper UTXO synchronization and spending

## 🔧 Technical Improvements

### Mining System
- **Address-based coinbase**: Mining rewards correctly assigned to provided addresses
- **BIP34 height commitment**: Prevents coinbase transaction ID collisions
- **ScriptPubKey validation**: Ensures mined UTXOs match address-derived scripts

### PSBT System
- **Robust funding**: Correct UTXO selection and change detection
- **Serialization fixes**: Proper PSBTv2 field handling and Base64 encoding
- **Error handling**: Detailed validation messages and failure reporting

### Database Architecture
- **SQLite integration**: Replaced RocksDB with Bitcoin Core-style SQLite
- **Atomic transactions**: Consistent UTXO updates during block connection
- **Schema migrations**: Versioned database schema with integrity checks

## 🛡️ Security & Reliability

### Placeholder Elimination
- **Zero placeholders**: Enforced by CI guard; only genesis DEADBEEF allowed
- **Script validation**: All scriptPubKeys must be 44 hex chars with OP_0+PUSH20 prefix
- **Mining validation**: No fallback to placeholder scripts in coinbase

### CI/CD Pipeline
- **Branch protection**: A→Z validation required for all PRs to main
- **Sanitizer support**: ASan/UBSan builds for memory safety
- **Cross-platform**: Linux and macOS validation
- **Nightly soak tests**: Extended mining and PSBT stress testing

## 📚 Documentation

### Developer Guide
- **vNext-only quickstart**: Complete setup and usage instructions
- **Address API documentation**: Field descriptions, validation rules, and examples
- **A→Z test guide**: How to run and interpret end-to-end validation

### API Reference
- **RPC methods**: 70+ methods with proper error handling
- **Address validation**: Bech32 decode/encode with network-specific HRP
- **PSBT operations**: Complete funding, signing, and finalization workflow

## 🚀 Performance

### Startup Time
- **vNext-only**: ~3 seconds (down from ~5 seconds with legacy)
- **Unified ports**: Single HTTP server handles both JSON-RPC and HTTP
- **Clean initialization**: Proper wallet and database setup order

### Mining Performance
- **Block generation**: ~0 seconds for regtest blocks
- **UTXO updates**: Atomic database transactions
- **Memory usage**: Reduced footprint without legacy dependencies

## 🔍 Testing

### A→Z Validation Suite
- **Build validation**: vNext-only compilation with strict flags
- **Health checks**: HTTP endpoint and RPC surface validation
- **Wallet operations**: Create, load, address generation, validation
- **Mining operations**: 105 blocks for coinbase maturity
- **PSBT operations**: Complete round-trip with broadcast
- **Address validation**: 5 unique addresses with witness program verification
- **Bech32 corpus**: Invalid addresses, wrong HRP, mixed-case, Bech32m rejection

### CI Matrix
- **Platforms**: Ubuntu 24.04, macOS 14
- **Build types**: Release, Debug+ASan/UBSan
- **Sanitizers**: AddressSanitizer, UndefinedBehaviorSanitizer
- **Timeout**: 30 minutes per job

## 🐛 Bug Fixes

### Address Validation
- **Fixed**: `wallet.validateaddress` returning `isvalid: false` for valid addresses
- **Fixed**: Missing witness program extraction
- **Fixed**: Incorrect Bech32 checksum handling

### Mining System
- **Fixed**: Placeholder script usage in coinbase transactions
- **Fixed**: ScriptPubKey mismatch between mining and wallet
- **Fixed**: UTXO funding failures due to script mismatch

### PSBT System
- **Fixed**: PSBT truncation during serialization
- **Fixed**: Missing `prev_txid` and `output_index` fields
- **Fixed**: Funding failures with "No spendable UTXOs found"

### RPC System
- **Fixed**: Duplicate method registrations causing log spam
- **Fixed**: Legacy RPC server dependencies in vNext-only builds
- **Fixed**: Port confusion between RPC and HTTP endpoints

## 🔄 Migration Notes

### From v1.0.4
- **Database**: Automatic migration to SQLite schema
- **Configuration**: Unified port configuration (20999 for both RPC and HTTP)
- **RPC**: Legacy methods removed; use vNext methods only
- **Addresses**: Bech32 validation now strict; mixed-case addresses rejected

### Breaking Changes
- **Legacy RPC**: All legacy RPC methods removed
- **Ports**: Single unified port (20999) for all operations
- **Address validation**: Stricter Bech32 validation rules
- **Mining**: Address parameter required for `mining.generatetoaddress`

## 📦 Installation

### Pre-built Binaries
- **macOS**: `dinerod-macos-arm64-v1.1.0-rc1.tar.gz`
- **Linux**: `dinerod-linux-x86_64-v1.1.0-rc1.tar.gz`

### Build from Source
```bash
git clone https://github.com/dinerocoin/dinerocoin.git
cd dinerocoin
git checkout v1.1.0-rc1
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

## 🧪 Testing

### Run A→Z Validation
```bash
bash scripts/run-a2z.sh
```

### Run with Sanitizers
```bash
export CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-fsanitize=address,undefined"
bash scripts/run-a2z.sh
```

### Nightly Soak Test
```bash
NIGHTLY_SOAK=true bash scripts/run-a2z.sh
```

## 🔮 Future Roadmap

### v1.1.0 (Final)
- **Bech32m support**: Add support for `witver >= 1` addresses
- **Cross-network testing**: Automated mainnet/testnet validation
- **Metrics**: Prometheus metrics for address validation failures
- **Fuzzing**: LibFuzzer integration for address decoder

### v1.2.0
- **Mempool policy**: Enhanced standardness and RBF/CPFP rules
- **Fee estimation**: Rolling median fee estimator
- **Schema migrations**: Automated database schema versioning
- **Docker support**: Containerized deployment options

## 📞 Support

### Issues
- **GitHub Issues**: https://github.com/dinerocoin/dinerocoin/issues
- **Discussions**: https://github.com/dinerocoin/dinerocoin/discussions

### Documentation
- **vNext Developer Guide**: `docs/VNEXT_DEVELOPER_GUIDE.md`
- **Address API**: `docs/VNEXT_DEVELOPER_GUIDE.md#address-api`
- **A→Z Testing**: `docs/VNEXT_DEVELOPER_GUIDE.md#a-z-end-to-end-testing`

---

**Full Changelog**: https://github.com/dinerocoin/dinerocoin/compare/v1.0.4...v1.1.0-rc1
