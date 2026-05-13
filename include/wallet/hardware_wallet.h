#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <array>
#include <map>
#include <atomic>
#include <thread>
#include <mutex>

namespace dinero {
class HIDTransport;  // Forward declaration

// Hardware wallet types
enum class HardwareWalletType {
    LEDGER,
    TREZOR,
    UNKNOWN
};

// Hardware wallet connection status
enum class ConnectionStatus {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    READY,
    ERROR
};

// Hardware wallet device information
struct DeviceInfo {
    std::string name;
    std::string serial_number;
    std::string firmware_version;
    HardwareWalletType type;
    bool supports_dinero;
    std::string app_version;
    
    DeviceInfo() : type(HardwareWalletType::UNKNOWN), supports_dinero(false) {}
};

// Hardware wallet address information
struct AddressInfo {
    std::string address;
    std::string public_key;
    std::string derivation_path;
    uint32_t index;
    bool is_change;
    
    AddressInfo() : index(0), is_change(false) {}
};

// Hardware wallet transaction signing request
struct SigningRequest {
    std::string transaction_hash;
    std::vector<std::string> input_addresses;
    std::vector<std::string> output_addresses;
    std::vector<uint64_t> input_amounts;
    std::vector<uint64_t> output_amounts;
    std::vector<std::string> derivation_paths;
    uint64_t fee;
    uint32_t lockTime;

    SigningRequest() : fee(0), lockTime(0) {}
};

// Hardware wallet signing result
struct SigningResult {
    bool success;
    std::vector<std::string> signatures;
    std::string error_message;
    std::string signed_transaction;
    
    SigningResult() : success(false) {}
};

// Hardware wallet callback types
using DeviceCallback = std::function<void(const DeviceInfo&)>;
using AddressCallback = std::function<void(const AddressInfo&)>;
using SigningCallback = std::function<void(const SigningResult&)>;
using StatusCallback = std::function<void(ConnectionStatus)>;

// Abstract base class for hardware wallet implementations
class HardwareWallet {
public:
    virtual ~HardwareWallet() = default;
    
    // Core functionality
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual bool isConnected() const = 0;
    virtual ConnectionStatus getStatus() const = 0;
    
    // Device management
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual DeviceInfo getDeviceInfo() = 0;
    virtual std::vector<DeviceInfo> enumerateDevices() = 0;
    
    // Address management
    virtual AddressInfo getAddress(const std::string& derivation_path) = 0;
    virtual std::vector<AddressInfo> getAddresses(uint32_t start_index, uint32_t count, bool change = false) = 0;
    virtual bool verifyAddress(const std::string& address, const std::string& derivation_path) = 0;
    
    // Transaction signing
    virtual SigningResult signTransaction(const SigningRequest& request) = 0;
    virtual SigningResult signMessage(const std::string& message, const std::string& derivation_path) = 0;
    virtual bool verifySignature(const std::string& message, const std::string& signature, const std::string& public_key) = 0;
    
    // Callbacks
    virtual void setDeviceCallback(DeviceCallback callback) = 0;
    virtual void setAddressCallback(AddressCallback callback) = 0;
    virtual void setSigningCallback(SigningCallback callback) = 0;
    virtual void setStatusCallback(StatusCallback callback) = 0;
    
    // Configuration
    virtual void setNetworkType(const std::string& network) = 0;
    virtual void setCoinType(uint32_t coin_type) = 0;
    virtual void setAccountIndex(uint32_t account_index) = 0;
    
    // Utility methods
    virtual std::string getDerivationPath(uint32_t account_index, bool change, uint32_t address_index) = 0;
    virtual bool isValidDerivationPath(const std::string& path) = 0;
    virtual std::string getFingerprint() = 0;
};

// Ledger hardware wallet implementation
class LedgerWallet : public HardwareWallet {
public:
    LedgerWallet();
    ~LedgerWallet();
    
    // Core functionality
    bool initialize() override;
    void shutdown() override;
    bool isConnected() const override;
    ConnectionStatus getStatus() const override;
    
    // Device management
    bool connect() override;
    void disconnect() override;
    DeviceInfo getDeviceInfo() override;
    std::vector<DeviceInfo> enumerateDevices() override;
    
    // Address management
    AddressInfo getAddress(const std::string& derivation_path) override;
    std::vector<AddressInfo> getAddresses(uint32_t start_index, uint32_t count, bool change = false) override;
    bool verifyAddress(const std::string& address, const std::string& derivation_path) override;
    
    // Transaction signing
    SigningResult signTransaction(const SigningRequest& request) override;
    SigningResult signMessage(const std::string& message, const std::string& derivation_path) override;
    bool verifySignature(const std::string& message, const std::string& signature, const std::string& public_key) override;
    
    // Callbacks
    void setDeviceCallback(DeviceCallback callback) override;
    void setAddressCallback(AddressCallback callback) override;
    void setSigningCallback(SigningCallback callback) override;
    void setStatusCallback(StatusCallback callback) override;
    
    // Configuration
    void setNetworkType(const std::string& network) override;
    void setCoinType(uint32_t coin_type) override;
    void setAccountIndex(uint32_t account_index) override;
    
    // Utility methods
    std::string getDerivationPath(uint32_t account_index, bool change, uint32_t address_index) override;
    bool isValidDerivationPath(const std::string& path) override;
    std::string getFingerprint() override;
    void setDevicePathForConnect(const std::string& device_path) { m_device_path = device_path; }
    std::string getLastError() const { return m_last_error; }

    // DescriptorStore integration (required for PSBT signing)
    void setDescriptorStore(void* descriptor_store) {
        m_descriptor_store = descriptor_store;
    }
    void* getDescriptorStore() const {
        return m_descriptor_store;
    }

    // Production Ledger PSBT signing (BIP174 + BIP371 Taproot)
    struct PSBTSigningResult {
        bool success = false;
        std::string psbt_base64;
        bool complete = false;
        std::string error_message;
    };
    PSBTSigningResult signPSBT(const std::string& psbt_base64);

private:
    // Ledger wallet policy (descriptor registration)
    struct LedgerWalletPolicy {
        std::string name;                   // Policy name (e.g., "Dinero BIP86")
        std::string descriptor_template;    // Template (e.g., "tr(@0/**)")
        std::vector<std::string> keys;      // xpubs
    };

    // Ledger signature response
    struct LedgerSignature {
        uint32_t input_index;
        std::vector<uint8_t> signature;  // 64-byte Schnorr for Taproot key-path
    };

    // Ledger-specific implementation
    bool openDevice();
    bool closeDevice();
    bool sendCommand(const std::vector<uint8_t>& command, std::vector<uint8_t>& response);
    bool exchangeAPDU(const std::vector<uint8_t>& command, std::vector<uint8_t>& response);
    std::vector<uint8_t> buildAPDU(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2, const std::vector<uint8_t>& data);

    // Ledger Bitcoin app APDUs
    std::string getMasterFingerprint();
    bool registerWalletPolicy(const LedgerWalletPolicy& policy, std::vector<uint8_t>& wallet_id, std::vector<uint8_t>& policy_hash);
    std::vector<LedgerSignature> signPSBTOnDevice(const std::vector<uint8_t>& psbt_binary, const std::vector<uint8_t>& wallet_id);

    // APDU helpers
    std::vector<uint8_t> serializeWalletPolicy(const LedgerWalletPolicy& policy);
    std::vector<LedgerSignature> parseLedgerSignatures(const std::vector<uint8_t>& response);
    std::string mapStatusWord(uint16_t sw);

    // Utility methods
    std::vector<uint32_t> parseDerivationPath(const std::string& path);
    std::string publicKeyToAddress(const std::string& public_key_hex);
    std::string bytesToHex(const std::vector<uint8_t>& bytes);
    std::vector<uint8_t> hexToBytes(const std::string& hex);
    std::vector<std::string> splitString(const std::string& str, char delimiter);

    // Device state
    std::atomic<ConnectionStatus> m_status;
    DeviceInfo m_device_info;
    std::string m_device_path;
    int m_device_handle;
    std::unique_ptr<HIDTransport> m_hid_transport;  // USB HID transport layer

    // Wallet integration
    void* m_descriptor_store = nullptr;  // Required for PSBT signing (din::DescriptorStore*)

    // Configuration
    std::string m_network_type;
    uint32_t m_coin_type;
    uint32_t m_account_index;

    // Callbacks
    DeviceCallback m_device_callback;
    AddressCallback m_address_callback;
    SigningCallback m_signing_callback;
    StatusCallback m_status_callback;

    // Threading
    mutable std::mutex m_mutex;
    std::thread m_monitor_thread;
    std::atomic<bool> m_running;

    std::string m_last_error;  // Last error message

    // Ledger-stored wallet state
    std::vector<uint8_t> m_wallet_id;      // Registered wallet ID from Ledger
    std::vector<uint8_t> m_policy_hash;    // Registered policy hash from Ledger

    // Ledger APDU constants
    static constexpr uint8_t CLA = 0xE0;

    // Legacy APDUs (for compatibility)
    static constexpr uint8_t INS_GET_PUBLIC_KEY = 0x02;
    static constexpr uint8_t INS_SIGN_TX = 0x04;
    static constexpr uint8_t INS_SIGN_MESSAGE = 0x08;
    static constexpr uint8_t INS_GET_APP_CONFIGURATION = 0x06;
    static constexpr uint8_t INS_GET_DEVICE_INFO = 0x0A;

    // Ledger Bitcoin app APDUs (production PSBT signing)
    static constexpr uint8_t INS_GET_MASTER_FINGERPRINT = 0x00;
    static constexpr uint8_t INS_REGISTER_WALLET = 0xF8;
    static constexpr uint8_t INS_SIGN_PSBT = 0xF9;

    // Ledger status words
    static constexpr uint16_t SW_OK = 0x9000;
    static constexpr uint16_t SW_USER_REJECTED = 0x6985;
    static constexpr uint16_t SW_INVALID_DATA = 0x6A80;
    static constexpr uint16_t SW_APP_NOT_OPEN = 0x6D00;
    static constexpr uint16_t SW_WRONG_LENGTH = 0x6700;
    static constexpr uint16_t SW_SECURITY_STATUS = 0x6982;
};

// Trezor hardware wallet implementation
class TrezorWallet : public HardwareWallet {
public:
    struct AccountDescriptorExport {
        std::string derivation_path;
        std::string policy;
        std::string master_fingerprint;
        std::string account_xpub;
        uint32_t coin_type = 0;
        uint32_t account = 0;
        std::string receive_descriptor;
        std::string change_descriptor;
    };

    TrezorWallet();
    ~TrezorWallet();
    
    // Core functionality
    bool initialize() override;
    void shutdown() override;
    bool isConnected() const override;
    ConnectionStatus getStatus() const override;
    
    // Device management
    bool connect() override;
    void disconnect() override;
    DeviceInfo getDeviceInfo() override;
    std::vector<DeviceInfo> enumerateDevices() override;
    
    // Address management
    AddressInfo getAddress(const std::string& derivation_path) override;
    std::vector<AddressInfo> getAddresses(uint32_t start_index, uint32_t count, bool change = false) override;
    bool verifyAddress(const std::string& address, const std::string& derivation_path) override;
    
    // Transaction signing
    SigningResult signTransaction(const SigningRequest& request) override;
    SigningResult signMessage(const std::string& message, const std::string& derivation_path) override;
    bool verifySignature(const std::string& message, const std::string& signature, const std::string& public_key) override;
    
    // Callbacks
    void setDeviceCallback(DeviceCallback callback) override;
    void setAddressCallback(AddressCallback callback) override;
    void setSigningCallback(SigningCallback callback) override;
    void setStatusCallback(StatusCallback callback) override;
    
    // Configuration
    void setNetworkType(const std::string& network) override;
    void setCoinType(uint32_t coin_type) override;
    void setAccountIndex(uint32_t account_index) override;
    
    // Utility methods
    std::string getDerivationPath(uint32_t account_index, bool change, uint32_t address_index) override;
    bool isValidDerivationPath(const std::string& path) override;
    std::string getFingerprint() override;
    void setDevicePathForConnect(const std::string& device_path) { m_device_path = device_path; }
    std::string getLastError() const { return m_last_error; }
    bool exportAccountDescriptors(const std::string& derivation_path,
                                  const std::string& requested_policy,
                                  AccountDescriptorExport& export_out);

    // DescriptorStore integration (required for PSBT signing)
    void setDescriptorStore(void* descriptor_store) {
        m_descriptor_store = descriptor_store;
    }
    void* getDescriptorStore() const {
        return m_descriptor_store;
    }

    // Production Trezor PSBT signing (BIP174 + BIP371 Taproot)
    struct PSBTSigningResult {
        bool success = false;
        std::string psbt_base64;
        bool complete = false;
        std::string error_message;
    };
    PSBTSigningResult signPSBT(const std::string& psbt_base64);

private:
    // Trezor-specific implementation
    bool openDevice();
    bool closeDevice();
    bool initializeSessionLocked();
    bool requestAddressLocked(const std::string& derivation_path,
                              bool show_display,
                              std::string& address_out);
    bool requestPublicKeyLocked(const std::string& derivation_path,
                                std::string& public_key_hex,
                                uint32_t* root_fingerprint = nullptr,
                                std::string* xpub_out = nullptr);

    // Utility methods
    std::vector<std::string> splitString(const std::string& str, char delimiter);
    std::string bytesToHex(const std::vector<uint8_t>& bytes);
    std::vector<uint8_t> hexToBytes(const std::string& hex);

    // Device state
    std::atomic<ConnectionStatus> m_status;
    DeviceInfo m_device_info;
    std::string m_device_path;
    std::string m_last_error;
    std::unique_ptr<class TrezorTransport> m_trezor_transport;  // Trezor wire protocol transport

    // Wallet integration
    void* m_descriptor_store = nullptr;  // Required for PSBT signing (din::DescriptorStore*)
    
    // Configuration
    std::string m_network_type;
    uint32_t m_coin_type;
    uint32_t m_account_index;
    
    // Callbacks
    DeviceCallback m_device_callback;
    AddressCallback m_address_callback;
    SigningCallback m_signing_callback;
    StatusCallback m_status_callback;
    
    // Threading
    mutable std::mutex m_mutex;
    std::thread m_monitor_thread;
    std::atomic<bool> m_running;
    
    // Constants
    static constexpr uint32_t TREZOR_VENDOR_ID = 0x534c;
    static constexpr uint32_t TREZOR_PRODUCT_ID = 0x0001;
};

// Hardware wallet manager - manages multiple hardware wallets
class HardwareWalletManager {
public:
    HardwareWalletManager();
    ~HardwareWalletManager();
    
    // Core functionality
    bool initialize();
    void shutdown();
    bool isInitialized() const;
    
    // Device management
    std::vector<DeviceInfo> enumerateDevices();
    std::shared_ptr<HardwareWallet> connectDevice(const std::string& device_path, HardwareWalletType type);
    void disconnectDevice(const std::string& device_path);
    std::shared_ptr<HardwareWallet> getDevice(const std::string& device_path);
    std::vector<std::shared_ptr<HardwareWallet>> getConnectedDevices();
    
    // Device discovery
    void startDeviceDiscovery();
    void stopDeviceDiscovery();
    bool isDiscoveryRunning() const;
    
    // Callbacks
    void setDeviceConnectedCallback(std::function<void(const DeviceInfo&)> callback);
    void setDeviceDisconnectedCallback(std::function<void(const DeviceInfo&)> callback);
    void setErrorCallback(std::function<void(const std::string&)> callback);
    
    // Configuration
    void setNetworkType(const std::string& network);
    void setCoinType(uint32_t coin_type);
    void setAccountIndex(uint32_t account_index);
    
    // Utility methods
    HardwareWalletType detectDeviceType(const std::string& device_path);
    std::string getDevicePath(const DeviceInfo& device);
    bool isDeviceSupported(const DeviceInfo& device);

private:
    // Device management
    void addDevice(const DeviceInfo& device);
    void removeDevice(const std::string& device_path);
    void updateDeviceStatus(const std::string& device_path, ConnectionStatus status);
    
    // Device discovery
    void discoveryThread();
    void monitorDevices();
    
    // Device state
    std::atomic<bool> m_initialized;
    std::atomic<bool> m_discovery_running;
    std::map<std::string, std::shared_ptr<HardwareWallet>> m_devices;
    std::map<std::string, DeviceInfo> m_device_info;
    
    // Configuration
    std::string m_network_type;
    uint32_t m_coin_type;
    uint32_t m_account_index;
    
    // Callbacks
    std::function<void(const DeviceInfo&)> m_device_connected_callback;
    std::function<void(const DeviceInfo&)> m_device_disconnected_callback;
    std::function<void(const std::string&)> m_error_callback;
    
    // Threading
    mutable std::mutex m_mutex;
    std::thread m_discovery_thread;
    std::thread m_monitor_thread;
    std::atomic<bool> m_running;
};

// Hardware wallet factory - creates hardware wallet instances
class HardwareWalletFactory {
public:
    static std::shared_ptr<HardwareWallet> createWallet(HardwareWalletType type);
    static std::shared_ptr<HardwareWallet> createWallet(const std::string& device_path);
    static HardwareWalletType detectType(const std::string& device_path);
    static std::vector<HardwareWalletType> getSupportedTypes();
    static std::string getTypeName(HardwareWalletType type);
};

} // namespace dinero 
