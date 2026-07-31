# Covenant protocol status

This file is the operator-facing status summary. It does not replace the
normative opcode specifications or activation parameters.

## Implemented and exercised

- Transparent Taproot key-path/script-path validation uses one BIP341 sighash
  implementation, checked against the official wallet vectors.
- Tapscript enforces the exact BIP342 `OP_SUCCESS` set, control-block parity,
  trailing-annex rules, unknown-leaf behavior, signature validation weight,
  NULLFAIL behavior, and 64/65-byte Schnorr hash-type parsing.
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

1. Finish a normative spec and vectors for every opcode being proposed.
2. Complete and differentially test the remaining BIP342 interpreter surface
   (including conditionals, arithmetic/hash operations, all push encodings,
   and `OP_CODESEPARATOR`). The current regtest covenant profiles intentionally
   exercise a restricted tapscript subset.
3. Add activation-boundary, reorg, mempool, mining, wallet/recovery, and
   multi-node tests for the chosen production height.
4. Benchmark adversarial scripts and remove any repeated transaction-wide
   hashing from per-input execution.
5. Obtain independent consensus and cryptographic review.
6. Coordinate the activation release across all validating nodes.

No mainnet/testnet activation height should be assigned merely because the
regtest implementation passes its local suites.
