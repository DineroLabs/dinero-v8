# Trustless Light-Client Shielded Scanning — M2 Daemon Output Feed Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Parent must verify `git status`, `git log`, and GitHub signature state after each worker commit.

**Goal:** Add the daemon-side shielded output feed described in `docs/superpowers/specs/2026-05-27-trustless-light-client-shielded-m2-design.md`.

**Why M2 exists:** DineroDPI M1 + T8 can scan shielded outputs once a block reaches the filter-discovery hook, but that hook is currently driven by transparent DNRF matches. Shielded-only wallet activity needs a daemon RPC that exposes public shielded output bytes by height range so iOS can trial-decrypt locally. The same feed carries public spend nullifiers so iOS can mark locally known notes spent without fetching full blocks.

**Important correction:** Do not implement a recipient-specific compact filter in M2. The current v5/v6 shielded output wire format has no client-precomputable recipient tag. `epk` is sender-random; `d` and `pk_d` are encrypted; the daemon cannot derive recipient tags. M2 therefore ships `blockchain.shielded.outputs`, a public output feed. A true recipient filter would require a future shielded wire-format change.

**Scope:**
- dinero-v8 daemon only
- No consensus change
- No view-key input
- No M3 spend witness RPC
- No iOS changes in this PR

**Branch:** `feature/m2-shielded-compact-filter-plan` for this plan. Implementation branch should use `feature/m2-shielded-output-feed` unless the reviewer chooses a different name.

**Signing:** All commits signed as `Dinero Labs <team@dinerolabs.org>`.

---

## File Map

New daemon files:

- Create: `include/consensus/shielded/shielded_output_feed.h` (~70 LOC)
- Create: `src/consensus/shielded/shielded_output_feed.cpp` (~160 LOC)
- Create: `src/test/shielded_output_feed_tests.cpp` (~220 LOC)

Modified daemon files:

- Modify: `src/consensus/shielded/CMakeLists.txt` — add `shielded_output_feed.cpp` to `dinero_shielded`
- Modify: `src/rpc/methods_blockchain_context.cpp` — add `blockchain.shielded.outputs` handler and alias
- Modify: the repo’s test CMake registration file — add `shielded_output_feed_tests` target using the existing `src/test/*` pattern

Optional follow-up, only after benchmark:

- Modify: `include/storage/chain_db.h` / `src/storage/chain_db.cpp` — add cached output summaries keyed by block hash

Expected implementation size: ~450-650 LOC including tests.

---

## Task 1: Pure Shielded Output Extractor

**Files:**
- Create: `include/consensus/shielded/shielded_output_feed.h`
- Create: `src/consensus/shielded/shielded_output_feed.cpp`
- Modify: `src/consensus/shielded/CMakeLists.txt`
- Create: `src/test/shielded_output_feed_tests.cpp`

Build a pure helper that extracts public shielded output metadata from a `Block`.

API shape:

```cpp
namespace dinero::consensus::shielded {

struct ShieldedOutputFeedEntry {
    uint256 block_hash;
    uint32_t height = 0;
    TxId txid;
    uint32_t tx_index = 0;
    uint32_t output_index = 0;
    uint64_t leaf_index = 0;
    Hash commitment{};
    std::vector<uint8_t> encrypted_note;
};

struct ShieldedNullifierFeedEntry {
    uint256 block_hash;
    uint32_t height = 0;
    TxId txid;
    uint32_t tx_index = 0;
    uint32_t spend_index = 0;
    Hash nullifier{};
};

struct ShieldedOutputFeedResult {
    std::vector<ShieldedOutputFeedEntry> outputs;
    std::vector<ShieldedNullifierFeedEntry> spent_nullifiers;
    uint64_t next_leaf_index = 0;
};

enum class ShieldedOutputFeedError : uint8_t {
    Ok = 0,
    BundleDecodeFailed = 1,
    EncryptedNoteWrongSize = 2,
};

ShieldedOutputFeedError ExtractShieldedOutputFeed(
    const Block& block,
    uint32_t height,
    uint64_t first_leaf_index,
    ShieldedOutputFeedResult* out);

} // namespace dinero::consensus::shielded
```

Implementation rules:

- Iterate transactions in block order.
- Skip non-v5/v6 transactions and transactions with empty `shielded_bundle_bytes`.
- Use `DeserializeShieldedBundle`.
- For each output, require `encrypted_note.size() == 611`. Prefer introducing a consensus-local constant such as `kShieldedEncryptedNoteBytes = 611` in the new feed helper rather than pulling `include/wallet/shielded_derivation.h` into the consensus layer.
- Preserve canonical within-bundle spend and output order from `DeserializeShieldedBundle`.
- Include public spend nullifiers in `spent_nullifiers`; they do not increment `leaf_index`.
- Assign `leaf_index = first_leaf_index + outputs_seen_so_far`.
- Do not include zk proofs, value commitments, range proofs, or binding sig in the feed.

Tests:

- Empty block / no shielded tx returns zero outputs and unchanged `next_leaf_index`.
- One bundle returns the exact commitment and encrypted note bytes.
- Multiple shielded txs preserve block tx order and canonical output order.
- Starting `first_leaf_index = 900` produces leaf indexes `900, 901, ...`.
- Shielded spends return nullifier entries in block tx order and do not change `next_leaf_index`.
- Malformed bundle returns `BundleDecodeFailed`.
- Wrong encrypted note size returns `EncryptedNoteWrongSize`.

Commit:

```bash
git add include/consensus/shielded/shielded_output_feed.h \
        src/consensus/shielded/shielded_output_feed.cpp \
        src/consensus/shielded/CMakeLists.txt \
        src/test/shielded_output_feed_tests.cpp
git commit -S -m "feat(shielded): extract public output feed entries"
```

---

## Task 2: Leaf Index Walk Helper

**Files:**
- Extend: `include/consensus/shielded/shielded_output_feed.h`
- Extend: `src/consensus/shielded/shielded_output_feed.cpp`
- Extend tests in `src/test/shielded_output_feed_tests.cpp`

RPC needs `first_leaf_index` for a requested height. For M2, keep this simple and deterministic:

```cpp
using BlockByHeightLookup = std::function<std::optional<Block>(uint32_t height)>;

StatusOr<uint64_t> CountShieldedOutputsBeforeHeight(
    uint32_t from_height,
    uint32_t shielded_activation_height,
    BlockByHeightLookup lookup);
```

Implementation:

- Walk `[shielded_activation_height, from_height)`.
- Decode shielded bundles and count outputs only.
- Return structured error if a historical bundle is malformed.

This is O(chain height), but at current height it is acceptable for M2 planning. The cache can be added later under benchmark pressure. Keep the pure helper small and independently testable.

Tests:

- Count is zero before activation.
- Count accumulates across several blocks.
- Malformed historical bundle errors instead of guessing.

Commit:

```bash
git add include/consensus/shielded/shielded_output_feed.h \
        src/consensus/shielded/shielded_output_feed.cpp \
        src/test/shielded_output_feed_tests.cpp
git commit -S -m "feat(shielded): derive output feed leaf indexes by height"
```

---

## Task 3: `blockchain.shielded.outputs` RPC

**Files:**
- Modify: `src/rpc/methods_blockchain_context.cpp`

Add a handler next to `rpc_context_getblockfilters`.

Params:

- Named: `{ "from_height": int, "count": int }`
- Positional: `[from_height, count]`
- Clamp `count` to the same `MAX_BATCH = 2000` used by `blockchain.getblockfilters`.

Response:

```json
{
  "from_height": 8650,
  "count": 2000,
  "tip_height": 30123,
  "blocks": [
    {
      "height": 12847,
      "block_hash": "...",
      "shielded_spend_count": 1,
      "shielded_output_count": 2,
      "spent_nullifiers": [
        { "txid": "...", "tx_index": 2, "spend_index": 0, "nullifier": "..." }
      ],
      "outputs": [
        {
          "txid": "...",
          "tx_index": 3,
          "output_index": 0,
          "leaf_index": 912,
          "commitment": "...",
          "encrypted_note": "..."
        }
      ]
    }
  ]
}
```

Implementation notes:

- Use existing `ReadRpcBlock`.
- Use `chain_db->getBlockHashByHeight`.
- Compute `first_leaf_index` once for `from_height`, then advance it as each block is extracted.
- Omit blocks with zero shielded outputs and zero shielded spends from `blocks`.
- Return structured JSON error for decode failures. Do not silently drop malformed shielded bundles.
- Register `blockchain.shielded.outputs`.
- Optional alias: `shieldedoutputs`.

Commit:

```bash
git add src/rpc/methods_blockchain_context.cpp
git commit -S -m "feat(rpc): add blockchain.shielded.outputs feed"
```

---

## Task 4: RPC Fixture / Integration Test

**Files:**
- Add the repo-appropriate test target or shell fixture.

The test should use daemon-generated bundle bytes from existing shielded serialization helpers rather than hand-written JSON.

Coverage:

- A block with no shielded outputs and no shielded spends is omitted from `blocks`.
- A block with one shielded output returns exactly one entry with exact commitment/encrypted-note hex.
- A block with one shielded spend returns its public nullifier.
- A range with multiple shielded blocks returns height order.
- `tip_height`, `from_height`, and `count` are present.
- Bad params clamp or error consistently with `blockchain.getblockfilters`.

Commit:

```bash
git add <test files> <CMake registration>
git commit -S -m "test(rpc): cover shielded outputs light-client feed"
```

---

## Task 5: Benchmark and Sanity Log

**Files:**
- Create: `docs/superpowers/plans/2026-05-27-trustless-light-client-shielded-m2-sanity.md`

Record:

- RPC latency for a 2000-block range with current mainnet-like data.
- Payload size compared to `blockchain.getblock` full block fetch.
- Output count and returned block count.
- Whether on-demand leaf-index counting is acceptable.

If on-demand counting is too slow, open a follow-up task for ChainDB sidecar cache keyed by block hash. Do not add the cache preemptively.

Commit:

```bash
git add docs/superpowers/plans/2026-05-27-trustless-light-client-shielded-m2-sanity.md
git commit -S -m "docs: record M2 shielded output feed sanity results"
```

---

## Done Criteria

- `blockchain.shielded.outputs` returns public shielded output feed entries over a height range.
- No wallet secret or query tag is accepted by the daemon.
- Malformed bundle bytes fail loudly.
- Leaf indexes are deterministic and match consensus append order.
- Tests cover extractor ordering, leaf indexing, and RPC JSON shape.
- Sanity log documents latency and payload savings.

## Follow-Ups

- DineroDPI consumer PR: add `ShieldedFilterDiscovery` that calls `blockchain.shielded.outputs` and feeds `ShieldedScanner.scan(block:)`.
- M3 spec/plan: `shielded.witness.by_index` + Spartan prover `.xcframework` + thin-client spend.
- Future protocol research: optional sender-provided viewing tag if Dinero wants a true recipient-specific shielded compact filter. That is not an M2 daemon-only patch.
