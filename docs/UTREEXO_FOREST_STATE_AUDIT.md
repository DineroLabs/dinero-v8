# UtreexoForest Internal State Audit (Phase 4)

**Date:** 2026-01-07
**Purpose:** Identify all state that must be snapshotted for reorg-safe rollback

---

## Internal State Components

### Private Member Variables (utreexo_accumulator.h:275-284)

```cpp
private:
    std::vector<Hash256> roots_;    // Current forest roots (ordered by tree height, descending)
    uint64_t numLeaves_;            // Total number of leaves in forest
    std::vector<Hash256> nodes_;    // Internal forest representation (for proof generation)
                                    // Map: position -> hash
```

---

## Current Serialization Status

### ✅ What IS Currently Serialized (utreexo_accumulator.cpp:580-606)

1. **numLeaves_** (8 bytes, little-endian)
   - Total number of leaves in forest
   - Required to reconstruct forest structure

2. **roots_** (variable length)
   - Number of roots (4 bytes)
   - Each root: 32 bytes (Hash256)
   - Required for commitment computation

### ❌ What IS MISSING from Serialization

3. **nodes_** (variable length) - **CRITICAL GAP**
   - Internal forest representation
   - Map: position → hash
   - Required for:
     - Proof generation
     - Remove operations
     - Tree structure maintenance
     - Complete state restoration

---

## Impact of Missing `nodes_` Serialization

### ❌ Current Deserialization Behavior

```cpp
UtreexoForest UtreexoForest::deserialize(const std::vector<uint8_t>& data) {
    UtreexoForest forest;

    forest.numLeaves_ = /* deserialize 8 bytes */;
    forest.roots_ = /* deserialize roots */;
    // ❌ nodes_ remains EMPTY!

    return forest;
}
```

**Result:** Restored forest has:
- ✅ Correct `numLeaves_`
- ✅ Correct `roots_`
- ❌ **Empty `nodes_` vector**

### 🚨 Consequences

1. **Cannot generate proofs** - `prove()` will fail
2. **Cannot remove UTXOs** - `remove()` will fail
3. **Cannot maintain tree structure** - Internal operations broken
4. **Reorg will corrupt state** - Restored forest is incomplete

---

## Required Fix

### Add `nodes_` to Serialization

**Format:**
```
UtreexoForest Serialization:
├─ numLeaves (8 bytes, little-endian)
├─ numRoots (4 bytes, little-endian)
├─ roots[] (32 bytes each)
├─ numNodes (4 bytes, little-endian)        ← NEW
└─ nodes[] (32 bytes each)                  ← NEW
```

**Implementation:**
1. Serialize `nodes_.size()` as uint32_t (4 bytes)
2. Serialize each `nodes_[i]` as Hash256 (32 bytes)
3. Deserialize in same order
4. Restore `nodes_` vector exactly

---

## Verification Requirements

After adding `nodes_` serialization:

### Test A: Roundtrip Consistency
```cpp
UtreexoForest original = /* ... */;
auto bytes = original.serialize();
UtreexoForest restored = UtreexoForest::deserialize(bytes);

// MUST be true:
assert(original.numLeaves_ == restored.numLeaves_);
assert(original.roots_ == restored.roots_);
assert(original.nodes_ == restored.nodes_);  // ← CRITICAL
```

### Test B: Proof Generation After Restore
```cpp
UtreexoForest original = /* with 3 UTXOs */;
auto proof1 = original.prove(0);  // Generate proof before save

auto bytes = original.serialize();
UtreexoForest restored = UtreexoForest::deserialize(bytes);
auto proof2 = restored.prove(0);  // Generate proof after restore

// MUST be true:
assert(proof1.has_value() && proof2.has_value());
assert(proof1->siblings == proof2->siblings);
```

### Test C: Operations After Restore
```cpp
UtreexoForest original = /* ... */;
original.add(leafHash1);
original.add(leafHash2);

auto bytes = original.serialize();
UtreexoForest restored = UtreexoForest::deserialize(bytes);

// MUST NOT crash or fail:
restored.add(leafHash3);  // Can add new leaves
restored.remove(leafHash1, proof);  // Can remove existing leaves
auto commitment = restored.getCommitment();  // Can compute commitment
```

---

## Conclusion

**Current state:** UtreexoForest serialization is INCOMPLETE.

**Missing component:** `nodes_` vector serialization.

**Impact:** Reorg rollback will produce corrupt/unusable accumulator state.

**Required action:** Add `nodes_` serialization before Phase 4 is complete.

**Estimated addition:** ~15 lines in serialize(), ~15 lines in deserialize().

---

**Next Step:** Implement complete serialization with `nodes_` included.
