# UI → Consensus Mapping

Every user action maps to exactly one consensus state transition.
No ambiguity. No hidden behavior.

## User Actions

| User sees | Mode | Source | Consensus transition |
|-----------|------|--------|---------------------|
| Send → Normal | transparent | transparent pool | UTXO → UTXO (Utreexo remove + add) |
| Send → Normal | transparent | shielded pool | Nullifier + UTXO (implicit unshield) |
| Send → Private | private | transparent pool | UTXO burn + Commitment (implicit shield) |
| Send → Private | private | shielded pool | Nullifier + Commitment (private transfer) |
| Receive (Taproot) | — | — | New UTXO in Utreexo |
| Receive (P2MR) | — | — | New UTXO in Utreexo |

## What the user NEVER sees

| Hidden concept | Why hidden | Where it lives |
|---------------|------------|----------------|
| Shield | Implicit when sending privately from transparent | Routing engine |
| Unshield | Implicit when sending normally from shielded | Routing engine |
| Nullifier | Protocol mechanism for spending shielded notes | NullifierSet |
| Commitment | Protocol representation of shielded value | CommitmentTree |
| ZK proof | Cryptographic verification of shielded spend | v5 ShieldedBundle |

## Routing Engine Decision Table

```
Route(destination, mode, has_shielded_balance) → decision

din1p + Normal  + transparent funds → TransparentTaproot
din1r + Normal  + transparent funds → TransparentP2MR
din1p + Normal  + shielded funds   → Unshield (implicit)
din1r + Normal  + shielded funds   → Unshield (implicit)
din1p + Private + transparent funds → ShieldToCommitment (implicit)
din1r + Private + transparent funds → ShieldToCommitment (implicit)
any   + Private + shielded funds   → PrivateTransfer
```

## VWU Cost by Route

| Route | VWU | User-visible cost |
|-------|-----|-------------------|
| TransparentTaproot | ~66 | Lowest fee |
| TransparentP2MR | ~5,274 | Higher (PQ signature) |
| ShieldToCommitment | ~500 | Moderate (proof) |
| Unshield | ~5,000 | Higher (spend proof) |
| PrivateTransfer | ~5,500 | Highest (spend + output) |

## Balance Display

```
Wallet Balance:
  Transparent:  100.00 DIN  (visible on-chain)
  Shielded:      50.00 DIN  (private, ZK-protected)
  Total:        150.00 DIN
  PQ ratio:     30%         (quantum-resistant portion)
```

## Invariant

Users choose INTENT (normal/private), not MECHANISM (shield/unshield).
The routing engine translates intent into the correct consensus
transition. If the user ever needs to understand nullifiers,
commitments, or proof systems, the UX has failed.
