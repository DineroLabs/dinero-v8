#include "wallet/taproot_sighash.h"
#include "common/sha256d.h"
#include "crypto/tagged_hash.h"
#include <algorithm>
#include <stdexcept>

namespace din {

std::vector<uint8_t> TaprootSighash::computeSighash(
    const std::vector<uint8_t>& unsigned_tx,
    uint32_t input_index,
    const std::vector<uint8_t>& prevouts_hash,
    const std::vector<uint8_t>& amounts_hash,
    const std::vector<uint8_t>& script_pubkeys_hash,
    const std::vector<uint8_t>& tapleaf_hash,
    uint8_t key_version,
    uint32_t codesep_pos,
    SighashType sighash_type
) {
    // Serialize sighash data
    auto sighash_data = serializeSighashData(
        unsigned_tx, input_index, prevouts_hash, amounts_hash,
        script_pubkeys_hash, tapleaf_hash, key_version, codesep_pos, sighash_type
    );
    
    // Compute tagged hash
    return taggedHash("TapSighash", sighash_data);
}

std::vector<uint8_t> TaprootSighash::computeScriptPathSighash(
    const std::vector<uint8_t>& unsigned_tx,
    uint32_t input_index,
    const std::vector<uint8_t>& prevouts_hash,
    const std::vector<uint8_t>& amounts_hash,
    const std::vector<uint8_t>& script_pubkeys_hash,
    const std::vector<uint8_t>& tapleaf_hash,
    uint8_t key_version,
    uint32_t codesep_pos,
    SighashType sighash_type
) {
    // Script path sighash is the same as key path sighash
    return computeSighash(
        unsigned_tx, input_index, prevouts_hash, amounts_hash,
        script_pubkeys_hash, tapleaf_hash, key_version, codesep_pos, sighash_type
    );
}

std::vector<uint8_t> TaprootSighash::computePrevoutsHash(
    const std::vector<std::vector<uint8_t>>& prevouts
) {
    if (prevouts.empty()) {
        return std::vector<uint8_t>(32, 0);
    }
    
    // Concatenate all prevouts
    std::vector<uint8_t> data;
    for (const auto& prevout : prevouts) {
        data.insert(data.end(), prevout.begin(), prevout.end());
    }
    
    return taggedHash("TapPrevouts", data);
}

std::vector<uint8_t> TaprootSighash::computeAmountsHash(
    const std::vector<std::vector<uint8_t>>& amounts
) {
    if (amounts.empty()) {
        return std::vector<uint8_t>(32, 0);
    }
    
    // Concatenate all amounts
    std::vector<uint8_t> data;
    for (const auto& amount : amounts) {
        data.insert(data.end(), amount.begin(), amount.end());
    }
    
    return taggedHash("TapAmounts", data);
}

std::vector<uint8_t> TaprootSighash::computeScriptPubkeysHash(
    const std::vector<std::vector<uint8_t>>& script_pubkeys
) {
    if (script_pubkeys.empty()) {
        return std::vector<uint8_t>(32, 0);
    }
    
    // Concatenate all scriptPubKeys
    std::vector<uint8_t> data;
    for (const auto& script_pubkey : script_pubkeys) {
        data.insert(data.end(), script_pubkey.begin(), script_pubkey.end());
    }
    
    return taggedHash("TapScriptPubkeys", data);
}

std::vector<uint8_t> TaprootSighash::computeTapleafHash(
    const std::vector<uint8_t>& script,
    uint8_t leaf_version
) {
    // Create tapleaf data: leaf_version + script
    std::vector<uint8_t> tapleaf_data;
    tapleaf_data.push_back(leaf_version);
    tapleaf_data.insert(tapleaf_data.end(), script.begin(), script.end());
    
    return taggedHash("TapLeaf", tapleaf_data);
}

std::vector<uint8_t> TaprootSighash::computeScriptTreeMerkleRoot(
    const std::vector<std::vector<uint8_t>>& scripts,
    uint8_t leaf_version
) {
    if (scripts.empty()) {
        return std::vector<uint8_t>(32, 0);
    }
    
    if (scripts.size() == 1) {
        return computeTapleafHash(scripts[0], leaf_version);
    }
    
    // Compute tapleaf hashes for all scripts
    std::vector<std::vector<uint8_t>> leaf_hashes;
    for (const auto& script : scripts) {
        leaf_hashes.push_back(computeTapleafHash(script, leaf_version));
    }
    
    // Build Merkle tree
    std::vector<std::vector<uint8_t>> current_level = leaf_hashes;
    
    while (current_level.size() > 1) {
        std::vector<std::vector<uint8_t>> next_level;
        
        for (size_t i = 0; i < current_level.size(); i += 2) {
            if (i + 1 < current_level.size()) {
                // Hash two nodes together
                std::vector<uint8_t> combined;
                combined.insert(combined.end(), current_level[i].begin(), current_level[i].end());
                combined.insert(combined.end(), current_level[i + 1].begin(), current_level[i + 1].end());
                
                next_level.push_back(taggedHash("TapBranch", combined));
            } else {
                // Odd number of nodes, promote the last one
                next_level.push_back(current_level[i]);
            }
        }
        
        current_level = next_level;
    }
    
    return current_level[0];
}

std::vector<uint8_t> TaprootSighash::taggedHash(
    const std::string& tag,
    const std::vector<uint8_t>& data
) {
    // BIP340 TaggedHash: SHA256(SHA256(tag) || SHA256(tag) || data)
    // Delegates to canonical implementation in crypto/tagged_hash.h
    return dinero::crypto::TaggedHash(tag, data);
}

std::vector<uint8_t> TaprootSighash::computeTxHash(
    const std::vector<uint8_t>& unsigned_tx,
    SighashType sighash_type
) {
    // For Taproot, we use the full transaction hash
    // In a real implementation, we would modify the transaction based on sighash_type
    Dinero::Common::sha256 hasher;
    hasher.update(unsigned_tx.data(), unsigned_tx.size());
    return hasher.finalize();
}

std::vector<uint8_t> TaprootSighash::serializeSighashData(
    const std::vector<uint8_t>& unsigned_tx,
    uint32_t input_index,
    const std::vector<uint8_t>& prevouts_hash,
    const std::vector<uint8_t>& amounts_hash,
    const std::vector<uint8_t>& script_pubkeys_hash,
    const std::vector<uint8_t>& tapleaf_hash,
    uint8_t key_version,
    uint32_t codesep_pos,
    SighashType sighash_type
) {
    std::vector<uint8_t> data;
    
    // Add sighash type
    data.push_back(static_cast<uint8_t>(sighash_type));
    
    // Add input index (4 bytes, little-endian)
    data.push_back(input_index & 0xFF);
    data.push_back((input_index >> 8) & 0xFF);
    data.push_back((input_index >> 16) & 0xFF);
    data.push_back((input_index >> 24) & 0xFF);
    
    // Add prevouts hash
    data.insert(data.end(), prevouts_hash.begin(), prevouts_hash.end());
    
    // Add amounts hash
    data.insert(data.end(), amounts_hash.begin(), amounts_hash.end());
    
    // Add scriptPubKeys hash
    data.insert(data.end(), script_pubkeys_hash.begin(), script_pubkeys_hash.end());
    
    // Add tapleaf hash
    data.insert(data.end(), tapleaf_hash.begin(), tapleaf_hash.end());
    
    // Add key version
    data.push_back(key_version);
    
    // Add code separator position (4 bytes, little-endian)
    data.push_back(codesep_pos & 0xFF);
    data.push_back((codesep_pos >> 8) & 0xFF);
    data.push_back((codesep_pos >> 16) & 0xFF);
    data.push_back((codesep_pos >> 24) & 0xFF);
    
    // Add transaction hash
    auto tx_hash = computeTxHash(unsigned_tx, sighash_type);
    data.insert(data.end(), tx_hash.begin(), tx_hash.end());
    
    return data;
}

} // namespace din
