// Copyright (c) 2025-2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// BIP158-style Golomb-Coded Set (GCS) block filter.
//
// Each block gets a compact filter encoding all scriptPubKeys that appear
// in the block (both created outputs and spent inputs). Mobile clients
// download filters and check whether any of their addresses match,
// without downloading full blocks.
//
// Parameters (BIP158 "basic" filter):
//   P = 19              (false positive rate = 1/2^19 ≈ 1 in 524,288)
//   M = 784931          (= P × 1.497137, BIP158 constant)
//   Key = SipHash-2-4 keyed with first 16 bytes of prev_block_hash
//
// Filter authenticity: verified via coinbase commitment → merkle proof → header chain.

#include "primitives/block.h"
#include "primitives/uint256.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace dinero {
namespace consensus {

/// Golomb-Coded Set block filter (BIP158).
struct GCSFilter {
    std::vector<uint8_t> encoded_data;   ///< Golomb-Rice encoded filter
    uint32_t element_count = 0;          ///< Number of elements in filter
    uint256 block_hash;                  ///< Prev block hash (provides SipHash key)

    // BIP158 parameters
    static constexpr uint8_t P = 19;
    static constexpr uint64_t M = 784931;

    /// Build a filter from a set of scriptPubKeys and the prev block hash.
    /// Scripts should include all output scriptPubKeys AND all spent input
    /// scriptPubKeys from the block. Duplicates are fine (will be deduplicated).
    static GCSFilter Build(const std::vector<std::vector<uint8_t>>& scripts,
                           const uint256& prev_block_hash);

    /// Build a filter directly from a Block.
    /// Requires a callback to resolve spent scriptPubKeys from input prevouts.
    /// If utxo_lookup is null, only output scripts are included (partial filter).
    using UTXOLookup = std::function<std::vector<uint8_t>(const uint256& txid, uint32_t vout)>;
    static GCSFilter BuildFromBlock(const Block& block,
                                    const uint256& prev_block_hash,
                                    UTXOLookup utxo_lookup = nullptr);

    /// Check if the filter might contain the given scriptPubKey.
    /// False positives possible (rate ≈ 1/2^P). No false negatives.
    bool Match(const std::vector<uint8_t>& script) const;

    /// Check if the filter might contain ANY of the given scriptPubKeys.
    /// More efficient than calling Match() in a loop.
    bool MatchAny(const std::vector<std::vector<uint8_t>>& scripts) const;

    /// SHA256d hash of the encoded filter data. Used for coinbase commitment.
    uint256 GetHash() const;

    /// Deserialize a filter from raw bytes + metadata.
    static GCSFilter FromEncoded(const std::vector<uint8_t>& data,
                                 uint32_t element_count,
                                 const uint256& prev_block_hash);

    /// Check if filter is empty/uninitialized.
    bool IsEmpty() const { return encoded_data.empty() || element_count == 0; }

private:
    /// Extract SipHash key (k0, k1) from prev block hash.
    void GetSipHashKey(uint64_t& k0, uint64_t& k1) const;

    /// Hash a script element into the filter's range [0, N*M).
    uint64_t HashElement(const std::vector<uint8_t>& script) const;

    /// Golomb-Rice encode a sorted list of values.
    static std::vector<uint8_t> GolombRiceEncode(const std::vector<uint64_t>& sorted_values);

    /// Golomb-Rice decode to a sorted list of values.
    static std::vector<uint64_t> GolombRiceDecode(const uint8_t* data, size_t len,
                                                   uint32_t element_count);
};

// ============================================================================
// Bit stream helpers (used internally by Golomb-Rice codec)
// ============================================================================

/// Write bits to a byte buffer, MSB-first.
class BitWriter {
public:
    void WriteBit(bool bit);
    void WriteBits(uint64_t value, int nbits);
    void WriteUnary(uint64_t value);
    std::vector<uint8_t> Finish();

private:
    std::vector<uint8_t> buf_;
    uint8_t current_ = 0;
    int bits_written_ = 0;
};

/// Read bits from a byte buffer, MSB-first.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t len);
    bool ReadBit();
    uint64_t ReadBits(int nbits);
    uint64_t ReadUnary();
    bool IsEOF() const;

private:
    const uint8_t* data_;
    size_t len_;
    size_t byte_pos_ = 0;
    int bit_pos_ = 0;   // 0-7, MSB-first
};

} // namespace consensus
} // namespace dinero
