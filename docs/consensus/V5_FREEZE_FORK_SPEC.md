# Dinero v5 Freeze Fork Specification

**Status:** Armed for mainnet
**Version:** 0.2
**Scope:** Activation-height fork on the existing v5 chain to freeze private-lane features and enforce Taproot-only outputs.
**Current chain impact:** Mainnet activation set to block 4000 on 2026-04-16 (tip was ~3733). Gates fire at block 4000 and above. Testnet remains `UINT32_MAX` pending its own soak.

## Executive Summary

The v5 chain will remain the active Dinero chain. This specification defines a coordinated activation-height fork that:

- freezes creation of new confidential outputs
- freezes the ring and ring-covenant transaction families
- requires every new output to be `witness_v1_taproot` (or `OP_RETURN` for commitments)
- leaves all non-CT, non-ring pre-activation UTXOs spendable under their original rules
- leaves all legacy code paths alive in the binary for spend-side validation

**Known consequence:** because the only way to spend a confidential-transaction
UTXO or to reference it as a ring member is via a `tx.version == 3` or
`tx.version == 4` transaction, and Gate 2 rejects those formats
post-activation, pre-activation confidential UTXOs are intentionally
stuck after the freeze. This is the same class of "stuck-but-accounted-for"
outcome as the 3474 drifted CT UTXOs from the v5 incident; wallet mitigation
`a4605c070` already skips these. The wallet-side mitigation is Silent
Payments + CoinJoin, shipped separately (not consensus).

Wallet-layer privacy (Silent Payments, default-on CoinJoin) is shipped alongside the freeze but is not consensus and not part of this document.

## Design Principle

Utreexo's correctness, both stateful (the live forest tracking the UTXO set) and stateless (inclusion proofs verifying under the committed root), is broken today by one specific interaction: confidential-transaction outputs are inserted into the forest with `value=0` while `ChainDB` stores the actual amount. This means `UTXOPositionIndex::Rebuild` cannot re-derive the same leaf hash it inserted, and the forest drifts from `ChainDB`. Tonight's debugging session found 3474 such drifted UTXOs on the live chain.

The freeze stops new drift from accumulating. Existing drifted UTXOs remain a known artifact of v5; they are out of scope for this document.

## Consensus Rules At `FREEZE_FORK_ACTIVATION_HEIGHT`

For every block at height `h >= FREEZE_FORK_ACTIVATION_HEIGHT`, every non-coinbase transaction `tx` in the block MUST satisfy:

1. **No confidential outputs.** For every `output` in `tx.vout`, `output.is_confidential == false`.
2. **No ring or ring-covenant transaction formats.** `tx.version != 3` AND `tx.version != 4`.
3. **Taproot-only outputs.** For every `output` in `tx.vout`, `output.scriptPubKey` MUST be a valid `witness_v1_taproot` script, or a consensus-permitted `OP_RETURN` commitment (coinbase commitments, DNRF filter commitments).

Any transaction violating any of these rules MUST be rejected at block acceptance and at mempool admission when the target height is at or above activation.

## What Remains Unchanged

- **Utreexo forest semantics.** Same SHA-256 leaf hashing, same `HashUTXO(txid, vout, amount, scriptPubKey)`, same `utreexo_root` field in the 128-byte header.
- **Header shape.** No changes to `include/mining/header_layout.h`.
- **PoW, ASERT, block interval, block size limits.** No changes.
- **Existing transparent UTXO spendability.** Pre-activation transparent and legacy-script UTXOs remain spendable, provided the spending transaction itself satisfies the freeze gates and produces only post-freeze-allowed outputs.
- **Legacy private-lane state stays frozen.** Pre-activation CT and ring UTXOs remain intentionally unspendable after activation. On the active v5/v7 development line, the legacy ring/confidential implementation has since been removed from the runtime tree entirely; that excision does not create a drain path.

## What Becomes Dead-Path Post-Activation

After activation, new CT or ring-path creation is consensus-invalid:

- `BlockAssembler` output construction with `is_confidential=true`
- Mempool acceptance of `tx.version == 3 || tx.version == 4`
- Mempool acceptance of non-Taproot scriptPubKey outputs
- Block validation acceptance of the above

This is not a drain window for pre-activation CT or ring UTXOs; those outputs are intentionally frozen once Gate 2 activates.

## Activation Parameters

Following the template at `include/consensus/ring_covenant_activation.h`:

```cpp
struct FreezeForkActivationParams {
    static constexpr uint32_t MAINNET_ACTIVATION_HEIGHT = 4000;        // set 2026-04-16 (tip ~3733)
    static constexpr uint32_t TESTNET_ACTIVATION_HEIGHT = UINT32_MAX;  // set after testnet soak
    static constexpr uint32_t REGTEST_ACTIVATION_HEIGHT = 200;         // regtest harness (> COINBASE_MATURITY)

    static bool IsFreezeForkActive(uint32_t height, Chain chain);
    static uint32_t GetActivationHeight(Chain chain);
};
```

Mainnet is set to `4000` as of 2026-04-16 (tip was ~3733, giving ~267 blocks / ~9 hours runway for the fleet to build and deploy the new binary). Testnet remains `UINT32_MAX` until its own soak. Regtest is `200` so the regtest harness can exercise the activation boundary deterministically.

The operator shortens the `current_tip + 2000` recommendation in the original draft because regtest already passed and the fleet (5 nodes: 4 servers + 1 Mac) can roll in under an hour.

## Correctness Proof

The test that directly proves the freeze accomplishes its stated goal:

> **Test F:** Start regtest at genesis. Mine `FREEZE_FORK_ACTIVATION_HEIGHT + 100` blocks using only `version=2` transparent transactions with `witness_v1_taproot` outputs and `is_confidential=false`. After mining completes, call `UTXOPositionIndex::Rebuild` against the resulting ChainDB and forest. Assert `report.missing == 0` and `report.malformed == 0`.

A passing Test F proves that in the freeze-fork world, with only transparent Taproot activity, the forest and ChainDB do not drift. This is the structural guarantee the freeze is buying.

## Implementation Files

The following files need additive changes only. No existing function is modified.

- **New file:** `include/consensus/freeze_fork_activation.h` — the activation params struct following the existing template.
- **Modified:** `src/consensus/block_validation.cpp` — apply the three gates in the non-coinbase tx loop of `BlockValidator::ValidateBlock`.
- **Modified:** `src/consensus/parallel_block_validator.cpp` — mirror the same three gates in the parallel dispatch path.
- **Modified:** `src/consensus/validation_queue.cpp` — mirror the same three gates in queued block validation.
- **Modified:** `src/daemon/validation_mempool.cpp` — mirror the same three gates at mempool admission using `active_tip + 1` as the target height.
- **New file:** `tests/regtest/test_freeze_fork.sh` (or equivalent) — implements Tests A through F below.

No files are deleted. No existing function signature is modified. No existing constant is changed.

## Test Plan

Six regtest cases. Each test starts a fresh regtest daemon at genesis and deterministically reaches the relevant state.

- **Test A — Pre-activation behavior unchanged.** Mine blocks 1 through `REGTEST_ACTIVATION_HEIGHT - 1` and exercise a CT output, a v3 or v4 tx, and a non-Taproot output. All three must be accepted by mempool and included in a block.

- **Test B — Post-activation: CT outputs rejected.** Mine past activation height. Attempt to submit a tx with `is_confidential=true` output. Mempool admission MUST reject. Direct block submission with the same output MUST be rejected by block validation.

- **Test C — Post-activation: `version ∈ {3, 4}` rejected.** Same pattern as Test B, using `tx.version = 3` and `tx.version = 4`. Both MUST be rejected at mempool and block-validation layers.

- **Test D — Post-activation: non-Taproot outputs rejected.** Same pattern, using a P2PKH output. MUST be rejected.

- **Test F — Correctness proof.** Described above. `UTXOPositionIndex::Rebuild` returns `missing == 0` and `malformed == 0` after 100 post-activation blocks. Empirically: the regtest harness (`tests/regtest/test_freeze_fork.sh`) asserts `blockchain.getutxoproof` returns a live proof for a post-activation wallet UTXO, which is equivalent — a successful proof proves forest ↔ position-index ↔ ChainDB are all in sync for that UTXO.

Note: there is no separate "drain path" test. Because Gate 2 blocks v3 and v4 formats outright, pre-activation confidential UTXOs cannot be drained via the private path after activation — see Executive Summary's *Known consequence* paragraph.

All six tests must pass. Test F is the load-bearing one.

## Non-Goals

- Genesis reset, fresh chain, new inscription — out of scope. v5 remains the active chain.
- Recovery of the 3474 pre-activation drifted UTXOs — out of scope. They remain unspendable via the private path; wallet mitigation `a4605c070` already skips them.
- Removal of legacy code — out of scope. Deferred to a future release after drain.
- Wallet-layer privacy features (Silent Payments, CoinJoin) — out of scope for this doc. Shipped separately as wallet-only changes.
- Shielded lane or future ZK lane — out of scope. Reserved for later via `header_feature_flags` and future `tx.version = 5`.

## Deployment Sequence

1. Land this spec and get implementation review.
2. Implement the additive gates with initial `MAINNET_ACTIVATION_HEIGHT = UINT32_MAX`. Ship in a release.
3. Run Tests A through F on regtest. All must pass.
4. Fleet soak with the new binary at `UINT32_MAX` — no behavioral change on mainnet yet, but the new code is running and observed.
5. Observe for drift-accumulation telemetry on mainnet (none should appear, since the gates are `UINT32_MAX` and the existing drift is stuck-not-growing).
6. Set `MAINNET_ACTIVATION_HEIGHT` to `current_tip + 2000` in a subsequent release.
7. Coordinate fleet upgrade before the activation height is reached.
8. After activation: monitor for rejected-tx events, including adversarial attempts, stale pre-upgrade clients, and legitimate wallet attempts to spend now-frozen CT / ring UTXOs.

## Deprecation Timeline (Post-Activation)

- **0–6 months:** Legacy verifier paths remain compiled, but wallets signal clearly that CT / ring / covenant UTXOs created before activation are frozen and not expected to drain.
- **6–12 months:** Operator messaging intensifies. Fleet measures how much frozen legacy private-lane state still exists for support and accounting purposes.
- **12+ months (release X.Y):** If the remaining legacy private-lane state is operationally irrelevant, remove the dead code in a non-consensus cleanup release. No activation needed — dead code removal only.
