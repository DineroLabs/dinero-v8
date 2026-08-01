# Covenant protocol status

This file is the operator-facing status summary. It does not replace the
normative opcode specifications or activation parameters.

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

## Deliberately dormant

- Mainnet and testnet CTV, CCV, CSFS, and TXHASH activation heights are
  `UINT32_MAX`.
- CSFS and TXHASH do not yet have an approved normative specification or
  external review. Their BIP342 slots remain `OP_SUCCESS`.
- Confidential CTV and CCV are unsupported rather than assigned a custom,
  unaudited commitment extension.

Regtest activates CTV and CCV at height 20 so boundary, wallet, recovery, and
multi-node work can exercise the reviewed semantics. Regtest behavior is not a
production activation decision.

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
that no unspent output contains a dormant covenant leaf. Keeping production
opcode activations dormant avoids assuming otherwise.

## Required before production activation

1. Finish a normative spec and vectors for every Dinero-specific opcode being
   proposed.
2. Add activation-boundary, reorg, mempool, mining, wallet/recovery, and
   multi-node tests for the chosen production height.
3. Benchmark adversarial scripts and remove any repeated transaction-wide
   hashing from per-input execution.
4. Obtain independent consensus and cryptographic review.
5. Coordinate the activation release across all validating nodes.

No mainnet/testnet activation height should be assigned merely because the
regtest implementation passes its local suites.
