/**
 * Hardware Wallet RPC Methods
 *
 * Provides file-based PSBT import/export for air-gapped hardware wallets like Coldcard.
 * Supports standard PSBT workflows for maximum compatibility.
 */

#include "rpc/rpc_registry.h"
#include "http_rpc_server.h"
#include "wallet/psbt.h"
#include "wallet/descriptor_checksum.h"
#include "wallet/retired_coin_type_guard.h"
#include "wallet/hardware_wallet_interface.h"
#include "wallet/wallet_manager.h"
#ifdef ENABLE_TREZOR
#include "wallet/hardware_wallet.h"
#endif
#include "common/json_utils.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <mutex>

namespace fs = std::filesystem;

using dinero::PSBT;
using dinero::hw::USBHardwareWallet;
using dinero::hw::DeviceType;
using dinero::hw::DeviceInfo;
using dinero::hw::TransportType;

namespace din {
namespace rpc {

namespace {

std::string BytesToHex(const std::vector<uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kHex[(b >> 4) & 0x0f]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

std::string HexUint32(uint32_t value) {
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << value;
    return oss.str();
}

const din::Json& NormalizeNamedParams(const din::Json& params) {
    if (params.isArray() && params.size() == 1 && params[0].isObject()) {
        return params[0];
    }
    return params;
}

bool IsLedgerType(DeviceType type) {
    return type == DeviceType::LEDGER_NANO_S ||
           type == DeviceType::LEDGER_NANO_X ||
           type == DeviceType::LEDGER_NANO_S_PLUS;
}

bool IsTrezorType(DeviceType type) {
    return type == DeviceType::TREZOR_ONE ||
           type == DeviceType::TREZOR_MODEL_T;
}

bool SupportsInteractiveUsbSession(DeviceType type) {
    if (IsLedgerType(type)) {
        return true;
    }
#ifdef ENABLE_TREZOR
    if (IsTrezorType(type)) {
        return true;
    }
#endif
    return false;
}

std::string DeviceTypeName(DeviceType type) {
    switch (type) {
        case DeviceType::LEDGER_NANO_S: return "LEDGER_NANO_S";
        case DeviceType::LEDGER_NANO_X: return "LEDGER_NANO_X";
        case DeviceType::LEDGER_NANO_S_PLUS: return "LEDGER_NANO_S_PLUS";
        case DeviceType::TREZOR_ONE: return "TREZOR_ONE";
        case DeviceType::TREZOR_MODEL_T: return "TREZOR_MODEL_T";
        case DeviceType::KEEPKEY: return "KEEPKEY";
        case DeviceType::BITBOX02: return "BITBOX02";
        case DeviceType::GENERIC_USB: return "GENERIC_USB";
        case DeviceType::UNKNOWN: return "UNKNOWN";
        default: return "UNSUPPORTED";
    }
}

std::string TransportTypeName(TransportType transport) {
    switch (transport) {
        case TransportType::USB_HID: return "usb_hid";
        case TransportType::USB_WEBUSB: return "usb_webusb";
        case TransportType::BLUETOOTH: return "bluetooth";
        case TransportType::SD_CARD: return "sd_card";
        case TransportType::QR_CODE: return "qr_code";
        case TransportType::FILE_SYSTEM: return "file_system";
        case TransportType::MOBILE_APP: return "mobile_app";
        case TransportType::NETWORK: return "network";
        default: return "unknown";
    }
}

din::Json SupportedOperationsFor(const DeviceInfo& device, bool connected) {
    din::Json ops = din::obj();
    const bool ledger_interactive = IsLedgerType(device.type);
    const bool trezor_interactive = IsTrezorType(device.type) && SupportsInteractiveUsbSession(device.type);

    ops["enumerate"] = true;
    ops["connect"] = ledger_interactive || trezor_interactive;
    ops["disconnect"] = connected;
    ops["inspect"] = connected;
    ops["get_address"] = trezor_interactive && connected;
    ops["display_address"] = trezor_interactive && connected;
    ops["verify_address"] = trezor_interactive && connected;
    ops["export_account_descriptor"] = trezor_interactive && connected;
    ops["sign_psbt"] = (ledger_interactive || trezor_interactive) && connected;
    ops["get_master_fingerprint"] = connected && (ledger_interactive || trezor_interactive);
    ops["notes"] = ledger_interactive
        ? (connected
            ? "Ledger USB session is open. Fingerprint export and direct PSBT signing are available for this session."
            : "Ledger detected over USB. Connect to inspect the device, export its fingerprint, or attempt direct PSBT signing.")
        : (trezor_interactive
            ? (connected
                ? "Trezor USB session is open. Device inspection, fingerprint export, address verification, watch-only account descriptor export, and constrained BIP86 direct PSBT signing are available."
                : "Trezor detected over USB. Connect to inspect the device, export its fingerprint, verify addresses on device, export watch-only account descriptors, or attempt constrained BIP86 direct PSBT signing.")
            : "Detected over USB, but interactive USB support is not wired for this device family yet.");
    return ops;
}

din::Json DeviceInfoToJson(const DeviceInfo& device, bool connected) {
    din::Json dev = din::obj();
    dev["device_id"] = device.device_id;
    dev["manufacturer"] = device.manufacturer;
    dev["model"] = device.model;
    dev["type"] = DeviceTypeName(device.type);
    dev["transport"] = TransportTypeName(device.transport);
    dev["serial_number"] = device.serial_number;
    dev["firmware_version"] = device.firmware_version;
    dev["initialized"] = device.initialized;
    dev["connected"] = connected;
    dev["interactive_usb_supported"] = SupportsInteractiveUsbSession(device.type);
    dev["supported_operations"] = SupportedOperationsFor(device, connected);
    return dev;
}

struct USBSessionState {
    enum class Backend {
        NONE,
        LEDGER_GENERIC,
        TREZOR_PROTO
    };

    std::mutex mutex;
    std::unique_ptr<USBHardwareWallet> wallet;
#ifdef ENABLE_TREZOR
    std::unique_ptr<dinero::TrezorWallet> trezor_wallet;
#endif
    DeviceInfo device;
    bool has_device = false;
    Backend backend = Backend::NONE;
};

USBSessionState& GetUSBSessionState() {
    static USBSessionState state;
    return state;
}

void ResetUSBSessionLocked(USBSessionState& session) {
    if (session.wallet) {
        session.wallet->Disconnect();
        session.wallet.reset();
    }
#ifdef ENABLE_TREZOR
    if (session.trezor_wallet) {
        session.trezor_wallet->disconnect();
        session.trezor_wallet->shutdown();
        session.trezor_wallet.reset();
    }
#endif
    session.has_device = false;
    session.backend = USBSessionState::Backend::NONE;
    session.device = DeviceInfo{};
}

void AttachMasterFingerprintIfAvailable(din::Json& target, USBSessionState& session) {
    switch (session.backend) {
        case USBSessionState::Backend::LEDGER_GENERIC: {
            if (!session.wallet) {
                return;
            }
            auto fingerprint = session.wallet->GetMasterFingerprint();
            if (!fingerprint.success) {
                target["master_fingerprint_error"] = fingerprint.error_message;
                return;
            }
            target["master_fingerprint"] = HexUint32(fingerprint.value);
            target["master_fingerprint_uint32"] = static_cast<Json::UInt64>(fingerprint.value);
            return;
        }
#ifdef ENABLE_TREZOR
        case USBSessionState::Backend::TREZOR_PROTO: {
            if (!session.trezor_wallet) {
                return;
            }
            const std::string fingerprint_hex = session.trezor_wallet->getFingerprint();
            if (fingerprint_hex.empty()) {
                const std::string error = session.trezor_wallet->getLastError();
                if (!error.empty()) {
                    target["master_fingerprint_error"] = error;
                }
                return;
            }
            target["master_fingerprint"] = fingerprint_hex;
            target["master_fingerprint_uint32"] =
                static_cast<Json::UInt64>(std::stoul(fingerprint_hex, nullptr, 16));
            return;
        }
#endif
        case USBSessionState::Backend::NONE:
        default:
            return;
    }
}

dinero::hw::HWResult<DeviceInfo> FindEnumeratedUSBDevice(const std::string& device_id) {
    USBHardwareWallet probe(DeviceType::LEDGER_NANO_S);
    auto enumeration = probe.EnumerateDevices();
    if (!enumeration.success) {
        return dinero::hw::HWResult<DeviceInfo>::Err(enumeration.error_message, enumeration.error_code);
    }

    for (const auto& device : enumeration.value) {
        if (device.device_id == device_id) {
            return dinero::hw::HWResult<DeviceInfo>::Ok(device);
        }
    }

    return dinero::hw::HWResult<DeviceInfo>::Err("Requested device is no longer present: " + device_id, -5);
}

}  // namespace

// Forward declarations (made extern for context-aware wrappers)
din::Json exportpsbttofile_impl(const ExecutionContext& ctx, const din::Json& params);
din::Json importpsbtfromfile_impl(const ExecutionContext& ctx, const din::Json& params);
din::Json analyzepsbt_impl(const ExecutionContext& ctx, const din::Json& params);
din::Json enumeratehwdevices_impl(const ExecutionContext& ctx, const din::Json& params);
din::Json connecthwdevice_impl(const ExecutionContext& ctx, const din::Json& params);
din::Json disconnecthwdevice_impl(const ExecutionContext& ctx, const din::Json& params);
din::Json gethwdeviceinfo_impl(const ExecutionContext& ctx, const din::Json& params);
din::Json gethwaddress_impl(const ExecutionContext& ctx, const din::Json& params);
din::Json gethwaccountdescriptor_impl(const ExecutionContext& ctx, const din::Json& params);
din::Json getmasterfingerprint_impl(const ExecutionContext& ctx, const din::Json& params);
din::Json signpsbt_impl(const ExecutionContext& ctx, const din::Json& params);

/**
 * exportpsbttofile
 *
 * Export a PSBT to a file for signing on an air-gapped hardware wallet.
 *
 * Arguments:
 * 1. psbt (string, required) - Base64-encoded PSBT
 * 2. filepath (string, required) - File path to write PSBT
 * 3. descriptor (string, optional) - Output descriptor for wallet policy (e.g., BIP86 Taproot)
 * 4. include_metadata (bool, optional, default=true) - Include .txt metadata file
 *
 * Returns:
 * {
 *   "filepath": "/path/to/unsigned.psbt",
 *   "metadata_file": "/path/to/unsigned.txt" (if include_metadata=true),
 *   "size_bytes": 1234,
 *   "hex": "70736274ff..." (optional, first 100 bytes),
 *   "descriptor": "tr([...]/0/\\*)" (if provided)
 * }
 *
 * Example:
 * dinero-cli exportpsbttofile "cHNidP8BAH..." "/tmp/coldcard/unsigned.psbt" \
 *   "tr([d4691818/86h/1448h/0h]xpub.../0/\\*)"
 */
din::Json exportpsbttofile_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";
    const auto& named = NormalizeNamedParams(params);

    // Validate parameters
    if (!named.isMember("psbt") || !named["psbt"].isString()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "psbt parameter required (base64 string)";
        return reply;
    }

    if (!named.isMember("filepath") || !named["filepath"].isString()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "filepath parameter required";
        return reply;
    }

    std::string psbt_b64 = named["psbt"].asString();
    std::string filepath = named["filepath"].asString();

    // Optional descriptor (BIP86 Taproot, BIP84 SegWit, etc.)
    std::string descriptor;
    if (named.isMember("descriptor") && named["descriptor"].isString()) {
        descriptor = named["descriptor"].asString();
    }

    // Optional metadata file creation
    bool include_metadata = true;
    if (named.isMember("include_metadata") && named["include_metadata"].isBool()) {
        include_metadata = named["include_metadata"].asBool();
    }

    try {
        // Decode and validate PSBT
        PSBT psbt;
        try {
            psbt = PSBT::FromBase64(psbt_b64);
        } catch (const std::exception& e) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -22;
            reply["error"]["message"] = std::string("Invalid PSBT: ") + e.what();
            return reply;
        }

        if (!psbt.IsValid()) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -22;
            reply["error"]["message"] = "PSBT validation failed: " + psbt.GetError();
            return reply;
        }

        // Serialize to bytes
        auto psbt_bytes = psbt.Serialize();

        // Create directory if it doesn't exist
        fs::path path(filepath);
        if (path.has_parent_path()) {
            fs::create_directories(path.parent_path());
        }

        // Write PSBT to file (binary format for hardware wallet compatibility)
        std::ofstream outfile(filepath, std::ios::binary);
        if (!outfile.is_open()) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = "Failed to open file for writing: " + filepath;
            return reply;
        }

        outfile.write(reinterpret_cast<const char*>(psbt_bytes.data()), psbt_bytes.size());
        outfile.close();

        // Verify file was written
        if (!fs::exists(filepath)) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = "File write verification failed";
            return reply;
        }

        // Build success response
        din::Json result = din::obj();
        result["filepath"] = filepath;
        result["size_bytes"] = static_cast<Json::UInt64>(psbt_bytes.size());

        // Include first 100 bytes as hex for verification (optional)
        size_t preview_len = std::min(static_cast<size_t>(100), psbt_bytes.size());
        std::string hex_preview;
        for (size_t i = 0; i < preview_len; i++) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", psbt_bytes[i]);
            hex_preview += buf;
        }
        result["hex_preview"] = hex_preview;

        // Write metadata file if descriptor is provided and include_metadata is true
        if (include_metadata && !descriptor.empty()) {
            fs::path psbt_path(filepath);
            fs::path metadata_path = psbt_path.parent_path() / (psbt_path.stem().string() + ".txt");

            std::ofstream metadata_file(metadata_path);
            if (metadata_file.is_open()) {
                metadata_file << "# Dinero Hardware Wallet PSBT Metadata\n";
                metadata_file << "# Generated: " << std::time(nullptr) << "\n";
                metadata_file << "# PSBT File: " << psbt_path.filename().string() << "\n";
                metadata_file << "#\n\n";
                metadata_file << "# Wallet Policy\n";
                metadata_file << "Descriptor: " << descriptor << "\n\n";

                // Detect policy type from descriptor
                std::string policy_type = "Unknown";
                if (descriptor.find("tr(") == 0) {
                    policy_type = "BIP86 Taproot (key-path only)";
                } else if (descriptor.find("wpkh(") == 0) {
                    policy_type = "BIP84 Native SegWit";
                } else if (descriptor.find("sh(wpkh(") == 0) {
                    policy_type = "BIP49 Nested SegWit";
                } else if (descriptor.find("pkh(") == 0) {
                    policy_type = "BIP44 Legacy";
                }
                metadata_file << "Policy Type: " << policy_type << "\n\n";

                metadata_file << "# Instructions\n";
                metadata_file << "1. Transfer " << psbt_path.filename().string() << " to your hardware wallet\n";
                metadata_file << "2. Sign the PSBT on your hardware wallet\n";
                metadata_file << "3. Transfer the signed PSBT back to this computer\n";
                metadata_file << "4. Import with: dinero-cli importpsbtfromfile <signed-psbt-path>\n";
                metadata_file << "5. Broadcast with: dinero-cli sendrawtransaction <tx-hex>\n";

                metadata_file.close();

                result["metadata_file"] = metadata_path.string();
            }
        }

        // Include descriptor in response if provided
        if (!descriptor.empty()) {
            result["descriptor"] = descriptor;
        }

        // Metadata
        result["instructions"] = "Transfer this file to your hardware wallet (e.g., Coldcard SD card) for signing";

        reply["result"] = result;
        return reply;

    } catch (const fs::filesystem_error& e) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("Filesystem error: ") + e.what();
        return reply;
    } catch (const std::exception& e) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("Error: ") + e.what();
        return reply;
    }
}

/**
 * importpsbtfromfile
 *
 * Import a signed PSBT from a file (e.g., from Coldcard SD card).
 *
 * Arguments:
 * 1. filepath (string, required) - File path to read PSBT from
 *
 * Returns:
 * {
 *   "psbt": "cHNidP8BAH...",  (base64-encoded PSBT)
 *   "complete": true/false,
 *   "hex": "02000000..." (if complete, final transaction hex)
 * }
 *
 * Example:
 * dinero-cli importpsbtfromfile "/tmp/coldcard/signed.psbt"
 */
din::Json importpsbtfromfile_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";
    const auto& named = NormalizeNamedParams(params);

    // Validate parameters
    if (!named.isMember("filepath") || !named["filepath"].isString()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "filepath parameter required";
        return reply;
    }

    std::string filepath = named["filepath"].asString();

    try {
        // Check if file exists
        if (!fs::exists(filepath)) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -5;
            reply["error"]["message"] = "File not found: " + filepath;
            return reply;
        }

        // Read PSBT file
        std::ifstream infile(filepath, std::ios::binary | std::ios::ate);
        if (!infile.is_open()) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = "Failed to open file: " + filepath;
            return reply;
        }

        // Get file size
        std::streamsize size = infile.tellg();
        infile.seekg(0, std::ios::beg);

        // Read file contents
        std::vector<uint8_t> psbt_bytes(size);
        if (!infile.read(reinterpret_cast<char*>(psbt_bytes.data()), size)) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = "Failed to read file";
            return reply;
        }
        infile.close();

        // Deserialize and validate PSBT
        PSBT psbt;
        if (!psbt.Deserialize(psbt_bytes)) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -22;
            reply["error"]["message"] = "Invalid PSBT format in file";
            return reply;
        }

        if (!psbt.IsValid()) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -22;
            reply["error"]["message"] = "PSBT validation failed: " + psbt.GetError();
            return reply;
        }

        // Convert to base64
        std::string psbt_b64 = psbt.ToBase64();

        // Check if PSBT is complete (all inputs signed)
        bool is_complete = true;
        for (const auto& input : psbt.inputs) {
            if (input.final_script_sig.empty() && input.final_script_witness.empty()) {
                is_complete = false;
                break;
            }
        }

        // Build response
        din::Json result = din::obj();
        result["psbt"] = psbt_b64;
        result["complete"] = is_complete;
        result["size_bytes"] = static_cast<Json::UInt64>(psbt_bytes.size());

        // If complete, extract final transaction
        if (is_complete) {
            try {
                auto final_tx = psbt.ExtractTransaction();
                auto tx_bytes = final_tx.Serialize();
                result["txid"] = final_tx.GetTxid().AsUint256().GetHex();
                result["hex"] = BytesToHex(tx_bytes);
                result["ready_to_broadcast"] = true;
                result["instructions"] = "PSBT is complete. Use 'sendrawtransaction' to broadcast.";
            } catch (const std::exception& e) {
                result["extraction_error"] = e.what();
                result["ready_to_broadcast"] = false;
            }
        } else {
            result["ready_to_broadcast"] = false;
            result["instructions"] = "PSBT is not yet fully signed. Continue signing or combine with other PSBTs.";
        }

        reply["result"] = result;
        return reply;

    } catch (const fs::filesystem_error& e) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("Filesystem error: ") + e.what();
        return reply;
    } catch (const std::exception& e) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("Error: ") + e.what();
        return reply;
    }
}

/**
 * analyzepsbt
 *
 * Analyze a PSBT to show signing status and metadata.
 * Useful for debugging and verifying PSBT state.
 *
 * Arguments:
 * 1. psbt (string, required) - Base64-encoded PSBT
 *
 * Returns:
 * {
 *   "inputs": [
 *     {
 *       "has_utxo": true,
 *       "has_sigs": true,
 *       "is_final": true,
 *       "missing_data": []
 *     }
 *   ],
 *   "estimated_vsize": 250,
 *   "estimated_feerate": 20.5,
 *   "next_role": "finalizer|signer|updater"
 * }
 */
din::Json analyzepsbt_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";
    const auto& named = NormalizeNamedParams(params);

    // Validate parameters
    if (!named.isMember("psbt") || !named["psbt"].isString()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "psbt parameter required (base64 string)";
        return reply;
    }

    std::string psbt_b64 = named["psbt"].asString();

    try {
        // Decode and validate PSBT
        PSBT psbt;
        try {
            psbt = PSBT::FromBase64(psbt_b64);
        } catch (const std::exception& e) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -22;
            reply["error"]["message"] = std::string("Invalid PSBT: ") + e.what();
            return reply;
        }

        // Analyze each input
        din::Json inputs_analysis = din::arr();
        bool all_final = true;
        bool any_signed = false;

        for (size_t i = 0; i < psbt.inputs.size(); i++) {
            const auto& input = psbt.inputs[i];
            din::Json input_info = din::obj();

            input_info["input_index"] = Json::UInt(i);
            input_info["has_utxo"] = !input.witness_utxo_script.empty() || !input.non_witness_utxo.empty();
            input_info["has_sigs"] = !input.partial_sigs.empty();
            input_info["is_final"] = !input.final_script_sig.empty() || !input.final_script_witness.empty();

            // Track missing data
            din::Json missing = din::arr();
            if (input.witness_utxo_script.empty() && input.non_witness_utxo.empty()) {
                missing.append("UTXO information");
            }
            if (input.partial_sigs.empty() && input.final_script_sig.empty()) {
                missing.append("Signatures");
            }
            input_info["missing_data"] = missing;

            inputs_analysis.append(input_info);

            if (!input_info["is_final"].asBool()) {
                all_final = false;
            }
            if (input_info["has_sigs"].asBool()) {
                any_signed = true;
            }
        }

        // Determine next role
        std::string next_role;
        if (all_final) {
            next_role = "extractor";  // Ready to extract final transaction
        } else if (any_signed) {
            next_role = "finalizer";  // Has signatures, needs finalization
        } else {
            next_role = "signer";     // Needs signatures
        }

        // Build response
        din::Json result = din::obj();
        result["inputs"] = inputs_analysis;
        result["num_inputs"] = static_cast<Json::UInt>(psbt.inputs.size());
        result["num_outputs"] = static_cast<Json::UInt>(psbt.outputs.size());
        result["next_role"] = next_role;

        // Fee analysis (if possible)
        const uint64_t estimated_vsize = static_cast<uint64_t>(psbt.tx.GetVirtualSize());
        result["estimated_vsize"] = Json::Value::UInt64(estimated_vsize);

        uint64_t total_input_value = 0;
        bool have_all_input_amounts = true;
        for (const auto& input : psbt.inputs) {
            if (input.witness_utxo_amount == 0) {
                have_all_input_amounts = false;
                break;
            }
            total_input_value += input.witness_utxo_amount;
        }

        uint64_t total_output_value = 0;
        for (const auto& output : psbt.tx.vout) {
            total_output_value += output.GetValue();
        }

        if (have_all_input_amounts && total_input_value >= total_output_value && estimated_vsize > 0) {
            const uint64_t fee_una = total_input_value - total_output_value;
            const double feerate = static_cast<double>(fee_una) / static_cast<double>(estimated_vsize);
            result["estimated_fee"] = Json::Value::UInt64(fee_una);
            result["estimated_feerate"] = feerate;
        } else {
            result["estimated_fee"] = din::Json();
            result["estimated_feerate"] = din::Json();
        }

        reply["result"] = result;
        return reply;

    } catch (const std::exception& e) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("Error: ") + e.what();
        return reply;
    }
}

/**
 * enumeratehwdevices
 *
 * Enumerate connected USB hardware wallets (Ledger, Trezor, etc.)
 *
 * Arguments: none
 *
 * Returns:
 * {
 *   "devices": [
 *     {
 *       "device_id": "/dev/hidraw0",
 *       "manufacturer": "Ledger",
 *       "model": "Nano S",
 *       "type": "LEDGER_NANO_S",
 *       "serial_number": "0001",
 *       "firmware_version": "2.1.0"
 *     }
 *   ],
 *   "count": 1
 * }
 */
din::Json enumeratehwdevices_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";

    try {
        // Create USB hardware wallet instance
        USBHardwareWallet usb_wallet(DeviceType::LEDGER_NANO_S);

        // Enumerate devices
        auto result = usb_wallet.EnumerateDevices();

        if (!result.success) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = result.error_message;
            return reply;
        }

        // Build devices array
        din::Json devices = din::arr();
        for (const auto& device : result.value) {
            devices.append(DeviceInfoToJson(device, false));
        }

        reply["result"] = din::obj();
        reply["result"]["devices"] = devices;
        reply["result"]["count"] = static_cast<int>(result.value.size());
        reply["result"]["interactive_usb_backend"] =
#ifdef ENABLE_TREZOR
            "ledger_sign_trezor_inspect";
#else
            "ledger_first";
#endif
        reply["result"]["notes"] =
            "USB detection is active. Ledger sessions can inspect the device, export fingerprints, and attempt direct PSBT signing."
#ifdef ENABLE_TREZOR
            " Trezor sessions can inspect the device, export fingerprints, verify addresses on device, export watch-only descriptors, and attempt constrained BIP86 direct PSBT signing.";
#else
            " Trezor remains detect-only.";
#endif

        return reply;

    } catch (const std::exception& e) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("Exception: ") + e.what();
        return reply;
    }
}

/**
 * connecthwdevice
 *
 * Open a USB session for a detected hardware wallet.
 * Ledger supports inspection/fingerprint/direct PSBT signing.
 * Trezor, when compiled in, supports inspection/fingerprint/address verification,
 * descriptor export, and constrained BIP86 direct PSBT signing.
 */
din::Json connecthwdevice_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";
    const auto& named = NormalizeNamedParams(params);

    if (!named.isMember("device_id") || !named["device_id"].isString() || named["device_id"].asString().empty()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "device_id parameter required";
        return reply;
    }

    const std::string device_id = named["device_id"].asString();
    auto located = FindEnumeratedUSBDevice(device_id);
    if (!located.success) {
        reply["error"] = din::obj();
        reply["error"]["code"] = located.error_code;
        reply["error"]["message"] = located.error_message;
        return reply;
    }

    const auto& device = located.value;
    if (!SupportsInteractiveUsbSession(device.type)) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -4;
        reply["error"]["message"] =
            device.manufacturer + " " + device.model +
            " is detect-only in this backend build. Interactive USB support is not available for this device family.";
        return reply;
    }

    auto& session = GetUSBSessionState();
    std::lock_guard<std::mutex> lock(session.mutex);
    ResetUSBSessionLocked(session);

    reply["result"] = din::obj();

    if (IsLedgerType(device.type)) {
        auto wallet = std::make_unique<USBHardwareWallet>(device.type);
        auto connected = wallet->Connect(device.device_id);
        if (!connected.success) {
            reply["error"] = din::obj();
            reply["error"]["code"] = connected.error_code;
            reply["error"]["message"] = connected.error_message;
            return reply;
        }

        auto info = wallet->GetDeviceInfo();
        if (!info.success) {
            reply["error"] = din::obj();
            reply["error"]["code"] = info.error_code;
            reply["error"]["message"] = info.error_message;
            return reply;
        }

        session.wallet = std::move(wallet);
        session.device = info.value;
        session.has_device = true;
        session.backend = USBSessionState::Backend::LEDGER_GENERIC;

        reply["result"]["connected"] = true;
        reply["result"]["device"] = DeviceInfoToJson(session.device, true);
        AttachMasterFingerprintIfAvailable(reply["result"], session);
        reply["result"]["notes"] =
            "Ledger USB session opened. Qt can inspect the device, export its fingerprint, and attempt direct USB PSBT signing.";
        return reply;
    }

#ifdef ENABLE_TREZOR
    auto trezor = std::make_unique<dinero::TrezorWallet>();
    if (!trezor->initialize()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -1;
        reply["error"]["message"] = trezor->getLastError().empty()
            ? "Failed to initialize Trezor session"
            : trezor->getLastError();
        return reply;
    }
    trezor->setDevicePathForConnect(device.device_id);
    if (!trezor->connect()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -1;
        reply["error"]["message"] = trezor->getLastError().empty()
            ? "Failed to connect to Trezor"
            : trezor->getLastError();
        trezor->shutdown();
        return reply;
    }

    const auto legacy_info = trezor->getDeviceInfo();
    session.device = device;
    if (!legacy_info.serial_number.empty()) {
        session.device.serial_number = legacy_info.serial_number;
    }
    if (!legacy_info.firmware_version.empty()) {
        session.device.firmware_version = legacy_info.firmware_version;
    }
    session.device.initialized = true;
    session.trezor_wallet = std::move(trezor);
    session.has_device = true;
    session.backend = USBSessionState::Backend::TREZOR_PROTO;

    reply["result"]["connected"] = true;
    reply["result"]["device"] = DeviceInfoToJson(session.device, true);
    AttachMasterFingerprintIfAvailable(reply["result"], session);
    reply["result"]["notes"] =
        "Trezor USB session opened. Qt can inspect the device, export its fingerprint, verify addresses on device, export watch-only descriptors, and attempt constrained BIP86 USB PSBT signing.";
    return reply;
#else
    reply["error"] = din::obj();
    reply["error"]["code"] = -4;
    reply["error"]["message"] = "Trezor support is not compiled into this daemon build.";
    return reply;
#endif
}

/**
 * disconnecthwdevice
 *
 * Close the active USB hardware-wallet session if one exists.
 */
din::Json disconnecthwdevice_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";

    auto& session = GetUSBSessionState();
    std::lock_guard<std::mutex> lock(session.mutex);

    const bool had_active_session = session.backend != USBSessionState::Backend::NONE;
    ResetUSBSessionLocked(session);

    reply["result"] = din::obj();
    reply["result"]["disconnected"] = true;
    reply["result"]["had_active_session"] = had_active_session;
    reply["result"]["notes"] = had_active_session
        ? "USB hardware-wallet session closed."
        : "No active USB hardware-wallet session was open.";
    return reply;
}

/**
 * gethwdeviceinfo
 *
 * Report the active USB session state, if any.
 */
din::Json gethwdeviceinfo_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";

    auto& session = GetUSBSessionState();
    std::lock_guard<std::mutex> lock(session.mutex);

    reply["result"] = din::obj();
    reply["result"]["connected"] = session.backend != USBSessionState::Backend::NONE && session.has_device;
    reply["result"]["interactive_usb_backend"] =
#ifdef ENABLE_TREZOR
        "ledger_sign_trezor_inspect";
#else
        "ledger_first";
#endif

    if (session.backend != USBSessionState::Backend::NONE && session.has_device) {
        if (session.backend == USBSessionState::Backend::LEDGER_GENERIC && session.wallet) {
            auto info = session.wallet->GetDeviceInfo();
            if (info.success) {
                session.device = info.value;
            }
        }
        reply["result"]["device"] = DeviceInfoToJson(session.device, true);
        AttachMasterFingerprintIfAvailable(reply["result"], session);
        reply["result"]["notes"] = session.backend == USBSessionState::Backend::LEDGER_GENERIC
            ? "Active Ledger USB session available for inspection, fingerprint export, and direct PSBT signing."
            : "Active Trezor USB session available for inspection, fingerprint export, address verification, watch-only descriptor export, and constrained BIP86 direct PSBT signing.";
    } else {
        reply["result"]["device"] = din::Json();
        reply["result"]["notes"] = "No active USB hardware-wallet session.";
    }

    return reply;
}

din::Json gethwaddress_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";
    const auto& named = NormalizeNamedParams(params);

    if (!named.isMember("derivation_path") || !named["derivation_path"].isString() || named["derivation_path"].asString().empty()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "derivation_path parameter required";
        return reply;
    }

    const std::string derivation_path = named["derivation_path"].asString();
    try {
        dinero::wallet::RejectRetiredLegacyCoinTypeText(derivation_path, "hwallet.gethwaddress");
        dinero::wallet::RejectNonCanonicalCoinTypeTextPath(derivation_path, "hwallet.gethwaddress");
    } catch (const std::exception& e) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = e.what();
        return reply;
    }
    const bool show_display = named.isMember("show_display") && named["show_display"].isBool()
        ? named["show_display"].asBool()
        : false;

    auto& session = GetUSBSessionState();
    std::lock_guard<std::mutex> lock(session.mutex);

    if (session.backend == USBSessionState::Backend::NONE || !session.has_device) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -4;
        reply["error"]["message"] = "No active USB hardware-wallet session.";
        return reply;
    }

#ifdef ENABLE_TREZOR
    if (session.backend == USBSessionState::Backend::TREZOR_PROTO && session.trezor_wallet) {
        auto address_info = session.trezor_wallet->getAddress(derivation_path);
        if (address_info.address.empty()) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            const std::string error = session.trezor_wallet->getLastError();
            reply["error"]["message"] = error.empty()
                ? "Failed to retrieve address from the active Trezor USB session"
                : error;
            return reply;
        }

        bool verified_on_device = false;
        if (show_display) {
            verified_on_device = session.trezor_wallet->verifyAddress(address_info.address, derivation_path);
            if (!verified_on_device) {
                reply["error"] = din::obj();
                reply["error"]["code"] = -1;
                const std::string error = session.trezor_wallet->getLastError();
                reply["error"]["message"] = error.empty()
                    ? "Failed to verify address on the active Trezor USB session"
                    : error;
                return reply;
            }
        }

        reply["result"] = din::obj();
        reply["result"]["device"] = DeviceInfoToJson(session.device, true);
        reply["result"]["derivation_path"] = derivation_path;
        reply["result"]["address"] = address_info.address;
        reply["result"]["public_key"] = address_info.public_key;
        reply["result"]["show_display_requested"] = show_display;
        reply["result"]["verified_on_device"] = verified_on_device;
        reply["result"]["notes"] = show_display
            ? "Address was returned and confirmed on the active Trezor device."
            : "Address was returned from the active Trezor USB session.";
        return reply;
    }
#endif

    reply["error"] = din::obj();
    reply["error"]["code"] = -4;
    reply["error"]["message"] = "The active USB session does not support address retrieval or on-device verification yet.";
    return reply;
}

din::Json gethwaccountdescriptor_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";
    const auto& named = NormalizeNamedParams(params);

    if (!named.isMember("derivation_path") || !named["derivation_path"].isString() || named["derivation_path"].asString().empty()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "derivation_path parameter required";
        return reply;
    }

    std::string requested_policy;
    if (named.isMember("policy") && named["policy"].isString()) {
        requested_policy = named["policy"].asString();
    }
    try {
        dinero::wallet::RejectRetiredLegacyCoinTypeText(named["derivation_path"].asString(),
                                                      "hwallet.gethwaccountdescriptor");
        dinero::wallet::RejectNonCanonicalCoinTypeTextPath(named["derivation_path"].asString(),
                                                          "hwallet.gethwaccountdescriptor");
    } catch (const std::exception& e) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = e.what();
        return reply;
    }

    auto& session = GetUSBSessionState();
    std::lock_guard<std::mutex> lock(session.mutex);

    if (session.backend == USBSessionState::Backend::NONE || !session.has_device) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -4;
        reply["error"]["message"] = "No active USB hardware-wallet session.";
        return reply;
    }

#ifdef ENABLE_TREZOR
    if (session.backend == USBSessionState::Backend::TREZOR_PROTO && session.trezor_wallet) {
        dinero::TrezorWallet::AccountDescriptorExport descriptor_export;
        if (!session.trezor_wallet->exportAccountDescriptors(named["derivation_path"].asString(),
                                                             requested_policy,
                                                             descriptor_export)) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            const std::string error = session.trezor_wallet->getLastError();
            reply["error"]["message"] = error.empty()
                ? "Failed to export account descriptors from the active Trezor USB session"
                : error;
            return reply;
        }

        reply["result"] = din::obj();
        reply["result"]["device"] = DeviceInfoToJson(session.device, true);
        reply["result"]["derivation_path"] = descriptor_export.derivation_path;
        reply["result"]["policy"] = descriptor_export.policy;
        reply["result"]["master_fingerprint"] = descriptor_export.master_fingerprint;
        reply["result"]["account_xpub"] = descriptor_export.account_xpub;
        reply["result"]["coin_type"] = static_cast<Json::UInt64>(descriptor_export.coin_type);
        reply["result"]["account"] = static_cast<Json::UInt64>(descriptor_export.account);
        reply["result"]["receive_descriptor"] = descriptor_export.receive_descriptor;
        reply["result"]["receive_descriptor_with_checksum"] =
            din::DescriptorChecksum::AddChecksum(descriptor_export.receive_descriptor);
        reply["result"]["change_descriptor"] = descriptor_export.change_descriptor;
        reply["result"]["change_descriptor_with_checksum"] =
            din::DescriptorChecksum::AddChecksum(descriptor_export.change_descriptor);
        reply["result"]["notes"] =
            "Watch-only account descriptors exported from the active Trezor USB session. Import them into Dinero or another descriptor wallet to bootstrap receive/change tracking.";
        return reply;
    }
#endif

    reply["error"] = din::obj();
    reply["error"]["code"] = -4;
    reply["error"]["message"] = "The active USB session does not support account descriptor export yet.";
    return reply;
}

din::Json getmasterfingerprint_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";

    auto& session = GetUSBSessionState();
    std::lock_guard<std::mutex> lock(session.mutex);

    if (session.backend == USBSessionState::Backend::NONE || !session.has_device) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -4;
        reply["error"]["message"] = "No active USB hardware-wallet session.";
        return reply;
    }

    if (session.backend == USBSessionState::Backend::LEDGER_GENERIC && session.wallet) {
        auto info = session.wallet->GetDeviceInfo();
        if (info.success) {
            session.device = info.value;
        }
    }

    reply["result"] = din::obj();
    reply["result"]["connected"] = true;
    reply["result"]["device"] = DeviceInfoToJson(session.device, true);
    AttachMasterFingerprintIfAvailable(reply["result"], session);
    if (!reply["result"].isMember("master_fingerprint")) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -1;
        reply["error"]["message"] = reply["result"].isMember("master_fingerprint_error")
            ? reply["result"]["master_fingerprint_error"].asString()
            : "Failed to export master fingerprint";
        reply.removeMember("result");
        return reply;
    }
    reply["result"]["notes"] = session.backend == USBSessionState::Backend::LEDGER_GENERIC
        ? "Master fingerprint exported from the active Ledger USB session."
        : "Master fingerprint exported from the active Trezor USB session.";
    return reply;
}

din::Json signpsbt_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";
    const auto& named = NormalizeNamedParams(params);

    if (!named.isMember("psbt") || !named["psbt"].isString() || named["psbt"].asString().empty()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "psbt parameter required (base64 string)";
        return reply;
    }

    std::vector<std::string> derivation_paths;
    if (named.isMember("derivation_paths") && named["derivation_paths"].isArray()) {
        for (const auto& entry : named["derivation_paths"]) {
            if (entry.isString()) {
                try {
                    const std::string path = entry.asString();
                    dinero::wallet::RejectRetiredLegacyCoinTypeText(path, "hwallet.signpsbt");
                    dinero::wallet::RejectNonCanonicalCoinTypeTextPath(path, "hwallet.signpsbt");
                    derivation_paths.push_back(path);
                } catch (const std::exception& e) {
                    reply["error"] = din::obj();
                    reply["error"]["code"] = -8;
                    reply["error"]["message"] = e.what();
                    return reply;
                }
            }
        }
    }

    auto& session = GetUSBSessionState();
    std::lock_guard<std::mutex> lock(session.mutex);

    if (session.backend == USBSessionState::Backend::NONE || !session.has_device) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -4;
        reply["error"]["message"] = "No active USB hardware-wallet session.";
        return reply;
    }

    PSBT psbt;
    try {
        psbt = PSBT::FromBase64(named["psbt"].asString());
    } catch (const std::exception& e) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -22;
        reply["error"]["message"] = std::string("Invalid PSBT: ") + e.what();
        return reply;
    }

    if (!psbt.IsValid()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -22;
        reply["error"]["message"] = "PSBT validation failed: " + psbt.GetError();
        return reply;
    }

    if (session.backend == USBSessionState::Backend::LEDGER_GENERIC && session.wallet) {
        auto info = session.wallet->GetDeviceInfo();
        if (info.success) {
            session.device = info.value;
        }

        auto signed_result = session.wallet->SignPSBT(psbt, derivation_paths, nullptr);
        if (!signed_result.success) {
            reply["error"] = din::obj();
            reply["error"]["code"] = signed_result.error_code;
            reply["error"]["message"] = signed_result.error_message;
            return reply;
        }

        const auto& signed_psbt = signed_result.value;
        bool complete = true;
        for (const auto& input : signed_psbt.inputs) {
            if (input.final_script_sig.empty() && input.final_script_witness.empty()) {
                complete = false;
                break;
            }
        }

        reply["result"] = din::obj();
        reply["result"]["device"] = DeviceInfoToJson(session.device, true);
        AttachMasterFingerprintIfAvailable(reply["result"], session);
        reply["result"]["psbt"] = signed_psbt.ToBase64();
        reply["result"]["complete"] = complete;
        reply["result"]["size_bytes"] = static_cast<Json::UInt64>(signed_psbt.Serialize().size());

        if (complete) {
            try {
                auto final_tx = signed_psbt.ExtractTransaction();
                auto tx_bytes = final_tx.Serialize();
                reply["result"]["txid"] = final_tx.GetTxid().AsUint256().GetHex();
                reply["result"]["hex"] = BytesToHex(tx_bytes);
                reply["result"]["ready_to_broadcast"] = true;
                reply["result"]["instructions"] = "Device returned a complete PSBT. You can now broadcast the extracted transaction.";
            } catch (const std::exception& e) {
                reply["result"]["extraction_error"] = e.what();
                reply["result"]["ready_to_broadcast"] = false;
                reply["result"]["instructions"] = "Device returned a signed PSBT, but extraction failed. Try wallet.finalizepsbt before broadcasting.";
            }
        } else {
            reply["result"]["ready_to_broadcast"] = false;
            reply["result"]["instructions"] = "Device returned a partially signed PSBT. Continue signing or combine signatures before broadcasting.";
        }

        return reply;
    }

#ifdef ENABLE_TREZOR
    if (session.backend == USBSessionState::Backend::TREZOR_PROTO && session.trezor_wallet) {
        if (!ctx.wallet_manager || !ctx.wallet_manager->getDescriptorStore()) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = "DescriptorStore is not available in this daemon context. Trezor direct signing requires the active wallet descriptor state.";
            return reply;
        }

        session.trezor_wallet->setDescriptorStore(ctx.wallet_manager->getDescriptorStore());
        auto signed_result = session.trezor_wallet->signPSBT(named["psbt"].asString());
        if (!signed_result.success) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = signed_result.error_message;
            return reply;
        }

        PSBT signed_psbt;
        try {
            signed_psbt = PSBT::FromBase64(signed_result.psbt_base64);
        } catch (const std::exception& e) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -22;
            reply["error"]["message"] = std::string("Trezor returned an invalid signed PSBT: ") + e.what();
            return reply;
        }

        reply["result"] = din::obj();
        reply["result"]["device"] = DeviceInfoToJson(session.device, true);
        AttachMasterFingerprintIfAvailable(reply["result"], session);
        reply["result"]["psbt"] = signed_result.psbt_base64;
        reply["result"]["complete"] = signed_result.complete;
        reply["result"]["size_bytes"] = static_cast<Json::UInt64>(signed_psbt.Serialize().size());
        reply["result"]["ready_to_broadcast"] = false;
        reply["result"]["instructions"] =
            "Trezor returned Taproot signatures in the PSBT. Finalize it with wallet.finalizepsbt before broadcasting.";
        reply["result"]["notes"] =
            "This Trezor USB path is constrained to active-wallet BIP86 Taproot inputs that can be matched against the loaded descriptor set.";
        return reply;
    }
#endif

    reply["error"] = din::obj();
    reply["error"]["code"] = -4;
    reply["error"]["message"] = "The active USB session does not support direct PSBT signing. Use File / SD Card or QR signing for this device.";
    return reply;
}

} // namespace rpc
} // namespace din

// ============================================================================
// Registration Function (vNext RpcRegistry)
// ============================================================================
// Registers hardware wallet RPC methods in the vNext RpcRegistry.
// These methods are automatically exposed via RpcAdapter to both HTTP and WebSocket.

extern RpcRegistry g_rpcRegistry;

void registerHardwareWalletRPC() {
    std::cout << "[Hardware Wallet RPC] Registering methods in vNext RpcRegistry..." << std::endl;

    // hwallet.exportpsbttofile - Export PSBT to file for air-gapped signing
    g_rpcRegistry.registerHandler("hwallet.exportpsbttofile", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        return din::rpc::exportpsbttofile_impl(ctx, params);
    }, "hardware_wallet");

    // hwallet.importpsbtfromfile - Import signed PSBT from file
    g_rpcRegistry.registerHandler("hwallet.importpsbtfromfile", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        return din::rpc::importpsbtfromfile_impl(ctx, params);
    }, "hardware_wallet");

    // hwallet.analyzepsbt - Analyze PSBT signing status
    g_rpcRegistry.registerHandler("hwallet.analyzepsbt", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        return din::rpc::analyzepsbt_impl(ctx, params);
    }, "hardware_wallet");

    // hwallet.enumeratehwdevices - Enumerate connected USB hardware wallets
    g_rpcRegistry.registerHandler("hwallet.enumeratehwdevices", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        return din::rpc::enumeratehwdevices_impl(ctx, params);
    }, "hardware_wallet");

    g_rpcRegistry.registerHandler("hwallet.connecthwdevice", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        return din::rpc::connecthwdevice_impl(ctx, params);
    }, "hardware_wallet");

    g_rpcRegistry.registerHandler("hwallet.disconnecthwdevice", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        return din::rpc::disconnecthwdevice_impl(ctx, params);
    }, "hardware_wallet");

    g_rpcRegistry.registerHandler("hwallet.gethwdeviceinfo", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        return din::rpc::gethwdeviceinfo_impl(ctx, params);
    }, "hardware_wallet");

    g_rpcRegistry.registerHandler("hwallet.gethwaddress", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        return din::rpc::gethwaddress_impl(ctx, params);
    }, "hardware_wallet");

    g_rpcRegistry.registerHandler("hwallet.gethwaccountdescriptor", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        return din::rpc::gethwaccountdescriptor_impl(ctx, params);
    }, "hardware_wallet");

    g_rpcRegistry.registerHandler("hwallet.getmasterfingerprint", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        return din::rpc::getmasterfingerprint_impl(ctx, params);
    }, "hardware_wallet");

    g_rpcRegistry.registerHandler("hwallet.signpsbt", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        return din::rpc::signpsbt_impl(ctx, params);
    }, "hardware_wallet");

    std::cout << "[Hardware Wallet RPC] Registered 11 methods: "
              << "hwallet.exportpsbttofile, hwallet.importpsbtfromfile, hwallet.analyzepsbt, "
              << "hwallet.enumeratehwdevices, hwallet.connecthwdevice, hwallet.disconnecthwdevice, "
              << "hwallet.gethwdeviceinfo, hwallet.gethwaddress, hwallet.gethwaccountdescriptor, "
              << "hwallet.getmasterfingerprint, hwallet.signpsbt" << std::endl;
}

// Legacy bridge function removed - all registration now happens in registerHardwareWalletRPC()
