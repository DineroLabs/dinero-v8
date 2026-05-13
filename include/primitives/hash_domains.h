/**
 * @file hash_domains.h
 * @brief Phase M.3: Semantic Hash Domains
 *
 * Purpose: Make it impossible to confuse different kinds of hashes at compile time
 *
 * Core Invariant:
 *   Every cryptographic hash lives in an explicit semantic domain.
 *   Same bytes ≠ same meaning.
 *
 * Design Principles:
 *   - No inheritance
 *   - No implicit conversions
 *   - No cross-assignment
 *   - Domain-locked constructors only
 *   - Storage/wire format unchanged (still 32 bytes)
 *
 * What M.3 Prevents:
 *   ❌ Passing txid where wtxid is required (malleability bugs)
 *   ❌ Indexing blocks by transaction hash
 *   ❌ Confusing merkle roots with commitments
 *   ❌ Lightning channels keyed by wrong hash
 *   ❌ Reorg logic comparing incompatible identities
 *
 * Phase Lineage:
 *   M.0 → Binary identity
 *   M.1 → Struct correctness
 *   M.2 → Boundary enforcement
 *   M.3 → Semantic meaning ← YOU ARE HERE
 */

#ifndef DINERO_PRIMITIVES_HASH_DOMAINS_H
#define DINERO_PRIMITIVES_HASH_DOMAINS_H

#include "primitives/uint256.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dinero {

// Forward declarations
struct BlockHeader;
struct Transaction;

// ═══════════════════════════════════════════════════════════════════════
// Semantic Hash Domain Types
// ═══════════════════════════════════════════════════════════════════════

/**
 * @brief Block hash domain - identifies a block
 *
 * Computed from: BlockHeader (all fields including prev_block, merkle_root, etc.)
 * Used for: Block indexing, chain navigation, reorg detection
 */
struct BlockHash {
    uint256 v;

    // Default constructor (creates null hash)
    BlockHash() : v() {}

    // Explicit constructor from uint256 (private use only)
    explicit BlockHash(const uint256& hash) : v(hash) {}

    // Domain-locked constructor (preferred)
    static BlockHash Compute(const BlockHeader& header);

    // Accessors
    const uint256& AsUint256() const { return v; }
    bool IsNull() const { return v.IsNull(); }

    // Comparison operators (within same domain)
    bool operator==(const BlockHash& other) const { return v == other.v; }
    bool operator!=(const BlockHash& other) const { return v != other.v; }
    bool operator<(const BlockHash& other) const { return v < other.v; }

    // Serialization support
    template <typename Stream>
    void Serialize(Stream& s) const { s << v; }

    template <typename Stream>
    void Unserialize(Stream& s) { s >> v; }
};

/**
 * @brief Transaction ID domain - canonical transaction identity
 *
 * Computed from: Transaction (without witness data)
 * Used for: UTXO indexing, mempool tracking, consensus validation
 * Note: Txid is NOT malleable (witness-stripped)
 */
struct TxId {
    uint256 v;

    TxId() : v() {}
    explicit TxId(const uint256& hash) : v(hash) {}

    static TxId Compute(const Transaction& tx);

    const uint256& AsUint256() const { return v; }
    bool IsNull() const { return v.IsNull(); }

    bool operator==(const TxId& other) const { return v == other.v; }
    bool operator!=(const TxId& other) const { return v != other.v; }
    bool operator<(const TxId& other) const { return v < other.v; }

    template <typename Stream>
    void Serialize(Stream& s) const { s << v; }

    template <typename Stream>
    void Unserialize(Stream& s) { s >> v; }
};

/**
 * @brief Witness Transaction ID domain - includes witness data
 *
 * Computed from: Transaction (with witness data)
 * Used for: SegWit relay, witness commitment validation
 * Note: WTxId IS malleable if witness data is malleated
 */
struct WTxId {
    uint256 v;

    WTxId() : v() {}
    explicit WTxId(const uint256& hash) : v(hash) {}

    static WTxId Compute(const Transaction& tx);

    const uint256& AsUint256() const { return v; }
    bool IsNull() const { return v.IsNull(); }

    bool operator==(const WTxId& other) const { return v == other.v; }
    bool operator!=(const WTxId& other) const { return v != other.v; }
    bool operator<(const WTxId& other) const { return v < other.v; }

    template <typename Stream>
    void Serialize(Stream& s) const { s << v; }

    template <typename Stream>
    void Unserialize(Stream& s) { s >> v; }
};

/**
 * @brief Merkle root domain - root of transaction merkle tree
 *
 * Computed from: std::vector<TxId> (block transactions)
 * Used for: Block header validation, SPV proofs
 */
struct MerkleRoot {
    uint256 v;

    MerkleRoot() : v() {}
    explicit MerkleRoot(const uint256& hash) : v(hash) {}

    static MerkleRoot Compute(const std::vector<uint256>& leaves);

    const uint256& AsUint256() const { return v; }
    bool IsNull() const { return v.IsNull(); }

    bool operator==(const MerkleRoot& other) const { return v == other.v; }
    bool operator!=(const MerkleRoot& other) const { return v != other.v; }
    bool operator<(const MerkleRoot& other) const { return v < other.v; }

    template <typename Stream>
    void Serialize(Stream& s) const { s << v; }

    template <typename Stream>
    void Unserialize(Stream& s) { s >> v; }
};

/**
 * @brief Utreexo accumulator root domain
 *
 * Computed from: Utreexo forest state
 * Used for: Compact UTXO set commitment
 */
struct UtreexoRoot {
    uint256 v;

    UtreexoRoot() : v() {}
    explicit UtreexoRoot(const uint256& hash) : v(hash) {}

    const uint256& AsUint256() const { return v; }
    bool IsNull() const { return v.IsNull(); }

    bool operator==(const UtreexoRoot& other) const { return v == other.v; }
    bool operator!=(const UtreexoRoot& other) const { return v != other.v; }
    bool operator<(const UtreexoRoot& other) const { return v < other.v; }

    template <typename Stream>
    void Serialize(Stream& s) const { s << v; }

    template <typename Stream>
    void Unserialize(Stream& s) { s >> v; }
};

// ═══════════════════════════════════════════════════════════════════════
// Compile-Time Enforcement (Phase M.3 Tripwires)
// ═══════════════════════════════════════════════════════════════════════

// Prevent implicit conversions between domain types
static_assert(!std::is_convertible<BlockHash, TxId>::value,
    "BlockHash must NOT be convertible to TxId");
static_assert(!std::is_convertible<TxId, BlockHash>::value,
    "TxId must NOT be convertible to BlockHash");
static_assert(!std::is_convertible<TxId, WTxId>::value,
    "TxId must NOT be convertible to WTxId (malleability risk)");
static_assert(!std::is_convertible<WTxId, TxId>::value,
    "WTxId must NOT be convertible to TxId (malleability risk)");
static_assert(!std::is_convertible<MerkleRoot, TxId>::value,
    "MerkleRoot must NOT be convertible to TxId");
static_assert(!std::is_convertible<UtreexoRoot, BlockHash>::value,
    "UtreexoRoot must NOT be convertible to BlockHash");

// Ensure all domain types are trivially copyable (performance guarantee)
static_assert(std::is_trivially_copyable<BlockHash>::value,
    "BlockHash must be trivially copyable");
static_assert(std::is_trivially_copyable<TxId>::value,
    "TxId must be trivially copyable");
static_assert(std::is_trivially_copyable<WTxId>::value,
    "WTxId must be trivially copyable");
static_assert(std::is_trivially_copyable<MerkleRoot>::value,
    "MerkleRoot must be trivially copyable");
static_assert(std::is_trivially_copyable<UtreexoRoot>::value,
    "UtreexoRoot must be trivially copyable");

// Ensure all domain types are exactly 32 bytes (storage guarantee)
static_assert(sizeof(BlockHash) == 32, "BlockHash must be 32 bytes");
static_assert(sizeof(TxId) == 32, "TxId must be 32 bytes");
static_assert(sizeof(WTxId) == 32, "WTxId must be 32 bytes");
static_assert(sizeof(MerkleRoot) == 32, "MerkleRoot must be 32 bytes");
static_assert(sizeof(UtreexoRoot) == 32, "UtreexoRoot must be 32 bytes");

}  // namespace dinero

// ═══════════════════════════════════════════════════════════════════════
// Hash Function Specializations (for std::unordered_map, std::unordered_set)
// ═══════════════════════════════════════════════════════════════════════

namespace std {

template<>
struct hash<dinero::BlockHash> {
    size_t operator()(const dinero::BlockHash& h) const {
        return std::hash<dinero::uint256>{}(h.v);
    }
};

template<>
struct hash<dinero::TxId> {
    size_t operator()(const dinero::TxId& h) const {
        return std::hash<dinero::uint256>{}(h.v);
    }
};

template<>
struct hash<dinero::WTxId> {
    size_t operator()(const dinero::WTxId& h) const {
        return std::hash<dinero::uint256>{}(h.v);
    }
};

template<>
struct hash<dinero::MerkleRoot> {
    size_t operator()(const dinero::MerkleRoot& h) const {
        return std::hash<dinero::uint256>{}(h.v);
    }
};

template<>
struct hash<dinero::UtreexoRoot> {
    size_t operator()(const dinero::UtreexoRoot& h) const {
        return std::hash<dinero::uint256>{}(h.v);
    }
};

}  // namespace std

#endif  // DINERO_PRIMITIVES_HASH_DOMAINS_H
