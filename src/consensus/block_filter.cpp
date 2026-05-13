// Copyright (c) 2025-2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "consensus/block_filter.h"
#include "crypto/siphash.h"
#include "common/sha256d.h"
#include "dinero/compat/int128.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace dinero {
namespace consensus {

// ============================================================================
// BitWriter
// ============================================================================

void BitWriter::WriteBit(bool bit) {
    current_ |= (bit ? 1 : 0) << (7 - bits_written_);
    bits_written_++;
    if (bits_written_ == 8) {
        buf_.push_back(current_);
        current_ = 0;
        bits_written_ = 0;
    }
}

void BitWriter::WriteBits(uint64_t value, int nbits) {
    for (int i = nbits - 1; i >= 0; --i) {
        WriteBit((value >> i) & 1);
    }
}

void BitWriter::WriteUnary(uint64_t value) {
    // Write 'value' 1-bits, then a 0-bit
    for (uint64_t i = 0; i < value; ++i) {
        WriteBit(true);
    }
    WriteBit(false);
}

std::vector<uint8_t> BitWriter::Finish() {
    if (bits_written_ > 0) {
        buf_.push_back(current_);
    }
    return std::move(buf_);
}

// ============================================================================
// BitReader
// ============================================================================

BitReader::BitReader(const uint8_t* data, size_t len)
    : data_(data), len_(len) {}

bool BitReader::ReadBit() {
    if (byte_pos_ >= len_) return false;
    bool bit = (data_[byte_pos_] >> (7 - bit_pos_)) & 1;
    bit_pos_++;
    if (bit_pos_ == 8) {
        byte_pos_++;
        bit_pos_ = 0;
    }
    return bit;
}

uint64_t BitReader::ReadBits(int nbits) {
    uint64_t value = 0;
    for (int i = 0; i < nbits; ++i) {
        value = (value << 1) | (ReadBit() ? 1 : 0);
    }
    return value;
}

uint64_t BitReader::ReadUnary() {
    uint64_t count = 0;
    while (ReadBit()) {
        count++;
    }
    return count;
}

bool BitReader::IsEOF() const {
    return byte_pos_ >= len_;
}

// ============================================================================
// GCSFilter — Key extraction
// ============================================================================

void GCSFilter::GetSipHashKey(uint64_t& k0, uint64_t& k1) const {
    // Dinero DNRF: key = first 16 bytes of prev_block_hash (LE)
    std::memcpy(&k0, block_hash.data, 8);
    std::memcpy(&k1, block_hash.data + 8, 8);
}

uint64_t GCSFilter::HashElement(const std::vector<uint8_t>& script) const {
    uint64_t k0, k1;
    GetSipHashKey(k0, k1);
    uint64_t hash = crypto::SipHash24(k0, k1, script);

    // Map into [0, N*M) where N = element_count
    // BIP158: hash % (N * M)
    // Use 128-bit multiply + shift to avoid modulo bias:
    // mapped = (hash * (N * M)) >> 64
    // This is the "fast range" technique from BIP158.
    return dinero::compat::hi64(
        dinero::compat::mul_u64(hash,
                                static_cast<uint64_t>(element_count) * M));
}

// ============================================================================
// GCSFilter — Golomb-Rice codec
// ============================================================================

std::vector<uint8_t> GCSFilter::GolombRiceEncode(const std::vector<uint64_t>& sorted_values) {
    BitWriter writer;

    uint64_t prev = 0;
    for (uint64_t value : sorted_values) {
        uint64_t delta = value - prev;
        prev = value;

        // Golomb-Rice: split delta into quotient and remainder
        uint64_t quotient = delta >> P;
        uint64_t remainder = delta & ((1ULL << P) - 1);

        writer.WriteUnary(quotient);
        writer.WriteBits(remainder, P);
    }

    return writer.Finish();
}

std::vector<uint64_t> GCSFilter::GolombRiceDecode(const uint8_t* data, size_t len,
                                                    uint32_t element_count) {
    std::vector<uint64_t> result;
    result.reserve(element_count);

    BitReader reader(data, len);
    uint64_t value = 0;

    for (uint32_t i = 0; i < element_count; ++i) {
        uint64_t quotient = reader.ReadUnary();
        uint64_t remainder = reader.ReadBits(P);
        uint64_t delta = (quotient << P) | remainder;
        value += delta;
        result.push_back(value);
    }

    return result;
}

// ============================================================================
// GCSFilter — Build
// ============================================================================

GCSFilter GCSFilter::Build(const std::vector<std::vector<uint8_t>>& scripts,
                            const uint256& prev_block_hash) {
    GCSFilter filter;
    filter.block_hash = prev_block_hash;

    // Deduplicate scripts
    std::set<std::vector<uint8_t>> unique_scripts(scripts.begin(), scripts.end());

    // Remove empty scripts
    unique_scripts.erase(std::vector<uint8_t>{});

    if (unique_scripts.empty()) {
        filter.element_count = 0;
        return filter;
    }

    filter.element_count = static_cast<uint32_t>(unique_scripts.size());

    // Hash each script into [0, N*M)
    std::vector<uint64_t> hashed;
    hashed.reserve(filter.element_count);

    uint64_t k0, k1;
    filter.GetSipHashKey(k0, k1);

    uint64_t range = static_cast<uint64_t>(filter.element_count) * M;

    for (const auto& script : unique_scripts) {
        uint64_t hash = crypto::SipHash24(k0, k1, script);
        // Fast range mapping: (hash * range) >> 64
        hashed.push_back(dinero::compat::hi64(dinero::compat::mul_u64(hash, range)));
    }

    // Sort
    std::sort(hashed.begin(), hashed.end());

    // Golomb-Rice encode
    filter.encoded_data = GolombRiceEncode(hashed);

    return filter;
}

GCSFilter GCSFilter::BuildFromBlock(const Block& block,
                                     const uint256& prev_block_hash,
                                     UTXOLookup utxo_lookup) {
    std::vector<std::vector<uint8_t>> scripts;

    for (const auto& tx : block.vtx) {
        // All output scriptPubKeys
        for (const auto& out : tx.vout) {
            if (!out.scriptPubKey.empty()) {
                scripts.push_back(out.scriptPubKey);
            }
        }

        // Spent input scriptPubKeys (requires UTXO lookup)
        if (utxo_lookup) {
            for (const auto& in : tx.vin) {
                // Skip coinbase inputs (prevout txid is null)
                if (in.prevout.txid.IsNull()) continue;

                auto spent_script = utxo_lookup(in.prevout.txid.AsUint256(), in.prevout.vout);
                if (!spent_script.empty()) {
                    scripts.push_back(std::move(spent_script));
                }
            }
        }
    }

    return Build(scripts, prev_block_hash);
}

// ============================================================================
// GCSFilter — Match
// ============================================================================

bool GCSFilter::Match(const std::vector<uint8_t>& script) const {
    if (element_count == 0 || encoded_data.empty()) return false;

    // Hash the query script
    uint64_t k0, k1;
    GetSipHashKey(k0, k1);
    uint64_t hash = crypto::SipHash24(k0, k1, script);

    uint64_t range = static_cast<uint64_t>(element_count) * M;
    uint64_t target = dinero::compat::hi64(dinero::compat::mul_u64(hash, range));

    // Decode filter and search (streaming decode — stop early on match)
    BitReader reader(encoded_data.data(), encoded_data.size());
    uint64_t value = 0;

    for (uint32_t i = 0; i < element_count; ++i) {
        uint64_t quotient = reader.ReadUnary();
        uint64_t remainder = reader.ReadBits(P);
        uint64_t delta = (quotient << P) | remainder;
        value += delta;

        if (value == target) return true;
        if (value > target) return false;  // Sorted — no point continuing
    }

    return false;
}

bool GCSFilter::MatchAny(const std::vector<std::vector<uint8_t>>& scripts) const {
    if (element_count == 0 || encoded_data.empty() || scripts.empty()) return false;

    // Hash all query scripts and sort
    uint64_t k0, k1;
    GetSipHashKey(k0, k1);

    uint64_t range = static_cast<uint64_t>(element_count) * M;

    std::vector<uint64_t> targets;
    targets.reserve(scripts.size());

    for (const auto& script : scripts) {
        if (script.empty()) continue;
        uint64_t hash = crypto::SipHash24(k0, k1, script);
        targets.push_back(dinero::compat::hi64(dinero::compat::mul_u64(hash, range)));
    }

    if (targets.empty()) return false;

    std::sort(targets.begin(), targets.end());

    // Merge-intersect: walk filter and targets in parallel (both sorted)
    BitReader reader(encoded_data.data(), encoded_data.size());
    uint64_t filter_value = 0;
    size_t target_idx = 0;

    for (uint32_t i = 0; i < element_count && target_idx < targets.size(); ++i) {
        uint64_t quotient = reader.ReadUnary();
        uint64_t remainder = reader.ReadBits(P);
        uint64_t delta = (quotient << P) | remainder;
        filter_value += delta;

        // Advance targets past current filter value
        while (target_idx < targets.size() && targets[target_idx] < filter_value) {
            target_idx++;
        }

        if (target_idx < targets.size() && targets[target_idx] == filter_value) {
            return true;
        }
    }

    return false;
}

// ============================================================================
// GCSFilter — Hash / Deserialize
// ============================================================================

uint256 GCSFilter::GetHash() const {
    if (encoded_data.empty()) {
        return uint256{};
    }

    auto hash_bytes = Dinero::Common::double_sha256_raw(
        encoded_data.data(), encoded_data.size()
    );

    uint256 result;
    if (hash_bytes.size() == 32) {
        std::memcpy(result.data, hash_bytes.data(), 32);
    }
    return result;
}

GCSFilter GCSFilter::FromEncoded(const std::vector<uint8_t>& data,
                                  uint32_t element_count,
                                  const uint256& prev_block_hash) {
    GCSFilter filter;
    filter.encoded_data = data;
    filter.element_count = element_count;
    filter.block_hash = prev_block_hash;
    return filter;
}

} // namespace consensus
} // namespace dinero
