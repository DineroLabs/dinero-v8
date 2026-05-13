/**
 * Universal Hardware Wallet Interface
 *
 * Provides a unified API for interacting with hardware wallets regardless of connection method:
 * - USB (Ledger, Trezor, KeepKey, BitBox)
 * - SD Card (Coldcard)
 * - Bluetooth (Ledger Nano X)
 * - QR Code (AirGap wallets, Keystone, Passport)
 * - Mobile App (Ledger Live, Trezor Suite)
 *
 * Design Philosophy:
 * - Transport-agnostic: Same API works for all devices
 * - PSBT-first: All operations use BIP 174 PSBT standard
 * - Plugin architecture: Easy to add new device support
 * - Async-ready: Non-blocking operations for GUI
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include "wallet/psbt.h"

namespace dinero {
namespace hw {

// Transport types
enum class TransportType {
    USB_HID,        // USB Human Interface Device (Ledger, Trezor)
    USB_WEBUSB,     // WebUSB (modern Ledger, Trezor)
    BLUETOOTH,      // Bluetooth LE (Ledger Nano X)
    SD_CARD,        // SD card files (Coldcard)
    QR_CODE,        // Animated QR codes (AirGap, Keystone, Passport)
    FILE_SYSTEM,    // File-based (generic)
    MOBILE_APP,     // Mobile app bridge (Ledger Live, Trezor Suite)
    NETWORK         // Network-based (experimental)
};

// Device types
enum class DeviceType {
    LEDGER_NANO_S,
    LEDGER_NANO_X,
    LEDGER_NANO_S_PLUS,
    TREZOR_ONE,
    TREZOR_MODEL_T,
    COLDCARD_MK3,
    COLDCARD_MK4,
    KEEPKEY,
    BITBOX02,
    KEYSTONE,
    PASSPORT,
    AIRGAP_VAULT,
    GENERIC_PSBT,
    GENERIC_USB,    // Unknown USB device
    UNKNOWN
};

// Device capabilities
struct DeviceCapabilities {
    bool supports_bitcoin = false;
    bool supports_segwit = false;
    bool supports_taproot = false;
    bool supports_multisig = false;
    bool supports_bip32_derivation = false;
    bool supports_display_address = false;
    bool supports_blind_signing = false;
    bool supports_message_signing = false;
    std::vector<std::string> supported_coin_types; // BIP 44 coin types
};

// Device information
struct DeviceInfo {
    DeviceType type = DeviceType::UNKNOWN;
    TransportType transport = TransportType::FILE_SYSTEM;
    std::string device_id;           // Unique device identifier (path/serial)
    std::string manufacturer;
    std::string model;
    std::string serial_number;
    std::string firmware_version;
    bool initialized = false;
    bool bootloader_mode = false;
    bool pin_cached = false;
    DeviceCapabilities capabilities;
};

// Operation result
template<typename T>
struct HWResult {
    bool success = false;
    T value;
    std::string error_message;
    int error_code = 0;

    static HWResult<T> Ok(const T& val) {
        HWResult<T> r;
        r.success = true;
        r.value = val;
        return r;
    }

    static HWResult<T> Err(const std::string& msg, int code = -1) {
        HWResult<T> r;
        r.success = false;
        r.error_message = msg;
        r.error_code = code;
        return r;
    }
};

// Async callback types
using ProgressCallback = std::function<void(int percent, const std::string& message)>;
using PinCallback = std::function<std::string()>;  // Called when device needs PIN
using PassphraseCallback = std::function<std::string()>;  // Called for BIP 39 passphrase
using ConfirmCallback = std::function<bool(const std::string& message)>;  // User confirmation

/**
 * Base Hardware Wallet Interface
 *
 * All hardware wallet implementations must inherit from this base class.
 */
class IHardwareWallet {
public:
    virtual ~IHardwareWallet() = default;

    // === Device Management ===

    /**
     * Enumerate available devices
     * Returns list of connected devices for this transport type
     */
    virtual HWResult<std::vector<DeviceInfo>> EnumerateDevices() = 0;

    /**
     * Connect to a specific device
     * @param device_id - Device identifier (serial number, path, etc.)
     */
    virtual HWResult<bool> Connect(const std::string& device_id = "") = 0;

    /**
     * Disconnect from device
     */
    virtual HWResult<bool> Disconnect() = 0;

    /**
     * Get device information
     */
    virtual HWResult<DeviceInfo> GetDeviceInfo() = 0;

    /**
     * Check if device is connected and ready
     */
    virtual bool IsConnected() const = 0;

    // === PSBT Operations ===

    /**
     * Sign a PSBT using the hardware wallet
     *
     * @param psbt - Partially Signed Bitcoin Transaction
     * @param derivation_paths - Optional BIP 32 derivation paths for signing keys
     * @param progress_cb - Optional progress callback
     * @return Signed PSBT (may be partially signed if not all keys are on device)
     */
    virtual HWResult<PSBT> SignPSBT(
        const PSBT& psbt,
        const std::vector<std::string>& derivation_paths = {},
        ProgressCallback progress_cb = nullptr
    ) = 0;

    /**
     * Display address on device screen for verification
     *
     * @param address - Address to display
     * @param derivation_path - BIP 32 path for the address
     * @return True if user confirmed address on device
     */
    virtual HWResult<bool> DisplayAddress(
        const std::string& address,
        const std::string& derivation_path
    ) = 0;

    /**
     * Get public key from device
     *
     * @param derivation_path - BIP 32 derivation path
     * @return Extended public key (xpub)
     */
    virtual HWResult<std::string> GetPublicKey(
        const std::string& derivation_path
    ) = 0;

    /**
     * Get master fingerprint
     * Used for PSBT signing to identify which keys belong to this device
     */
    virtual HWResult<uint32_t> GetMasterFingerprint() = 0;

    // === Callbacks ===

    /**
     * Set callback for PIN entry (for PIN-protected devices)
     */
    virtual void SetPinCallback(PinCallback callback) {
        pin_callback_ = callback;
    }

    /**
     * Set callback for passphrase entry (for BIP 39 passphrase)
     */
    virtual void SetPassphraseCallback(PassphraseCallback callback) {
        passphrase_callback_ = callback;
    }

    /**
     * Set callback for user confirmation
     */
    virtual void SetConfirmCallback(ConfirmCallback callback) {
        confirm_callback_ = callback;
    }

protected:
    PinCallback pin_callback_;
    PassphraseCallback passphrase_callback_;
    ConfirmCallback confirm_callback_;
};

/**
 * Hardware Wallet Factory
 *
 * Creates hardware wallet instances based on transport type
 */
class HardwareWalletFactory {
public:
    /**
     * Create hardware wallet instance for given transport
     */
    static std::unique_ptr<IHardwareWallet> Create(TransportType transport);

    /**
     * Auto-detect and return all available devices
     */
    static std::vector<std::pair<TransportType, DeviceInfo>> DetectDevices();

    /**
     * Register a custom hardware wallet implementation
     */
    static void RegisterDevice(
        TransportType transport,
        std::function<std::unique_ptr<IHardwareWallet>()> factory
    );
};

/**
 * File-Based Hardware Wallet (SD Card / File System)
 *
 * For devices like Coldcard that use file-based PSBT exchange
 */
class FileBasedHardwareWallet : public IHardwareWallet {
public:
    FileBasedHardwareWallet(const std::string& export_dir, const std::string& import_dir);

    HWResult<std::vector<DeviceInfo>> EnumerateDevices() override;
    HWResult<bool> Connect(const std::string& device_id = "") override;
    HWResult<bool> Disconnect() override;
    HWResult<DeviceInfo> GetDeviceInfo() override;
    bool IsConnected() const override { return connected_; }

    HWResult<PSBT> SignPSBT(
        const PSBT& psbt,
        const std::vector<std::string>& derivation_paths = {},
        ProgressCallback progress_cb = nullptr
    ) override;

    HWResult<bool> DisplayAddress(
        const std::string& address,
        const std::string& derivation_path
    ) override;

    HWResult<std::string> GetPublicKey(const std::string& derivation_path) override;
    HWResult<uint32_t> GetMasterFingerprint() override;

    // File-specific methods
    std::string GetExportPath() const { return export_dir_; }
    std::string GetImportPath() const { return import_dir_; }
    void SetExportPath(const std::string& path) { export_dir_ = path; }
    void SetImportPath(const std::string& path) { import_dir_ = path; }

private:
    std::string export_dir_;  // Where to write unsigned PSBTs
    std::string import_dir_;  // Where to read signed PSBTs
    bool connected_ = false;
    DeviceInfo device_info_;
};

/**
 * QR Code Hardware Wallet
 *
 * For devices like Keystone, Passport, AirGap that use animated QR codes
 */
class QRCodeHardwareWallet : public IHardwareWallet {
public:
    QRCodeHardwareWallet();

    HWResult<std::vector<DeviceInfo>> EnumerateDevices() override;
    HWResult<bool> Connect(const std::string& device_id = "") override;
    HWResult<bool> Disconnect() override;
    HWResult<DeviceInfo> GetDeviceInfo() override;
    bool IsConnected() const override { return connected_; }

    HWResult<PSBT> SignPSBT(
        const PSBT& psbt,
        const std::vector<std::string>& derivation_paths = {},
        ProgressCallback progress_cb = nullptr
    ) override;

    HWResult<bool> DisplayAddress(
        const std::string& address,
        const std::string& derivation_path
    ) override;

    HWResult<std::string> GetPublicKey(const std::string& derivation_path) override;
    HWResult<uint32_t> GetMasterFingerprint() override;

    // QR-specific methods
    std::vector<std::string> EncodeQRSequence(const PSBT& psbt);
    HWResult<PSBT> DecodeQRSequence(const std::vector<std::string>& qr_codes);

private:
    bool connected_ = false;
    DeviceInfo device_info_;
};

/**
 * USB Hardware Wallet
 *
 * For devices like Ledger, Trezor that connect via USB HID or WebUSB
 *
 * Note: This is a stub. Actual implementation would require:
 * - libusb or hidapi for USB communication
 * - Device-specific protocol implementation (APDU for Ledger, Protobuf for Trezor)
 * - Platform-specific USB permissions handling
 */
class USBHardwareWallet : public IHardwareWallet {
public:
    USBHardwareWallet(DeviceType type);

    HWResult<std::vector<DeviceInfo>> EnumerateDevices() override;
    HWResult<bool> Connect(const std::string& device_id = "") override;
    HWResult<bool> Disconnect() override;
    HWResult<DeviceInfo> GetDeviceInfo() override;
    bool IsConnected() const override { return connected_; }

    HWResult<PSBT> SignPSBT(
        const PSBT& psbt,
        const std::vector<std::string>& derivation_paths = {},
        ProgressCallback progress_cb = nullptr
    ) override;

    HWResult<bool> DisplayAddress(
        const std::string& address,
        const std::string& derivation_path
    ) override;

    HWResult<std::string> GetPublicKey(const std::string& derivation_path) override;
    HWResult<uint32_t> GetMasterFingerprint() override;

private:
    DeviceType device_type_;
    bool connected_ = false;
    DeviceInfo device_info_;
    void* hid_device_ = nullptr;  // hidapi device handle (hid_device*)

    // Helper methods for USB communication
    #ifdef HAVE_USB_HWALLET
    HWResult<std::vector<uint8_t>> SendAPDU(const std::vector<uint8_t>& apdu);
    HWResult<std::vector<uint8_t>> ReceiveAPDU();
    HWResult<std::string> GetLedgerVersion();
    #endif
};

} // namespace hw
} // namespace dinero
