# Mainnet Readiness

**Status:** FROZEN
**Date:** 2026-01-21
**Commit:** `aff2511b0cc8d63390079bc6ef592db2677af583`
**Tag:** `v4.0.0-mainnet-ready`
**Tests:** 605

---

## Hardening Phases

| Phase | Scope | Tests |
|-------|-------|-------|
| A | Wallet Correctness | 127 |
| B | Mining & Block Production | 317 |
| C | Stratum & External Miner Safety | 96 |
| D | P2P Adversarial Integrity | 25 |
| E | Daemon Operational Safety | 21 |
| F | Mainnet Dry Run | 19 |

---

## Freeze Policy

No architecture changes after this commit.
Only bug fixes with explicit justification.

---

## Consensus Parameters

- Total supply: 265,428,000 DIN
- Initial subsidy: 100 DIN
- Halving interval: 1,314,000 blocks (~5 years)
- Coinbase maturity: 100 blocks

---

## Genesis (Canonical)

- Canonical genesis block hash (Phase 3 — Final Production Release):
  `00000000eadf62f8d47c9ac0da0eaa0c1c91444ef52563a99225655b85c15eab`

- Notes:
  - Any pre-Phase-3 genesis hashes are obsolete and must not be used.
  - Consensus enforcement sources:
    - `src/consensus/chainparams_impl.cpp`
    - `src/consensus/genesis_canonical.cpp`

---

## Premine (Canonical)

- Height: 1
- Amount: 2,627,900 DIN
- Address: `din1pc2nrhuzc04a7sf3p3t02wr53wk0ctwru5z4z4k4gu4q7vq06p2sqyrrk3s`

Source: `include/consensus/premine_canonical.h`

---

## Utreexo Activation

- Height: 3 (mandatory from block 3 onward)

Source: `include/consensus/utreexo_activation.h`

---

## Runtime Verification

```bash
# Genesis block
dinero-cli getblockhash 0
# Expected: 00000000eadf62f8d47c9ac0da0eaa0c1c91444ef52563a99225655b85c15eab

# Premine block
dinero-cli getblockhash 1
dinero-cli getblock <hash>

# Utreexo boundary
dinero-cli getblockhash 2   # No Utreexo required
dinero-cli getblockhash 3   # Utreexo required
```

---

This document is immutable. Do not modify.
