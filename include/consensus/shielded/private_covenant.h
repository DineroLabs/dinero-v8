#pragma once
#include "consensus/shielded/shielded_tx.h"
#include "crypto/sha256.h"
#include <algorithm>
#include <stdexcept>
#include <optional>

namespace dinero::consensus::shielded {
inline constexpr uint8_t kPrivateCovenantProofVersion = 0x07;
inline std::optional<uint32_t> PrivateCovenantProofHeight(const std::vector<uint8_t>& proof) {
    if (proof.size() <= 5 || proof[0] != kPrivateCovenantProofVersion) return std::nullopt;
    uint32_t height = 0;
    for (unsigned i = 0; i < 4; ++i) height |= uint32_t(proof[1 + i]) << (8 * i);
    return height;
}
// Experimental proof profile. Transaction validation is wired, but its gate
// remains dormant until wallet recovery and full lifecycle qualification.
inline Hash PrivateCovenantTag() {
    Hash tag{};
    constexpr char domain[] = "DIN/shielded/covenant/v1";
    std::copy(domain, domain + sizeof(domain) - 1, tag.begin());
    return tag;
}
inline Hash CovenantHeightScalar(uint32_t height) {
    Hash out{};
    for (unsigned i = 0; i < 4; ++i) out[31 - i] = static_cast<uint8_t>(height >> (8 * i));
    return out;
}
// Count and order are committed. Initial profile supports the existing
// shielded bundle limit of two outputs; it is not arbitrary script execution.
inline Hash PrivateCovenantOutputRoot(const std::vector<ShieldedOutput>& outputs) {
    if (outputs.empty() || outputs.size() > 2) throw std::invalid_argument("covenant output count");
    Hash root = PoseidonHash2(PrivateCovenantTag(), CovenantHeightScalar(outputs.size()));
    for (const auto& output : outputs) {
        Hash encrypted_hash{};
        dinero::crypto::CSHA256().Write(output.encrypted_note.data(), output.encrypted_note.size()).Finalize(encrypted_hash.data());
        // Commit ciphertext too: otherwise the spender can preserve the note
        // commitment but destroy the recipient's ability to discover/spend it.
        root = PoseidonHash2(root, PoseidonHash2(output.commitment, encrypted_hash));
    }
    return root;
}
inline Hash PrivateCovenantOwnershipKey(const Hash& ownership, const Hash& outputs, uint32_t minimum_height) {
    const auto policy = PoseidonHash2(PrivateCovenantTag(),
                                    PoseidonHash2(CovenantHeightScalar(minimum_height), outputs));
    return PoseidonHash2(ownership, policy);
}
}
