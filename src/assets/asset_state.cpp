/**
 * Phase 30: Taproot Asset Layer - Asset State Implementation
 */

#include "assets/asset_state.h"
#include "crypto/sha256.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace dinero {
namespace assets {

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

std::array<uint8_t, 32> sha256(const std::vector<uint8_t>& data) {
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

void writeVarString(std::vector<uint8_t>& out, const std::string& str) {
    size_t len = str.size();
    if (len < 0xFD) {
        out.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xFFFF) {
        out.push_back(0xFD);
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    } else {
        out.push_back(0xFE);
        writeLE32(out, static_cast<uint32_t>(len));
    }
    out.insert(out.end(), str.begin(), str.end());
}

std::string readVarString(const uint8_t*& data, size_t& remaining) {
    if (remaining < 1) return "";

    size_t len = data[0];
    data++; remaining--;

    if (len == 0xFD) {
        if (remaining < 2) return "";
        len = data[0] | (static_cast<size_t>(data[1]) << 8);
        data += 2; remaining -= 2;
    } else if (len == 0xFE) {
        if (remaining < 4) return "";
        len = readLE32(data);
        data += 4; remaining -= 4;
    }

    if (remaining < len) return "";
    std::string result(reinterpret_cast<const char*>(data), len);
    data += len; remaining -= len;
    return result;
}

void writeVarBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& bytes) {
    size_t len = bytes.size();
    if (len < 0xFD) {
        out.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xFFFF) {
        out.push_back(0xFD);
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    } else {
        out.push_back(0xFE);
        writeLE32(out, static_cast<uint32_t>(len));
    }
    out.insert(out.end(), bytes.begin(), bytes.end());
}

std::vector<uint8_t> readVarBytes(const uint8_t*& data, size_t& remaining) {
    if (remaining < 1) return {};

    size_t len = data[0];
    data++; remaining--;

    if (len == 0xFD) {
        if (remaining < 2) return {};
        len = data[0] | (static_cast<size_t>(data[1]) << 8);
        data += 2; remaining -= 2;
    } else if (len == 0xFE) {
        if (remaining < 4) return {};
        len = readLE32(data);
        data += 4; remaining -= 4;
    }

    if (remaining < len) return {};
    std::vector<uint8_t> result(data, data + len);
    data += len; remaining -= len;
    return result;
}

} // anonymous namespace

// ============================================================================
// AssetUTXO Implementation
// ============================================================================

std::string AssetUTXO::outpoint() const {
    return txid + ":" + std::to_string(vout);
}

std::vector<uint8_t> AssetUTXO::serialize() const {
    std::vector<uint8_t> data;

    // Txid (32 bytes from hex)
    auto txid_bytes = hexToBytes(txid);
    data.insert(data.end(), txid_bytes.begin(), txid_bytes.end());

    // Vout
    writeLE32(data, vout);

    // Asset ID
    data.insert(data.end(), asset_id.begin(), asset_id.end());

    // Amount
    writeLE64(data, amount);

    // State hash
    data.insert(data.end(), state_hash.begin(), state_hash.end());

    // Script pubkey
    writeVarBytes(data, script_pubkey);

    // Owner address
    writeVarString(data, owner_address);

    // Block info
    writeLE32(data, height);
    writeLE64(data, timestamp);

    // Spending info
    data.push_back(is_spent ? 1 : 0);
    writeVarString(data, spending_txid);
    writeLE32(data, spending_input_index);

    return data;
}

std::optional<AssetUTXO> AssetUTXO::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 32 + 4 + 32 + 8 + 32) return std::nullopt;

    const uint8_t* ptr = data.data();
    size_t remaining = data.size();

    AssetUTXO utxo;

    // Txid
    utxo.txid = bytesToHex(ptr, 32);
    ptr += 32; remaining -= 32;

    // Vout
    utxo.vout = readLE32(ptr);
    ptr += 4; remaining -= 4;

    // Asset ID
    std::copy(ptr, ptr + 32, utxo.asset_id.begin());
    ptr += 32; remaining -= 32;

    // Amount
    utxo.amount = readLE64(ptr);
    ptr += 8; remaining -= 8;

    // State hash
    std::copy(ptr, ptr + 32, utxo.state_hash.begin());
    ptr += 32; remaining -= 32;

    // Script pubkey
    utxo.script_pubkey = readVarBytes(ptr, remaining);

    // Owner address
    utxo.owner_address = readVarString(ptr, remaining);

    // Block info
    if (remaining < 12) return std::nullopt;
    utxo.height = readLE32(ptr);
    ptr += 4; remaining -= 4;

    utxo.timestamp = readLE64(ptr);
    ptr += 8; remaining -= 8;

    // Spending info
    if (remaining < 1) return std::nullopt;
    utxo.is_spent = (*ptr++ != 0);
    remaining--;

    utxo.spending_txid = readVarString(ptr, remaining);

    if (remaining < 4) return std::nullopt;
    utxo.spending_input_index = readLE32(ptr);

    return utxo;
}

// ============================================================================
// AssetStateTransition Implementation
// ============================================================================

bool AssetStateTransition::validate() const {
    // Type-specific validation
    switch (type) {
        case TransitionType::TRANSFER:
            // Transfers must have matching input/output asset types
            return checkConservation();

        case TransitionType::MINT:
            // Mints need authorization (checked elsewhere)
            return !outputs.empty();

        case TransitionType::BURN:
            // Burns must have inputs
            return !inputs.empty();

        case TransitionType::CONTRACT_CALL:
            // Contract calls need both inputs and outputs
            return !inputs.empty() && !outputs.empty();

        case TransitionType::SPLIT:
            // Split needs one input, multiple outputs
            return inputs.size() == 1 && outputs.size() > 1;

        case TransitionType::MERGE:
            // Merge needs multiple inputs, one output
            return inputs.size() > 1 && outputs.size() == 1;
    }

    return false;
}

bool AssetStateTransition::checkConservation() const {
    // Group by asset type
    std::map<AssetID, uint64_t> input_totals;
    std::map<AssetID, uint64_t> output_totals;

    for (const auto& input : inputs) {
        input_totals[input.asset_id] += input.amount;
    }

    for (const auto& output : outputs) {
        output_totals[output.asset_id] += output.amount;
    }

    // Check conservation for each asset type
    for (const auto& [asset_id, input_total] : input_totals) {
        auto it = output_totals.find(asset_id);
        uint64_t output_total = (it != output_totals.end()) ? it->second : 0;

        // Output cannot exceed input (no inflation)
        if (output_total > input_total) {
            return false;
        }
    }

    // Check that no new asset types appear in outputs
    for (const auto& [asset_id, output_total] : output_totals) {
        if (input_totals.find(asset_id) == input_totals.end()) {
            // New asset type in outputs - only allowed for MINT
            if (type != TransitionType::MINT) {
                return false;
            }
        }
    }

    return true;
}

std::array<uint8_t, 32> AssetStateTransition::computeHash() const {
    std::vector<uint8_t> data;

    // Type
    data.push_back(static_cast<uint8_t>(type));

    // Inputs
    data.push_back(static_cast<uint8_t>(inputs.size()));
    for (const auto& input : inputs) {
        auto txid_bytes = hexToBytes(input.txid);
        data.insert(data.end(), txid_bytes.begin(), txid_bytes.end());
        writeLE32(data, input.vout);
        data.insert(data.end(), input.asset_id.begin(), input.asset_id.end());
        writeLE64(data, input.amount);
        data.insert(data.end(), input.prev_state_hash.begin(), input.prev_state_hash.end());
    }

    // Outputs
    data.push_back(static_cast<uint8_t>(outputs.size()));
    for (const auto& output : outputs) {
        data.insert(data.end(), output.asset_id.begin(), output.asset_id.end());
        writeLE64(data, output.amount);
        data.insert(data.end(), output.new_state_hash.begin(), output.new_state_hash.end());
        writeVarBytes(data, output.script_pubkey);
    }

    return sha256(data);
}

// ============================================================================
// AssetStateMachine Implementation
// ============================================================================

std::array<uint8_t, 32> AssetStateMachine::computeNewState(
    const std::vector<uint8_t>& input_data) const {

    std::vector<uint8_t> preimage;

    // Current state
    preimage.insert(preimage.end(), current_state.begin(), current_state.end());

    // Transition count
    writeLE64(preimage, transition_count + 1);

    // Input data hash
    auto input_hash = sha256(input_data);
    preimage.insert(preimage.end(), input_hash.begin(), input_hash.end());

    return sha256(preimage);
}

bool AssetStateMachine::verifyTransition(
    const std::array<uint8_t, 32>& old_state,
    const std::array<uint8_t, 32>& new_state,
    const std::vector<uint8_t>& proof) const {

    // Verify old state matches current
    if (old_state != current_state) {
        return false;
    }

    // Compute expected new state from proof
    auto expected_new = computeNewState(proof);

    return new_state == expected_new;
}

// ============================================================================
// AssetCoinSelection Implementation
// ============================================================================

AssetCoinSelection SelectAssetCoins(
    const std::vector<AssetUTXO>& available,
    const AssetID& asset_id,
    uint64_t target_amount) {

    AssetCoinSelection result;
    result.target_amount = target_amount;
    result.total_selected = 0;
    result.change_amount = 0;
    result.fee_rate = 1;

    // Filter by asset type and unspent
    std::vector<const AssetUTXO*> candidates;
    for (const auto& utxo : available) {
        if (utxo.asset_id == asset_id && !utxo.is_spent) {
            candidates.push_back(&utxo);
        }
    }

    // Sort by amount descending for largest-first selection
    std::sort(candidates.begin(), candidates.end(),
        [](const AssetUTXO* a, const AssetUTXO* b) {
            return a->amount > b->amount;
        });

    // First pass: try to find exact match
    for (const auto* utxo : candidates) {
        if (utxo->amount == target_amount) {
            result.selected_utxos.push_back(*utxo);
            result.total_selected = target_amount;
            return result;
        }
    }

    // Second pass: find smallest single UTXO >= target
    // Since sorted descending, iterate in reverse to find smallest sufficient UTXO
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        if ((*it)->amount >= target_amount) {
            result.selected_utxos.push_back(**it);
            result.total_selected = (*it)->amount;
            result.change_amount = (*it)->amount - target_amount;
            return result;
        }
    }

    // Third pass: accumulate until target met
    result.selected_utxos.clear();
    result.total_selected = 0;

    for (const auto* utxo : candidates) {
        result.selected_utxos.push_back(*utxo);
        result.total_selected += utxo->amount;

        if (result.total_selected >= target_amount) {
            result.change_amount = result.total_selected - target_amount;
            break;
        }
    }

    // Estimate fee (rough estimate: 68 bytes per input, 34 per output, 10 overhead)
    size_t estimated_size = 10 + (result.selected_utxos.size() * 68) + 34;
    if (result.change_amount > 0) {
        estimated_size += 34; // Change output
    }
    result.estimated_fee = estimated_size * result.fee_rate;

    return result;
}

// ============================================================================
// MultiAssetTransaction Implementation
// ============================================================================

std::vector<uint8_t> MultiAssetTransaction::buildRawTx() const {
    std::vector<uint8_t> tx;

    // Version (2)
    writeLE32(tx, 2);

    // Placeholder for inputs/outputs
    // This would be filled in with actual transaction building logic
    // that interacts with the covenant system

    // Number of movements as inputs (simplified)
    tx.push_back(static_cast<uint8_t>(movements.size()));

    for (const auto& mov : movements) {
        tx.insert(tx.end(), mov.asset_id.begin(), mov.asset_id.end());
        writeLE64(tx, mov.amount);
        writeVarString(tx, mov.from_address);
        writeVarString(tx, mov.to_address);
    }

    // Fee
    writeLE64(tx, fee_amount);

    // Locktime
    writeLE32(tx, 0);

    return tx;
}

uint64_t MultiAssetTransaction::estimateVSize() const {
    // Base overhead
    uint64_t vsize = 10;

    // Each movement needs ~150 vbytes (input + output)
    vsize += movements.size() * 150;

    // Fee output
    vsize += 34;

    return vsize;
}

} // namespace assets
} // namespace dinero
