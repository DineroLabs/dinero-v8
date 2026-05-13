# WALLET-CONSENSUS-INVARIANTS.md

Status: FROZEN
Frozen On: 2026-02-12
Scope: Mainnet wallet safety invariants for Dinero (Taproot-from-genesis, BIP86-only)

## Purpose
This document defines wallet invariants that are treated as consensus-adjacent safety requirements.
Any change to these invariants requires an explicit mainnet readiness review and versioned update.

## Immutable Invariants

1. Policy Invariant (BIP86-only)
- Wallet creation and restore RPC paths MUST accept only `bip86` policy.
- Any other policy value MUST fail fast before wallet DB write.

2. Mnemonic Prewrite Invariant
- During `wallet.restore`, mnemonic -> seed conversion and index-0 BIP86 Taproot address derivation MUST happen before wallet DB state changes.
- If `expected_first_address` is provided, mismatch MUST fail before wallet DB write.

3. First-Address Consistency Invariant
- The first address generated from persisted wallet state MUST equal the mnemonic-derived BIP86 index-0 address.
- Any mismatch is an invariant violation and restore MUST fail.

4. Seed Continuity Invariant
- `encryptWallet` MUST re-encrypt existing seed material and MUST NOT reset address, derivation, or UTXO tables.
- Compile-time guard enforces no reset during encryption flow.

5. Derivation Persistence Invariant
- External chain path format: `m/86'/1447'/0'/0/index`
- Change chain path format: `m/86'/1447'/0'/1/index`
- Derivation paths MUST persist across encrypt/restart/unlock/restore flows.

6. Negative-Restore Safety Invariant
- Invalid mnemonic, missing words, wrong passphrase mismatch, or policy mismatch MUST fail cleanly.
- Failed restore MUST NOT leave partial wallet DB artifacts.

7. Memory Hygiene Invariant
- Lock/timeout-lock/close paths MUST scrub in-memory seed and encryption key material.
- Seed material at rest MUST be encrypted for encrypted wallets.

## Test Mapping (Required)

- `/Users/haydarevich/src/dinero/tests/wallet/test_wallet_mainnet_readiness.cpp`
  - `EncryptionRoundTripRestoreAndDerivationPersistence`
  - `InvalidMnemonicRestoreFailsWithoutPartialWallet`
  - `RejectsNonBip86PolicyWithoutPartialWallet`
  - `RestoreRpcFuzzMalformedPayloadsNoCrashNoPartialWallets`
  - `Bip86DeterminismProperty1000RandomMnemonics`
  - `WrongBip39PassphraseFailsCleanlyWithExpectedAddressGuard`

## Change Control
- This file is frozen for mainnet candidate evaluation.
- Modifications require:
  1. explicit rationale,
  2. updated tests,
  3. re-run of wallet readiness suite,
  4. version bump in this document header.
