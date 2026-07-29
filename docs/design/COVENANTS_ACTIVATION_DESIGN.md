# Dinero Covenant Activation Design Document

**Status:** SUPERSEDED / HISTORICAL
**Date:** 2026-03-28
**Authors:** Mirsad Hajdarevic

---

> **Do not use this document as the current consensus specification.** Its
> statements that CCV, the covenant wallet, and confidential stateful
> covenants are complete predate the successor-binding audit and are false.
> The normative CCV rule, limitations, and activation status are defined in
> [`docs/consensus/CCV_SUCCESSOR_BINDING_V1.md`](../consensus/CCV_SUCCESSOR_BINDING_V1.md).
> This file remains only as design history.

## 1. Current State

Dinero has a complete covenant implementation that is **built but not active**:

| Component | Status | Location |
|---|---|---|
| OP_CTV (BIP-119) | Implemented, consensus-integrated | `consensus/covenants.cpp`, `script_interpreter.cpp` |
| OP_CHECKSIGFROMSTACK | Implemented, consensus-integrated | `consensus/covenants.cpp`, `script_interpreter.cpp` |
| OP_TXHASH | Implemented, consensus-integrated | `consensus/covenants.cpp`, `script_interpreter.cpp` |
| OP_CHECKCONTRACTVERIFY | Implemented, consensus-integrated | `consensus/covenants.cpp`, `script_interpreter.cpp` |
| Tapscript interpreter | Implemented with covenant handlers | `consensus/tapscript_interpreter.cpp` |
| Covenant wallet | Full CTV/CSFS/contract management | `wallet/covenant_wallet.cpp` |
| Covenant builders | Template construction, vault patterns | `wallet/covenant_builders.cpp`, `covenant_patterns.cpp` |
| Mempool policy | Depth limits, DoS protection, mining priority | `mempool/covenant_policy.cpp` |
| RPC methods | 14+ endpoints (create, decode, spend, list) | `rpc/methods_wallet_covenant.cpp` |
| Tests | Unit, integration, property-based | `tests/covenant/`, `tests/test_covenants.cpp` |

**The blocker:** Taproot script-path spending is disabled by consensus policy. Only key-path (single Schnorr signature) is supported. Covenant scripts live in Tapscript — they need script-path execution to run.

**Verification flags exist but are gated:**
- `SCRIPT_VERIFY_CHECKTEMPLATEVERIFY` (bit 20)
- `SCRIPT_VERIFY_CHECKSIGFROMSTACK` (bit 21)
- `SCRIPT_VERIFY_TXHASH` (bit 22)
- `SCRIPT_VERIFY_CHECKCONTRACT` (bit 23)
- Combined: `SCRIPT_VERIFY_COVENANTS`

These are included in `SCRIPT_VERIFY_STANDARD` but cannot fire because script-path execution never reaches them.

---

## 2. What Activation Unlocks

Enabling Taproot script-path + covenant opcodes gives Dinero:

### Immediately available (code already exists)

**Vaults** — Two-step withdrawal with time delay and recovery sweep. Funds locked so they can only move to pre-specified destinations. Key compromise alone is not enough to steal — attacker must wait through the delay while the owner sweeps to recovery.

**Treasury controls** — Capped withdrawals, approved destinations, staged release schedules. Protocol-enforced spending policy for DAOs, foundations, project funds.

**Escrow** — Cooperative settlement, timeout-based refund, arbitration branch. Narrower than smart contracts but sufficient for practical settlement.

**Inheritance** — Normal spend while holder is active, delayed recovery after timeout, family/executor multisig release.

**Congestion control** — CTV payment trees where one on-chain output commits to a tree of follow-up transactions. Batch payouts with sub-branches claimable independently.

**Delegation via CSFS** — Authorize spending without sharing keys. Oracle-based contracts. Off-chain policy enforcement.

**Stateful contracts via CCV** — State-carrying UTXOs with Merkle-committed data. State transitions enforced by consensus. Dynamic re-vaulting with partial withdrawals.

### Not available (deferred)

**OP_CAT** — Defined in script.h but disabled. Enables recursive covenants, STARK verification, arbitrary computation. Deferred due to unbounded expressiveness and complex interaction with CT/ring signatures.

**Private-lane covenants with ring mixing** — Covenant-locked outputs in the ring-anonymous pool require ZK proofs that covenant constraints hold for the hidden input. This is a research problem, not an engineering task. Deferred.

---

## 3. Design Decisions

### 3.1 Covenant scope: transparent and confidential, not ring-anonymous

Covenants operate on outputs where the spending conditions are knowable. This means:

- **Transparent outputs (`din1`):** Full covenant support. Amounts visible, input identified directly. CTV commits to plaintext amounts. All vault/treasury/escrow patterns work immediately.

- **Confidential outputs (`dina1`):** Covenant support with commitment-based constraints. CTV commits to Pedersen commitment bytes instead of plaintext amounts. The covenant creator must know the blinding factors at construction time (same as Liquid's approach). Amounts remain hidden from observers but the covenant creator knows them.

- **Ring-anonymous spends:** Covenants do NOT apply inside ring-mixed spending. A covenant-locked confidential output is excluded from the ring decoy pool. To spend a covenanted confidential output, the spender reveals which specific output they are spending (no ring ambiguity) and satisfies the covenant script. This is a deliberate privacy tradeoff: covenants provide policy enforcement, rings provide sender anonymity — you choose one per spend.

**Rationale:** Ring signatures hide which input is being spent. Covenants constrain how a specific input may be spent. These are fundamentally in tension. Forcing both simultaneously requires ZK proofs that are not yet implemented. The clean boundary is: covenants apply to identified inputs (transparent or confidential), rings apply to anonymous inputs. Users who want vault protection accept that the vault lifecycle is not ring-anonymous. Users who want ring anonymity move funds out of the vault first.

### 3.2 Opcode activation: all four at once

Activate CTV, CSFS, TXHASH, and CCV together. They are already implemented, tested, and integrated into the script interpreter. Activating them separately creates unnecessary complexity — the mempool policy, wallet, and RPC layers already handle all four.

### 3.3 OP_CAT: deferred

OP_CAT remains disabled. Reasons:
- Enables recursive covenants (irremovable spending restrictions that propagate through the coin supply)
- With confidential transactions, recursive covenants on hidden amounts create hard-to-analyze economic dynamics
- MEV surface increases significantly with CAT-enabled DeFi constructions
- The simpler covenant primitives (CTV, CSFS, TXHASH, CCV) cover all vault/treasury/escrow use cases without recursion

OP_CAT can be reconsidered as a separate activation after the ecosystem has experience with the simpler primitives.

### 3.4 Script-path activation: height-gated

Script-path spending becomes valid at a specific block height. Before that height, only key-path Taproot spending is accepted. This is a consensus rule change (hard fork for script-path transactions).

### 3.5 Tapscript resource limits

Enforce limits to prevent DoS via complex scripts:
- Maximum script size: 10,000 bytes (existing `MAX_SCRIPT_SIZE`)
- Maximum stack elements: 1,000 (existing `MAX_STACK_SIZE`)
- Maximum opcode count: 201 (existing `MAX_SCRIPT_OPCODES`)
- Covenant-specific: maximum 100 CTV outputs per transaction, 520-byte CSFS message, 4096-byte contract state (already in mempool policy)

---

## 4. Activation Plan

### Phase C.1: Script-Path Enablement

**What:** Remove the policy rejection of Taproot script-path spends. Update `script_validation.h` to accept `witness.size() > 1`.

**Where:** `include/consensus/script_validation.h` line 125 (currently: "Script-path spends not yet supported").

**Tests needed:**
- Script-path spending with simple scripts (OP_TRUE, pubkey check)
- Script-path with each covenant opcode
- Script-path rejection before activation height
- Script-path acceptance after activation height
- Reorg across activation boundary

**Risk:** Low — the Tapscript interpreter already handles all opcodes. The policy rejection is the only gate.

### Phase C.2: Covenant Activation Height

**What:** Gate covenant opcode execution on a block height. Before the height, covenant opcodes in script-path scripts cause script failure. After the height, they execute normally.

**Consensus parameter:** `covenant_activation_height` in chainparams.

**Testnet:** Activate immediately (height 0) for testing.
**Mainnet:** TBD — requires ecosystem readiness assessment.

### Phase C.3: Confidential Covenant Support

**What:** Update CTV template hash computation to work with Pedersen commitment bytes when outputs are confidential. The current CTV hash commits to output amounts as uint64. For confidential outputs, it should commit to the 33-byte commitment instead.

**Where:** `ComputeCTVHash()` in `consensus/covenants.cpp`.

**Design:** When an output has `is_confidential = true`, the template hash includes:
- `commitment` (33 bytes) instead of `value` (8 bytes)
- `range_proof_hash` (32 bytes, SHA256 of the range proof) instead of nothing

This allows covenant creators to pre-compute the template with known blinding factors while hiding amounts from chain observers.

### Phase C.4: Wallet Integration Testing

**What:** Verify the existing covenant wallet (CTV templates, CSFS delegations, vault patterns) works end-to-end with the enabled script-path.

**Tests:**
- Create a CTV-locked output via RPC
- Spend it with the pre-committed transaction template
- Create a vault with recovery path
- Trigger vault withdrawal
- Exercise recovery sweep during delay
- Create CSFS delegation, verify, and spend
- Create a stateful contract via CCV, advance state

### Phase C.5: Documentation and RPC Hardening

**What:** Update RPC help text, add `getcovenantinfo` to the privacy status output, document the covenant/privacy boundary rules.

---

## 5. Covenant-Privacy Boundary Rules

These rules define how covenants interact with the two-lane system:

### Rule 1: Covenant outputs are not ring-mixable

A UTXO locked by a covenant script (any script-path spend condition) is excluded from the CT output index used for ring decoy selection. It cannot appear as a decoy in someone else's ring, and spending it does not use ring signatures.

**Rationale:** Ring decoys must be indistinguishable. Covenant-locked outputs have distinct spending conditions that make them distinguishable from regular confidential outputs.

### Rule 2: Covenant outputs can be confidential

A covenant-locked output can hide its amount via Pedersen commitments. The CTV template commits to the commitment bytes. Observers see that a covenant exists but cannot determine the amount.

### Rule 3: Exiting a covenant to the ring pool requires uncovenanting

To move funds from a covenanted state into the ring-anonymous pool:
1. Spend the covenant (satisfying its conditions)
2. Create a regular confidential output (no covenant script)
3. That output enters the CT output index and becomes ring-mixable

This is analogous to unshielding then re-shielding, but within the private lane. The covenant lifecycle ends, and the output re-enters the anonymous pool.

### Rule 4: Shielding into a covenant is allowed

`shieldcoins` can create a covenant-locked confidential output directly. The transparent inputs are consumed publicly, the output is confidential and covenanted. This is useful for creating a private vault in one step.

### Rule 5: Private sends cannot target covenant addresses

`sendprivate` (ring-16 CLSAG) creates standard confidential outputs, not covenanted ones. To create a covenanted output from private funds, the user must first unshield or use a dedicated covenant construction RPC.

**Rationale:** Ring signatures prove the spender controls one of 16 possible inputs without revealing which. Adding covenant constraints to the output would require the ring signature to also prove the covenant is satisfied for the hidden input — which is the unsolved ZK problem.

---

## 6. Transaction Types with Covenants

The four-type model expands to six:

| Type | Inputs | Outputs | Ring? | Covenant? |
|---|---|---|---|---|
| Transparent transfer | `din1` | `din1` | No | No |
| Shield | `din1` | `dina1` | No | Optional |
| Private send | `dina1` (ring) | `dina1` | Yes (ring-16) | No |
| Unshield | `dina1` (ring) | `din1` | Yes (ring-16) | No |
| Covenant create | `din1` or `dina1` (no ring) | covenanted output | No | Yes |
| Covenant spend | covenanted output | any (per covenant rules) | No | Yes (enforced) |

---

## 7. Implementation Phases

### Phase 1: Enable script-path (1-2 days)
- Remove policy rejection in `script_validation.h`
- Add activation height check
- Write script-path spending tests
- Verify existing Tapscript interpreter handles all opcodes correctly

### Phase 2: Test existing covenant infrastructure (2-3 days)
- Run full covenant test suite with script-path enabled
- Test CTV create → spend lifecycle via RPC
- Test vault pattern (create, trigger, recover)
- Test CSFS delegation flow
- Test CCV state advancement
- Fix any issues found

### Phase 3: Confidential covenant support (3-5 days)
- Update `ComputeCTVHash()` for commitment-based amounts
- Update covenant wallet for confidential outputs
- Test CTV with confidential outputs
- Test vault with hidden amounts
- Update covenant policy for CT outputs

### Phase 4: Boundary rule enforcement (2-3 days)
- Exclude covenant outputs from CT output index (ring decoy pool)
- Enforce no-ring-spend for covenant inputs
- Test covenant → regular CT output flow (uncovenanting)
- Test shield-to-covenant flow

### Phase 5: Regtest E2E and activation (2-3 days)
- Full lifecycle test: create vault → fund → trigger → recover → uncovenant → ring spend
- Two-node sync with covenant transactions
- Reorg across covenant activation boundary
- Set mainnet activation height

---

## 8. Security Considerations

### Covenant DoS

Complex Tapscript execution is more expensive than key-path verification. Mitigations:
- Existing script size/opcode/stack limits apply
- Covenant-specific mempool policy limits (100 CTV outputs, 520B CSFS messages, 4096B state)
- Higher minimum fee for covenant transactions (already in mempool policy)

### Recursive covenant risk (mitigated)

Without OP_CAT, recursive covenants are not possible with CTV alone (you must know the full spending tree at creation). CCV can create limited recursion (state-carrying UTXOs) but the depth is bounded by mempool policy. OP_CAT remains disabled.

### Covenant-based censorship

A concern from the Bitcoin community: governments forcing exchanges to withdraw only to covenants requiring 3rd-party approval. This risk exists but is not unique to covenants — the same restriction can be implemented via multisig today. Covenants make it slightly more automated but do not fundamentally change the threat model.

### Privacy leakage via covenant structure

Executing a covenant branch in Taproot reveals the script. Even though unexecuted branches are hidden (Taproot's key property), the executed branch is public. This means covenant transactions are distinguishable from non-covenant transactions, reducing the anonymity set.

Mitigation: standard covenant templates that all users share (e.g., a canonical vault structure) maximize the anonymity set within covenant usage. But covenant transactions will always be distinguishable from ring-anonymous private sends — this is an inherent property of the design.

---

## 9. Open Questions

1. **Covenant activation height?** Needs to be after the ring activation (height 15000) to avoid overlapping consensus changes. Suggest height 20000.

2. **Should CCV state size be larger for complex contracts?** Current limit: 4096 bytes. May need increase for Merkle-tree-based state.

3. **Should covenant-locked confidential outputs use a distinct address prefix?** E.g., `dinv1` for "vault" addresses? Or keep them as `dina1` with the covenant encoded in the Tapscript tree?

4. **Fee multiplier for covenant transactions?** Currently in mempool policy. Need to determine the right ratio relative to simple Taproot key-path spends.

5. **Should the wallet auto-detect covenant-locked UTXOs during rescan?** The wallet needs to know if a UTXO has spending conditions beyond a simple signature. This requires parsing the Tapscript tree during scanning.

---

## 10. Relationship to Whitepaper

The whitepaper sections 9 (Covenants and Programmable Money), 10 (Threat Model), and 11 (Wallet Model) describe the design philosophy and user experience. This document describes the implementation plan.

The whitepaper is accurate: Dinero's covenant model is programmable money, not general-purpose on-chain computation. The implementation confirms this — CTV, CSFS, TXHASH, and CCV cover vaults, treasury, escrow, inheritance, and stateful contracts without requiring a virtual machine or unbounded script execution.
