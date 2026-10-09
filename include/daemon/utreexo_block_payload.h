#pragma once
#include "primitives/block.h"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dinero {
// Owned transport fields only. Neither proof bytes nor the advertised height
// and roots are authenticated here. The selected-chain validator remains the
// authority for proof validity, canonical acceptance and download completion.
struct UtreexoBlockPayload {
    uint8_t version{0};
    uint256 hash{};
    uint32_t height{0};
    std::vector<uint8_t> block_bytes;
    std::vector<uint8_t> root_after;
    std::vector<uint8_t> batch_proof;
    std::vector<uint8_t> transition_proof;
};

inline std::optional<UtreexoBlockPayload> DecodeUtreexoBlockPayload(
    std::span<const uint8_t> bytes) {
    constexpr size_t max_payload=5*1024*1024;
    constexpr uint32_t max_block=4*1024*1024;
    constexpr uint32_t max_proof=1024*1024;
    constexpr uint32_t max_transition=512*1024;
    if(bytes.size()<77 || bytes.size()>max_payload) return {};
    try {
        size_t pos=0;
        const auto take=[&](size_t count)->std::span<const uint8_t> {
            if(count>bytes.size()-pos) throw std::out_of_range("utxoblk frame");
            const auto part=bytes.subspan(pos,count);pos+=count;return part;
        };
        const auto number=[&] {
            const auto b=take(4);
            return uint32_t(b[0]) | (uint32_t(b[1])<<8) |
                   (uint32_t(b[2])<<16) | (uint32_t(b[3])<<24);
        };
        const auto frame=[&](uint32_t limit) {
            const auto count=number();
            if(count>limit) throw std::out_of_range("utxoblk limit");
            const auto part=take(count);return std::vector<uint8_t>(part.begin(),part.end());
        };
        UtreexoBlockPayload result;
        result.version=take(1)[0];
        if(result.version!=2 && result.version!=3) return {};
        const auto hash=take(32);std::copy(hash.begin(),hash.end(),result.hash.data);
        result.height=number();
        const auto root=take(32);result.root_after.assign(root.begin(),root.end());
        result.block_bytes=frame(max_block);
        if(result.block_bytes.size()<128) return {};
        // Header framing/hash only: full transaction parsing belongs to the
        // selected-chain validation path, not this transport capture.
        const auto header=BlockHeader::Deserialize(std::vector<uint8_t>(
            result.block_bytes.begin(),result.block_bytes.begin()+128));
        if(!header || header->GetHash()!=result.hash) return {};
        result.batch_proof=frame(max_proof);
        if(result.version==3) result.transition_proof=frame(max_transition);
        if(pos!=bytes.size() || (result.batch_proof.empty() && result.transition_proof.empty())) return {};
        return result;
    } catch (...) { return {}; }
}
} // namespace dinero
