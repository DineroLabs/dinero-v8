# Dinero v1.0.1 Release Artifacts

## 🎯 **Release Summary**
- **Version**: v1.0.1
- **Branch**: release/1.0  
- **Tag**: v1.0.1
- **Build**: RelWithDebInfo + ASan/UBSan
- **Platform**: macOS ARM64

## ✅ **Key Improvements**
- SQLite wallet durability with WAL recovery
- Kill-9 crash safety with integrity checks  
- CI-stable tests across Ubuntu/macOS
- Generator-proof test discovery
- P0 crypto gate with 100% pass rate

## 🧪 **Test Results**
✅ **Wallet Integration**: 100% pass (2/2)
- wallet_sqlite_lifecycle: PASSED (0.03s)
- wallet_kill9_durability: PASSED (0.10s)

✅ **P0 Crypto Core**: 100% pass (13/13)  
- test_crypto_vectors: PASSED
- test_bip39_seed_kat: PASSED
- test_bip32_fpr: PASSED
- test_hd_wallet_fixed: PASSED
- test_bip84_bech32_roundtrip: PASSED
- test_slip132_prefix: PASSED (custom Dinero prefixes)
- test_dinero_mining: PASSED (ASan buffer overflow fixed)
- test_premine_creation: PASSED (shell timeout eliminated)
- test_blockheader_serialize: PASSED (80-byte layout guard)
- test_regtest_integration: PASSED (one-block mining)

🔒 **Production Hardening**: 
- CheckPreminePoW: Fixed critical 80-byte header serialization bug
- Debug assertions: Test/production hash consistency validation
- Regtest integration: Height progression and block validation

## 🔧 **Environment Variables**
- DIN_WAL_STRONG=1 (production default)
- DINERO_WALLET_SYNC=FULL (maximum safety)

## 📋 **Verification**
```bash
git verify-tag v1.0.1
git log --oneline v1.0.0..v1.0.1
ctest --test-dir build-test -L "(p0|wallet)" --output-on-failure
```

Built: Sun Aug 31 07:08:01 EDT 2025
Commit: 49952b28e79a3468360d81fdc173b96eb901c41d

