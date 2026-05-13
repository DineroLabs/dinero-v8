// SPDX-License-Identifier: MIT
// Dinero - Descriptor-based PSBT Creation (Phase 2 Step 3)

#include "wallet/descriptor_psbt.h"
#include "wallet/descriptor_checksum.h"
#include "crypto/extended_pubkey.h"
#include "common/logger.h"
#include "common/address_script_builder.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {

// Helper: Convert hex string to bytes
std::vector<uint8_t> DescriptorPsbtFactory::hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        bytes.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
    }
    return bytes;
}

// Helper: Parse derivation path string to indices
std::vector<uint32_t> DescriptorPsbtFactory::parseDerivationPath(const std::string& path_str) {
    std::vector<uint32_t> path;
    std::string current;

    for (char c : path_str) {
        if (c == '/') {
            if (!current.empty()) {
                bool hardened = false;
                if (current.back() == 'h' || current.back() == '\'') {
                    hardened = true;
                    current.pop_back();
                }

                uint32_t index = std::stoul(current);
                if (hardened) {
                    index |= 0x80000000;
                }
                path.push_back(index);
                current.clear();
            }
        } else {
            current += c;
        }
    }

    // Handle last component
    if (!current.empty()) {
        bool hardened = false;
        if (current.back() == 'h' || current.back() == '\'') {
            hardened = true;
            current.pop_back();
        }

        uint32_t index = std::stoul(current);
        if (hardened) {
            index |= 0x80000000;
        }
        path.push_back(index);
    }

    return path;
}

// Helper: Add BIP32 derivation info to PSBT input
void DescriptorPsbtFactory::addBip32DerivationInfo(
    din::Psbt& psbt,
    size_t input_idx,
    const std::vector<uint8_t>& pubkey,
    const std::string& fingerprint,
    const std::vector<uint32_t>& derivation_path
) {
    std::vector<uint8_t> fp_bytes = hexToBytes(fingerprint);

    // Ensure we have exactly 4 bytes for fingerprint
    if (fp_bytes.size() != 4) {
        g_logger.warning("Invalid fingerprint size: " + std::to_string(fp_bytes.size()));
        return;
    }

    din::add_in_bip32_deriv(psbt, input_idx, pubkey, fp_bytes, derivation_path);
}

// Main factory function: Create PSBT from descriptor
DescriptorPsbtResult DescriptorPsbtFactory::createPsbtFromDescriptor(
    const DescriptorPsbtRequest& request,
    WalletPolicy wallet_policy,
    const std::string& wallet_fingerprint
) {
    DescriptorPsbtResult result;
    result.wallet_policy = wallet_policy;

    // Validate descriptor checksum
    if (!din::DescriptorChecksum::Verify(request.descriptor)) {
        result.error = "Invalid descriptor checksum";
        return result;
    }

    // Strip checksum for parsing
    std::string clean_descriptor = din::DescriptorChecksum::StripChecksum(request.descriptor);

    // Determine descriptor type
    if (clean_descriptor.substr(0, 3) == "tr(") {
        result.descriptor_type = "tr";

        // Parse BIP86 descriptor
        auto parsed = din::BIP86DescriptorFactory::parseDescriptor(clean_descriptor);
        if (!parsed.valid) {
            result.error = "Failed to parse BIP86 descriptor: " + parsed.error;
            return result;
        }

        // Validate policy matches
        if (wallet_policy != WalletPolicy::BIP86_TAPROOT) {
            result.error = "Descriptor type (tr) does not match wallet policy";
            return result;
        }

        // Create BIP86 PSBT
        return createBIP86Psbt(request, parsed, wallet_fingerprint);

    } else if (clean_descriptor.substr(0, 5) == "wpkh(") {
        result.descriptor_type = "wpkh";

        // Parse BIP84 descriptor
        auto parsed = din::BIP84DescriptorFactory::parseDescriptor(clean_descriptor);
        if (!parsed.valid) {
            result.error = "Failed to parse BIP84 descriptor: " + parsed.error;
            return result;
        }

        // Validate policy matches
        if (wallet_policy != WalletPolicy::BIP84_LEGACY) {
            result.error = "Descriptor type (wpkh) does not match wallet policy";
            return result;
        }

        // Create BIP84 PSBT
        return createBIP84Psbt(request, parsed, wallet_fingerprint);

    } else {
        result.error = "Unsupported descriptor type (only tr and wpkh supported)";
        return result;
    }
}

// Create BIP86 Taproot PSBT
DescriptorPsbtResult DescriptorPsbtFactory::createBIP86Psbt(
    const DescriptorPsbtRequest& request,
    const din::BIP86DescriptorFactory::ParsedBIP86& parsed,
    const std::string& wallet_fingerprint
) {
    DescriptorPsbtResult result;
    result.success = false;
    result.descriptor_type = "tr";
    result.wallet_policy = WalletPolicy::BIP86_TAPROOT;

    // Validate inputs
    if (request.inputs.empty()) {
        result.error = "At least one input required";
        return result;
    }

    if (request.outputs.empty()) {
        result.error = "At least one output required";
        return result;
    }

    // Initialize PSBT
    din::Psbt psbt;
    psbt.version = 0; // PSBTv0

    // Resize inputs and outputs
    psbt.inputs.resize(request.inputs.size());
    psbt.outputs.resize(request.outputs.size());

    // Add PSBTv2 metadata (optional but useful)
    din::add_global_tx_version(psbt, 2); // Bitcoin tx version 2
    din::add_global_input_count(psbt, request.inputs.size());
    din::add_global_output_count(psbt, request.outputs.size());

    // Calculate total amounts
    result.input_count = request.inputs.size();
    result.output_count = request.outputs.size();
    result.total_input_amount = 0;
    result.total_output_amount = 0;

    // Deserialize xpub for key derivation
    crypto::ExtendedPubKey ext_key = crypto::ExtendedPubKey::FromString(parsed.xpub);

    // Process inputs
    for (size_t i = 0; i < request.inputs.size(); ++i) {
        const auto& input = request.inputs[i];

        // Add WITNESS_UTXO (required for Taproot)
        din::add_in_witness_utxo(psbt, i, input.scriptPubKey, input.amount);

        // Add previous txid and vout (PSBTv2)
        std::vector<uint8_t> txid_bytes = hexToBytes(input.txid);
        std::reverse(txid_bytes.begin(), txid_bytes.end()); // Bitcoin uses reversed txid
        din::add_in_prev_txid(psbt, i, txid_bytes);
        din::add_in_output_index(psbt, i, input.vout);
        din::add_in_sequence(psbt, i, input.sequence);

        // Add sighash type if specified
        if (request.sighash_type.has_value()) {
            din::add_in_sighash(psbt, i, request.sighash_type.value());
        }

        // Add BIP32 derivation info if requested
        if (request.include_bip32_derivation) {
            // Derive public key for this input
            // BIP86: m/86h/1448h/0h/{change}/{address_index}
            try {
                crypto::ExtendedPubKey derived_key = ext_key;

                // Derive m/.../change
                derived_key = derived_key.Derive(parsed.is_change ? 1 : 0);

                // Derive m/.../change/address_index
                derived_key = derived_key.Derive(input.address_index);

                // Get the public key (x-only for Taproot)
                std::vector<uint8_t> pubkey = derived_key.GetPublicKey();

                // Build full derivation path including address index
                std::vector<uint32_t> full_path = parsed.derivation_path;
                full_path.push_back(parsed.is_change ? 1 : 0);
                full_path.push_back(input.address_index);

                // Add BIP32 derivation
                addBip32DerivationInfo(psbt, i, pubkey, parsed.fingerprint, full_path);

            } catch (const std::exception& e) {
                g_logger.warning("Failed to derive key for input " + std::to_string(i) + ": " + e.what());
            }
        }

        result.total_input_amount += input.amount;
    }

    // Process outputs
    for (size_t i = 0; i < request.outputs.size(); ++i) {
        const auto& output = request.outputs[i];

        // Convert address to scriptPubKey
        std::vector<uint8_t> scriptPubKey;
        std::string error;
        if (!BuildScriptPubKeyFromAddress(output.address, scriptPubKey, error)) {
            result.error = "Failed to build scriptPubKey for output " + std::to_string(i) + ": " + error;
            return result;
        }

        // Add output amount and script (PSBTv2)
        din::add_out_amount(psbt, i, output.amount);
        din::add_out_script(psbt, i, scriptPubKey);

        result.total_output_amount += output.amount;
    }

    // Calculate fee
    if (result.total_input_amount >= result.total_output_amount) {
        result.fee = result.total_input_amount - result.total_output_amount;
    } else {
        result.error = "Output amount exceeds input amount";
        return result;
    }

    // Serialize PSBT to base64
    try {
        std::vector<uint8_t> psbt_bytes = din::serialize(psbt);
        result.psbt_base64 = din::to_base64(psbt_bytes);
        result.psbt = psbt;
        result.success = true;

        g_logger.info("Created BIP86 PSBT: " + std::to_string(result.input_count) +
                     " inputs, " + std::to_string(result.output_count) + " outputs, " +
                     "fee: " + std::to_string(result.fee) + " sat");

    } catch (const std::exception& e) {
        result.error = "Failed to serialize PSBT: " + std::string(e.what());
        return result;
    }

    return result;
}

// Create BIP84 SegWit PSBT
DescriptorPsbtResult DescriptorPsbtFactory::createBIP84Psbt(
    const DescriptorPsbtRequest& request,
    const din::BIP84DescriptorFactory::ParsedBIP84& parsed,
    const std::string& wallet_fingerprint
) {
    DescriptorPsbtResult result;
    result.success = false;
    result.descriptor_type = "wpkh";
    result.wallet_policy = WalletPolicy::BIP84_LEGACY;

    // Validate inputs
    if (request.inputs.empty()) {
        result.error = "At least one input required";
        return result;
    }

    if (request.outputs.empty()) {
        result.error = "At least one output required";
        return result;
    }

    // Initialize PSBT
    din::Psbt psbt;
    psbt.version = 0; // PSBTv0

    // Resize inputs and outputs
    psbt.inputs.resize(request.inputs.size());
    psbt.outputs.resize(request.outputs.size());

    // Add PSBTv2 metadata
    din::add_global_tx_version(psbt, 2);
    din::add_global_input_count(psbt, request.inputs.size());
    din::add_global_output_count(psbt, request.outputs.size());

    // Calculate total amounts
    result.input_count = request.inputs.size();
    result.output_count = request.outputs.size();
    result.total_input_amount = 0;
    result.total_output_amount = 0;

    // Deserialize xpub for key derivation
    crypto::ExtendedPubKey ext_key = crypto::ExtendedPubKey::FromString(parsed.xpub);

    // Process inputs (similar to BIP86)
    for (size_t i = 0; i < request.inputs.size(); ++i) {
        const auto& input = request.inputs[i];

        // Add WITNESS_UTXO (required for SegWit)
        din::add_in_witness_utxo(psbt, i, input.scriptPubKey, input.amount);

        // Add previous txid and vout
        std::vector<uint8_t> txid_bytes = hexToBytes(input.txid);
        std::reverse(txid_bytes.begin(), txid_bytes.end());
        din::add_in_prev_txid(psbt, i, txid_bytes);
        din::add_in_output_index(psbt, i, input.vout);
        din::add_in_sequence(psbt, i, input.sequence);

        // Add sighash type if specified
        if (request.sighash_type.has_value()) {
            din::add_in_sighash(psbt, i, request.sighash_type.value());
        }

        // Add BIP32 derivation info
        if (request.include_bip32_derivation) {
            // BIP84: m/84h/0h/0h/{change}/{address_index}
            try {
                crypto::ExtendedPubKey derived_key = ext_key;

                // Derive m/.../change
                derived_key = derived_key.Derive(parsed.is_change ? 1 : 0);

                // Derive m/.../change/address_index
                derived_key = derived_key.Derive(input.address_index);

                std::vector<uint8_t> pubkey = derived_key.GetPublicKey();

                // Build full derivation path including address index
                std::vector<uint32_t> full_path = parsed.derivation_path;
                full_path.push_back(parsed.is_change ? 1 : 0);
                full_path.push_back(input.address_index);

                addBip32DerivationInfo(psbt, i, pubkey, parsed.fingerprint, full_path);

            } catch (const std::exception& e) {
                g_logger.warning("Failed to derive key for input " + std::to_string(i) + ": " + e.what());
            }
        }

        result.total_input_amount += input.amount;
    }

    // Process outputs
    for (size_t i = 0; i < request.outputs.size(); ++i) {
        const auto& output = request.outputs[i];

        // Convert address to scriptPubKey
        std::vector<uint8_t> scriptPubKey;
        std::string error;
        if (!BuildScriptPubKeyFromAddress(output.address, scriptPubKey, error)) {
            result.error = "Failed to build scriptPubKey for output " + std::to_string(i) + ": " + error;
            return result;
        }

        din::add_out_amount(psbt, i, output.amount);
        din::add_out_script(psbt, i, scriptPubKey);
        result.total_output_amount += output.amount;
    }

    // Calculate fee
    if (result.total_input_amount >= result.total_output_amount) {
        result.fee = result.total_input_amount - result.total_output_amount;
    } else {
        result.error = "Output amount exceeds input amount";
        return result;
    }

    // Serialize PSBT
    try {
        std::vector<uint8_t> psbt_bytes = din::serialize(psbt);
        result.psbt_base64 = din::to_base64(psbt_bytes);
        result.psbt = psbt;
        result.success = true;

        g_logger.info("Created BIP84 PSBT: " + std::to_string(result.input_count) +
                     " inputs, " + std::to_string(result.output_count) + " outputs, " +
                     "fee: " + std::to_string(result.fee) + " sat");

    } catch (const std::exception& e) {
        result.error = "Failed to serialize PSBT: " + std::string(e.what());
        return result;
    }

    return result;
}

} // namespace dinero
