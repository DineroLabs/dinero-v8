# Phase 9: Proof Compression Design

## Current State (Baseline)

### Proof Structure
```cpp
struct BlockUtreexoProof {
    std::vector<Hash256> targets;       // Leaf hashes (32 bytes each)
    std::vector<Hash256> proof_hashes;  // Merkle proof nodes (32 bytes each)
};
```

### Wire Format (Uncompressed)
```
[varint: num_targets]
[target_0: 32 bytes]
[target_1: 32 bytes]
...
[varint: num_proof_hashes]
[proof_hash_0: 32 bytes]
[proof_hash_1: 32 bytes]
...
```

### Size Estimates (Uncompressed)
- **Coinbase-only block**: ~100 bytes (no spends)
- **Typical block (10 inputs)**: ~500-1000 bytes
- **Large block (100 inputs)**: ~5-10 KB

### Problem
- Every hash is 32 bytes
- Same hashes may appear multiple times (especially in proof_hashes)
- No sharing between proofs for different blocks
- Raw hash data doesn't compress well without structure

## Phase 9 Compression Strategy

### 1. Hash Deduplication (30-50% savings)

**Problem:** Same hash appears multiple times in `proof_hashes`

**Solution:** Dictionary encoding
```cpp
struct CompressedBlockUtreexoProof {
    std::vector<Hash256> hash_dictionary;  // Unique hashes only
    std::vector<uint16_t> target_indices;  // Index into dictionary
    std::vector<uint16_t> proof_indices;   // Index into dictionary
};
```

**Wire Format:**
```
[varint: dict_size]
[hash_0: 32 bytes]
[hash_1: 32 bytes]
...
[varint: num_targets]
[target_idx_0: varint]  // << Index, not full hash
[target_idx_1: varint]
...
[varint: num_proofs]
[proof_idx_0: varint]
[proof_idx_1: varint]
...
```

**Savings:**
- Unique hashes: N × 32 bytes
- Indices: M × 1-3 bytes (varint)
- If 50% duplication: ~30-40% size reduction

### 2. Frontier Reuse (Not Implemented in Phase 9)

**Rationale:** Requires batch proof coordination
- Multiple blocks share frontier nodes
- Needs stateful cache across requests
- Defer to Phase 10+ (optimization)

**Phase 9:** Skip this (complexity >> benefit for single-proof case)

### 3. zstd Framing (10-20% additional savings)

**Problem:** Even deduplicated data has entropy

**Solution:** Wrap compressed proof in zstd frame
```cpp
struct ZstdCompressedProof {
    uint32_t uncompressed_size;
    uint32_t compressed_size;
    std::vector<uint8_t> zstd_data;
};
```

**Wire Format:**
```
[uncompressed_size: 4 bytes]
[compressed_size: 4 bytes]
[zstd_data: variable]
```

**Settings:**
- Compression level: 3 (fast, good ratio)
- Dictionary: None (proofs are too small)
- Streaming: No (proofs are atomic)

**When to use:**
- Proof size > 256 bytes (overhead threshold)
- FLAG_COMPRESSED set in request

### 4. Proof Caching (Avoid Regeneration)

**Problem:** Same block proof requested multiple times

**Solution:** LRU cache in BridgeNode
```cpp
class ProofCache {
    std::unordered_map<uint256, CachedProof> cache_;
    std::list<uint256> lru_list_;
    size_t max_size_ = 1000;  // ~10 MB for typical blocks
};

struct CachedProof {
    UtreexoProofMessage proof;
    std::chrono::steady_clock::time_point cached_at;
    size_t access_count;
};
```

**Eviction:**
- LRU eviction when cache full
- TTL: 10 minutes (reorgs rare)
- Size limit: 1000 proofs (~10 MB)

## Implementation Plan

### Phase 9.1: Hash Deduplication
1. Add `CompressedBlockUtreexoProof` struct
2. Implement `compress()` and `decompress()` methods
3. Update serialization to support compressed format
4. Add FLAG_COMPRESSED handling

### Phase 9.2: zstd Framing
1. Add zstd dependency (already in third_party/)
2. Implement `compressWithZstd()` wrapper
3. Add threshold logic (compress only if > 256 bytes)
4. Update wire format versioning

### Phase 9.3: Proof Caching
1. Add `ProofCache` class to BridgeNode
2. Implement LRU eviction
3. Cache proofs after generation
4. Return cached proofs on duplicate requests

### Phase 9.4: Metrics & Measurement
1. Track compression ratios
2. Measure cache hit rates
3. Log bandwidth savings

## Wire Format Versioning

### Uncompressed (version 1 - current)
```
[version: 1]
[targets: varint + 32*N bytes]
[proof_hashes: varint + 32*M bytes]
```

### Deduplicated (version 2)
```
[version: 2]
[dictionary: varint + 32*D bytes]
[target_indices: varint + varint*N]
[proof_indices: varint + varint*M]
```

### Deduplicated + zstd (version 3)
```
[version: 3]
[uncompressed_size: 4 bytes]
[compressed_size: 4 bytes]
[zstd(deduplicated_format): variable]
```

## Compatibility

**Backward compatibility:** Required
- Stateless nodes MUST support version 1 (uncompressed)
- Bridge nodes SHOULD support all versions
- Version negotiation via FLAG_COMPRESSED

**Feature detection:**
- If FLAG_COMPRESSED not set → send version 1
- If FLAG_COMPRESSED set → send highest supported version

## Expected Bandwidth Savings

| Scenario | Uncompressed | Deduplicated | + zstd | Savings |
|----------|-------------|--------------|--------|---------|
| Coinbase-only | 100 B | 100 B | 100 B | 0% |
| Typical (10 in) | 800 B | 500 B | 400 B | 50% |
| Large (100 in) | 8 KB | 5 KB | 4 KB | 50% |

**Real-world estimate:** 40-50% bandwidth reduction for typical blocks

## Security Considerations

1. **Decompression bomb protection**
   - Limit `uncompressed_size` to MAX_PROOF_SIZE (100 KB)
   - Reject if ratio > 100:1

2. **Dictionary attack protection**
   - Validate dictionary size < MAX_PROOF_SIZE
   - Validate all indices < dictionary size

3. **zstd safety**
   - Use bounded decompression
   - Timeout on decompression (1 second)

## Testing Strategy

1. **Unit tests:**
   - Compress/decompress round-trip
   - Dictionary deduplication correctness
   - zstd boundary conditions

2. **Integration tests:**
   - Mixed version compatibility
   - Cache hit/miss scenarios
   - Bandwidth measurement

3. **Property tests:**
   - Compression preserves proof validity
   - Decompression is deterministic
   - Cache eviction doesn't break correctness

## Non-Goals (Phase 9)

❌ **Frontier reuse** - requires batch coordination (Phase 10+)
❌ **Delta proofs** - requires state sync (Phase 10+)
❌ **Custom dictionaries** - premature optimization
❌ **Hardware acceleration** - not bottleneck yet

## Success Criteria

✅ 40%+ bandwidth reduction on typical blocks
✅ < 1ms compression overhead
✅ < 1ms decompression overhead
✅ 90%+ cache hit rate on duplicate requests
✅ Zero correctness regressions
