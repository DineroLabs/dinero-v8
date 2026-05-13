#include "wallet/hardware_wallet.h"
#include <iostream>
#include <algorithm>
#include <sstream>
#include <thread>
#include <chrono>

#ifdef __APPLE__
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/hid/IOHIDLib.h>
#endif

namespace Dinero {
namespace HardwareWallet {

// HardwareWalletManager implementation
HardwareWalletManager& HardwareWalletManager::getInstance() {
    static HardwareWalletManager instance;
    return instance;
}

std::vector<DeviceInfo> HardwareWalletManager::enumerateDevices() {
    std::vector<DeviceInfo> devices;
    
#ifdef __APPLE__
    // macOS HID device enumeration
    IOHIDManagerRef hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!hidManager) {
        return devices;
    }
    
    // Set up device matching criteria for Ledger devices
    CFMutableDictionaryRef ledgerCriteria = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int ledgerVendorIdValue = 0x2c97; // Ledger vendor ID
    int ledgerProductIdValue = 0x0001; // Nano S product ID
    CFNumberRef ledgerVendorId = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ledgerVendorIdValue);
    CFNumberRef ledgerProductId = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ledgerProductIdValue);
    CFDictionarySetValue(ledgerCriteria, CFSTR(kIOHIDVendorIDKey), ledgerVendorId);
    CFDictionarySetValue(ledgerCriteria, CFSTR(kIOHIDProductIDKey), ledgerProductId);
    
    // Set up device matching criteria for Trezor devices
    CFMutableDictionaryRef trezorCriteria = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int trezorVendorIdValue = 0x1209; // Trezor vendor ID
    int trezorProductIdValue = 0x53c1; // Model T product ID
    CFNumberRef trezorVendorId = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &trezorVendorIdValue);
    CFNumberRef trezorProductId = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &trezorProductIdValue);
    CFDictionarySetValue(trezorCriteria, CFSTR(kIOHIDVendorIDKey), trezorVendorId);
    CFDictionarySetValue(trezorCriteria, CFSTR(kIOHIDProductIDKey), trezorProductId);
    
    IOHIDManagerSetDeviceMatching(hidManager, ledgerCriteria);
    
    IOReturn result = IOHIDManagerOpen(hidManager, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
        CFRelease(hidManager);
        return devices;
    }
    
    CFSetRef deviceSet = IOHIDManagerCopyDevices(hidManager);
    if (deviceSet) {
        CFIndex deviceCount = CFSetGetCount(deviceSet);
        CFTypeRef* deviceArray = new CFTypeRef[deviceCount];
        CFSetGetValues(deviceSet, deviceArray);
        
        for (CFIndex i = 0; i < deviceCount; i++) {
            IOHIDDeviceRef device = (IOHIDDeviceRef)deviceArray[i];
            
            DeviceInfo info;
            info.deviceId = "ledger_device_" + std::to_string(i);
            info.deviceName = "Ledger Nano S";
            info.type = DeviceType::LEDGER_NANO_S;
            info.status = DeviceStatus::CONNECTED;
            info.firmwareVersion = "Unknown";
            info.appVersion = "Unknown";
            info.supportsDinero = true;
            
            devices.push_back(info);
        }
        
        delete[] deviceArray;
        CFRelease(deviceSet);
    }
    
    CFRelease(ledgerVendorId);
    CFRelease(ledgerProductId);
    CFRelease(trezorVendorId);
    CFRelease(trezorProductId);
    CFRelease(ledgerCriteria);
    CFRelease(trezorCriteria);
    CFRelease(hidManager);
#endif
    
    // Add a test device for development
    DeviceInfo mockDevice;
    mockDevice.deviceId = "mock_ledger_001";
    mockDevice.deviceName = "Mock Ledger Nano S";
    mockDevice.type = DeviceType::LEDGER_NANO_S;
    mockDevice.status = DeviceStatus::CONNECTED;
    mockDevice.firmwareVersion = "2.1.0";
    mockDevice.appVersion = "Dinero 1.0.0";
    mockDevice.supportsDinero = true;
    devices.push_back(mockDevice);
    
    return devices;
}

std::shared_ptr<HardwareWalletDevice> HardwareWalletManager::connectDevice(const std::string& deviceId) {
    auto devices = enumerateDevices();
    auto it = std::find_if(devices.begin(), devices.end(), 
        [&deviceId](const DeviceInfo& info) { return info.deviceId == deviceId; });
    
    if (it == devices.end()) {
        return nullptr;
    }
    
    std::shared_ptr<HardwareWalletDevice> device;
    
    switch (it->type) {
        case DeviceType::LEDGER_NANO_S:
        case DeviceType::LEDGER_NANO_X:
            device = std::make_shared<LedgerDevice>(*it);
            break;
        case DeviceType::TREZOR_ONE:
        case DeviceType::TREZOR_MODEL_T:
            device = std::make_shared<TrezorDevice>(*it);
            break;
        default:
            return nullptr;
    }
    
    if (device && device->connect()) {
        connectedDevices_.push_back(device);
        if (deviceConnectedCallback_) {
            deviceConnectedCallback_(*it);
        }
        return device;
    }
    
    return nullptr;
}

bool HardwareWalletManager::disconnectDevice(const std::string& deviceId) {
    auto it = std::find_if(connectedDevices_.begin(), connectedDevices_.end(),
        [&deviceId](const std::shared_ptr<HardwareWalletDevice>& device) {
            return device->getDeviceInfo().deviceId == deviceId;
        });
    
    if (it != connectedDevices_.end()) {
        (*it)->disconnect();
        if (deviceDisconnectedCallback_) {
            deviceDisconnectedCallback_((*it)->getDeviceInfo());
        }
        connectedDevices_.erase(it);
        return true;
    }
    
    return false;
}

std::vector<std::shared_ptr<HardwareWalletDevice>> HardwareWalletManager::getConnectedDevices() {
    return connectedDevices_;
}

std::shared_ptr<HardwareWalletDevice> HardwareWalletManager::getDevice(const std::string& deviceId) {
    auto it = std::find_if(connectedDevices_.begin(), connectedDevices_.end(),
        [&deviceId](const std::shared_ptr<HardwareWalletDevice>& device) {
            return device->getDeviceInfo().deviceId == deviceId;
        });
    
    return (it != connectedDevices_.end()) ? *it : nullptr;
}

std::string HardwareWalletManager::generateDineroAddress(const std::string& deviceId, uint32_t account, uint32_t address) {
    auto device = getDevice(deviceId);
    if (!device) {
        return "";
    }
    
    DerivationPath path;
    path.purpose = 84; // P2WPKH
    path.coinType = coinType_;
    path.account = account;
    path.change = 0; // External addresses
    path.address = address;
    
    return device->getAddress(path);
}

std::vector<uint8_t> HardwareWalletManager::signDineroTransaction(const std::string& deviceId, const std::vector<uint8_t>& transaction, uint32_t account) {
    auto device = getDevice(deviceId);
    if (!device) {
        return {};
    }
    
    DerivationPath path;
    path.purpose = 84; // P2WPKH
    path.coinType = coinType_;
    path.account = account;
    path.change = 0; // External addresses
    path.address = 0; // Will be determined by transaction
    
    return device->signTransaction(transaction, path);
}

void HardwareWalletManager::setDeviceConnectedCallback(std::function<void(const DeviceInfo&)> callback) {
    deviceConnectedCallback_ = callback;
}

void HardwareWalletManager::setDeviceDisconnectedCallback(std::function<void(const DeviceInfo&)> callback) {
    deviceDisconnectedCallback_ = callback;
}

// LedgerDevice implementation
LedgerDevice::LedgerDevice(const DeviceInfo& info) : deviceInfo_(info), connected_(false), unlocked_(false) {
}

LedgerDevice::~LedgerDevice() {
    disconnect();
}

bool LedgerDevice::connect() {
    // Mock implementation - in real implementation, would establish HID connection
    connected_ = true;
    deviceInfo_.status = DeviceStatus::CONNECTED;
    
    if (connectionCallback_) {
        connectionCallback_(true);
    }
    
    return true;
}

bool LedgerDevice::disconnect() {
    connected_ = false;
    unlocked_ = false;
    deviceInfo_.status = DeviceStatus::DISCONNECTED;
    
    if (connectionCallback_) {
        connectionCallback_(false);
    }
    
    return true;
}

bool LedgerDevice::isConnected() const {
    return connected_;
}

DeviceInfo LedgerDevice::getDeviceInfo() const {
    return deviceInfo_;
}

std::vector<uint8_t> LedgerDevice::getPublicKey(const DerivationPath& path) {
    if (!connected_ || !unlocked_) {
        return {};
    }
    
    // Basic implementation - would send APDU commands to Ledger in production
    // For development, return a test public key
    std::vector<uint8_t> testPublicKey(33, 0x02); // Compressed public key format
    return testPublicKey;
}

std::string LedgerDevice::getAddress(const DerivationPath& path) {
    if (!connected_ || !unlocked_) {
        return "";
    }
    
    // Mock implementation - would derive address from public key
    return "din1qledger" + std::to_string(path.address);
}

std::vector<uint8_t> LedgerDevice::signTransaction(const std::vector<uint8_t>& transaction, const DerivationPath& path) {
    if (!connected_ || !unlocked_) {
        return {};
    }
    
    // Basic implementation - would sign transaction on device in production
    // For development, return a test signature
    std::vector<uint8_t> testSignature(64, 0x42); // Test signature
    return testSignature;
}

bool LedgerDevice::unlock(const std::string& pin) {
    if (!connected_) {
        return false;
    }
    
    // Mock implementation - would verify PIN on device
    unlocked_ = true;
    deviceInfo_.status = DeviceStatus::READY;
    return true;
}

bool LedgerDevice::lock() {
    unlocked_ = false;
    deviceInfo_.status = DeviceStatus::LOCKED;
    return true;
}

bool LedgerDevice::isUnlocked() const {
    return unlocked_;
}

void LedgerDevice::setConnectionCallback(std::function<void(bool)> callback) {
    connectionCallback_ = callback;
}

void LedgerDevice::setErrorCallback(std::function<void(const std::string&)> callback) {
    errorCallback_ = callback;
}

// TrezorDevice implementation
TrezorDevice::TrezorDevice(const DeviceInfo& info) : deviceInfo_(info), connected_(false), unlocked_(false) {
}

TrezorDevice::~TrezorDevice() {
    disconnect();
}

bool TrezorDevice::connect() {
    // Mock implementation - would establish HID connection
    connected_ = true;
    deviceInfo_.status = DeviceStatus::CONNECTED;
    
    if (connectionCallback_) {
        connectionCallback_(true);
    }
    
    return true;
}

bool TrezorDevice::disconnect() {
    connected_ = false;
    unlocked_ = false;
    deviceInfo_.status = DeviceStatus::DISCONNECTED;
    
    if (connectionCallback_) {
        connectionCallback_(false);
    }
    
    return true;
}

bool TrezorDevice::isConnected() const {
    return connected_;
}

DeviceInfo TrezorDevice::getDeviceInfo() const {
    return deviceInfo_;
}

std::vector<uint8_t> TrezorDevice::getPublicKey(const DerivationPath& path) {
    if (!connected_ || !unlocked_) {
        return {};
    }
    
    // Basic implementation - would send protobuf messages to Trezor in production
    // For development, return a test public key
    std::vector<uint8_t> testPublicKey(33, 0x03); // Compressed public key format
    return testPublicKey;
}

std::string TrezorDevice::getAddress(const DerivationPath& path) {
    if (!connected_ || !unlocked_) {
        return "";
    }
    
    // Mock implementation - would derive address from public key
    return "din1qtrezor" + std::to_string(path.address);
}

std::vector<uint8_t> TrezorDevice::signTransaction(const std::vector<uint8_t>& transaction, const DerivationPath& path) {
    if (!connected_ || !unlocked_) {
        return {};
    }
    
    // Basic implementation - would sign transaction on device in production
    // For development, return a test signature
    std::vector<uint8_t> testSignature(64, 0x43); // Test signature
    return testSignature;
}

bool TrezorDevice::unlock(const std::string& pin) {
    if (!connected_) {
        return false;
    }
    
    // Mock implementation - would verify PIN on device
    unlocked_ = true;
    deviceInfo_.status = DeviceStatus::READY;
    return true;
}

bool TrezorDevice::lock() {
    unlocked_ = false;
    deviceInfo_.status = DeviceStatus::LOCKED;
    return true;
}

bool TrezorDevice::isUnlocked() const {
    return unlocked_;
}

void TrezorDevice::setConnectionCallback(std::function<void(bool)> callback) {
    connectionCallback_ = callback;
}

void TrezorDevice::setErrorCallback(std::function<void(const std::string&)> callback) {
    errorCallback_ = callback;
}

// Utility functions
std::string derivationPathToString(const DerivationPath& path) {
    std::ostringstream oss;
    oss << "m/" << path.purpose << "'/" << path.coinType << "'/" << path.account << "'/" << path.change << "/" << path.address;
    return oss.str();
}

DerivationPath parseDerivationPath(const std::string& path) {
    DerivationPath result = {};
    
    if (path.substr(0, 2) != "m/") {
        return result;
    }
    
    std::string pathStr = path.substr(2);
    std::istringstream iss(pathStr);
    std::string segment;
    int index = 0;
    
    while (std::getline(iss, segment, '/') && index < 5) {
        bool hardened = segment.back() == '\'';
        if (hardened) {
            segment.pop_back();
        }
        
        uint32_t value = std::stoul(segment);
        if (hardened) {
            value |= 0x80000000;
        }
        
        switch (index) {
            case 0: result.purpose = value & 0x7FFFFFFF; break; // Remove hardened bit for purpose
            case 1: result.coinType = value & 0x7FFFFFFF; break; // Remove hardened bit for coin type
            case 2: result.account = value & 0x7FFFFFFF; break; // Remove hardened bit for account
            case 3: result.change = value; break;
            case 4: result.address = value; break;
        }
        index++;
    }
    
    return result;
}

bool isValidDerivationPath(const DerivationPath& path) {
    return path.purpose == 84 && // P2WPKH
           path.coinType >= 0 &&
           path.account >= 0 &&
           path.change <= 1 &&
           path.address >= 0;
}

} // namespace HardwareWallet
} // namespace Dinero
