/**
 * Shielded pool block commitment — coinbase OP_RETURN encoding.
 * See include/consensus/shielded/shielded_commitment.h.
 */

#include "consensus/shielded/shielded_commitment.h"

#include <cstring>

namespace dinero::consensus::shielded {

std::vector<uint8_t> BuildShieldedCommitmentScript(const Hash& tree_root,
                                                    uint64_t nullifier_count) {
    std::vector<uint8_t> script;
    script.reserve(2 + SHIELDED_COMMITMENT_SIZE);

    script.push_back(0x6a);  // OP_RETURN
    script.push_back(static_cast<uint8_t>(SHIELDED_COMMITMENT_SIZE));  // PUSH 44 bytes

    // Magic
    script.insert(script.end(),
                  SHIELDED_COMMITMENT_MAGIC.begin(),
                  SHIELDED_COMMITMENT_MAGIC.end());

    // Tree root (32 bytes)
    script.insert(script.end(), tree_root.begin(), tree_root.end());

    // Nullifier count (8 bytes, little-endian)
    for (int i = 0; i < 8; ++i) {
        script.push_back(static_cast<uint8_t>((nullifier_count >> (8 * i)) & 0xFF));
    }

    return script;
}

std::optional<ShieldedBlockCommitment>
ExtractShieldedCommitment(const std::vector<std::vector<uint8_t>>& coinbase_outputs) {
    for (const auto& spk : coinbase_outputs) {
        // OP_RETURN (0x6a) + PUSH(44) + 44 bytes payload
        if (spk.size() < 2 + SHIELDED_COMMITMENT_SIZE) continue;
        if (spk[0] != 0x6a) continue;
        if (spk[1] != SHIELDED_COMMITMENT_SIZE) continue;

        // Check magic
        if (std::memcmp(spk.data() + 2,
                        SHIELDED_COMMITMENT_MAGIC.data(),
                        SHIELDED_COMMITMENT_MAGIC.size()) != 0) {
            continue;
        }

        ShieldedBlockCommitment out{};
        std::memcpy(out.tree_root.data(), spk.data() + 6, HASH_BYTES);

        out.nullifier_count = 0;
        for (int i = 0; i < 8; ++i) {
            out.nullifier_count |=
                static_cast<uint64_t>(spk[6 + HASH_BYTES + i]) << (8 * i);
        }

        return out;
    }

    return std::nullopt;
}

bool VerifyShieldedCommitment(const ShieldedBlockCommitment& commitment,
                              const CommitmentTree& tree,
                              uint64_t nullifier_count) {
    return commitment.tree_root == tree.Root()
        && commitment.nullifier_count == nullifier_count;
}

} // namespace dinero::consensus::shielded
