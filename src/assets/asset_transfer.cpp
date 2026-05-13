/**
 * Phase 30: Taproot Asset Layer - Transfer Protocol Implementation
 */

#include "assets/asset_transfer.h"
#include "crypto/sha256.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <map>

namespace dinero {
namespace assets {

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

std::array<uint8_t, 32> sha256_single(const std::vector<uint8_t>& data) {
    std::array<uint8_t, 32> hash;
    crypto::CSHA256 ctx;
    ctx.Write(data.data(), data.size());
    ctx.Finalize(hash.data());
    return hash;
}

std::string bytesToHex(const uint8_t* data, size_t len) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; i++) {
        ss << std::setw(2) << static_cast<int>(data[i]);
    }
    return ss.str();
}

std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        bytes.push_back(static_cast<uint8_t>(std::stoi(byteString, nullptr, 16)));
    }
    return bytes;
}

void writeLE64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        value >>= 8;
    }
}

void writeLE32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        value >>= 8;
    }
}

uint64_t readLE64(const uint8_t* data) {
    uint64_t result = 0;
    for (int i = 7; i >= 0; i--) {
        result = (result << 8) | data[i];
    }
    return result;
}

uint32_t readLE32(const uint8_t* data) {
    uint32_t result = 0;
    for (int i = 3; i >= 0; i--) {
        result = (result << 8) | data[i];
    }
    return result;
}

// Compute CTV hash for a transaction
std::array<uint8_t, 32> computeCTVHash(
    const std::vector<uint8_t>& tx,
    uint32_t input_index) {

    // Simplified CTV hash computation
    // Real implementation would follow BIP-119
    std::vector<uint8_t> preimage;
    writeLE32(preimage, input_index);
    preimage.insert(preimage.end(), tx.begin(), tx.end());
    return sha256_single(preimage);
}

// Compute TXHASH for introspection
std::array<uint8_t, 32> computeTXHASH(
    const std::vector<uint8_t>& tx,
    uint32_t flags) {

    // Simplified TXHASH computation
    std::vector<uint8_t> preimage;
    writeLE32(preimage, flags);
    preimage.insert(preimage.end(), tx.begin(), tx.end());
    return sha256_single(preimage);
}

} // anonymous namespace

// ============================================================================
// TransferError String Conversion
// ============================================================================

std::string TransferErrorToString(TransferError err) {
    switch (err) {
        case TransferError::SUCCESS: return "SUCCESS";

        // Input errors
        case TransferError::INPUT_NOT_FOUND: return "INPUT_NOT_FOUND";
        case TransferError::INPUT_ALREADY_SPENT: return "INPUT_ALREADY_SPENT";
        case TransferError::INPUT_ASSET_MISMATCH: return "INPUT_ASSET_MISMATCH";
        case TransferError::INPUT_AMOUNT_MISMATCH: return "INPUT_AMOUNT_MISMATCH";
        case TransferError::INPUT_STATE_MISMATCH: return "INPUT_STATE_MISMATCH";

        // Output errors
        case TransferError::OUTPUT_ASSET_INVALID: return "OUTPUT_ASSET_INVALID";
        case TransferError::OUTPUT_AMOUNT_ZERO: return "OUTPUT_AMOUNT_ZERO";
        case TransferError::OUTPUT_SCRIPT_INVALID: return "OUTPUT_SCRIPT_INVALID";

        // Conservation errors
        case TransferError::CONSERVATION_VIOLATION: return "CONSERVATION_VIOLATION";
        case TransferError::ASSET_TYPE_MISMATCH: return "ASSET_TYPE_MISMATCH";
        case TransferError::INFLATION_DETECTED: return "INFLATION_DETECTED";

        // Authorization errors
        case TransferError::MINT_UNAUTHORIZED: return "MINT_UNAUTHORIZED";
        case TransferError::BURN_UNAUTHORIZED: return "BURN_UNAUTHORIZED";
        case TransferError::CONTRACT_UNAUTHORIZED: return "CONTRACT_UNAUTHORIZED";

        // Proof errors
        case TransferError::PROOF_INVALID: return "PROOF_INVALID";
        case TransferError::PROOF_MISSING: return "PROOF_MISSING";
        case TransferError::TAPROOT_INVALID: return "TAPROOT_INVALID";

        // Template errors
        case TransferError::CTV_MISMATCH: return "CTV_MISMATCH";
        case TransferError::TXHASH_MISMATCH: return "TXHASH_MISMATCH";

        default: return "UNKNOWN_ERROR";
    }
}

// ============================================================================
// AssetTransferRequest Implementation
// ============================================================================

TransferError AssetTransferRequest::validate() const {
    // Must have at least one source
    if (sources.empty()) {
        return TransferError::INPUT_NOT_FOUND;
    }

    // Must have at least one destination
    if (destinations.empty()) {
        return TransferError::OUTPUT_ASSET_INVALID;
    }

    // Check each source
    for (const auto& source : sources) {
        if (source.txid.empty()) {
            return TransferError::INPUT_NOT_FOUND;
        }
        if (source.amount == 0) {
            return TransferError::INPUT_AMOUNT_MISMATCH;
        }
    }

    // Check each destination
    for (const auto& dest : destinations) {
        if (dest.amount == 0) {
            return TransferError::OUTPUT_AMOUNT_ZERO;
        }
        if (dest.address.empty() && dest.script_pubkey.empty()) {
            return TransferError::OUTPUT_SCRIPT_INVALID;
        }
    }

    // Check coverage
    if (!checkCoverage()) {
        return TransferError::CONSERVATION_VIOLATION;
    }

    return TransferError::SUCCESS;
}

bool AssetTransferRequest::checkCoverage() const {
    // Sum sources by asset type
    std::map<AssetID, uint64_t> source_totals;
    for (const auto& source : sources) {
        source_totals[source.asset_id] += source.amount;
    }

    // Sum destinations by asset type
    std::map<AssetID, uint64_t> dest_totals;
    for (const auto& dest : destinations) {
        dest_totals[dest.asset_id] += dest.amount;
    }

    // Check each destination asset is covered
    for (const auto& [asset_id, dest_total] : dest_totals) {
        auto it = source_totals.find(asset_id);
        if (it == source_totals.end()) {
            return false; // Asset type not in sources
        }
        if (it->second < dest_total) {
            return false; // Not enough of this asset
        }
    }

    return true;
}

// ============================================================================
// AssetTransferBuilder Implementation
// ============================================================================

AssetTransferBuilder::AssetTransferBuilder() = default;

AssetTransferBuilder& AssetTransferBuilder::addInput(
    const std::string& txid,
    uint32_t vout,
    const AssetID& asset_id,
    uint64_t amount,
    const std::vector<uint8_t>& script_pubkey) {

    Input input;
    input.txid = txid;
    input.vout = vout;
    input.asset_id = asset_id;
    input.amount = amount;
    input.script_pubkey = script_pubkey;
    inputs_.push_back(input);

    return *this;
}

AssetTransferBuilder& AssetTransferBuilder::addOutput(
    const AssetID& asset_id,
    uint64_t amount,
    const std::string& address) {

    Output output;
    output.asset_id = asset_id;
    output.amount = amount;

    // Convert address to script_pubkey
    // This is simplified - real implementation would use address decoding
    if (address.length() >= 2) {
        output.script_pubkey.push_back(0x51); // OP_1 (witness v1)
        output.script_pubkey.push_back(0x20); // PUSH32
        // Placeholder for decoded address
        for (int i = 0; i < 32; i++) {
            output.script_pubkey.push_back(0x00);
        }
    }

    outputs_.push_back(output);
    return *this;
}

AssetTransferBuilder& AssetTransferBuilder::addOutputScript(
    const AssetID& asset_id,
    uint64_t amount,
    const std::vector<uint8_t>& script_pubkey) {

    Output output;
    output.asset_id = asset_id;
    output.amount = amount;
    output.script_pubkey = script_pubkey;
    outputs_.push_back(output);

    return *this;
}

AssetTransferBuilder& AssetTransferBuilder::setFeeRate(uint64_t sat_per_vb) {
    fee_rate_ = sat_per_vb;
    return *this;
}

AssetTransferBuilder& AssetTransferBuilder::setChangeAddress(const std::string& address) {
    change_address_ = address;
    return *this;
}

AssetTransferBuilder::BuildResult AssetTransferBuilder::build() {
    BuildResult result;
    result.success = false;

    // Validate inputs
    if (inputs_.empty()) {
        result.error = TransferError::INPUT_NOT_FOUND;
        return result;
    }

    if (outputs_.empty()) {
        result.error = TransferError::OUTPUT_ASSET_INVALID;
        return result;
    }

    // Check conservation
    std::map<AssetID, uint64_t> input_totals;
    for (const auto& input : inputs_) {
        input_totals[input.asset_id] += input.amount;
    }

    std::map<AssetID, uint64_t> output_totals;
    for (const auto& output : outputs_) {
        output_totals[output.asset_id] += output.amount;
    }

    // Calculate change for each asset
    std::map<AssetID, uint64_t> change_amounts;
    for (const auto& [asset_id, input_total] : input_totals) {
        auto it = output_totals.find(asset_id);
        uint64_t output_total = (it != output_totals.end()) ? it->second : 0;

        if (output_total > input_total) {
            result.error = TransferError::INFLATION_DETECTED;
            return result;
        }

        if (output_total < input_total) {
            change_amounts[asset_id] = input_total - output_total;
        }
    }

    // Build raw transaction
    std::vector<uint8_t> tx;

    // Version (2 for witness)
    writeLE32(tx, 2);

    // Marker and flag for witness
    tx.push_back(0x00);
    tx.push_back(0x01);

    // Input count
    tx.push_back(static_cast<uint8_t>(inputs_.size()));

    // Inputs
    for (const auto& input : inputs_) {
        // Previous output txid (reversed)
        auto txid_bytes = hexToBytes(input.txid);
        std::reverse(txid_bytes.begin(), txid_bytes.end());
        tx.insert(tx.end(), txid_bytes.begin(), txid_bytes.end());

        // Previous output index
        writeLE32(tx, input.vout);

        // ScriptSig (empty for witness)
        tx.push_back(0x00);

        // Sequence
        writeLE32(tx, 0xFFFFFFFF);
    }

    // Output count (regular outputs + change outputs)
    size_t total_outputs = outputs_.size() + change_amounts.size();
    tx.push_back(static_cast<uint8_t>(total_outputs));

    // Regular outputs
    for (const auto& output : outputs_) {
        // Value (we use amount as una for the carrier UTXO)
        writeLE64(tx, output.amount);

        // ScriptPubKey length and data
        tx.push_back(static_cast<uint8_t>(output.script_pubkey.size()));
        tx.insert(tx.end(), output.script_pubkey.begin(), output.script_pubkey.end());
    }

    // Change outputs
    for (const auto& [asset_id, change] : change_amounts) {
        if (!change_address_.empty() && change > 0) {
            writeLE64(tx, change);
            // Simplified change scriptPubKey
            tx.push_back(0x22); // Script length
            tx.push_back(0x51); // OP_1
            tx.push_back(0x20); // PUSH32
            for (int i = 0; i < 32; i++) {
                tx.push_back(0x00);
            }
            result.change_amount = change;
        }
    }

    // Witness data (placeholder)
    for (size_t i = 0; i < inputs_.size(); i++) {
        tx.push_back(0x01); // Witness stack count
        tx.push_back(0x00); // Empty witness element (to be filled by signing)
    }

    // Locktime
    writeLE32(tx, 0);

    // Compute CTV hash
    result.ctv_hash = computeCTVHash(tx, 0);

    // Calculate fee
    uint64_t vsize = tx.size() / 4 + tx.size(); // Rough vsize estimate
    result.total_fee = vsize * fee_rate_;

    result.raw_tx = tx;
    result.success = true;
    result.error = TransferError::SUCCESS;

    return result;
}

void AssetTransferBuilder::reset() {
    inputs_.clear();
    outputs_.clear();
    fee_rate_ = 1;
    change_address_.clear();
}

// ============================================================================
// AssetTransferValidator Implementation
// ============================================================================

TransferError AssetTransferValidator::validate(
    const AssetStateTransition& transition,
    const AssetTransferProof& proof,
    std::function<std::optional<AssetUTXO>(const std::string&, uint32_t)> utxo_lookup) {

    // Verify each input exists and matches
    for (const auto& input : transition.inputs) {
        auto utxo = utxo_lookup(input.txid, input.vout);
        if (!utxo) {
            return TransferError::INPUT_NOT_FOUND;
        }

        if (utxo->is_spent) {
            return TransferError::INPUT_ALREADY_SPENT;
        }

        if (utxo->asset_id != input.asset_id) {
            return TransferError::INPUT_ASSET_MISMATCH;
        }

        if (utxo->amount != input.amount) {
            return TransferError::INPUT_AMOUNT_MISMATCH;
        }

        if (utxo->state_hash != input.prev_state_hash) {
            return TransferError::INPUT_STATE_MISMATCH;
        }
    }

    // Verify conservation
    auto conservation_result = validateConservation(
        [&]() {
            std::vector<std::pair<AssetID, uint64_t>> inputs;
            for (const auto& input : transition.inputs) {
                inputs.emplace_back(input.asset_id, input.amount);
            }
            return inputs;
        }(),
        [&]() {
            std::vector<std::pair<AssetID, uint64_t>> outputs;
            for (const auto& output : transition.outputs) {
                outputs.emplace_back(output.asset_id, output.amount);
            }
            return outputs;
        }());

    if (conservation_result != TransferError::SUCCESS) {
        return conservation_result;
    }

    // Verify proof
    if (!proof.verify()) {
        return TransferError::PROOF_INVALID;
    }

    return TransferError::SUCCESS;
}

TransferError AssetTransferValidator::validateConservation(
    const std::vector<std::pair<AssetID, uint64_t>>& inputs,
    const std::vector<std::pair<AssetID, uint64_t>>& outputs) {

    // Sum by asset type
    std::map<AssetID, uint64_t> input_totals;
    for (const auto& [asset_id, amount] : inputs) {
        input_totals[asset_id] += amount;
    }

    std::map<AssetID, uint64_t> output_totals;
    for (const auto& [asset_id, amount] : outputs) {
        output_totals[asset_id] += amount;
    }

    // Check each output asset type
    for (const auto& [asset_id, output_total] : output_totals) {
        auto it = input_totals.find(asset_id);
        if (it == input_totals.end()) {
            return TransferError::ASSET_TYPE_MISMATCH;
        }

        if (output_total > it->second) {
            return TransferError::INFLATION_DETECTED;
        }
    }

    return TransferError::SUCCESS;
}

TransferError AssetTransferValidator::validateCTVTemplate(
    const std::array<uint8_t, 32>& expected_hash,
    const std::vector<uint8_t>& actual_tx,
    uint32_t input_index) {

    auto computed = computeCTVHash(actual_tx, input_index);

    if (computed != expected_hash) {
        return TransferError::CTV_MISMATCH;
    }

    return TransferError::SUCCESS;
}

TransferError AssetTransferValidator::validateTXHASH(
    const std::array<uint8_t, 32>& expected_hash,
    const std::vector<uint8_t>& tx,
    uint32_t flags) {

    auto computed = computeTXHASH(tx, flags);

    if (computed != expected_hash) {
        return TransferError::TXHASH_MISMATCH;
    }

    return TransferError::SUCCESS;
}

// ============================================================================
// AssetTransferSigner Implementation
// ============================================================================

std::vector<uint8_t> AssetTransferSigner::sign(
    const std::vector<uint8_t>& raw_tx,
    uint32_t input_index,
    const std::array<uint8_t, 32>& private_key,
    const std::vector<uint8_t>& asset_script) {

    // Create signature message
    std::vector<uint8_t> sighash_preimage;

    // Simplified sighash computation (real would be BIP-341)
    sighash_preimage.push_back(0x00); // Epoch
    sighash_preimage.push_back(0x00); // Sighash type (all)
    writeLE32(sighash_preimage, 2); // Version
    writeLE32(sighash_preimage, 0); // Locktime

    // Add transaction data
    sighash_preimage.insert(sighash_preimage.end(), raw_tx.begin(), raw_tx.end());

    // Compute sighash
    auto sighash = sha256_single(sighash_preimage);

    // Sign with private key (placeholder - real would use secp256k1)
    std::vector<uint8_t> signature(64);
    for (size_t i = 0; i < 32; i++) {
        signature[i] = sighash[i] ^ private_key[i];
        signature[i + 32] = sighash[i];
    }

    // Build witness
    std::vector<uint8_t> witness;

    // Signature
    witness.push_back(static_cast<uint8_t>(signature.size()));
    witness.insert(witness.end(), signature.begin(), signature.end());

    // Asset script
    if (!asset_script.empty()) {
        witness.push_back(static_cast<uint8_t>(asset_script.size()));
        witness.insert(witness.end(), asset_script.begin(), asset_script.end());
    }

    return witness;
}

std::vector<uint8_t> AssetTransferSigner::signAuthorization(
    const AssetID& asset_id,
    uint64_t amount,
    const std::array<uint8_t, 32>& authority_key) {

    // Create message for CSFS
    std::vector<uint8_t> message;
    message.insert(message.end(), asset_id.begin(), asset_id.end());
    writeLE64(message, amount);

    auto msg_hash = sha256_single(message);

    // Sign (placeholder)
    std::vector<uint8_t> signature(64);
    for (size_t i = 0; i < 32; i++) {
        signature[i] = msg_hash[i] ^ authority_key[i];
        signature[i + 32] = msg_hash[i];
    }

    return signature;
}

// ============================================================================
// Script Generation Functions
// ============================================================================

std::vector<uint8_t> GenerateAssetScript(
    const AssetID& asset_id,
    uint64_t amount,
    const std::array<uint8_t, 32>& state_hash) {

    std::vector<uint8_t> script;

    // OP_PUSH(asset_id) - 32 bytes
    script.push_back(0x20);
    script.insert(script.end(), asset_id.begin(), asset_id.end());

    // OP_PUSH(amount) - 8 bytes
    script.push_back(0x08);
    writeLE64(script, amount);

    // OP_PUSH(state_hash) - 32 bytes
    script.push_back(0x20);
    script.insert(script.end(), state_hash.begin(), state_hash.end());

    // OP_CHECKTEMPLATEVERIFY
    script.push_back(0xB3);

    return script;
}

std::vector<uint8_t> GenerateAssetTaprootOutput(
    const std::array<uint8_t, 32>& internal_key,
    const std::vector<uint8_t>& asset_script) {

    // Compute leaf hash
    std::vector<uint8_t> leaf_preimage;
    leaf_preimage.push_back(0xC0); // Leaf version
    leaf_preimage.push_back(static_cast<uint8_t>(asset_script.size()));
    leaf_preimage.insert(leaf_preimage.end(), asset_script.begin(), asset_script.end());
    auto leaf_hash = sha256_single(leaf_preimage);

    // Compute tweaked pubkey (simplified - real would use EC operations)
    std::array<uint8_t, 32> tweaked_key;
    for (size_t i = 0; i < 32; i++) {
        tweaked_key[i] = internal_key[i] ^ leaf_hash[i];
    }

    // Build P2TR scriptPubKey
    std::vector<uint8_t> script_pubkey;
    script_pubkey.push_back(0x51); // OP_1 (witness v1)
    script_pubkey.push_back(0x20); // PUSH32
    script_pubkey.insert(script_pubkey.end(), tweaked_key.begin(), tweaked_key.end());

    return script_pubkey;
}

std::optional<AssetCommitment> ParseAssetScript(const std::vector<uint8_t>& script) {
    // Expected format:
    // 0x20 [32 bytes asset_id] 0x08 [8 bytes amount] 0x20 [32 bytes state_hash] 0xB3

    if (script.size() < 76) return std::nullopt;

    size_t pos = 0;

    // Check asset_id push
    if (script[pos++] != 0x20) return std::nullopt;

    AssetCommitment commit;
    std::copy(script.begin() + pos, script.begin() + pos + 32, commit.asset_id.begin());
    pos += 32;

    // Check amount push
    if (script[pos++] != 0x08) return std::nullopt;

    commit.amount = readLE64(script.data() + pos);
    pos += 8;

    // Check state_hash push
    if (script[pos++] != 0x20) return std::nullopt;

    std::copy(script.begin() + pos, script.begin() + pos + 32, commit.state_hash.begin());
    pos += 32;

    // Check OP_CHECKTEMPLATEVERIFY
    if (script[pos] != 0xB3) return std::nullopt;

    return commit;
}

} // namespace assets
} // namespace dinero
