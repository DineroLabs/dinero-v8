#pragma once

#include "primitives/uint256.h"
#include "primitives/hash_domains.h"  // Phase M.4.3-B: TxId semantic type
#include <cstdint>
#include <functional>
#include <string>

namespace dinero {

/**
 * OutPoint - Canonical transaction output identifier
 *
 * Phase M.4.3-B: Malleability-proof UTXO references
 *
 * Invariants:
 * - txid is TxId (NOT WTxId - compile-time enforced)
 * - RPC conversion happens at boundary only
 * - Equality is structural (txid == txid && vout == vout)
 * - Hashable for unordered containers
 */
struct OutPoint {
    TxId txid;       // Phase M.4.3-B: Semantic type (NOT uint256, NOT WTxId)
    uint32_t vout;

    OutPoint() : vout(0) {}
    OutPoint(const TxId& hash, uint32_t n) : txid(hash), vout(n) {}

    // Check if this is a null/coinbase outpoint
    bool IsNull() const {
        return txid.IsNull() && vout == 0xFFFFFFFF;
    }

    // Equality
    bool operator==(const OutPoint& other) const {
        return txid == other.txid && vout == other.vout;
    }

    bool operator!=(const OutPoint& other) const {
        return !(*this == other);
    }

    // Phase M.2: Binary comparison for consensus logic
    // NOTE: uint256::operator< performs lexicographic comparison on the
    // internal little-endian representation. This ordering is deterministic
    // and consensus-safe, used ONLY for duplicate detection (not semantic ordering).
    bool operator<(const OutPoint& other) const {
        if (txid != other.txid) {
            return txid < other.txid;  // uint256::operator< uses memcmp
        }
        return vout < other.vout;
    }

    // NOTE: RPC/logging boundary only. Core logic must not depend on these methods.
    // Use for: RPC input/output, logging, debugging only
    // Never use for: identity comparison, storage keys, algorithm logic
    std::string ToString() const {
        return txid.AsUint256().GetHex() + ":" + std::to_string(vout);  // Phase M.4.3-B: Explicit boundary
    }

    static OutPoint FromString(const std::string& str) {
        size_t colon = str.find(':');
        if (colon == std::string::npos) {
            return OutPoint{};
        }
        uint256 hash = uint256::FromHexUnsafe(str.substr(0, colon));
        uint32_t n = std::stoul(str.substr(colon + 1));
        return OutPoint{TxId(hash), n};  // Phase M.4.3-B: Wrap in TxId
    }
};

} // namespace dinero

// std::hash specialization for unordered containers
namespace std {
template<>
struct hash<dinero::OutPoint> {
    size_t operator()(const dinero::OutPoint& outpoint) const {
        // Combine txid hash with vout - Phase M.4.3-B: Uses TxId hash
        size_t h1 = std::hash<dinero::TxId>{}(outpoint.txid);
        size_t h2 = std::hash<uint32_t>{}(outpoint.vout);
        return h1 ^ (h2 << 1);
    }
};
} // namespace std
