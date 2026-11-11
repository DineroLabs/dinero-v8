#include "consensus/block_index.h"
#include "consensus/chainwork.h"
#include "consensus/target_helpers.h"
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace dinero {

// === CBlockIndex Implementation ===

CBlockIndex::CBlockIndex(const BlockHeader& header, uint32_t height)
    : height(height) {
    // BlockHeader v1: Clean fields with no duplication
    hash = header.GetHash();
    prev_hash = header.prev_block_hash;
    version = header.version;
    merkle_root = header.merkle_root;
    timestamp = header.timestamp;
    bits = header.difficulty;
    nonce = header.nonce;
    status = BLOCK_VALID_HEADER; // Header validated by default
}

bool CBlockIndex::HasMoreWork(const CBlockIndex* other) const {
    if (!other) return true;
    return chainwork::CompareWork(chainwork, other->chainwork) > 0;
}

std::unique_ptr<CBlockIndex> CBlockIndex::FromHeader(const BlockHeader& header, uint32_t height) {
    return std::make_unique<CBlockIndex>(header, height);
}

// === Chainwork Calculation Implementation ===

namespace chainwork {

std::string WorkForBits(uint32_t bits) {
    if (bits == 0) return "0000000000000000000000000000000000000000000000000000000000000000";

    // Use proper chainwork calculation
    arith_uint256 work = GetBlockProof(bits);
    return ChainworkToHex(work);
}

std::string BitsToTarget(uint32_t bits) {
    auto target_array = dinero::TargetFromBits(bits);
    return dinero::TargetToHex(target_array);
}

std::string TargetToWork(const std::string& target_hex) {
    if (target_hex.length() != 64) {
        return "0000000000000000000000000000000000000000000000000000000000000000";
    }

    // Work = 2^256 / (target + 1)
    // For simplicity, we'll use a big integer approximation

    // Convert target hex to bytes for calculation
    std::vector<uint8_t> target_bytes(32);
    for (size_t i = 0; i < 32; ++i) {
        std::string byte_str = target_hex.substr(i * 2, 2);
        target_bytes[i] = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
    }

    // Simple approximation: work = 0xFFFFFFFFFFFFFFFF / (first 8 bytes of target)
    uint64_t target_head = 0;
    for (int i = 0; i < 8; ++i) {
        target_head = (target_head << 8) | target_bytes[i];
    }

    if (target_head == 0) target_head = 1; // Avoid division by zero

    uint64_t work_approx = 0xFFFFFFFFFFFFFFFFULL / target_head;

    // Convert back to hex string (padded to 64 chars)
    std::ostringstream work_stream;
    work_stream << std::hex << std::setfill('0') << std::setw(16) << work_approx;
    std::string work_hex = work_stream.str();

    // Pad to 64 characters
    while (work_hex.length() < 64) {
        work_hex = "0" + work_hex;
    }

    return work_hex;
}

std::string AddWork(const std::string& work_a, const std::string& work_b) {
    if (work_a.length() != 64 || work_b.length() != 64) {
        return "0000000000000000000000000000000000000000000000000000000000000000";
    }

    // Use proper big integer addition
    arith_uint256 a = ChainworkFromHex(work_a);
    arith_uint256 b = ChainworkFromHex(work_b);
    arith_uint256 result = a + b;

    return ChainworkToHex(result);
}

int CompareWork(const std::string& work_a, const std::string& work_b) {
    return CompareChainwork(work_a, work_b);
}

} // namespace chainwork

} // namespace dinero
