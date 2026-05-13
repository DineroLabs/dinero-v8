# Dinero v6 Genesis And Upgrade Surfaces

**Status:** Draft proposal
**Scope:** Fresh-genesis design note for a future `v6` chain
**Current chain impact:** None. This document does not change current consensus rules.

## Executive Summary

Dinero `v6` is proposed as a fresh-genesis chain with one consensus lane at block 0:

- transparent UTXOs only
- Utreexo as the sole consensus accumulator
- mandatory witness v1 Taproot outputs
- wallet-layer privacy defaults such as Silent Payments
- no confidential transactions, no rings, and no shielded pool in consensus at genesis

The core design rule is that Dinero must keep two different future-upgrade problems in two different places:

1. **Block-level or chain-state upgrades**
   - signaled in the 12-byte reserved header space
   - committed in full through a coinbase `OP_RETURN`, BIP141-style
2. **Transaction-format upgrades**
   - signaled with `Transaction.version`

This keeps block commitments out of transaction-format machinery and keeps future transaction families out of header flags.

## Goals

- let Utreexo do one job well: track transparent UTXOs
- remove the structural need for confidential outputs to masquerade as `value=0` UTXOs
- start with a consensus surface that is smaller, testable, and maintainable
- preserve room for a future shielded lane without paying that complexity at genesis
- keep the existing ZK code parked in-tree, but not wired into consensus

## Non-Goals At Genesis

- no confidential amounts
- no Pedersen commitments in consensus outputs
- no Bulletproofs in the consensus validation path
- no CLSAG or ring-member selection
- no `KeyImageDB`
- no note commitment tree
- no Spartan or Hyrax verification in the consensus path

## v6 Consensus Shape At Block 0

### Header

Dinero `v6` continues to use the 128-byte block header shape already defined in [`/Users/haydarevich/src/dinero/include/mining/header_layout.h`](/Users/haydarevich/src/dinero/include/mining/header_layout.h):

```text
offset 0:    version           (4 bytes)
offset 4:    prev_block_hash   (32 bytes)
offset 36:   merkle_root       (32 bytes)
offset 68:   utreexo_root      (32 bytes)
offset 100:  timestamp         (8 bytes)
offset 108:  difficulty        (4 bytes)
offset 112:  nonce             (4 bytes)
offset 116:  reserved          (12 bytes)
total:       128 bytes
```

For `v6`, those final 12 bytes are interpreted as:

```text
offset 116:  header_feature_flags  (4 bytes)
offset 120:  reserved_strict_zero  (8 bytes)
```

Genesis consensus rule:

- `header_feature_flags == 0`
- `reserved_strict_zero == 0`

This keeps the existing header size, preserves cache alignment, and gives `v6` a clean block-level upgrade surface from block 1 onward.

### UTXO Model

- one transparent UTXO lane only
- one Utreexo forest only
- one uniform leaf hashing scheme only
- no private output ever enters the transparent Utreexo forest

Leaf hashing remains conceptually:

```text
HashUTXO(txid, vout, amount, scriptPubKey)
```

Amounts are plaintext. There is no confidential-output special case and no `value=0` substitution hack.

### Output Policy

At `v6` genesis:

- all spendable outputs MUST be witness v1 Taproot outputs
- no P2PKH
- no P2SH
- no bare multisig
- no legacy witness outputs

`OP_RETURN` remains allowed for:

- coinbase commitments
- filter commitments
- other explicitly-specified non-spendable commitments

### Privacy At Genesis

Privacy is wallet-layer, not consensus-layer:

- Silent Payments for receive-side unlinkability
- mandatory Taproot for script uniformity
- CoinJoin may be shipped post-genesis as a wallet/coordinator feature

This is intentionally a "privacy-aware Bitcoin fork" model, not a shielded-pool launch.

## Two Upgrade Surfaces

### 1. Block-Level And Chain-State Upgrades

Block-level upgrades belong in:

- `header_feature_flags`, for signaling that a block uses feature `X`
- a coinbase `OP_RETURN`, for the full commitment payload

This is the correct place for future items such as:

- shielded-state roots
- epoch markers
- fee-burn accumulators
- other whole-block commitments

The important rule is:

- **header flags say that a commitment exists**
- **coinbase `OP_RETURN` carries the actual commitment bytes**

The 12-byte reserved header region is not large enough for a secure 32-byte Merkle or accumulator root. The full commitment therefore belongs in coinbase, committed indirectly through `merkle_root`.

### 2. Transaction-Format Upgrades

Transaction-family upgrades belong in [`/Users/haydarevich/src/dinero/include/primitives/transaction.h`](/Users/haydarevich/src/dinero/include/primitives/transaction.h)'s existing `Transaction.version` field.

Current code already defines:

- `1 = LEGACY`
- `2 = SEGWIT`
- `3 = RING`
- `4 = RING_COVENANT`

For `v6` genesis, the proposal is:

- accept only `version = 2`
- reject `1`, `3`, and `4` from block 0 onward
- reserve `version = 5` for a future shielded-lane transaction family

That gives Dinero one clean transaction format at genesis while keeping a separate tx-format lever available for later upgrades.

## Future Shielded Lane Hook

`v6` does not ship a shielded lane at genesis, but it keeps a clean upgrade path open.

When a future shielded lane is introduced:

1. a specific `header_feature_flags` bit is activated
2. blocks that set that bit MUST include a coinbase `OP_RETURN` carrying the shielded-state commitment
3. a new transaction version, such as `5`, can be admitted for shielded transactions

This mirrors the SegWit-style pattern:

- block-level commitment in coinbase
- tx-family versioning in transactions

That separation is intentional and should remain strict.

## Genesis Inscription

Dinero `v6` pins the genesis inscription in the coinbase **scriptSig**, not in an `OP_RETURN`.

Exact inscription text:

```text
Dinero - Real Money For Free People | 15th day of April 2026
```

Encoding requirements:

- encoding: UTF-8
- byte length: `60`
- because the string is plain ASCII, UTF-8 and ASCII bytes are identical

Exact byte sequence in hex:

```text
44696e65726f202d205265616c204d6f6e657920466f7220467265652050656f706c65207c203135746820646179206f6620417072696c2032303236
```

Consensus note:

- the byte sequence above is the consensus artifact, not just the rendered text
- no BOM
- no trailing newline
- no extra whitespace

Recommended implementation pattern when `v6` genesis is eventually wired:

```cpp
constexpr std::string_view kDineroV6GenesisInscription =
    "Dinero - Real Money For Free People | 15th day of April 2026";
static_assert(kDineroV6GenesisInscription.size() == 60,
              "Genesis inscription byte length is part of consensus");
```

## Why Recent Bug Classes Structurally Disappear

This design removes the primitives that created the recent Utreexo/privacy contradictions:

- no confidential outputs in the Utreexo forest
- no ring-member index as consensus-critical spend data
- no `KeyImageDB`
- no mixed transparent/private accumulator semantics
- no Spartan verification in the consensus path

That means the following classes of issue do not exist at genesis in this design:

- CT outputs hashed into the transparent forest with substituted values
- ring-member determinism drift across nodes
- key-image versus chainstate divergence
- cross-architecture ZK consensus bugs
- canonical-roots activation complexity caused by mid-chain private-lane interactions

This is not "because we will be more careful." It is because the primitives that caused those failures are out of the block-0 consensus surface.

## Parked But Preserved

The existing ZK and privacy code should remain in-tree, but not wired into `v6` genesis consensus:

- Spartan
- Hyrax
- Poseidon-2
- CLSAG and related legacy private-lane code

That preserves engineering work and future optionality without forcing those systems to be consensus load-bearing on day 1.

## Implementation Sequencing

The intended order for real implementation work is:

0. stabilize the current `v5` chain first
   - finish recovery work on the live chain and local datadirs before starting `v6` implementation
   - do not let fresh-genesis work proceed in parallel with an unresolved `v5` incident state
1. finalize the `v6` spec and genesis parameters
2. wire the genesis inscription into coinbase scriptSig generation
3. redefine `v6` tx-version acceptance rules from block 0
4. reinterpret the header reserved bytes as `header_feature_flags + reserved_strict_zero`
5. launch with transparent UTXO + Utreexo only
6. keep shielded-lane work dormant until the base chain is boring and stable

The main principle is simple:

- **Utreexo handles deterministic transparent state**
- **future privacy systems, if any, must be added as separate lanes**
- **block commitments and transaction formats remain separate upgrade surfaces**

## Honest Positioning

Dinero `v6` should be described honestly as:

> Bitcoin-style transparent money with Utreexo-backed stateless validation, mandatory Taproot, and wallet-layer privacy defaults.

It should **not** be marketed as a shielded chain at genesis.

That honesty is a feature, not a weakness:

- the stateless claim is real
- the privacy-aware claim is real
- the bug surface is dramatically smaller than the mixed CT/ring/Utreexo model

## Open Items

This document intentionally leaves the following to later spec work:

- exact genesis transaction layout beyond the inscription bytes
- final `v6` address prefixes and network magic
- ASERT parameter choices for the fresh chain
- BIP157/158 commitment specifics
- CoinJoin standard denomination policy
- the shape of a future shielded-lane transaction family

Those are important, but they are downstream of the architectural decisions pinned here.
