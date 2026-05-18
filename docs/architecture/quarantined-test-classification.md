# Quarantined Test Classification

Generated from `dinero-main` at `c6a2a366`.

This is the first read-only classification pass after Test Workflow v2 and
`ReleaseSuiteConfigSmoke` landed. The goal is to decide what each quarantined
test represents before re-enabling, rewriting, converting, or deleting it.

Local evidence used:

- `ctest --test-dir build-cmake-tests -N -V` for registered names and labels.
- Focused local runs against `build-cmake-tests` with `--timeout 60`.
- CMake registration comments and test-file headers.

The local build reported `368` registered tests. Label-quarantined inventory in
that build was:

| Label | Local count | Classification |
| --- | ---: | --- |
| `integration` | 79 | Mixed harness/runtime integration surface; classify by sub-suite before changing CI ownership. |
| `gate` | 4 | Release/readiness gates; heavy or policy-oriented, not normal PR-CI by default. |
| `release` | 3 | Release ownership; keep full gates manual or release-blocking unless split into smoke checks. |
| `canonicality` | 30 before the first split | Too broad as a single quarantine; contains tests that already pass and tests that need deeper repair. |
| `fuzz` | 2 | Move to dedicated fuzz/nightly ownership; do not fold into normal PR CI. |
| `packaging` | 3 | Move to packaging workflow ownership. |

## Exact-Name Quarantine

| Test | Current status | Original purpose | Still relevant | Replacement coverage | Action |
| --- | --- | --- | --- | --- | --- |
| `ConsensusFormalVerification` | Fails locally. `SupplyInvariantTest.PropertySupplyFormulaCorrectness_AllEpochs` reports a supply formula mismatch at end of epoch 0. | Ring 1 property tests for consensus-critical supply, UTXO, and chain-selection invariants. | Yes. This is consensus safety coverage, not obsolete. | Partial: lower-level subsidy/supply tests exist, but this property suite is the broad invariant gate. | Keep quarantined. Fix the formula/test-vector disagreement in a dedicated consensus test PR, then re-enable. |
| ~~`ShieldedDerivation`~~ | **RESOLVED 2026-05-18 via PR #55** (squash `81655449`). Root cause was a 4-byte uninitialized-stack read in `ChaCha20Diversifier` (passed 12-byte buffer to OpenSSL `EVP_chacha20()` which expects 16-byte IV with counter prefix). Mac happened to read zero bytes there (accidentally correct); Linux read non-zero garbage (divergent). Fix: explicit 16-byte IV with counter=0. Both platforms now produce identical output; pinned hex vectors unchanged. De-quarantined from `tests.yml` exclude regex in the same PR series. | Pins shielded key-derivation vectors for independent implementation parity. | Yes. | n/a (active). | **Re-enabled.** Test runs cleanly on both Mac and Linux CI. |
| ~~`WalletDescriptorActiveContext`~~ | **RESOLVED 2026-05-18.** Test expectation was stale: pinned `1447h` (legacy coin type) when the canonical coin type for v7+ is `1448h` (per `src/wallet/*` + `src/daemon/*` derivation paths; the 1447 legacy scan path was removed entirely on 2026-04-18). Fix: updated both assertions to expect `1448h`. 16/16 assertions now pass on Mac + Linux. De-quarantined from `tests.yml` exclude regex in the same PR. The active-wallet-DB context check (the test's actual purpose) was always correct — only the coin-type pin was wrong. | Ensures descriptor RPC reads from the active wallet DB instead of leaking default-wallet descriptors. | Yes. | n/a (active). | **Re-enabled.** Test runs cleanly on both platforms. |
| `ArchivalBlockReader` | Aborts locally during `Reindexer_UsesFlatfilesWithoutShadowBodyWrites`; reindex forest root mismatch at height 1. | Ensures archival reads prefer flatfiles and legacy ChainDB body fallback is explicit. | Yes. This protects storage/reindex behavior. | Partial storage tests exist, but not the same flatfile/fallback signal. | Keep quarantined. Treat as harness/runtime debt around reindex Utreexo roots, not obsolete test debt. |
| `UtreexoE2ERelay` | Fails locally. CSN rejects a transaction spending a block-2 output due to local/proof root mismatch. | End-to-end Bridge to CSN block sync plus transaction proof relay. | Yes. This is Utreexo relay safety coverage. | Unit-level Utreexo tests exist, but not this full relay path. | Keep quarantined. Split root-tracking failure into a focused Utreexo relay repair PR. |
| ~~`WalletMainnetReadiness`~~ | **SPLIT 2026-05-18.** The original bundled entry was split into 9 per-subcase ctest entries (`WalletMainnetReadiness_<SubName>`). 5 subcases pass on Mac + Linux and have graduated to the active CI set (now run on every PR): `InvalidMnemonicRestoreFailsWithoutPartialWallet`, `RejectsNonBip86PolicyWithoutPartialWallet`, `RestoreRpcFuzzMalformedPayloadsNoCrashNoPartialWallets`, `ReorgDepthFourClearsConfirmedBalanceWithoutChangeIndexRollback`, `WrongBip39PassphraseFailsCleanlyWithExpectedAddressGuard`. The 4 individually-failing subcases remain quarantined and are listed below as their own rows for sharper per-test fix-or-delete tracking. | Wallet-only mainnet hardening: encryption, restore determinism, invalid mnemonic handling, BIP86 persistence. | Yes (per subcase). | n/a (bundle replaced). | **Bundle resolved via split.** Individual quarantine entries below. |
| `WalletMainnetReadiness_EncryptionRoundTripRestoreAndDerivationPersistence` | Fails on Mac + Linux. Encrypted-wallet round-trip restore loses derivation persistence somewhere in the encrypt/save/load/decrypt cycle. ~12 sec runtime to failure. | Mainnet hardening: ensures users can encrypt, dump, restore, and continue deriving addresses without state loss. | Yes. Core wallet safety property. | None identified. | Keep quarantined. Diagnose the persistence step that drops state during the encrypt-restore round-trip. Likely a wallet-state serialization or re-derivation ordering bug. |
| `WalletMainnetReadiness_RestoreResetsLegacyEncryptionStateBeforeReEncrypt` | Fails on Mac + Linux. Restore path doesn't fully reset legacy encryption metadata before re-encrypting, leaving inconsistent dual-encryption state. ~6 sec runtime to failure. | Migration hardening: legacy-encrypted wallets must restore cleanly into the current encryption scheme. | Yes. Migration paths are user-facing and break silently if wrong. | None identified. | Keep quarantined. Audit the restore-then-reencrypt sequence for ordering / state-clear gaps; fix in dedicated wallet-encryption PR. |
| `WalletMainnetReadiness_Bip86DeterminismProperty1000RandomMnemonics` | Fails on Mac + Linux in ~12 ms — fast property failure indicates determinism break on a near-immediate input. Tests that 1000 random mnemonics deterministically derive the same BIP86 address. | BIP86 derivation determinism — the property without which wallet restore from seed is unreliable. | Yes. This is a foundational wallet correctness property. | Partial: smaller BIP86 unit tests exist but not the random-mnemonic property fuzz. | Keep quarantined. Investigate which input class triggers the determinism break; fix may require touching `dinero_wallet` BIP86 derivation. |
| `WalletMainnetReadiness_ReorgSelfSpendWithChangeRestoresSpentStateAndHeightMetadata` | Fails on Mac + Linux. After reorg, a self-spend with change doesn't correctly restore spent-state and height metadata. ~5 sec runtime to failure. | Reorg correctness: self-spends must unwind cleanly on chain reorganization. | Yes. Reorg safety is consensus-adjacent. | Partial: reorg unit tests exist but not the self-spend-with-change scenario. | Keep quarantined. Diagnose the reorg-unwind path for self-spends with change outputs; likely touches wallet UTXO bookkeeping. |

## Already Running

`CsnProofRefresh` is registered as `CsnProofRefresh`, but the old exclusion
regex listed `test_csn_proof_refresh`, which is the binary name. CTest excludes
by registered test name, so that regex did not match.

Focused local run:

```text
ctest --test-dir build-cmake-tests --output-on-failure --timeout 60 -R '^CsnProofRefresh$'
Result: passed in 1.81s
```

Action: remove the stale `test_csn_proof_refresh` entry from the workflow and
plan. No behavior should change because the old regex was ineffective.

## Label-Only Passing Candidates

These tests are excluded only because their labels match the broad
`canonicality` quarantine. Focused local run:

```text
ctest --test-dir build-cmake-tests --output-on-failure --timeout 60 -R '^ShieldedV030Vectors$|^HeaderRestartSafety$'
Result: both passed
```

| Test | Labels | Current status | Action |
| --- | --- | --- | --- |
| `ShieldedV030Vectors` | Was `shielded;consensus;canonicality`; split to `shielded;consensus;canonical-vectors`. | Passes locally in 1.40s. | Graduated from the broad `canonicality` quarantine into normal Test Workflow v2. |
| `HeaderRestartSafety` | Was `headers;restart;canonicality;phase1-canonical-recovery`; split to `headers;restart;canonical-restart;phase1-canonical-recovery`. | Passes locally in 0.48s. | Graduated from the broad `canonicality` quarantine into normal Test Workflow v2 while retaining release-suite ownership via `phase1-canonical-recovery`. |

## Recommended Next PRs

1. Split `WalletMainnetReadiness` into smaller CTest entries. Re-enable passing
   subcases first, then fix the four failing hardening cases.
2. Repair `ConsensusFormalVerification` as a consensus-test-vector/formula PR.
3. Repair `WalletDescriptorActiveContext` after deciding whether `1447h` or the
   current derivation path is the canonical wallet descriptor expectation.
4. Keep `ArchivalBlockReader` and `UtreexoE2ERelay` quarantined until their
   root-mismatch failures are understood. They are valid safety signals, not
   deletion candidates.

Deletion is not recommended for any remaining exact-name quarantined test. The
only retired item in this pass was the stale, ineffective
`test_csn_proof_refresh` exclusion entry.
