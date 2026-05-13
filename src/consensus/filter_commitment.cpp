/**
 * BIP158 Filter Commitment Implementation
 *
 * Coinbase OP_RETURN commitment to the block's GCS filter hash.
 * See include/consensus/filter_commitment.h for specification.
 */

#include "consensus/filter_commitment.h"
#include <cstring>

namespace dinero::consensus {

std::vector<uint8_t> BuildFilterCommitmentScript(const uint256& filter_hash) {
    if (filter_hash.IsNull()) {
        return {};
    }

    std::vector<uint8_t> script;
    script.reserve(1 + 1 + FilterCommitment::SIZE);

    // OP_RETURN (0x6a)
    script.push_back(0x6a);

    // Push data size (37 bytes)
    script.push_back(static_cast<uint8_t>(FilterCommitment::SIZE));

    // DNRF magic: 0x44 0x4E 0x52 0x46
    script.push_back(0x44);  // D
    script.push_back(0x4E);  // N
    script.push_back(0x52);  // R
    script.push_back(0x46);  // F

    // Version byte
    script.push_back(FilterCommitment::VERSION);

    // Filter hash (32 bytes)
    script.insert(script.end(), filter_hash.data, filter_hash.data + 32);

    return script;
}

std::optional<size_t> FindFilterCommitmentIndex(const Transaction& coinbase) {
    // Must be a coinbase transaction
    if (coinbase.vin.size() != 1 || !coinbase.vin[0].prevout.txid.AsUint256().IsNull()) {
        return std::nullopt;
    }

    // Search outputs backwards (last matching output wins)
    for (size_t i = coinbase.vout.size(); i-- > 0; ) {
        const auto& script = coinbase.vout[i].scriptPubKey;

        // Check: OP_RETURN + correct size + DNRF magic + version
        if (script.size() < 39) continue;           // 1 (OP_RETURN) + 1 (size) + 37 (data)
        if (script[0] != 0x6a) continue;             // OP_RETURN
        if (script[1] != FilterCommitment::SIZE) continue;  // 37 bytes

        // Check DNRF magic
        if (script[2] == 0x44 && script[3] == 0x4E &&
            script[4] == 0x52 && script[5] == 0x46) {
            if (script[6] == FilterCommitment::VERSION) {
                return i;
            }
        }
    }

    return std::nullopt;
}

std::optional<uint256> ExtractFilterCommitment(
    const Transaction& coinbase,
    size_t index
) {
    if (index >= coinbase.vout.size()) {
        return std::nullopt;
    }

    const auto& script = coinbase.vout[index].scriptPubKey;

    if (script.size() < 39) return std::nullopt;
    if (script[0] != 0x6a) return std::nullopt;
    if (script[1] != FilterCommitment::SIZE) return std::nullopt;
    if (script[2] != 0x44 || script[3] != 0x4E ||
        script[4] != 0x52 || script[5] != 0x46) {
        return std::nullopt;
    }
    if (script[6] != FilterCommitment::VERSION) {
        return std::nullopt;
    }

    uint256 filter_hash;
    std::memcpy(filter_hash.data, &script[7], 32);
    return filter_hash;
}

bool ValidateFilterCommitment(
    const Transaction& coinbase,
    const uint256& filter_hash,
    uint64_t height,
    std::string& error
) {
    // Find filter commitment in coinbase
    auto index_opt = FindFilterCommitmentIndex(coinbase);

    // No commitment found
    if (!index_opt.has_value()) {
        if (RequiresFilterCommitment(height)) {
            error = "Missing DNRF filter commitment (mandatory at height " +
                    std::to_string(height) + ", activation=" +
                    std::to_string(FilterCommitment::ACTIVATION_HEIGHT) + ")";
            return false;
        }
        return true;  // Pre-activation: optional
    }

    // Extract committed hash
    auto committed_hash_opt = ExtractFilterCommitment(coinbase, index_opt.value());
    if (!committed_hash_opt.has_value()) {
        error = "Invalid filter commitment format";
        return false;
    }

    // Compare committed hash to actual filter hash
    if (committed_hash_opt.value() != filter_hash) {
        error = "Filter commitment mismatch: committed " +
                committed_hash_opt.value().GetHex().substr(0, 16) +
                "... but computed " + filter_hash.GetHex().substr(0, 16) + "...";
        return false;
    }

    return true;
}

} // namespace dinero::consensus
