# Covenant profile v1 external-review package

Status: ready for independent review; not ready for production activation.

Date: 2026-07-30

Implementation base: `06148559b4673f2863c59a8ab1090679a52f64df`
(`origin/dinero-main` at the start of this work)

Implementation tip before the wallet/RPC lifecycle:
`d56b6dd6b`

Wallet/RPC implementation and live lifecycle:
`efa7cda89` and `744a1ce19`

Reviewers should review the final branch tip, including
`DINERO_COVENANT_PROFILE_V1.md`, against that base.

## 1. Review objective

Determine whether the dormant CTV/CCV implementation:

- exactly preserves inherited BIP341/BIP342 behavior;
- correctly implements the stated BIP119 profile and Dinero CCV v1;
- is consistent across mempool, mining, serial/stateless/parallel block
  validation, reorg, and restart paths;
- can be constructed, recovered, wallet-signed, relayed, revalidated after a
  reorg, and reconfirmed without a non-consensus bypass;
- has deterministic and sufficient denial-of-service bounds; and
- is suitable to proceed to a later activation proposal.

This review is not a request to approve an activation height.

## 2. Normative material

Read in this order:

1. `DINERO_COVENANT_PROFILE_V1.md`
2. `CTV_BIP119_PROFILE.md`
3. `CCV_SUCCESSOR_BINDING_V1.md`
4. `COVENANT_RESOURCE_LIMITS.md`
5. `COVENANT_PROTOCOL_STATUS.md`
6. `COVENANT_MAINNET_REACHABILITY_2026-07-30.md`

Upstream references:

- BIP341: `https://github.com/bitcoin/bips/blob/master/bip-0341.mediawiki`
- BIP342: `https://github.com/bitcoin/bips/blob/master/bip-0342.mediawiki`
- BIP119 proposal and vectors:
  `https://github.com/bitcoin/bips/tree/master/bip-0119`

## 3. Commit sequence

| Commit | Purpose |
|---|---|
| `139b88b03` | mainnet reachability scan and reproducible audit |
| `ba3263147` | fail-closed covenant and Taproot verification foundations |
| `94c509ca3` | Taproot conformance vectors |
| `16fdb0b5a` | activation and CCV binding vectors |
| `5542410cf` | covenant/Taproot test registration |
| `c6650c904` | dormant CTV and CCV specifications |
| `770087521` | tapscript pushes and conditional execution |
| `c44c28827` | BIP342 code-separator position commitment |
| `3d8e3abe2` | remaining inherited BIP342 execution surface |
| `71a390876` | conformance and differential evidence |
| `3b435f93c` | activation/reorg/mempool/mining/restart lifecycle |
| `4da3bfc04` | deterministic resource bounds and precomputation |
| `d56b6dd6b` | frozen combined profile-v1 specification |
| `efa7cda89` | checksummed wallet/RPC construction and recovery |
| `744a1ce19` | live mainnet guard and two-node regtest lifecycle |

Recommended review commands:

```text
git diff 06148559b4673f2863c59a8ab1090679a52f64df..HEAD
git log --reverse --oneline \
  06148559b4673f2863c59a8ab1090679a52f64df..HEAD
```

## 4. High-risk code

Primary consensus surfaces:

- `include/consensus/covenant_activation.h`
- `include/consensus/covenants.h`
- `include/consensus/tapscript_interpreter.h`
- `src/consensus/covenants.cpp`
- `src/consensus/tapscript_interpreter.cpp`
- `src/consensus/script_sighash.cpp`
- `src/consensus/script_verify.cpp`
- `src/consensus/script_validation.cpp`
- `src/consensus/transaction_validator.cpp`
- `src/consensus/block_validation.cpp`
- `src/consensus/parallel_block_validator.cpp`
- `src/daemon/mempool.cpp`

Chain and activation surfaces:

- `include/consensus/chainparams.h`
- `src/consensus/chainparams_impl.cpp`

Executable evidence:

- `tests/consensus/test_taproot_scriptpath_consensus.cpp`
- `tests/consensus/test_bip341_sighash_vectors.cpp`
- `tests/consensus/test_bip119_ctv_vectors.cpp`
- `tests/consensus/test_ccv_successor_binding.cpp`
- `tests/consensus/test_covenant_activation.cpp`
- `tests/consensus/test_covenant_system_lifecycle.cpp`
- `tests/wallet/test_covenant_profile.cpp`
- `tests/wallet/test_covenant_wallet_recovery.cpp`
- `tests/integration/test_covenant_wallet_multinode_lifecycle.sh`
- `tests/benchmarks/benchmark_covenant_validation.cpp`
- `tests/test_script_json.cpp`

Wallet/RPC construction and recovery surfaces:

- `include/wallet/covenant_profile.h`
- `src/wallet/covenant_profile.cpp`
- `src/rpc/methods_wallet_covenant_profile.cpp`
- `include/wallet/wallet_manager.h`
- `src/wallet/wallet_manager.cpp`
- `src/rpc/methods_wallet_context.cpp`

## 5. Questions requiring explicit reviewer answers

1. Does `0xb3` preserve NOP4 behavior before activation and BIP119 reserved
   argument behavior after activation?
2. Does activated `0xbe` correctly leave the BIP342 `OP_SUCCESS` set without
   altering any still-unassigned `OP_SUCCESS` opcode?
3. Are control-block parity, unknown leaf versions, annex placement,
   code-separator position, hash types, signature budget, NULLFAIL, and
   upgradable pubkey behavior BIP341/BIP342-compatible?
4. Is every byte of the CTV preimage serialized with the intended width,
   endianness, and conditional scriptsig field?
5. Does CTV fail closed for every Dinero transaction extension not committed
   by BIP119?
6. Is CCV's hash-to-x internal key derivation sound and domain-separated?
7. Does CCV bind the revealed code, previous state, exact tree, parity, input
   index, transparent value, successor state, and unique successor without an
   alternate unbound path?
8. Are the one-execution CTV/CCV limits consensus-safe and semantically
   non-restrictive for successful scripts?
9. Is `PrecomputedTransactionData` immutable for every live caller, correctly
   scoped to one transaction, and safe in parallel validation?
10. Are cached BIP341 values identical for default, ALL/NONE/SINGLE,
    ANYONECANPAY, and Dinero confidential-prevout hashing?
11. Can any mempool, block, stateless, parallel, reorg, restart, or cache path
    validate with the wrong target height or stale activation flags?
12. Does any supposedly constrained wallet leaf contain a dormant
    `OP_SUCCESS` opcode that makes it immediately spendable?
13. Can a malformed, non-canonical, wrong-profile, or checksum-corrupted
    recovery descriptor ever be imported or used to construct a spend?
14. Does wallet persistence atomically bind descriptor id, profile,
    descriptor, Taproot scriptPubKey, watch registration, and CCV successor
    lineage without implying possession of a key-path secret?
15. Does `wallet.signrawtransaction` preserve a preconstructed covenant
    script-path witness, sign only the remaining wallet-owned inputs, and
    report `complete` only after canonical validation of every input?

## 6. Reproduction

Configure a review build in the normal project manner, then:

```text
cmake --build build-covenants --target \
  dinerod \
  test_bip119_ctv_vectors \
  test_bip341_sighash_vectors \
  test_taproot_scriptpath_consensus \
  test_covenant_activation \
  test_covenant_system_lifecycle \
  test_ccv_successor_binding \
  test_covenant_profile_wallet \
  test_covenant_wallet_recovery \
  test_covenant_scriptpath \
  test_escape_hatches \
  test_covenant_semantic_oracles \
  benchmark_covenant_validation \
  -j8

ctest --test-dir build-covenants --output-on-failure \
  --no-tests=error \
  -R '^(BIP119CTVVectors|TaprootScriptPathConsensus|BIP341SighashVectors|CovenantActivation|CovenantSystemLifecycle|CcvSuccessorBinding|CovenantProfileWallet|CovenantWalletRecovery|CovenantScriptPath|CovenantWalletMultinodeLifecycle|EscapeHatchTests|Execution_CovenantSemantics_R7_7d)$'

./build-covenants/benchmark_covenant_validation
```

Additional conformance commands:

```text
./build-covenants/test_script_json
./build-covenants/test_taproot_scriptpath_consensus
```

Expected evidence at the review tip:

- 1,213/1,213 executable script-corpus vectors;
- 640/640 deterministic differential interpreter comparisons;
- 42/42 Taproot script-path tests;
- 4/4 BIP341 sighash-vector tests;
- 5/5 activation tests;
- 9/9 CCV tests;
- 3/3 lifecycle tests;
- 5/5 checksummed CTV/CCV construction tests;
- 2/2 wallet persistence and restart-recovery tests;
- 1/1 live mainnet-guard and two-daemon wallet/RPC relay, restart, reorg, and
  reconfirmation lifecycle; and
- 6/6 adjacent mempool/mining/UTXO tests.

The CTV and CCV execution-limit tests were independently neutered by changing
each maximum from one to two. Each test then failed because the repeated
script became valid; both limits were restored to one.

## 7. Historical evidence

The canonical mainnet scan through height 75,490 found:

- 73,555 P2TR outputs;
- 2,352 P2TR spends;
- all observed spends using key path;
- zero revealed script paths;
- zero annexes; and
- zero revealed or bare covenant opcodes.

This does not prove that no unspent P2TR output commits to a hidden leaf.
Production activation therefore remains a coordinated soft fork and must not
reinterpret hidden trees without explicit deployment planning.

## 8. Known limitations and blockers

- Mainnet and testnet CTV/CCV activation heights remain dormant.
- Independent review has not yet occurred.
- The profile-v1 wallet/RPC surface is intentionally regtest-only. Its
  successful lifecycle evidence does not approve testnet or mainnet
  activation.
- Release-candidate tests must be repeated at any proposed activation
  boundary and on every supported platform before production use.
- CSFS and TXHASH remain incomplete, uncosted, and deliberately dormant.
- Confidential CTV/CCV is undefined and rejected.
- A mainnet history scan cannot inspect hidden Taproot leaves.

These are blockers, not deferred release notes. They must remain visible in
any PR and activation proposal.

## 9. Reviewer deliverable

For each question in section 5, record:

- `accept`, `reject`, or `needs change`;
- the reviewed commit hash;
- file and line references;
- any reproducer or counterexample; and
- whether the finding blocks wallet work, testnet activation, or mainnet
  activation.

At least one reviewer should be independent of the implementation authors and
should reproduce the vectors and neuter checks from a clean checkout.
