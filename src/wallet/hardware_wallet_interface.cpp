/**
 * Hardware Wallet Interface Implementation
 *
 * Concrete implementations for file-based and QR-code hardware wallets.
 * USB implementations require platform-specific libraries (libusb/hidapi).
 */

#include "wallet/hardware_wallet_interface.h"
#include "wallet/psbt.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <ctime>
#include <thread>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>

#ifdef HAVE_USB_HWALLET
#include <hidapi.h>
#include "crypto/hash.h"
// Note: libusb is linked but we use hidapi for all USB communication
#endif

namespace fs = std::filesystem;

namespace dinero {
namespace hw {

// ============================================================================
// FileBasedHardwareWallet Implementation
// ============================================================================

FileBasedHardwareWallet::FileBasedHardwareWallet(
    const std::string& export_dir,
    const std::string& import_dir
) : export_dir_(export_dir), import_dir_(import_dir) {
    device_info_.type = DeviceType::GENERIC_PSBT;
    device_info_.transport = TransportType::FILE_SYSTEM;
    device_info_.manufacturer = "Generic";
    device_info_.model = "File-Based PSBT";
    device_info_.initialized = true;

    // Generic file-based device capabilities
    device_info_.capabilities.supports_bitcoin = true;
    device_info_.capabilities.supports_segwit = true;
    device_info_.capabilities.supports_taproot = true;
    device_info_.capabilities.supports_multisig = true;
    device_info_.capabilities.supports_bip32_derivation = true;
    device_info_.capabilities.supports_display_address = false;  // No screen
    device_info_.capabilities.supports_blind_signing = true;
    device_info_.capabilities.supports_message_signing = true;
}

HWResult<std::vector<DeviceInfo>> FileBasedHardwareWallet::EnumerateDevices() {
    std::vector<DeviceInfo> devices;

    // Check if import directory exists (indicates device presence)
    if (fs::exists(import_dir_) && fs::is_directory(import_dir_)) {
        DeviceInfo info = device_info_;
        info.serial_number = "file://" + import_dir_;
        devices.push_back(info);
    }

    return HWResult<std::vector<DeviceInfo>>::Ok(devices);
}

HWResult<bool> FileBasedHardwareWallet::Connect(const std::string& device_id) {
    // For file-based devices, "connection" means verifying directories exist
    try {
        // Create directories if they don't exist
        if (!export_dir_.empty()) {
            fs::create_directories(export_dir_);
        }
        if (!import_dir_.empty()) {
            fs::create_directories(import_dir_);
        }

        connected_ = true;
        return HWResult<bool>::Ok(true);

    } catch (const fs::filesystem_error& e) {
        return HWResult<bool>::Err(
            std::string("Failed to create directories: ") + e.what()
        );
    }
}

HWResult<bool> FileBasedHardwareWallet::Disconnect() {
    connected_ = false;
    return HWResult<bool>::Ok(true);
}

HWResult<DeviceInfo> FileBasedHardwareWallet::GetDeviceInfo() {
    if (!connected_) {
        return HWResult<DeviceInfo>::Err("Device not connected");
    }
    return HWResult<DeviceInfo>::Ok(device_info_);
}

HWResult<PSBT> FileBasedHardwareWallet::SignPSBT(
    const PSBT& psbt,
    const std::vector<std::string>& derivation_paths,
    ProgressCallback progress_cb
) {
    if (!connected_) {
        return HWResult<PSBT>::Err("Device not connected");
    }

    try {
        // Step 1: Export unsigned PSBT to file
        if (progress_cb) {
            progress_cb(10, "Exporting PSBT to device...");
        }

        // Generate filename with timestamp
        auto now = std::time(nullptr);
        std::string timestamp = std::to_string(now);
        std::string unsigned_filename = "unsigned-" + timestamp + ".psbt";
        std::string unsigned_path = export_dir_ + "/" + unsigned_filename;

        // Serialize PSBT
        auto psbt_bytes = psbt.Serialize();

        // Write to export directory
        std::ofstream outfile(unsigned_path, std::ios::binary);
        if (!outfile.is_open()) {
            return HWResult<PSBT>::Err("Failed to write PSBT file: " + unsigned_path);
        }
        outfile.write(reinterpret_cast<const char*>(psbt_bytes.data()), psbt_bytes.size());
        outfile.close();

        if (progress_cb) {
            progress_cb(30, "PSBT exported. Waiting for signed file...");
        }

        // Step 2: Wait for signed PSBT to appear in import directory
        std::string signed_filename = "signed-" + timestamp + ".psbt";
        std::string signed_path = import_dir_ + "/" + signed_filename;

        // Poll for signed file (timeout after 5 minutes)
        int timeout_seconds = 300;
        int elapsed = 0;

        while (elapsed < timeout_seconds) {
            if (fs::exists(signed_path)) {
                break;
            }

            // Update progress every 10 seconds
            if (elapsed % 10 == 0 && progress_cb) {
                int progress = 30 + (elapsed * 50) / timeout_seconds;
                progress_cb(progress,
                    "Waiting for signed PSBT... (" +
                    std::to_string(timeout_seconds - elapsed) + "s remaining)");
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
            elapsed++;
        }

        if (!fs::exists(signed_path)) {
            return HWResult<PSBT>::Err(
                "Timeout waiting for signed PSBT. "
                "Expected file: " + signed_path
            );
        }

        if (progress_cb) {
            progress_cb(80, "Reading signed PSBT...");
        }

        // Step 3: Read signed PSBT
        std::ifstream infile(signed_path, std::ios::binary | std::ios::ate);
        if (!infile.is_open()) {
            return HWResult<PSBT>::Err("Failed to read signed PSBT: " + signed_path);
        }

        std::streamsize size = infile.tellg();
        infile.seekg(0, std::ios::beg);

        std::vector<uint8_t> signed_bytes(size);
        if (!infile.read(reinterpret_cast<char*>(signed_bytes.data()), size)) {
            return HWResult<PSBT>::Err("Failed to read signed PSBT file");
        }
        infile.close();

        // Step 4: Deserialize signed PSBT
        PSBT signed_psbt;
        if (!signed_psbt.Deserialize(signed_bytes)) {
            return HWResult<PSBT>::Err("Invalid signed PSBT format");
        }

        // Validate signed PSBT
        if (!signed_psbt.IsValid()) {
            return HWResult<PSBT>::Err("Signed PSBT validation failed: " + signed_psbt.GetError());
        }

        if (progress_cb) {
            progress_cb(100, "PSBT signed successfully");
        }

        return HWResult<PSBT>::Ok(signed_psbt);

    } catch (const std::exception& e) {
        return HWResult<PSBT>::Err(std::string("Error: ") + e.what());
    }
}

HWResult<bool> FileBasedHardwareWallet::DisplayAddress(
    const std::string& address,
    const std::string& derivation_path
) {
    // File-based devices have no screen
    return HWResult<bool>::Err("File-based devices do not support address display");
}

HWResult<std::string> FileBasedHardwareWallet::GetPublicKey(
    const std::string& derivation_path
) {
    // File-based devices don't support interactive key derivation
    // Keys must be provided in the PSBT metadata
    return HWResult<std::string>::Err(
        "File-based devices do not support interactive key derivation. "
        "Include xpub in PSBT global metadata."
    );
}

HWResult<uint32_t> FileBasedHardwareWallet::GetMasterFingerprint() {
    // File-based devices don't support interactive queries
    return HWResult<uint32_t>::Err(
        "File-based devices do not support interactive queries. "
        "Master fingerprint must be included in PSBT."
    );
}

// ============================================================================
// QRCodeHardwareWallet Implementation
// ============================================================================

QRCodeHardwareWallet::QRCodeHardwareWallet() {
    device_info_.type = DeviceType::GENERIC_PSBT;
    device_info_.transport = TransportType::QR_CODE;
    device_info_.manufacturer = "Generic";
    device_info_.model = "QR-Based PSBT";
    device_info_.initialized = true;

    device_info_.capabilities.supports_bitcoin = true;
    device_info_.capabilities.supports_segwit = true;
    device_info_.capabilities.supports_taproot = true;
    device_info_.capabilities.supports_multisig = true;
    device_info_.capabilities.supports_bip32_derivation = true;
    device_info_.capabilities.supports_display_address = true;  // Has screen
    device_info_.capabilities.supports_blind_signing = false;
    device_info_.capabilities.supports_message_signing = true;
}

HWResult<std::vector<DeviceInfo>> QRCodeHardwareWallet::EnumerateDevices() {
    std::vector<DeviceInfo> devices;

    // QR-based devices are always "available" (user-initiated)
    DeviceInfo info = device_info_;
    info.serial_number = "qr://generic";
    devices.push_back(info);

    return HWResult<std::vector<DeviceInfo>>::Ok(devices);
}

HWResult<bool> QRCodeHardwareWallet::Connect(const std::string& device_id) {
    // QR devices don't need explicit connection
    connected_ = true;
    return HWResult<bool>::Ok(true);
}

HWResult<bool> QRCodeHardwareWallet::Disconnect() {
    connected_ = false;
    return HWResult<bool>::Ok(true);
}

HWResult<DeviceInfo> QRCodeHardwareWallet::GetDeviceInfo() {
    return HWResult<DeviceInfo>::Ok(device_info_);
}

std::vector<std::string> QRCodeHardwareWallet::EncodeQRSequence(const PSBT& psbt) {
    // Serialize PSBT to bytes and encode as base64
    std::string psbt_b64 = psbt.ToBase64();

    // Split into chunks (QR codes have size limits)
    // Use 1000 chars per chunk for reliable scanning
    const size_t chunk_size = 1000;
    std::vector<std::string> chunks;

    for (size_t i = 0; i < psbt_b64.size(); i += chunk_size) {
        size_t len = std::min(chunk_size, psbt_b64.size() - i);
        chunks.push_back(psbt_b64.substr(i, len));
    }

    // Add sequence headers (for animated QR)
    std::vector<std::string> qr_sequence;
    for (size_t i = 0; i < chunks.size(); i++) {
        std::string header = "PSBT:" + std::to_string(i + 1) + "/" +
                            std::to_string(chunks.size()) + ":";
        qr_sequence.push_back(header + chunks[i]);
    }

    return qr_sequence;
}

HWResult<PSBT> QRCodeHardwareWallet::DecodeQRSequence(
    const std::vector<std::string>& qr_codes
) {
    try {
        // Reconstruct base64 from QR sequence
        std::string psbt_b64;

        for (const auto& qr : qr_codes) {
            // Remove sequence header (PSBT:N/M:)
            size_t data_start = qr.find_last_of(':');
            if (data_start == std::string::npos) {
                return HWResult<PSBT>::Err("Invalid QR code format");
            }
            psbt_b64 += qr.substr(data_start + 1);
        }

        // Decode base64 and deserialize PSBT
        PSBT psbt;
        try {
            psbt = PSBT::FromBase64(psbt_b64);
        } catch (const std::exception& e) {
            return HWResult<PSBT>::Err("Failed to decode PSBT: " + std::string(e.what()));
        }

        // Validate PSBT
        if (!psbt.IsValid()) {
            return HWResult<PSBT>::Err("PSBT validation failed: " + psbt.GetError());
        }

        return HWResult<PSBT>::Ok(psbt);

    } catch (const std::exception& e) {
        return HWResult<PSBT>::Err(std::string("Error: ") + e.what());
    }
}

HWResult<PSBT> QRCodeHardwareWallet::SignPSBT(
    const PSBT& psbt,
    const std::vector<std::string>& derivation_paths,
    ProgressCallback progress_cb
) {
    if (!connected_) {
        return HWResult<PSBT>::Err("Device not connected");
    }

    // QR signing is user-driven (display QR, scan signed QR)
    // This would typically be handled by GUI layer
    return HWResult<PSBT>::Err(
        "QR signing requires GUI interaction. "
        "Use EncodeQRSequence() to generate QR codes, "
        "then DecodeQRSequence() to read signed PSBT."
    );
}

HWResult<bool> QRCodeHardwareWallet::DisplayAddress(
    const std::string& address,
    const std::string& derivation_path
) {
    // QR devices can display addresses on their screen
    // Implementation would require GUI QR display
    return HWResult<bool>::Err("Address display requires GUI integration");
}

HWResult<std::string> QRCodeHardwareWallet::GetPublicKey(
    const std::string& derivation_path
) {
    return HWResult<std::string>::Err(
        "QR devices do not support interactive key queries. "
        "Export xpub via QR code from device."
    );
}

HWResult<uint32_t> QRCodeHardwareWallet::GetMasterFingerprint() {
    return HWResult<uint32_t>::Err(
        "QR devices do not support interactive queries. "
        "Export master fingerprint via QR code from device."
    );
}

// ============================================================================
// USB Hardware Wallet Constants and Helpers
// ============================================================================

#ifdef HAVE_USB_HWALLET

// Ledger USB Vendor/Product IDs
constexpr uint16_t LEDGER_VENDOR_ID = 0x2c97;
constexpr uint16_t LEDGER_NANO_S_PRODUCT_ID = 0x0001;
constexpr uint16_t LEDGER_NANO_X_PRODUCT_ID = 0x0004;
constexpr uint16_t LEDGER_NANO_S_PLUS_PRODUCT_ID = 0x0005;

// Trezor USB Vendor/Product IDs
constexpr uint16_t TREZOR_VENDOR_ID = 0x534c;
constexpr uint16_t TREZOR_VENDOR_ID_ALT = 0x1209;
constexpr uint16_t TREZOR_ONE_PRODUCT_ID = 0x0001;
constexpr uint16_t TREZOR_MODEL_T_PRODUCT_ID = 0x0002;
constexpr uint16_t TREZOR_ONE_PRODUCT_ID_ALT = 0x53c1;
constexpr uint16_t TREZOR_MODEL_T_PRODUCT_ID_ALT = 0x53c0;
constexpr uint16_t TREZOR_MODEL_T_PRODUCT_ID_ALT2 = 0x0006;

// Ledger APDU Constants
constexpr uint8_t LEDGER_CLA = 0xE0;  // Ledger instruction class
constexpr uint8_t LEDGER_INS_GET_MASTER_FINGERPRINT = 0x00;
constexpr uint8_t LEDGER_INS_GET_VERSION = 0x01;
constexpr uint8_t LEDGER_INS_GET_PUBKEY = 0x02;
constexpr uint8_t LEDGER_INS_SIGN_PSBT = 0x04;

// HID packet structure
constexpr size_t HID_PACKET_SIZE = 64;
constexpr uint8_t HID_REPORT_ID = 0x00;

// Helper: Convert bytes to hex string
static std::string BytesToHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

static bool IsRootDerivationPath(const std::string& path) {
    return path == "m" || path == "m/";
}

static std::vector<uint8_t> HexToBytes(const std::string& hex) {
    if ((hex.size() % 2) != 0) {
        return {};
    }

    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_value(hex[i]);
        const int lo = hex_value(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return {};
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

static std::vector<uint8_t> CompressSecp256k1Pubkey(const std::vector<uint8_t>& pubkey) {
    if (pubkey.size() == 33) {
        return pubkey;
    }
    if (pubkey.size() != 65 || pubkey[0] != 0x04) {
        return {};
    }

    std::vector<uint8_t> compressed(33);
    compressed[0] = (pubkey[64] & 0x01) ? 0x03 : 0x02;
    std::copy(pubkey.begin() + 1, pubkey.begin() + 33, compressed.begin() + 1);
    return compressed;
}

// Helper: Parse derivation path string to uint32 array
static std::vector<uint32_t> ParseDerivationPath(const std::string& path) {
    std::vector<uint32_t> result;
    if (path.empty() || path[0] != 'm') return result;
    if (IsRootDerivationPath(path)) return result;
    if (path.size() < 3 || path[1] != '/') return result;

    std::string p = path.substr(2);  // Skip "m/"
    size_t pos = 0;
    while ((pos = p.find('/')) != std::string::npos) {
        std::string token = p.substr(0, pos);
        if (token.empty()) return {};
        bool hardened = (token.back() == '\'' || token.back() == 'h');
        if (hardened) token.pop_back();
        if (token.empty()) return {};

        uint32_t value = std::stoul(token);
        if (hardened) value |= 0x80000000;
        result.push_back(value);

        p.erase(0, pos + 1);
    }

    // Last element
    if (!p.empty()) {
        bool hardened = (p.back() == '\'' || p.back() == 'h');
        if (hardened) p.pop_back();
        if (p.empty()) return {};
        uint32_t value = std::stoul(p);
        if (hardened) value |= 0x80000000;
        result.push_back(value);
    }

    return result;
}

#endif  // HAVE_USB_HWALLET

// ============================================================================
// USBHardwareWallet Implementation
// ============================================================================

USBHardwareWallet::USBHardwareWallet(DeviceType type) : device_type_(type), hid_device_(nullptr) {
    device_info_.type = type;
    device_info_.transport = TransportType::USB_HID;
    device_info_.initialized = false;

    // Set manufacturer/model based on type
    switch (type) {
        case DeviceType::LEDGER_NANO_S:
            device_info_.manufacturer = "Ledger";
            device_info_.model = "Nano S";
            break;
        case DeviceType::LEDGER_NANO_X:
            device_info_.manufacturer = "Ledger";
            device_info_.model = "Nano X";
            break;
        case DeviceType::TREZOR_ONE:
            device_info_.manufacturer = "Trezor";
            device_info_.model = "One";
            break;
        case DeviceType::TREZOR_MODEL_T:
            device_info_.manufacturer = "Trezor";
            device_info_.model = "Model T";
            break;
        default:
            device_info_.manufacturer = "Unknown";
            device_info_.model = "Unknown";
    }
}

HWResult<std::vector<DeviceInfo>> USBHardwareWallet::EnumerateDevices() {
#ifdef HAVE_USB_HWALLET
    std::vector<DeviceInfo> devices;

    // Initialize hidapi
    if (hid_init() != 0) {
        return HWResult<std::vector<DeviceInfo>>::Err("Failed to initialize HIDAPI");
    }

    // Enumerate all HID devices
    struct hid_device_info* devs = hid_enumerate(0x0, 0x0);
    struct hid_device_info* cur_dev = devs;

    while (cur_dev) {
        DeviceInfo info;
        info.transport = TransportType::USB_HID;
        info.initialized = false;

        // Check if it's a Ledger device
        if (cur_dev->vendor_id == LEDGER_VENDOR_ID) {
            info.manufacturer = "Ledger";
            if (cur_dev->product_id == LEDGER_NANO_S_PRODUCT_ID) {
                info.type = DeviceType::LEDGER_NANO_S;
                info.model = "Nano S";
            } else if (cur_dev->product_id == LEDGER_NANO_X_PRODUCT_ID) {
                info.type = DeviceType::LEDGER_NANO_X;
                info.model = "Nano X";
            } else if (cur_dev->product_id == LEDGER_NANO_S_PLUS_PRODUCT_ID) {
                info.type = DeviceType::LEDGER_NANO_S_PLUS;
                info.model = "Nano S Plus";
            } else {
                info.type = DeviceType::GENERIC_USB;
                info.model = "Unknown Ledger";
            }

            // Get device path as unique ID
            if (cur_dev->path) {
                info.device_id = std::string(cur_dev->path);
            }

            // Get serial number if available
            if (cur_dev->serial_number) {
                // Convert wide string to regular string
                std::wstring ws(cur_dev->serial_number);
                info.serial_number = std::string(ws.begin(), ws.end());
            }

            info.firmware_version = "Unknown";  // Will be fetched on connect
            devices.push_back(info);
        }
        // Check if it's a Trezor device
        else if (cur_dev->vendor_id == TREZOR_VENDOR_ID ||
                 cur_dev->vendor_id == TREZOR_VENDOR_ID_ALT) {
            info.manufacturer = "Trezor";
            if (cur_dev->product_id == TREZOR_ONE_PRODUCT_ID ||
                cur_dev->product_id == TREZOR_ONE_PRODUCT_ID_ALT) {
                info.type = DeviceType::TREZOR_ONE;
                info.model = "One";
            } else if (cur_dev->product_id == TREZOR_MODEL_T_PRODUCT_ID ||
                       cur_dev->product_id == TREZOR_MODEL_T_PRODUCT_ID_ALT ||
                       cur_dev->product_id == TREZOR_MODEL_T_PRODUCT_ID_ALT2) {
                info.type = DeviceType::TREZOR_MODEL_T;
                info.model = "Model T";
            } else {
                info.type = DeviceType::GENERIC_USB;
                info.model = "Unknown Trezor";
            }

            if (cur_dev->path) {
                info.device_id = std::string(cur_dev->path);
            }

            if (cur_dev->serial_number) {
                std::wstring ws(cur_dev->serial_number);
                info.serial_number = std::string(ws.begin(), ws.end());
            }

            info.firmware_version = "Unknown";
            devices.push_back(info);
        }

        cur_dev = cur_dev->next;
    }

    hid_free_enumeration(devs);

    return HWResult<std::vector<DeviceInfo>>::Ok(devices);
#else
    return HWResult<std::vector<DeviceInfo>>::Err(
        "USB support not compiled. Build with -DENABLE_HARDWARE_WALLETS=ON"
    );
#endif
}

HWResult<bool> USBHardwareWallet::Connect(const std::string& device_id) {
#ifdef HAVE_USB_HWALLET
    if (connected_) {
        return HWResult<bool>::Err("Already connected");
    }

    if (device_id.empty()) {
        return HWResult<bool>::Err("Device identifier required");
    }

    auto devices_result = EnumerateDevices();
    if (!devices_result.success) {
        return HWResult<bool>::Err("Failed to enumerate devices before connect: " + devices_result.error_message);
    }

    auto device_it = std::find_if(
        devices_result.value.begin(),
        devices_result.value.end(),
        [&device_id](const DeviceInfo& info) {
            return info.device_id == device_id;
        });
    if (device_it == devices_result.value.end()) {
        return HWResult<bool>::Err("Requested device is no longer present: " + device_id);
    }

    device_type_ = device_it->type;
    device_info_ = *device_it;

    // Initialize hidapi
    if (hid_init() != 0) {
        return HWResult<bool>::Err("Failed to initialize HIDAPI");
    }

    // Open device by path
    hid_device* dev = hid_open_path(device_id.c_str());
    if (!dev) {
        return HWResult<bool>::Err("Failed to open device: " + device_id);
    }

    hid_device_ = dev;
    connected_ = true;
    device_info_.initialized = true;

    // For Ledger devices, get firmware version
    if (device_info_.manufacturer == "Ledger") {
        auto version_result = GetLedgerVersion();
        if (version_result.success) {
            device_info_.firmware_version = version_result.value;
        }
    }

    return HWResult<bool>::Ok(true);
#else
    return HWResult<bool>::Err("USB support not compiled");
#endif
}

HWResult<bool> USBHardwareWallet::Disconnect() {
#ifdef HAVE_USB_HWALLET
    if (!connected_ || !hid_device_) {
        return HWResult<bool>::Err("Not connected");
    }

    hid_close(static_cast<hid_device*>(hid_device_));
    hid_device_ = nullptr;
    connected_ = false;
    device_info_.initialized = false;

    return HWResult<bool>::Ok(true);
#else
    return HWResult<bool>::Err("USB support not compiled");
#endif
}

HWResult<DeviceInfo> USBHardwareWallet::GetDeviceInfo() {
    if (!connected_) {
        return HWResult<DeviceInfo>::Err("Not connected");
    }
    return HWResult<DeviceInfo>::Ok(device_info_);
}

HWResult<PSBT> USBHardwareWallet::SignPSBT(
    const PSBT& psbt,
    const std::vector<std::string>& derivation_paths,
    ProgressCallback progress_cb
) {
#ifdef HAVE_USB_HWALLET
    if (!connected_) {
        return HWResult<PSBT>::Err("Device not connected");
    }

    // Serialize PSBT
    auto psbt_bytes = psbt.Serialize();
    if (psbt_bytes.empty()) {
        return HWResult<PSBT>::Err("Failed to serialize PSBT");
    }

    // Build APDU for Sign PSBT
    // Note: This is a simplified implementation
    // Real Ledger PSBT signing requires multiple APDU exchanges
    std::vector<uint8_t> apdu;
    apdu.push_back(LEDGER_CLA);
    apdu.push_back(LEDGER_INS_SIGN_PSBT);
    apdu.push_back(0x00);  // P1: first block
    apdu.push_back(0x00);  // P2: continuation

    // For large PSBTs, we'd need to chunk the data
    // This is a placeholder implementation
    if (psbt_bytes.size() > 200) {
        return HWResult<PSBT>::Err(
            "PSBT too large for single APDU. Multi-APDU signing not yet implemented."
        );
    }

    apdu.push_back(static_cast<uint8_t>(psbt_bytes.size()));
    apdu.insert(apdu.end(), psbt_bytes.begin(), psbt_bytes.end());

    auto result = SendAPDU(apdu);
    if (!result.success) {
        return HWResult<PSBT>::Err("Failed to sign PSBT: " + result.error_message);
    }

    auto& response = result.value;
    if (response.size() < 2) {
        return HWResult<PSBT>::Err("Invalid response length");
    }

    // Check status word
    uint16_t sw = (response[response.size() - 2] << 8) | response[response.size() - 1];
    if (sw != 0x9000) {
        if (sw == 0x6985) {
            return HWResult<PSBT>::Err("User denied signing on device");
        }
        return HWResult<PSBT>::Err("Device returned error: " + std::to_string(sw));
    }

    // Parse signed PSBT from response
    // Remove status word
    std::vector<uint8_t> signed_bytes(response.begin(), response.end() - 2);

    PSBT signed_psbt;
    if (!signed_psbt.Deserialize(signed_bytes)) {
        return HWResult<PSBT>::Err("Failed to deserialize signed PSBT");
    }

    if (progress_cb) {
        progress_cb(100, "Signing complete");
    }

    return HWResult<PSBT>::Ok(signed_psbt);
#else
    return HWResult<PSBT>::Err("USB support not compiled");
#endif
}

HWResult<bool> USBHardwareWallet::DisplayAddress(
    const std::string& address,
    const std::string& derivation_path
) {
#ifdef HAVE_USB_HWALLET
    if (!connected_) {
        return HWResult<bool>::Err("Device not connected");
    }

    // Parse derivation path
    auto path_elements = ParseDerivationPath(derivation_path);
    if (path_elements.empty()) {
        return HWResult<bool>::Err("Invalid derivation path: " + derivation_path);
    }

    // Build APDU for Display Address
    // CLA INS P1 P2 LC [path_len] [path]
    std::vector<uint8_t> apdu;
    apdu.push_back(LEDGER_CLA);
    apdu.push_back(LEDGER_INS_GET_PUBKEY);  // Same as get pubkey
    apdu.push_back(0x01);  // P1: display address on screen
    apdu.push_back(0x00);  // P2: require user confirmation

    // Data length
    uint8_t data_len = 1 + (path_elements.size() * 4);
    apdu.push_back(data_len);

    // Path length
    apdu.push_back(static_cast<uint8_t>(path_elements.size()));

    // Path elements (big-endian)
    for (uint32_t element : path_elements) {
        apdu.push_back(static_cast<uint8_t>(element >> 24));
        apdu.push_back(static_cast<uint8_t>(element >> 16));
        apdu.push_back(static_cast<uint8_t>(element >> 8));
        apdu.push_back(static_cast<uint8_t>(element & 0xFF));
    }

    auto result = SendAPDU(apdu);
    if (!result.success) {
        return HWResult<bool>::Err("Failed to display address: " + result.error_message);
    }

    auto& response = result.value;
    if (response.size() < 2) {
        return HWResult<bool>::Err("Invalid response length");
    }

    // Check status word
    uint16_t sw = (response[response.size() - 2] << 8) | response[response.size() - 1];
    if (sw != 0x9000) {
        if (sw == 0x6985) {
            return HWResult<bool>::Err("User rejected address verification");
        }
        return HWResult<bool>::Err("Device returned error: " + std::to_string(sw));
    }

    return HWResult<bool>::Ok(true);
#else
    return HWResult<bool>::Err("USB support not compiled");
#endif
}

HWResult<std::string> USBHardwareWallet::GetPublicKey(const std::string& derivation_path) {
#ifdef HAVE_USB_HWALLET
    if (!connected_) {
        return HWResult<std::string>::Err("Device not connected");
    }

    // Parse derivation path
    auto path_elements = ParseDerivationPath(derivation_path);
    if (path_elements.empty() && !IsRootDerivationPath(derivation_path)) {
        return HWResult<std::string>::Err("Invalid derivation path: " + derivation_path);
    }

    // Build APDU for Get Public Key
    // CLA INS P1 P2 LC [path_len] [path] [display]
    std::vector<uint8_t> apdu;
    apdu.push_back(LEDGER_CLA);
    apdu.push_back(LEDGER_INS_GET_PUBKEY);
    apdu.push_back(0x00);  // P1: return public key
    apdu.push_back(0x00);  // P2: no user confirmation

    // Data length (1 byte path length + 4 bytes per element)
    uint8_t data_len = 1 + (path_elements.size() * 4);
    apdu.push_back(data_len);

    // Path length
    apdu.push_back(static_cast<uint8_t>(path_elements.size()));

    // Path elements (big-endian)
    for (uint32_t element : path_elements) {
        apdu.push_back(static_cast<uint8_t>(element >> 24));
        apdu.push_back(static_cast<uint8_t>(element >> 16));
        apdu.push_back(static_cast<uint8_t>(element >> 8));
        apdu.push_back(static_cast<uint8_t>(element & 0xFF));
    }

    auto result = SendAPDU(apdu);
    if (!result.success) {
        return HWResult<std::string>::Err("Failed to get public key: " + result.error_message);
    }

    auto& response = result.value;
    if (response.size() < 2) {
        return HWResult<std::string>::Err("Invalid response length");
    }

    // Check status word
    uint16_t sw = (response[response.size() - 2] << 8) | response[response.size() - 1];
    if (sw != 0x9000) {
        return HWResult<std::string>::Err("Device returned error: " + std::to_string(sw));
    }

    // Parse public key from response (typically 65 bytes for uncompressed key)
    if (response.size() < 67) {  // 65 bytes pubkey + 2 bytes SW
        return HWResult<std::string>::Err("Response too short for public key");
    }

    // Extract public key (skip first byte which is length)
    size_t pubkey_len = response[0];
    if (pubkey_len != 65) {
        return HWResult<std::string>::Err("Unexpected public key length: " + std::to_string(pubkey_len));
    }

    // Convert to hex string
    std::string pubkey_hex = BytesToHex(&response[1], pubkey_len);

    return HWResult<std::string>::Ok(pubkey_hex);
#else
    return HWResult<std::string>::Err("USB support not compiled");
#endif
}

HWResult<uint32_t> USBHardwareWallet::GetMasterFingerprint() {
#ifdef HAVE_USB_HWALLET
    if (!connected_) {
        return HWResult<uint32_t>::Err("Device not connected");
    }

    std::vector<uint8_t> apdu;
    apdu.push_back(LEDGER_CLA);
    apdu.push_back(LEDGER_INS_GET_MASTER_FINGERPRINT);
    apdu.push_back(0x00);
    apdu.push_back(0x00);
    apdu.push_back(0x00);

    auto direct_result = SendAPDU(apdu);
    if (direct_result.success) {
        const auto& response = direct_result.value;
        if (response.size() >= 6) {
            const uint16_t sw = (response[response.size() - 2] << 8) | response[response.size() - 1];
            if (sw == 0x9000) {
                const uint32_t fingerprint =
                    (static_cast<uint32_t>(response[0]) << 24) |
                    (static_cast<uint32_t>(response[1]) << 16) |
                    (static_cast<uint32_t>(response[2]) << 8) |
                    static_cast<uint32_t>(response[3]);
                return HWResult<uint32_t>::Ok(fingerprint);
            }
        }
    }

    auto pubkey_result = GetPublicKey("m");
    if (!pubkey_result.success) {
        return HWResult<uint32_t>::Err("Failed to get master fingerprint");
    }

    auto pubkey_bytes = HexToBytes(pubkey_result.value);
    if (pubkey_bytes.empty()) {
        return HWResult<uint32_t>::Err("Device returned invalid public key encoding");
    }

    auto compressed_pubkey = CompressSecp256k1Pubkey(pubkey_bytes);
    if (compressed_pubkey.empty()) {
        return HWResult<uint32_t>::Err("Device returned unsupported public key shape");
    }

    const auto hash160 = din::crypto::HASH160(compressed_pubkey);
    const uint32_t fingerprint =
        (static_cast<uint32_t>(hash160[0]) << 24) |
        (static_cast<uint32_t>(hash160[1]) << 16) |
        (static_cast<uint32_t>(hash160[2]) << 8) |
        static_cast<uint32_t>(hash160[3]);
    return HWResult<uint32_t>::Ok(fingerprint);
#else
    return HWResult<uint32_t>::Err("USB support not compiled");
#endif
}

// ============================================================================
// USB Helper Methods (Ledger APDU Protocol)
// ============================================================================

#ifdef HAVE_USB_HWALLET

HWResult<std::vector<uint8_t>> USBHardwareWallet::SendAPDU(const std::vector<uint8_t>& apdu) {
    if (!connected_ || !hid_device_) {
        return HWResult<std::vector<uint8_t>>::Err("Device not connected");
    }

    hid_device* dev = static_cast<hid_device*>(hid_device_);

    // Ledger HID protocol: wrap APDU in HID packets
    std::vector<uint8_t> packet(HID_PACKET_SIZE, 0);
    packet[0] = 0x00;  // Report ID
    packet[1] = 0x05;  // TAG: APDU
    packet[2] = 0x00;  // Sequence index (high byte)
    packet[3] = 0x00;  // Sequence index (low byte)
    packet[4] = static_cast<uint8_t>(apdu.size() >> 8);  // Data length (high byte)
    packet[5] = static_cast<uint8_t>(apdu.size() & 0xFF); // Data length (low byte)

    // Copy APDU data into packet (max 58 bytes in first packet after 6 byte header)
    size_t to_copy = std::min(apdu.size(), size_t(58));
    std::memcpy(&packet[6], apdu.data(), to_copy);

    // Send first packet
    int res = hid_write(dev, packet.data(), HID_PACKET_SIZE);
    if (res < 0) {
        return HWResult<std::vector<uint8_t>>::Err("Failed to send APDU packet");
    }

    // If APDU is larger than 58 bytes, send continuation packets
    size_t sent = to_copy;
    uint16_t seq = 0;
    while (sent < apdu.size()) {
        seq++;
        packet.assign(HID_PACKET_SIZE, 0);
        packet[0] = 0x00;  // Report ID
        packet[1] = 0x05;  // TAG: APDU
        packet[2] = static_cast<uint8_t>(seq >> 8);
        packet[3] = static_cast<uint8_t>(seq & 0xFF);

        to_copy = std::min(apdu.size() - sent, size_t(60));  // 60 bytes data in continuation packets
        std::memcpy(&packet[4], apdu.data() + sent, to_copy);

        res = hid_write(dev, packet.data(), HID_PACKET_SIZE);
        if (res < 0) {
            return HWResult<std::vector<uint8_t>>::Err("Failed to send APDU continuation");
        }
        sent += to_copy;
    }

    return ReceiveAPDU();
}

HWResult<std::vector<uint8_t>> USBHardwareWallet::ReceiveAPDU() {
    if (!connected_ || !hid_device_) {
        return HWResult<std::vector<uint8_t>>::Err("Device not connected");
    }

    hid_device* dev = static_cast<hid_device*>(hid_device_);
    std::vector<uint8_t> response;

    uint8_t packet[HID_PACKET_SIZE];
    int res = hid_read_timeout(dev, packet, HID_PACKET_SIZE, 5000);  // 5 second timeout

    if (res < 0) {
        return HWResult<std::vector<uint8_t>>::Err("Failed to read APDU response");
    }

    if (res == 0) {
        return HWResult<std::vector<uint8_t>>::Err("Timeout reading APDU response");
    }

    // Parse first packet
    if (packet[1] != 0x05) {  // TAG must be APDU
        return HWResult<std::vector<uint8_t>>::Err("Invalid response tag");
    }

    uint16_t data_len = (packet[4] << 8) | packet[5];
    size_t to_read = std::min(size_t(data_len), size_t(58));

    response.insert(response.end(), packet + 6, packet + 6 + to_read);

    // Read continuation packets if needed
    size_t received = to_read;
    while (received < data_len) {
        res = hid_read_timeout(dev, packet, HID_PACKET_SIZE, 5000);
        if (res <= 0) {
            return HWResult<std::vector<uint8_t>>::Err("Failed to read APDU continuation");
        }

        to_read = std::min(size_t(data_len - received), size_t(60));
        response.insert(response.end(), packet + 4, packet + 4 + to_read);
        received += to_read;
    }

    return HWResult<std::vector<uint8_t>>::Ok(response);
}

HWResult<std::string> USBHardwareWallet::GetLedgerVersion() {
    if (!connected_) {
        return HWResult<std::string>::Err("Device not connected");
    }

    // Build APDU: CLA INS P1 P2 LC
    std::vector<uint8_t> apdu = {LEDGER_CLA, LEDGER_INS_GET_VERSION, 0x00, 0x00, 0x00};

    auto result = SendAPDU(apdu);
    if (!result.success) {
        return HWResult<std::string>::Err("Failed to get version: " + result.error_message);
    }

    auto& response = result.value;
    if (response.size() < 2) {
        return HWResult<std::string>::Err("Invalid response length");
    }

    // Check status word (last 2 bytes should be 0x9000 for success)
    uint16_t sw = (response[response.size() - 2] << 8) | response[response.size() - 1];
    if (sw != 0x9000) {
        return HWResult<std::string>::Err("Device returned error: " + std::to_string(sw));
    }

    // Parse version from response data
    if (response.size() >= 5) {
        std::ostringstream version;
        version << static_cast<int>(response[0]) << "."
                << static_cast<int>(response[1]) << "."
                << static_cast<int>(response[2]);
        return HWResult<std::string>::Ok(version.str());
    }

    return HWResult<std::string>::Ok("Unknown");
}

#endif  // HAVE_USB_HWALLET

// ============================================================================
// HardwareWalletFactory Implementation
// ============================================================================

std::unique_ptr<IHardwareWallet> HardwareWalletFactory::Create(TransportType transport) {
    switch (transport) {
        case TransportType::FILE_SYSTEM:
        case TransportType::SD_CARD:
            return std::make_unique<FileBasedHardwareWallet>("/tmp/hw_export", "/tmp/hw_import");

        case TransportType::QR_CODE:
            return std::make_unique<QRCodeHardwareWallet>();

        case TransportType::USB_HID:
        case TransportType::USB_WEBUSB:
            // Default to Ledger for USB (most common)
            return std::make_unique<USBHardwareWallet>(DeviceType::LEDGER_NANO_S);

        default:
            return nullptr;
    }
}

std::vector<std::pair<TransportType, DeviceInfo>> HardwareWalletFactory::DetectDevices() {
    std::vector<std::pair<TransportType, DeviceInfo>> detected;

    // Try all transport types
    std::vector<TransportType> transports = {
        TransportType::FILE_SYSTEM,
        TransportType::QR_CODE,
        TransportType::USB_HID
    };

    for (auto transport : transports) {
        auto wallet = Create(transport);
        if (wallet) {
            auto result = wallet->EnumerateDevices();
            if (result.success) {
                for (const auto& device : result.value) {
                    detected.push_back({transport, device});
                }
            }
        }
    }

    return detected;
}

// Static registry for custom device factories
static std::map<TransportType, std::function<std::unique_ptr<IHardwareWallet>()>> custom_factories_;

void HardwareWalletFactory::RegisterDevice(
    TransportType transport,
    std::function<std::unique_ptr<IHardwareWallet>()> factory
) {
    custom_factories_[transport] = factory;
}

} // namespace hw
} // namespace dinero
