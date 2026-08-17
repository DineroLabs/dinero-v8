/**
 * Phase 11c.1: Witness Commitment Implementation
 *
 * BIP 141-style witness commitments for segwit compatibility.
 * See include/consensus/witness_commitment.h for specification.
 */

#include "consensus/witness_commitment.h"
#include "consensus/merkle_root.h"
#include "common/sha256d.h"
#include <algorithm>
#include <cstring>

namespace dinero::consensus {

// Default witness nonce: 32 zero bytes (BIP 141 standard)
const std::vector<uint8_t> WitnessCommitment::DEFAULT_NONCE = std::vector<uint8_t>(32, 0x00);

std::vector<uint8_t> BuildWitnessCommitment(
    const std::vector<Transaction>& vtx,
    const std::vector<uint8_t>& witness_nonce
) {
    // Validate inputs
    if (vtx.empty()) {
        return {};  // Empty block - no commitment
    }

    if (witness_nonce.size() != 32) {
        return {};  // Invalid nonce size
    }

    // Step 1: Compute witness merkle root
    uint256 witness_merkle_root = ComputeWitnessMerkleRoot(vtx);

    // Step 2: Concatenate witness_merkle_root || witness_nonce
    std::vector<uint8_t> preimage;
    preimage.reserve(64);

    // Append witness merkle root (32 bytes, internal format)
    preimage.insert(preimage.end(), witness_merkle_root.data, witness_merkle_root.data + 32);

    // Append witness nonce (32 bytes)
    preimage.insert(preimage.end(), witness_nonce.begin(), witness_nonce.end());

    // Step 3: Compute commitment = Double-SHA256(preimage)
    // BIP-141 specifies: commitment = SHA256(SHA256(witness_root || witness_nonce))
    auto commitment_bytes = Dinero::Common::double_sha256_raw(preimage.data(), preimage.size());

    // Step 4: Build script: OP_RETURN <DINW magic> <version> <commitment>
    std::vector<uint8_t> script;
    script.reserve(1 + 1 + WitnessCommitment::SIZE);

    // OP_RETURN (0x6a)
    script.push_back(0x6a);

    // Push data size (37 bytes = 4 byte magic + 1 byte version + 32 byte hash)
    script.push_back(WitnessCommitment::SIZE);

    // Dinero witness magic: DINW (0x444E5257) - little-endian
    // 0x44 = 'D', 0x4E = 'N', 0x52 = 'R', 0x57 = 'W'
    script.push_back(0x44);  // D
    script.push_back(0x4E);  // N
    script.push_back(0x52);  // R
    script.push_back(0x57);  // W

    // DINW commitment format version
    script.push_back(WitnessCommitment::VERSION);

    // Commitment hash (32 bytes)
    script.insert(script.end(), commitment_bytes.begin(), commitment_bytes.end());

    return script;
}

std::optional<size_t> FindWitnessCommitmentIndex(const Transaction& coinbase) {
    // Witness commitment must be in coinbase transaction
    if (coinbase.vin.size() != 1 || !coinbase.vin[0].prevout.txid.AsUint256().IsNull()) {
        return std::nullopt;  // Not a coinbase
    }

    // Search outputs for witness commitment
    // BIP 141 pattern: Search backwards (last matching output wins)
    for (size_t i = coinbase.vout.size(); i-- > 0; ) {
        const auto& script = coinbase.vout[i].scriptPubKey;

        // Check for OP_RETURN with correct size
        if (script.size() < 39) continue;  // Minimum: 1 (OP_RETURN) + 1 (size) + 37 (data)
        if (script[0] != 0x6a) continue;   // Must be OP_RETURN
        if (script[1] != WitnessCommitment::SIZE) continue;  // Must be 37 bytes

        // Check Dinero witness magic: DINW (0x444E5257)
        if (script[2] == 0x44 && script[3] == 0x4E &&  // DN
            script[4] == 0x52 && script[5] == 0x57) {  // RW

            // Check version (0x01 for Phase 11c)
            if (script[6] == WitnessCommitment::VERSION) {
                return i;  // Found Dinero witness commitment
            }
        }
    }

    return std::nullopt;  // No commitment found
}

std::optional<uint256> ExtractWitnessCommitment(
    const Transaction& coinbase,
    size_t index
) {
    // Validate index
    if (index >= coinbase.vout.size()) {
        return std::nullopt;
    }

    const auto& script = coinbase.vout[index].scriptPubKey;

    // Validate script format
    if (script.size() < 39) return std::nullopt;
    if (script[0] != 0x6a) return std::nullopt;  // OP_RETURN
    if (script[1] != WitnessCommitment::SIZE) return std::nullopt;  // 37 bytes

    // Validate Dinero witness magic: DINW
    if (script[2] != 0x44 || script[3] != 0x4E ||
        script[4] != 0x52 || script[5] != 0x57) {
        return std::nullopt;
    }

    // Validate version
    if (script[6] != WitnessCommitment::VERSION) {
        return std::nullopt;
    }

    // Extract commitment hash (32 bytes after magic + version)
    uint256 commitment;
    std::memcpy(commitment.data, &script[7], 32);

    return commitment;
}

bool ValidateWitnessCommitment(
    const std::vector<Transaction>& vtx,
    std::string& error
) {
    // Must have at least coinbase
    if (vtx.empty()) {
        error = "Block has no transactions";
        return false;
    }

    const Transaction& coinbase = vtx[0];

    // Step 1: Find witness commitment
    auto index_opt = FindWitnessCommitmentIndex(coinbase);

    // Step 2: If no recognized DINW v1 commitment is found, validation passes.
    if (!index_opt.has_value()) {
        // This helper validates recognized commitments. The activation-aware
        // caller decides whether absence is allowed at this height.
        return true;
    }

    size_t index = index_opt.value();

    // Step 3: Extract commitment hash
    auto extracted_commitment_opt = ExtractWitnessCommitment(coinbase, index);
    if (!extracted_commitment_opt.has_value()) {
        error = "Invalid witness commitment format";
        return false;
    }

    uint256 extracted_commitment = extracted_commitment_opt.value();

    // Step 4: Compute expected witness merkle root
    // CVE-2012-2459 (witness tree): a duplicated transaction forges the witness
    // merkle root / commitment of another valid block. A valid block cannot
    // contain a duplicated transaction (double-spend), so this never rejects a
    // valid block.
    bool witness_mutated = false;
    uint256 witness_merkle_root = ComputeWitnessMerkleRoot(vtx, &witness_mutated);
    if (witness_mutated) {
        error = "bad-witness-duplicate: duplicated transaction in witness merkle tree (CVE-2012-2459)";
        return false;
    }

    // Step 5: Compute expected commitment
    // Commitment = SHA256(witness_merkle_root || witness_nonce)
    // Use default nonce (32 zero bytes) for now
    std::vector<uint8_t> preimage;
    preimage.reserve(64);
    preimage.insert(preimage.end(), witness_merkle_root.data, witness_merkle_root.data + 32);
    preimage.insert(preimage.end(), WitnessCommitment::DEFAULT_NONCE.begin(),
                                     WitnessCommitment::DEFAULT_NONCE.end());

    auto expected_commitment_bytes = Dinero::Common::double_sha256_raw(preimage.data(), preimage.size());
    uint256 expected_commitment;
    std::memcpy(expected_commitment.data, expected_commitment_bytes.data(), 32);

    // Step 6: Compare
    if (extracted_commitment != expected_commitment) {
        error = "Witness commitment mismatch: computed " + expected_commitment.GetHex().substr(0, 16) +
                "... but block has " + extracted_commitment.GetHex().substr(0, 16) + "...";
        return false;
    }

    return true;  // Commitment is valid
}

bool EnforceWitnessCommitment(
    const std::vector<Transaction>& vtx,
    uint32_t height,
    bool enforce_commitment,
    uint32_t enforcement_height,
    std::string& error
) {
    // A recognized DINW v1 commitment has always been consensus-validated,
    // including before mandatory enforcement. Keep that historical behavior
    // independent of the activation switch.
    std::string validation_error;
    if (!ValidateWitnessCommitment(vtx, validation_error)) {
        error = "bad-witness-commitment: " + validation_error;
        return false;
    }

    if (!enforce_commitment || height < enforcement_height) {
        return true;
    }

    // Production serialization uses witness_version != 0xFF as the witness
    // marker. Do not substitute a non-empty-stack test: that would disagree
    // with Transaction::HasWitness() and change the deployed block rule.
    const bool has_witness = std::any_of(
        vtx.begin(), vtx.end(), [](const Transaction& tx) {
            return tx.HasWitness();
        });
    if (!has_witness) {
        return true;
    }

    if (!FindWitnessCommitmentIndex(vtx[0]).has_value()) {
        error = "missing-witness-commitment (required at height " +
                std::to_string(height) + ", mandatory since height " +
                std::to_string(enforcement_height) + ")";
        return false;
    }

    return true;
}

uint32_t TranslateWitnessMagic(
    uint32_t magic,
    uint8_t version,
    uint32_t height,
    bool enable_translation,
    uint32_t translation_height
) {
    // ═════════════════════════════════════════════════════════════════════════
    // Phase 11e.1: Bitcoin Magic Translation (OFF by default)
    // ═════════════════════════════════════════════════════════════════════════
    // This is INTERPRETATION, not mutation - no blocks are changed.
    // When enabled, validation interprets DINW as Bitcoin magic.
    // Mining still creates DINW (this only affects parsing).
    // ═══════════════════════════════════════════════════════════════════════

    // Step 1: Check if translation is enabled
    if (!enable_translation) {
        return magic;  // Translation disabled - return original magic
    }

    // Step 2: Check if we've reached translation height
    if (height < translation_height) {
        return magic;  // Before translation height - return original magic
    }

    // Step 3: Check if this is a DINW commitment with version 0x01
    if (magic == WitnessCommitment::MAGIC && version == WitnessCommitment::VERSION) {
        // Translate DINW → Bitcoin magic (interpretation only)
        return WitnessCommitment::BITCOIN_MAGIC;
    }

    // Step 4: Not a DINW commitment - return original magic unchanged
    return magic;
}

} // namespace dinero::consensus
