# DineroCoin Reference Wallet

This wallet passes official BIP39 and BIP32 test vectors and serves as the behavioral oracle for DineroCoin wallets.

## Status

**Version:** 1.0.0
**Test Coverage:** 30/30 tests passing (100%)
**Standards Compliance:** BIP39, BIP32, BIP84, Bech32

## Purpose

This is the **reference implementation** of the DineroCoin wallet. It prioritizes:
1. **External correctness** - Full BIP39/BIP32 compatibility for cross-wallet portability
2. **Internal determinism** - Reproducible behavior for testing other implementations
3. **Simplicity** - Minimal dependencies, clear code structure

Other wallet implementations should validate their behavior against this wallet.

## Test Suite Guarantees

Tests in `tests/wallet/test_reference_wallet.cpp` are **consensus-adjacent** and must not be:
- Weakened
- Conditionally skipped
- Marked as "non-critical"

All 30 tests must pass for the wallet to be considered compliant.

## Critical Compliance Notes

- **BIP39 Test Vectors**: Official test vectors use passphrase "TREZOR" (not empty string)
- **Coinbase Maturity**: Requires >100 confirmations (101+), not >=100
- **BIP32 Path Parsing**: Hardened derivation apostrophes must be stripped before conversion
- **UTXO Selection**: Deterministic lowest-first algorithm (no randomization)

## Building

```bash
cd /path/to/dinerocoin/build
cmake ..
make test_reference_wallet
./test_reference_wallet
```

All tests should pass.

## Architecture

- `wallet.cpp/h` - Main wallet interface
- `database.cpp/h` - SQLite persistence layer
- `crypto.cpp/h` - BIP39/BIP32 wrappers
- `utxo_manager.cpp/h` - Deterministic UTXO selection
- `transaction_builder.cpp/h` - Transaction construction

## Version History

### v1.0.0 (2026-01-06)
- Initial release
- Full BIP39/BIP32/BIP84 compliance
- 30 comprehensive tests
- Ready for production use as behavioral oracle
