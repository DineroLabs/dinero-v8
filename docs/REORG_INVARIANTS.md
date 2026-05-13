# Reorg Invariants

This document describes the critical invariants that must be maintained for
correct blockchain reorganization behavior in Dinero.

## 1. ONE BlockIndex Graph

**Invariant:** There must be exactly ONE `CBlockIndex` entry per block hash
in the entire process.

**Why it matters:**
- `pprev` pointers must form a single coherent ancestry chain
- Chainwork calculations depend on following `pprev` to genesis
- Multiple entries for the same hash would create "parallel universes" where
  blocks have different parents depending on which entry you look up

**Enforcement:**
- All BlockIndex creation goes through `dinero::AddBlockIndex()` (global)
- `ChainstateService::AddBlockIndex()` delegates to the global function
- Debug assertions verify `FindBlockIndex(hash) == result` after every insertion
- Disk load path includes the same assertion

**What breaks if violated:**
- Fork discovery fails (can't find common ancestor)
- `pprev` chains become fragmented
- Chainwork comparisons become meaningless
- Reorgs fail or corrupt state

## 2. Undo Data Must Be Reversible

**Invariant:** Every block connection must produce undo data that can exactly
reverse the state change.

**Components:**
- `BlockUndo.spent_coins` - UTXOs consumed (restore on disconnect)
- `BlockUndo.utreexo_delta` - Accumulator changes (unapply on disconnect)

**Why it matters:**
- Reorgs disconnect blocks by applying undo data in reverse
- If undo data is incomplete, disconnect leaves corrupted state
- The new chain then connects on top of garbage

**Note on persistence:**
- `BlockUndo` (consensus layer) contains full undo data including `utreexo_delta`
- `UndoRecord` (storage layer) persists spent coins only
- `utreexo_delta` is regenerated from the Utreexo forest during disconnect

## 3. Tests Derive Height Dynamically

**Invariant:** Test scripts must not assume a fixed initial chain height.

**Why it matters:**
- Dinero starts with a premine block at height 1 (not genesis at height 0)
- Hard-coded height expectations like `assert height == 0` will fail
- Tests become brittle and break when chain parameters change

**Correct pattern:**
```bash
# Get initial height dynamically
INITIAL_HEIGHT=$(rpc "blockchain.getblockcount")

# Use offsets for all expectations
EXPECTED_HEIGHT=$((INITIAL_HEIGHT + 2))
```

**What this enables:**
- Tests work regardless of premine configuration
- Tests are portable across networks (mainnet, testnet, regtest)
- Future chain parameter changes don't break tests

---

## Debug Assertions

In debug builds (`NDEBUG` not defined), the following assertions guard these
invariants:

1. **AddBlockIndex (global):** `src/consensus/block_index.cpp`
2. **AddBlockIndex (service):** `src/daemon/services/chainstate_service.cpp`
3. **Disk load:** `src/consensus/block_index_persistence.cpp`

If any assertion fires, it indicates a bug in BlockIndex management that
would cause reorg failures.
