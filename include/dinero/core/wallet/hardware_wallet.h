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
    
    SigningRequest() : fee(0), locktime(0) {}
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

private:
    // Ledger-specific implementation
    bool openDevice();
    bool closeDevice();
    bool sendCommand(const std::vector<uint8_t>& command, std::vector<uint8_t>& response);
    bool exchangeAPDU(const std::vector<uint8_t>& command, std::vector<uint8_t>& response);
    std::vector<uint8_t> buildAPDU(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2, const std::vector<uint8_t>& data);
    
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
    static constexpr uint8_t CLA = 0xE0;
    static constexpr uint8_t INS_GET_PUBLIC_KEY = 0x02;
    static constexpr uint8_t INS_SIGN_TX = 0x04;
    static constexpr uint8_t INS_SIGN_MESSAGE = 0x08;
    static constexpr uint8_t INS_GET_APP_CONFIGURATION = 0x06;
    static constexpr uint8_t INS_GET_DEVICE_INFO = 0x0A;
};

// Trezor hardware wallet implementation
class TrezorWallet : public HardwareWallet {
public:
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

private:
    // Trezor-specific implementation
    bool openDevice();
    bool closeDevice();
    bool sendMessage(const std::string& message_type, const std::string& data, std::string& response);
    bool exchangeMessage(const std::string& message_type, const std::string& data, std::string& response);
    std::string buildMessage(const std::string& message_type, const std::string& data);
    
    // Utility methods
    std::vector<std::string> splitString(const std::string& str, char delimiter);
    
    // Device state
    std::atomic<ConnectionStatus> m_status;
    DeviceInfo m_device_info;
    std::string m_device_path;
    int m_device_handle;
    
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