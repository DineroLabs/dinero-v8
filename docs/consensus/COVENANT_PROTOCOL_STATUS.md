# Covenant protocol status

## Local assurance command

Run the complete covenant-labelled implementation, wallet, recovery,
restart/reorg, boundary, and policy lane with:

```sh
./scripts/covenant-readiness.sh --build-dir build
```

Add `--mutation` to require the complete consensus mutation score and write a
machine-readable report into the build directory. The runner deliberately uses
CTest's `--no-tests=error`; a renamed or de-registered covenant test must fail
closed instead of producing an empty green run. Passing this command does not
arm mainnet activation: the production activation heights remain governed by
the chain parameters and the release gates below.

This file is the operator-facing status summary. It does not replace the
normative opcode specifications or activation parameters.

The combined normative candidate is `DINERO_COVENANT_PROFILE_V1.md`. The
reviewer handoff is `COVENANT_EXTERNAL_REVIEW_PACKAGE.md`.

## Implemented and exercised

- Transparent Taproot key-path/script-path validation uses one BIP341 sighash
  implementation, checked against the official wallet vectors.
- Tapscript enforces the exact BIP342 `OP_SUCCESS` set, control-block parity,
  trailing-annex rules, unknown-leaf behavior, signature validation weight,
  NULLFAIL behavior, and 64/65-byte Schnorr hash-type parsing. The inherited
  BIP342 interpreter surface is implemented: all push encodings, conditionals,
  main/alternate-stack operations, arithmetic and comparisons, hash
  operations, CLTV/CSV, `OP_CODESEPARATOR`, `OP_CHECKSIGADD`, disabled-opcode
  behavior, minimal-encoding policy, and the combined 1,000-element limit.
- `OP_CHECKTEMPLATEVERIFY` computes the BIP119 default template hash and is
  checked against the upstream compact vectors. Dinero-only transaction forms
  that BIP119 does not commit to—shielded versions, explicit-fee encoding, and
  confidential outputs—fail closed. Dinero's normative profile is
  `CTV_BIP119_PROFILE.md`.
- CCV successor binding v1 authenticates previous state, immutable code/tree,
  exact transparent value, and the next-state output. Its normative definition
  is in `CCV_SUCCESSOR_BINDING_V1.md`.
- Mempool admission, mining selection, block validation, activation rollback,
  reorg revalidation, and persistence restart are exercised through production
  components by `CovenantSystemLifecycle`.
- Transaction-wide CTV and BIP341 data is precomputed once and shared across
  validation inputs. CTV and CCV may each execute at most once per revealed
  tapscript. Limits, neuter evidence, and scaling results are recorded in
  `COVENANT_RESOURCE_LIMITS.md`.
- The regtest-only wallet/RPC profile constructs CTV and CCV Taproot
  script-path artifacts, stores checksummed public recovery descriptors and
  watch scripts atomically, and re-derives them after wallet restart. Its
  construction and recovery contract is documented in
  `../wallet/COVENANT_PROFILE_V1_RECOVERY.md`.
- The default wallet CCV artifact is owner-authorized by a BIP340 x-only key.
  Its script executes CCV continuity first and then `OP_CHECKSIG`; the signed
  transaction commits the successor output derived from the chosen next state.
  The public key and wallet derivation origin survive in the checksummed
  descriptor, while the private key is re-derived and matched at spend time.
  The original permissionless type remains available only with explicit
  `permissionless: true` acknowledgement.
- `CovenantWalletMultinodeLifecycle` exercises two live daemons: CTV and CCV
  funding, owner and ordinary fee-input signing on a CCV transition, normal
  peer relay and mining, wallet restart and descriptor/key recovery, a
  longer-chain reorg, mempool revalidation, independent rebroadcast, and
  reconfirmation.

## Activation policy

- Mainnet CTV and CCV are deferred and dormant (`UINT32_MAX`). The previously
  proposed block-100,000 activation was superseded after complete upstream
  BIP119 transaction vectors exposed a material interpreter-semantic defect.
  No replacement height is authorized by this change. The associated
  consensus-loosening P2WSH/Taproot outer-stack correction is staged behind
  the same CTV verification flag, so deferral preserves deployed mainnet
  acceptance behavior during mixed-version operation.
- Testnet CTV and CCV remain dormant (`UINT32_MAX`).
- CSFS and TXHASH do not yet have an approved normative specification or
  external review. They remain dormant on every production network, and their
  BIP342 slots retain `OP_SUCCESS` behavior.
- Confidential CTV and CCV are unsupported rather than assigned a custom,
  unaudited commitment extension.

Regtest activates CTV and CCV at height 20 so boundary, wallet, recovery, and
multi-node work can exercise the candidate semantics. Mainnet remains dormant;
the regtest height is not a production activation signal.

The regtest wallet refuses to construct spends before the candidate height has
the relevant rule active. Mempool admission and block-template selection also
reject revealed CTV, CCV, CSFS, or TXHASH scripts before that individual
opcode's activation height while preserving historical consensus NOP/
`OP_SUCCESS` behavior. `CovenantSystemLifecycle` separates Taproot script-path
activation from the CTV boundary and proves admission, mining selection,
rollback, revalidation, and restart behavior across it. Production release
candidates must repeat this evidence at any newly proposed production boundary.

## Interpreter conformance evidence

- `TaprootScriptPathConsensus` exercises the BIP342-specific execution rules
  and every inherited opcode family. Its deterministic differential test runs
  128 generated input states through five opcode programs in both Dinero's
  independent legacy interpreter and its tapscript interpreter, then compares
  the resulting stacks (640 comparisons).
- `ScriptJSONTests` executes the imported `tests/data/script_tests.json`
  corpus: 1,213 executable vectors, zero failures. The runner now returns a
  failing process status for any failed vector; it no longer tolerates up to
  100 failures.
- `BIP341SighashVectors` checks the upstream wallet sighash vectors, while
  `CovenantActivation`, `CcvSuccessorBinding`, and `CovenantScriptPath` cover
  Dinero-specific activation and transaction semantics.
- Neuter checks prove the combined main/alternate-stack guard and strict
  corpus exit status are load-bearing.

Bitcoin-derived vectors are authoritative only for behavior Dinero explicitly
inherits. Dinero transaction forms and covenant opcodes remain governed by the
Dinero specifications and native vectors in this directory.

## Historical mainnet evidence

The canonical scan through height 76,105 found 74,170 P2TR outputs and 2,352
P2TR spends, all key-path with 64-byte implicit `SIGHASH_DEFAULT`. It found
zero explicit-sighash key-path spends, revealed script paths, annexes, or
covenant opcodes. The reproducible report and scanner are:

- `COVENANT_MAINNET_REACHABILITY_2026-07-30.md`
- `scripts/audit/covenant_history_scan.js`

Unspent P2TR outputs can commit to hidden trees, so chain history cannot prove
that no unspent output contains a dormant covenant leaf. The activation is a
coordinated soft fork and must not treat absence of revealed historical use as
a substitute for deployment discipline.

## Release gates before any future mainnet activation

1. Complete the reproducible open-source assurance record: frozen
   specification and release commit, an implementation-independent CCV
   reference model, differential and property tests, sanitizer fuzzing,
   mutation evidence, bounded-state analysis, public review opportunity,
   resolution of every reported critical/high finding, and explicit owner
   acceptance of residual unaudited risk.
2. Repeat the activation-boundary, wallet-recovery, and multi-node deployment
   tests against the exact release candidate carrying the newly reviewed
   height.
3. Define a conservative go/no-go checkpoint, deploy the identical release to
   every validating node before it, and verify the expected consensus checksum,
   tip, peer set, and block-template behavior on each node.
4. If either assurance completion or fleet deployment misses that checkpoint,
   do not compress the rollout window. Schedule a later activation height in a
   new coordinated release.

The old height decision and its supersession are retained for audit in
`COVENANT_MAINNET_ACTIVATION_100000.md`. External review remains invited, but
the release gate is the objective open-source assurance record plus
release-candidate evidence. The deferral record is
`../audits/COVENANT_DEFERRAL_2026-08-22.md`.
