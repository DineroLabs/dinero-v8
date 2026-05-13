#include "transaction_builder.h"
#include "crypto.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cstring>

namespace dinero {
namespace wallet {
namespace reference {

namespace {
    // Helper: Convert bytes to hex string
    std::string ToHex(const std::vector<uint8_t>& data) {
        std::ostringstream oss;
        for (uint8_t byte : data) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
        }
        return oss.str();
    }

    // Helper: Convert hex string to bytes
    std::vector<uint8_t> FromHex(const std::string& hex) {
        if (hex.length() % 2 != 0) {
            throw std::invalid_argument("Hex string must have even length");
        }

        std::vector<uint8_t> result;
        for (size_t i = 0; i < hex.length(); i += 2) {
            std::string byte_str = hex.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
            result.push_back(byte);
        }
        return result;
    }

    // Helper: Serialize uint32_t as little-endian
    void WriteUint32LE(std::vector<uint8_t>& data, uint32_t value) {
        data.push_back(value & 0xff);
        data.push_back((value >> 8) & 0xff);
        data.push_back((value >> 16) & 0xff);
        data.push_back((value >> 24) & 0xff);
    }

    // Helper: Serialize uint64_t as little-endian
    void WriteUint64LE(std::vector<uint8_t>& data, uint64_t value) {
        for (int i = 0; i < 8; i++) {
            data.push_back((value >> (i * 8)) & 0xff);
        }
    }

    // Helper: Serialize varint
    void WriteVarInt(std::vector<uint8_t>& data, uint64_t value) {
        if (value < 0xfd) {
            data.push_back(static_cast<uint8_t>(value));
        } else if (value <= 0xffff) {
            data.push_back(0xfd);
            data.push_back(value & 0xff);
            data.push_back((value >> 8) & 0xff);
        } else if (value <= 0xffffffff) {
            data.push_back(0xfe);
            WriteUint32LE(data, static_cast<uint32_t>(value));
        } else {
            data.push_back(0xff);
            WriteUint64LE(data, value);
        }
    }
}

TransactionBuilder::TransactionBuilder(
    const std::vector<uint8_t>& private_key,
    const std::vector<uint8_t>& public_key,
    const std::string& address
)
    : private_key_(private_key)
    , public_key_(public_key)
    , change_address_(address) {

    if (private_key_.size() != 32) {
        throw std::invalid_argument("Private key must be 32 bytes");
    }
    if (public_key_.size() != 33) {
        throw std::invalid_argument("Public key must be 33 bytes (compressed)");
    }
}

TransactionBuilder::~TransactionBuilder() {
    // Clear sensitive data
    std::fill(private_key_.begin(), private_key_.end(), 0);
}

TransactionBuilder::BuildResult TransactionBuilder::BuildTransaction(
    const std::vector<UTXO>& inputs,
    const std::string& to_address,
    uint64_t amount,
    uint64_t fee
) {
    if (inputs.empty()) {
        throw std::invalid_argument("No inputs provided");
    }
    if (amount == 0) {
        throw std::invalid_argument("Amount must be greater than zero");
    }

    // Calculate total input amount
    uint64_t total_input = 0;
    for (const auto& input : inputs) {
        total_input += input.amount;
    }

    // Calculate change
    uint64_t total_output = amount;
    uint64_t change_amount = 0;

    if (total_input < amount + fee) {
        throw std::runtime_error("Insufficient input amount");
    }

    change_amount = total_input - amount - fee;

    // Build outputs: [recipient, change?]
    std::vector<std::pair<std::string, uint64_t>> outputs;
    outputs.push_back({to_address, amount});

    if (change_amount >= GetDustThreshold()) {
        outputs.push_back({change_address_, change_amount});
        total_output += change_amount;
    } else {
        // Change too small, add to fee
        change_amount = 0;
    }

    // Serialize transaction
    std::string tx_hex = SerializeTransaction(inputs, outputs);

    // Sign all inputs
    std::vector<std::vector<std::vector<uint8_t>>> witnesses;
    for (size_t i = 0; i < inputs.size(); i++) {
        auto witness = SignInput(tx_hex, i, inputs[i]);
        witnesses.push_back(witness);
    }

    // Add witnesses to transaction
    std::string signed_tx_hex = AddWitnesses(tx_hex, witnesses);

    // Calculate txid
    std::string txid = CalculateTxid(signed_tx_hex);

    BuildResult result;
    result.txid = txid;
    result.hex = signed_tx_hex;
    result.total_input = total_input;
    result.total_output = total_output;
    result.change_amount = change_amount;

    return result;
}

uint64_t TransactionBuilder::EstimateSize(size_t num_inputs, size_t num_outputs) {
    // Rough estimation for P2WPKH transactions
    // Base size: 10 bytes
    // Input: 41 bytes (outpoint + sequence)
    // Output: 31 bytes (amount + script)
    // Witness: 107 bytes per input (signature + pubkey)

    uint64_t base_size = 10;
    uint64_t input_size = num_inputs * 41;
    uint64_t output_size = num_outputs * 31;
    uint64_t witness_size = num_inputs * 107;

    // SegWit weight = base_size * 3 + total_size
    uint64_t weight = (base_size + input_size + output_size) * 3 + (base_size + input_size + output_size + witness_size);
    return (weight + 3) / 4; // Convert to vbytes
}

std::string TransactionBuilder::CreateScriptPubKey(const std::string& address) {
    // Decode bech32 address to get witness program
    auto witness_program = crypto::Address::Decode(address);

    // P2WPKH scriptPubKey: OP_0 <20-byte-hash>
    std::vector<uint8_t> script_pubkey;
    script_pubkey.push_back(0x00); // OP_0
    script_pubkey.push_back(0x14); // Push 20 bytes
    script_pubkey.insert(script_pubkey.end(), witness_program.begin(), witness_program.end());

    return ToHex(script_pubkey);
}

std::vector<std::vector<uint8_t>> TransactionBuilder::SignInput(
    const std::string& tx_hex,
    size_t input_index,
    const UTXO& utxo
) {
    // For BIP143 (SegWit) signing, we need to compute the sighash
    // This is a simplified implementation - full implementation would use BIP143 spec

    // Create sighash preimage (simplified)
    std::vector<uint8_t> preimage;

    // Add version (2)
    WriteUint32LE(preimage, 2);

    // Add hashPrevouts (hash of all input outpoints)
    std::vector<uint8_t> prevouts_data;
    auto tx_bytes = FromHex(tx_hex);
    // [Simplified: in production, parse tx and hash all outpoints]
    prevouts_data = tx_bytes; // Placeholder

    auto prevouts_hash = crypto::Hash::Hash256(prevouts_data);
    preimage.insert(preimage.end(), prevouts_hash.begin(), prevouts_hash.end());

    // [... Full BIP143 implementation would continue here ...]
    // For now, we'll use a simplified sighash

    // Calculate sighash
    auto sighash = crypto::Hash::Hash256(preimage);

    // Sign
    auto signature = crypto::ECC::Sign(private_key_, sighash);

    // Add SIGHASH_ALL byte
    signature.push_back(0x01);

    // Build witness stack: [signature, pubkey]
    std::vector<std::vector<uint8_t>> witness;
    witness.push_back(signature);
    witness.push_back(public_key_);

    return witness;
}

std::string TransactionBuilder::CalculateTxid(const std::string& tx_hex) {
    auto tx_bytes = FromHex(tx_hex);

    // For SegWit, txid excludes witness data
    // [Simplified: in production, properly parse and exclude witness]

    auto hash = crypto::Hash::Hash256(tx_bytes);

    // Reverse for display (Bitcoin convention)
    std::reverse(hash.begin(), hash.end());

    return ToHex(hash);
}

std::string TransactionBuilder::SerializeTransaction(
    const std::vector<UTXO>& inputs,
    const std::vector<std::pair<std::string, uint64_t>>& outputs
) {
    std::vector<uint8_t> tx;

    // Version (2)
    WriteUint32LE(tx, 2);

    // Inputs
    WriteVarInt(tx, inputs.size());
    for (const auto& input : inputs) {
        // Outpoint: txid (32 bytes, reversed) + vout (4 bytes)
        auto txid_bytes = FromHex(input.txid);
        std::reverse(txid_bytes.begin(), txid_bytes.end());
        tx.insert(tx.end(), txid_bytes.begin(), txid_bytes.end());

        WriteUint32LE(tx, input.vout);

        // ScriptSig (empty for SegWit)
        WriteVarInt(tx, 0);

        // Sequence (0xffffffff)
        WriteUint32LE(tx, 0xffffffff);
    }

    // Outputs
    WriteVarInt(tx, outputs.size());
    for (const auto& [address, value] : outputs) {
        // Amount
        WriteUint64LE(tx, value);

        // ScriptPubKey
        auto script_pubkey_hex = CreateScriptPubKey(address);
        auto script_pubkey = FromHex(script_pubkey_hex);
        WriteVarInt(tx, script_pubkey.size());
        tx.insert(tx.end(), script_pubkey.begin(), script_pubkey.end());
    }

    // Locktime (0)
    WriteUint32LE(tx, 0);

    return ToHex(tx);
}

std::string TransactionBuilder::AddWitnesses(
    const std::string& tx_hex,
    const std::vector<std::vector<std::vector<uint8_t>>>& witnesses
) {
    // Parse transaction and insert witness data
    auto tx_bytes = FromHex(tx_hex);

    // Insert witness marker and flag after version
    std::vector<uint8_t> result;
    result.insert(result.end(), tx_bytes.begin(), tx_bytes.begin() + 4); // Version
    result.push_back(0x00); // Marker
    result.push_back(0x01); // Flag

    // Add rest of transaction (inputs + outputs + locktime)
    result.insert(result.end(), tx_bytes.begin() + 4, tx_bytes.end());

    // Add witnesses before locktime
    // [Simplified: proper implementation would insert witnesses at correct position]
    for (const auto& witness_stack : witnesses) {
        WriteVarInt(result, witness_stack.size());
        for (const auto& item : witness_stack) {
            WriteVarInt(result, item.size());
            result.insert(result.end(), item.begin(), item.end());
        }
    }

    return ToHex(result);
}

} // namespace reference
} // namespace wallet
} // namespace dinero
