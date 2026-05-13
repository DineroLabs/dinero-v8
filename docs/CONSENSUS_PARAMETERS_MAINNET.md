# DineroCoin Mainnet Consensus Parameters (FROZEN)

> **Status:** FROZEN
> **Effective Height:** 0 (genesis)
> **Freeze Date:** 2026-02-01
> **Document Version:** 1.0.0

---

## Purpose

This document is the **SINGLE AUTHORITATIVE SOURCE** for all mainnet consensus parameters.

---

## Chain Identity vs. Forkable Parameters

> **Genesis and premine define chain identity. They are not forkable parameters. Any change creates a new blockchain.**

| Category | Examples | Changeability |
|----------|----------|---------------|
| **Chain Identity** | Genesis hash, genesis timestamp, genesis nonce, premine amount, premine height | **NEVER** - defines what "DineroCoin" is |
| **Forkable Parameters** | Block size, difficulty algorithm, reward schedule (post-genesis) | Changeable via coordinated hard fork |

Changing chain identity parameters doesn't fork DineroCoin - it creates a different coin entirely.

---

## Modification Rules

Any change to **forkable** values in this document constitutes a **hard fork** and requires:

1. Network-wide coordination
2. Explicit version bump
3. New release with migration plan

**DO NOT modify these values without a coordinated hard fork process.**

---

## 1. Network Identity

| Parameter | Value | Source File |
|-----------|-------|-------------|
| Network Name | `mainnet` | `src/consensus/chainparams_impl.cpp:42` |
| Network Magic | `0xd9b4bef9` | `src/consensus/chainparams_impl.cpp:44` |
| HRP (Bech32) | `din` | `src/consensus/chainparams_impl.cpp:43` |
| Network ID | `main` | `src/consensus/chainparams_impl.cpp:50` |
| Protocol Version | `10000` (1.0.0) | `include/consensus/subsidy.h:46` |

---

## 2. Genesis Block (CHAIN IDENTITY - IMMUTABLE)

> These values define what "DineroCoin" is. Changing any of them creates a different blockchain.

| Parameter | Value | Status |
|-----------|-------|--------|
| **Hash** | `00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74` | IDENTITY |
| **Merkle Root** | `c997af69be7140ede59c8114ae24f36944c4302b4664fdd82b45473f95a6fba1` | IDENTITY |
| **Timestamp** | `1772496000` (2026-03-03 00:00:00 UTC) | IDENTITY |
| **Nonce** | `2954325912` | IDENTITY |
| **Difficulty (nBits)** | `0x1d31ffce` | IDENTITY |
| **Version** | `1` | IDENTITY |
| **Motto** | `"Dinero: Real Money For Free People"` | IDENTITY |

### Genesis Coinbase (CHAIN IDENTITY)

- **Output:** 100 DIN (unspendable via `OP_RETURN`)
- **Double Commitment:** Motto embedded in both scriptSig AND OP_RETURN
- **Hex:** See `src/consensus/chainparams_impl.cpp:90-94`

---

## 3. Monetary Policy

### Chain Identity (Blocks 0-1)

> Genesis burn and premine define chain identity. These are IMMUTABLE.

| Parameter | Value | Status | Verification |
|-----------|-------|--------|--------------|
| **Genesis Burn** | `100 DIN` (unspendable) | IDENTITY | `subsidy.h:60-61` |
| **Premine Amount** | `2,627,900 DIN` | IDENTITY | `static_assert` in `subsidy.h:169` |
| **Premine Height** | `1` | IDENTITY | `static_assert` in `subsidy.h:196` |

### Forkable Parameters (Post-Genesis)

| Parameter | Value | Status | Verification |
|-----------|-------|--------|--------------|
| **Total Supply** | `265,428,000 DIN` | FROZEN | `static_assert` in `subsidy.h:164` |
| **Decimals** | `8` (1 DIN = 100,000,000 una) | FROZEN | `static_assert` in `subsidy.h:155` |
| **PoW Mineable** | `262,800,000 DIN` | FROZEN | `subsidy.h:66` |
| **Initial Block Reward** | `100 DIN` | FROZEN | `static_assert` in `subsidy.h:158` |
| **Halving Interval** | `1,314,000 blocks` (~5 years) | FROZEN | `static_assert` in `subsidy.h:161` |
| **Total Halvings** | `33` | FROZEN | `subsidy.h:105` |

### Block Subsidy Schedule

| Height | Type | Subsidy | Status |
|--------|------|---------|--------|
| 0 | Genesis | 100 DIN (OP_RETURN, unspendable) | IDENTITY |
| 1 | Premine | 2,627,900 DIN | IDENTITY |
| 2+ | PoW | 100 DIN (halving every 1,314,000 blocks) | FROZEN |

---

## 4. Block Parameters

| Parameter | Value | Source File |
|-----------|-------|-------------|
| **Header Size** | `128 bytes` | `include/mining/header_layout.h:47` |
| **Max Block Size** | `1,000,000 bytes` (1 MB) | `chainparams_impl.cpp:56` |
| **Target Spacing** | `120 seconds` (2 minutes) | `chainparams_impl.cpp:52` |
| **Coinbase Maturity** | `100 blocks` | `coinbase_maturity.h:18` |
| **Dust Threshold** | `546 una` | `chainparams_impl.cpp:54` |
| **Min Relay Fee** | `1000 una/kb` | `chainparams_impl.cpp:55` |

### BlockHeader v1 Layout (128 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0x00 | 4 | version |
| 0x04 | 32 | prev_block_hash |
| 0x24 | 32 | merkle_root |
| 0x44 | 32 | **utreexo_root** |
| 0x64 | 8 | timestamp |
| 0x6C | 4 | difficulty (nBits) |
| 0x70 | 4 | nonce |
| 0x74 | 12 | reserved (MUST be zero) |

---

## 5. Difficulty Algorithm

### Phase 1: Bootstrap (Blocks 2-200,002)

| Parameter | Value | Source |
|-----------|-------|--------|
| **Start Block** | `2` | `consensus.hpp:49` |
| **End Block** | `200,002` | `consensus.hpp:50` |
| **Initial Difficulty** | `0x1d31ffce` (50x easier than Bitcoin genesis) | `consensus.hpp:51` |
| **Retarget Window** | `720 blocks` (~1 day) | `consensus.hpp:52` |
| **Max Increase** | `+10%` per retarget | `consensus.hpp:53` |
| **Max Decrease** | `-20%` per retarget | `consensus.hpp:54` |

### Phase 2: ASERT (Blocks 200,003+)

| Parameter | Value | Source |
|-----------|-------|--------|
| **Anchor Height** | `200,002` | `consensus.hpp:69` |
| **Anchor Bits** | `0x1d31ffce` (read from chain) | `consensus.hpp:70` |
| **Half Life** | `43,200 seconds` (12 hours) | `consensus.hpp:71` |

### Global Limits

| Parameter | Value | Source |
|-----------|-------|--------|
| **POW Limit (Floor)** | `0x1d31ffce` | `consensus.hpp:60` |
| **Emergency Min Difficulty** | `0x1f00ffff` (256x easier) | `consensus.hpp:76` |

---

## 6. Utreexo Accumulator

| Parameter | Value | Source |
|-----------|-------|--------|
| **Activation Height** | `0` (genesis) | `utreexo_activation.h:34` |
| **Header Commitment** | Yes (32 bytes at offset 0x44) | `header_layout.h:26` |
| **Enforcement** | **MANDATORY** (no bypass, no shadow mode) | `block_validation.cpp` |

### Consensus Sealing (Structural Guarantees)

- `strict_utreexo_enforcement_` flag: **REMOVED** (cannot be disabled)
- Shadow mode: **REMOVED** (root mismatch always rejects)
- Null forest check: **ABORTS** if forest missing at active height

See: `docs/UTREEXO_CONSENSUS_SEALING.md`

---

## 7. Feature Activation Heights

| Feature | Activation Height | Source |
|---------|-------------------|--------|
| Utreexo | `0` (genesis) | `utreexo_activation.h:34` |
| Witness Commitment | `2` | `chainparams_impl.cpp:66` |
| Confidential Transactions | `2` | `chainparams_impl.cpp:73` |
| Full Rules | `0` | `utreexo_activation.h:142` |

---

## 8. Network Ports

| Service | Port | Source |
|---------|------|--------|
| RPC | `20997` | `chainparams_impl.cpp:45` |
| P2P | `20999` | `chainparams_impl.cpp:48` |
| HTTP | `8080` | `chainparams_impl.cpp:46` |
| WebSocket | `8081` | `chainparams_impl.cpp:47` |

---

## 9. Checkpoints

### Chain Identity Checkpoints (IMMUTABLE)

| Height | Hash | Status |
|--------|------|--------|
| `0` | `00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74` | IDENTITY (Genesis) |
| `1` | `0000000dc70e6392b93ca710821628eed2ed0e2cdc5e77ce6f731d22514b8763` | IDENTITY (Premine) |

### Future Checkpoints (Additive Only)

Checkpoints at height 2+ may be added for security but cannot be modified once set.

---

## 10. Address Prefixes

| Type | Prefix | Network |
|------|--------|---------|
| P2PKH | `0x00` | Mainnet |
| P2SH | `0x05` | Mainnet |
| Bech32 HRP | `din` | Mainnet |

---

## Source File Reference

All consensus parameters MUST be sourced from these canonical files:

| Category | Canonical Source |
|----------|------------------|
| Network Identity | `src/consensus/chainparams_impl.cpp` |
| Monetary Policy | `include/consensus/subsidy.h` |
| Difficulty Rules | `src/consensus/consensus.hpp` |
| Header Layout | `include/mining/header_layout.h` |
| Utreexo Activation | `include/consensus/utreexo_activation.h` |
| Coinbase Maturity | `include/dinero/core/consensus/coinbase_maturity.h` |

---

## Compile-Time Verification

The following `static_assert` guards protect against accidental changes:

```cpp
// subsidy.h
static_assert(MAX_SUPPLY_UNA == 26542800000000000ULL);
static_assert(UNA_PER_DIN == 100000000ULL);
static_assert(INITIAL_SUBSIDY == 10000000000ULL);
static_assert(HALVING_INTERVAL == 1314000);
static_assert(PREMINE_UNA == 262790000000000ULL);

// header_layout.h
static_assert(sizeof(BlockHeaderV1) == 128);

// utreexo_activation.h
static_assert(UTREEXO_ACTIVATION_HEIGHT_MAINNET == 0);
```

---

## Change Log

| Version | Date | Description |
|---------|------|-------------|
| 1.0.0 | 2026-02-01 | Initial freeze |

---

## Cryptographic Commitment

This document's SHA-256 hash at freeze time:

```
[TO BE COMPUTED AT TAG TIME]
```

To verify: `shasum -a 256 docs/CONSENSUS_PARAMETERS_MAINNET.md`

---

**WARNING:** Any modification to frozen parameters constitutes a **consensus-breaking change** and will result in a network fork. All changes require explicit coordination, version bump, and migration plan.
