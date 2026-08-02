# Covenant profile v1 external-review package

Status: ready for public and independent review. Mainnet CTV/CCV activation is
scheduled for block 100,000, but the activation release must not ship until the
open-source assurance record and deployment gates are satisfied. External
review is invited and valuable; a paid audit is not a release prerequisite.

Date: 2026-08-01

Implementation base: `ed3fb9cb8f58485b578dc63708ea8237a57c26c9`
(`origin/dinero-main` after PR #476)

Activation review target: the exact head of
`codex/covenant-mainnet-80000`, including
`COVENANT_MAINNET_ACTIVATION_100000.md`. Record its full hash before review.
The branch name is historical; the normative file and consensus parameters pin
height 100,000.

Historical review stack branches (the short hashes below are implementation
anchors, not a substitute for reviewing the merged implementation and the
activation target above):

- foundation and dormant protocol: `codex/covenants-01-foundation`
  (anchor `c7b8cb5612`);
- inherited BIP342 completion: `codex/covenants-02-bip342`
  (anchor `501f6c21fe`);
- lifecycle, resource bounds, and frozen profile:
  `codex/covenants-03-lifecycle` (anchor `7c47744e48`); and
- wallet/RPC construction, recovery, and live lifecycle:
  `codex/covenants-04-wallet` (anchor `2c1f180618`).

Reviewers should review the activation target against the implementation base
and use the historical branches only to recover authorship and sequencing.
The project assurance record must remain reproducible even if no external
reviewer participates.

## 1. Review objective

Determine whether the dormant CTV/CCV implementation:

- exactly preserves inherited BIP341/BIP342 behavior;
- correctly implements the stated BIP119 profile and Dinero CCV v1;
- is consistent across mempool, mining, serial/stateless/parallel block
  validation, reorg, and restart paths;
- can be constructed, recovered, wallet-signed, relayed, revalidated after a
  reorg, and reconfirmed without a non-consensus bypass;
- has deterministic and sufficient denial-of-service bounds; and
- is suitable for the scheduled block-100,000 mainnet activation.

This review explicitly includes the height-100,000 boundary and the deployment
and abort conditions in the activation plan.

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
| `76f32f0a7` | mainnet reachability scan and reproducible audit |
| `20d0c9e67` | fail-closed covenant and Taproot verification foundations |
| `0c4751fac` | Taproot conformance vectors |
| `a95d11012` | activation and CCV binding vectors |
| `16f097ea3` | covenant/Taproot test registration |
| `e6dca246e` | dormant CTV and CCV specifications |
| `c7b8cb561` | canonical tagged-hash and SHA-256 test helpers |
| `4c503f2aa` | tapscript pushes and conditional execution |
| `5d376a26b` | BIP342 code-separator position commitment |
| `6e5b2f3ba` | remaining inherited BIP342 execution surface |
| `501f6c21f` | conformance and differential evidence |
| `7cd102ba1` | activation/reorg/mempool/mining/restart lifecycle |
| `71309689e` | deterministic resource bounds and precomputation |
| `7c47744e4` | frozen combined profile-v1 specification |
| `497c68e80` | checksummed wallet/RPC construction and recovery |
| `0e27df1b9` | live mainnet guard and two-node regtest lifecycle |
| `2c1f18061` | wallet lifecycle and recovery readiness record |

Recommended review commands:

```text
git diff cb21a91d4226a8a0b8b798f065d1b867f7de88bb..HEAD
git log --reverse --oneline \
  cb21a91d4226a8a0b8b798f065d1b867f7de88bb..HEAD
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
16. Given that activation is a flag-day soft fork with no miner signalling and
    CTV/CCV remain dormant on public testnet, are the height-100,000 deployment,
    four-node readiness checkpoint, monitoring, and abort rules sufficient for
    first public-network enforcement?

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

Expected evidence at the review tip (re-run; do not inherit these counts from
an earlier head):

- 1,213/1,213 executable script-corpus vectors;
- 640/640 deterministic differential interpreter comparisons;
- 42/42 Taproot script-path tests;
- 4/4 BIP341 sighash-vector tests;
- 6/6 activation tests;
- 9/9 CCV tests;
- 3/3 lifecycle tests;
- 6/6 checksummed CTV/CCV construction tests;
- 3/3 wallet persistence and restart-recovery tests;
- 1/1 live mainnet-guard and two-daemon wallet/RPC relay, restart, reorg, and
  reconfirmation lifecycle; and
- 6/6 adjacent mempool/mining/UTXO tests.

An earlier 2026-07-31 rebased tip passed all 98 tests selected by
`ctest -L 'consensus|covenant|taproot'`, including the cold-start harness,
consensus fuzzer, two-node sync, and the serialized live covenant wallet
lifecycle. Every selected executable was built before the run; the result was
98/98 with zero `Not Run` entries. This is historical evidence only; the final
review head must repeat the selection.

The CTV and CCV execution-limit tests were independently neutered by changing
each maximum from one to two. Each test then failed because the repeated
script became valid; both limits were restored to one.

Wallet-layer guards were also neuter-verified independently: removing
canonical lowercase descriptor enforcement made the uppercase-alias test
fail; removing watch-path collision verification committed a descriptor under
the wrong existing watch path; removing the next-height activation guard let
the pre-activation CTV spend constructor succeed; removing the explicit CCV
acknowledgement let permissionless construction proceed silently; and removing
the output money-range check admitted a zero-valued CTV output. Each guard was
restored before the passing focused and two-daemon runs.

## 7. Historical evidence

The canonical mainnet scan through height 76,105 found:

- 74,170 P2TR outputs;
- 2,352 P2TR spends;
- all observed spends using key path;
- all 2,352 key-path spends using 64-byte implicit `SIGHASH_DEFAULT`;
- zero 65-byte explicit sighash forms;
- zero revealed script paths;
- zero annexes; and
- zero revealed or bare covenant opcodes.

This does not prove that no unspent P2TR output commits to a hidden leaf.
Production activation therefore remains a coordinated soft fork and must not
reinterpret hidden trees without explicit deployment planning.

## 8. Known limitations and blockers

- Mainnet CTV/CCV activation is scheduled at block 100,000; testnet remains
  dormant.
- Activation has no miner-signalling or versionbits phase. Because testnet also
  remains dormant, mainnet would be the profile's first public-network
  enforcement. The controlled four-node fleet permits coordinated deployment
  but provides no public soak period; this risk must be explicit in the public
  review package and final owner acceptance record.
- No paid or formal independent audit is required. External review remains
  invited, and every substantive report must be resolved publicly, but release
  readiness is determined by the reproducible open-source assurance gates in
  `COVENANT_MAINNET_ACTIVATION_100000.md`.
- The profile-v1 wallet/RPC surface remains intentionally regtest-only. The
  consensus activation does not silently expose mainnet construction RPCs;
  production wallet enablement requires a separate reviewed release.
- The default profile-v1 wallet CCV artifact is owner-authorized with BIP340.
  The legacy permissionless form remains available only after explicit
  acknowledgement.
- Mempool admission and block-template selection reject revealed CTV, CCV,
  CSFS, and TXHASH scripts before each opcode's independent activation height.
  Historical consensus NOP/`OP_SUCCESS` behavior is unchanged. The lifecycle
  suite separates Taproot script-path activation from CTV and exercises
  admission, mining, rollback, revalidation, and restart across that boundary;
  the same tests must be repeated at any proposed production boundaries.
- Release-candidate tests must be repeated at the block-100,000 boundary and on
  every supported platform before production use.
- Importing the upstream `tx_valid.json` and `tx_invalid.json` interpreter
  corpus requires an explicit Bitcoin-to-Dinero verification-flag mapping and
  prevout plumbing. This recommended pre-activation work is tracked in #483;
  it must be complete by the height-99,000 checkpoint or activation must be
  rescheduled.
- CSFS and TXHASH remain incomplete, uncosted, and deliberately dormant.
- Confidential CTV/CCV is undefined and rejected.
- A mainnet history scan cannot inspect hidden Taproot leaves.

These are blockers, not deferred release notes. They must remain visible in
any PR and activation proposal.

## 9. Open-source assurance deliverable

The activation record must identify the frozen specification and release
commit and provide reproducible evidence for:

- an implementation-independent CCV reference model;
- randomized differential tests for valid and invalid transitions;
- explicit properties covering value preservation, unique successor mapping,
  replay/rewind/skip rejection, immutable code/tree binding, and canonical
  encodings;
- sanitizer-backed fuzzing of wire parsing and the live Taproot verification
  path;
- mutation tests proving every normative CCV clause is load-bearing;
- bounded exhaustive/model analysis of the transition rules;
- resource, lifecycle, restart, reorg, and multi-node results; and
- a public comment period of at least 14 calendar days, closing before height
  99,000, with every reported critical/high finding resolved.

The owner must then record whether the residual risk is accepted, including
the explicit fact that no external expert may have signed off. This is an
achievable open-source assurance gate, not a claim of commercial audit
equivalence.

## 10. Optional external-review deliverable

For each question in section 5, record:

- `accept`, `reject`, or `needs change`;
- the reviewed commit hash;
- file and line references;
- any reproducer or counterexample; and
- whether the finding blocks wallet work, testnet activation, or mainnet
  activation.

Any external reviewer should state whether they are independent of the
implementation authors and should reproduce the vectors and neuter checks from
a clean checkout. Their participation strengthens the record but is not a
condition that can remain open indefinitely.
