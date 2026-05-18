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
| `ShieldedDerivation` | Fails on Linux CI. `ShieldedDerivationVectorFixture` reports four vector failures, despite passing locally on macOS. | Pins shielded key-derivation vectors for independent implementation parity. | Yes. Vector coverage is valuable. | No complete replacement identified. | Keep quarantined. Fix vectors or platform-dependent implementation expectations, then re-enable. Do not delete. |
| `WalletDescriptorActiveContext` | Fails locally. Descriptor check expects canonical coin type `1447h`; current wallet output appears to use the newer wallet derivation policy. | Ensures descriptor RPC reads from the active wallet DB instead of leaking default-wallet descriptors. | Yes, but expectation may be stale. | No replacement identified for active-wallet descriptor isolation. | Keep quarantined. Decide the canonical coin-type policy first, then update the expectation or fix implementation. |
| `ArchivalBlockReader` | Aborts locally during `Reindexer_UsesFlatfilesWithoutShadowBodyWrites`; reindex forest root mismatch at height 1. | Ensures archival reads prefer flatfiles and legacy ChainDB body fallback is explicit. | Yes. This protects storage/reindex behavior. | Partial storage tests exist, but not the same flatfile/fallback signal. | Keep quarantined. Treat as harness/runtime debt around reindex Utreexo roots, not obsolete test debt. |
| `UtreexoE2ERelay` | Fails locally. CSN rejects a transaction spending a block-2 output due to local/proof root mismatch. | End-to-end Bridge to CSN block sync plus transaction proof relay. | Yes. This is Utreexo relay safety coverage. | Unit-level Utreexo tests exist, but not this full relay path. | Keep quarantined. Split root-tracking failure into a focused Utreexo relay repair PR. |
| `WalletMainnetReadiness` | Fails locally. 5 of 9 tests pass; 4 fail across encryption restore, legacy encryption migration, BIP86 determinism, and reorg self-spend history. | Wallet-only mainnet hardening: encryption, restore determinism, invalid mnemonic handling, BIP86 persistence. | Yes. Several subcases are core wallet readiness signals. | Partial coverage exists in smaller wallet tests, but not the combined mainnet readiness suite. | Keep quarantined. Split into smaller CTest entries so passing subcases can graduate without waiting for all wallet-hardening debt. |

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
