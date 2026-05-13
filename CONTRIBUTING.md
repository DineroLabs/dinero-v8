# Contributing to DineroCoin

Thank you for your interest in contributing to DineroCoin!

## 🔒 Consensus Invariants

**CRITICAL:** Any change that causes `test_genesis_invariants` to fail constitutes a hard fork and requires explicit governance approval.

The genesis invariant tests (`tests/consensus/test_genesis_invariants.cpp`) protect consensus-critical constants that are locked forever:

- Genesis block hash, merkle root, nonce, and timestamp
- Block 1 premine scriptPubKey and amount
- Subsidy schedule (halving interval, initial reward)
- Network identity (network_id, target_spacing)
- Taproot enforcement (witness version 1 requirement)

**These tests MUST pass on every commit.** If they fail:
1. STOP immediately
2. Revert your changes
3. Understand what consensus parameter you broke
4. Either fix your code without changing consensus parameters, OR
5. Initiate the formal hard fork governance process

See [CONSENSUS_LOCK.md](CONSENSUS_LOCK.md) for the complete list of locked constants and detailed explanation.

## Running Tests

Before submitting a pull request:

```bash
# Build and run genesis invariant tests
cmake --build build --target test_genesis_invariants
./build/bin/test_genesis_invariants

# Or via CTest
cd build && ctest -R GenesisInvariants -V
```

All 16 invariant tests must pass.

## What You CAN Change

See [CONSENSUS_LOCK.md](CONSENSUS_LOCK.md) for a detailed list of safe changes:
- RPC API enhancements
- Wallet features
- P2P protocol optimizations
- Mining improvements
- Logging and metrics
- GUI/UX

## Pull Request Process

1. Fork the repository
2. Create a feature branch from `develop`
3. Make your changes
4. Run all tests including `test_genesis_invariants`
5. Submit a pull request

## Code Review Standards

- All consensus-touching code requires multiple reviews
- Genesis invariant tests must pass in CI
- Changes to `src/consensus/` require detailed justification

## Questions?

- Open an issue for discussion
- Join the community channels
- Review existing pull requests

---

**Remember:** "Past-you stopping future-you from breaking money."
