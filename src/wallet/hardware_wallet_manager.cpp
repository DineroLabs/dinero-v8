#include "wallet/hardware_wallet.h"
#include "common/logger.h"
#include "consensus/coin_type.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <thread>

namespace dinero {

HardwareWalletManager::HardwareWalletManager()
    : m_initialized(false)
    , m_discovery_running(false)
    , m_coin_type(dinero::consensus::DINERO_COIN_TYPE)
    , m_account_index(0)
    , m_running(false) {
    
    g_logger.info("Initializing Hardware Wallet Manager");
}

HardwareWalletManager::~HardwareWalletManager() {
    shutdown();
}

bool HardwareWalletManager::initialize() {
    if (m_initialized) {
        g_logger.warning("Hardware Wallet Manager already initialized");
        return true;
    }
    
    g_logger.info("Hardware Wallet Manager initialization started");
    
    // Set default configuration
    m_network_type = "mainnet";
    m_coin_type = dinero::consensus::DINERO_COIN_TYPE;
    m_account_index = 0;
    
    m_initialized = true;
    m_running = true;
    
    g_logger.info("Hardware Wallet Manager initialized successfully");
    return true;
}

void HardwareWalletManager::shutdown() {
    if (!m_initialized) {
        return;
    }
    
    g_logger.info("Hardware Wallet Manager shutdown started");
    
    // Stop device discovery
    stopDeviceDiscovery();
    
    // Disconnect all devices
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& device : m_devices) {
        if (device.second) {
            device.second->disconnect();
        }
    }
    m_devices.clear();
    m_device_info.clear();
    
    m_running = false;
    m_initialized = false;
    
    g_logger.info("Hardware Wallet Manager shutdown complete");
}

bool HardwareWalletManager::isInitialized() const {
    return m_initialized;
}

std::vector<DeviceInfo> HardwareWalletManager::enumerateDevices() {
    std::vector<DeviceInfo> devices;
    
    if (!m_initialized) {
        g_logger.error("Hardware Wallet Manager not initialized");
        return devices;
    }
    
    g_logger.info("Enumerating hardware wallet devices");
    
    // Enumerate Ledger devices
    try {
        auto ledger_wallet = std::make_shared<LedgerWallet>();
        if (ledger_wallet->initialize()) {
            auto ledger_devices = ledger_wallet->enumerateDevices();
            devices.insert(devices.end(), ledger_devices.begin(), ledger_devices.end());
        }
    } catch (const std::exception& e) {
        g_logger.warning("Failed to enumerate Ledger devices: " + std::string(e.what()));
    }
    
    // Enumerate Trezor devices
    try {
        auto trezor_wallet = std::make_shared<TrezorWallet>();
        if (trezor_wallet->initialize()) {
            auto trezor_devices = trezor_wallet->enumerateDevices();
            devices.insert(devices.end(), trezor_devices.begin(), trezor_devices.end());
        }
    } catch (const std::exception& e) {
        g_logger.warning("Failed to enumerate Trezor devices: " + std::string(e.what()));
    }
    
    g_logger.info("Found " + std::to_string(devices.size()) + " hardware wallet devices");
    return devices;
}

std::shared_ptr<HardwareWallet> HardwareWalletManager::connectDevice(const std::string& device_path, HardwareWalletType type) {
    if (!m_initialized) {
        g_logger.error("Hardware Wallet Manager not initialized");
        return nullptr;
    }
    
    g_logger.info("Connecting to hardware wallet device: " + device_path);
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Check if device is already connected
    auto it = m_devices.find(device_path);
    if (it != m_devices.end()) {
        g_logger.warning("Device already connected: " + device_path);
        return it->second;
    }
    
    // Create hardware wallet instance
    std::shared_ptr<HardwareWallet> wallet;
    try {
        wallet = HardwareWalletFactory::createWallet(type);
        if (!wallet) {
            g_logger.error("Failed to create hardware wallet instance for type: " + std::to_string(static_cast<int>(type)));
            return nullptr;
        }
        
        // Initialize and connect
        if (!wallet->initialize()) {
            g_logger.error("Failed to initialize hardware wallet: " + device_path);
            return nullptr;
        }
        
        if (!wallet->connect()) {
            g_logger.error("Failed to connect to hardware wallet: " + device_path);
            return nullptr;
        }
        
        // Get device info
        DeviceInfo device_info = wallet->getDeviceInfo();
        device_info.name = device_path;
        
        // Add device to manager
        m_devices[device_path] = wallet;
        m_device_info[device_path] = device_info;
        
        g_logger.info("Successfully connected to hardware wallet: " + device_path);
        
        // Notify callback
        if (m_device_connected_callback) {
            m_device_connected_callback(device_info);
        }
        
        return wallet;
        
    } catch (const std::exception& e) {
        g_logger.error("Exception while connecting to device: " + std::string(e.what()));
        return nullptr;
    }
}

void HardwareWalletManager::disconnectDevice(const std::string& device_path) {
    if (!m_initialized) {
        return;
    }
    
    g_logger.info("Disconnecting hardware wallet device: " + device_path);
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_devices.find(device_path);
    if (it != m_devices.end()) {
        // Get device info before disconnecting
        DeviceInfo device_info = m_device_info[device_path];
        
        // Disconnect device
        it->second->disconnect();
        m_devices.erase(it);
        m_device_info.erase(device_path);
        
        g_logger.info("Successfully disconnected hardware wallet: " + device_path);
        
        // Notify callback
        if (m_device_disconnected_callback) {
            m_device_disconnected_callback(device_info);
        }
    } else {
        g_logger.warning("Device not found for disconnection: " + device_path);
    }
}

std::shared_ptr<HardwareWallet> HardwareWalletManager::getDevice(const std::string& device_path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_devices.find(device_path);
    if (it != m_devices.end()) {
        return it->second;
    }
    
    return nullptr;
}

std::vector<std::shared_ptr<HardwareWallet>> HardwareWalletManager::getConnectedDevices() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<std::shared_ptr<HardwareWallet>> devices;
    for (const auto& device : m_devices) {
        devices.append(device.second);
    }
    
    return devices;
}

void HardwareWalletManager::startDeviceDiscovery() {
    if (!m_initialized) {
        g_logger.error("Hardware Wallet Manager not initialized");
        return;
    }
    
    if (m_discovery_running) {
        g_logger.warning("Device discovery already running");
        return;
    }
    
    g_logger.info("Starting hardware wallet device discovery");
    
    m_discovery_running = true;
    m_discovery_thread = std::thread(&HardwareWalletManager::discoveryThread, this);
    
    g_logger.info("Hardware wallet device discovery started");
}

void HardwareWalletManager::stopDeviceDiscovery() {
    if (!m_discovery_running) {
        return;
    }
    
    g_logger.info("Stopping hardware wallet device discovery");
    
    m_discovery_running = false;
    
    if (m_discovery_thread.joinable()) {
        m_discovery_thread.join();
    }
    
    g_logger.info("Hardware wallet device discovery stopped");
}

bool HardwareWalletManager::isDiscoveryRunning() const {
    return m_discovery_running;
}

void HardwareWalletManager::setDeviceConnectedCallback(std::function<void(const DeviceInfo&)> callback) {
    m_device_connected_callback = callback;
}

void HardwareWalletManager::setDeviceDisconnectedCallback(std::function<void(const DeviceInfo&)> callback) {
    m_device_disconnected_callback = callback;
}

void HardwareWalletManager::setErrorCallback(std::function<void(const std::string&)> callback) {
    m_error_callback = callback;
}

void HardwareWalletManager::setNetworkType(const std::string& network) {
    m_network_type = network;
    
    // Update all connected devices
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& device : m_devices) {
        if (device.second) {
            device.second->setNetworkType(network);
        }
    }
}

void HardwareWalletManager::setCoinType(uint32_t coin_type) {
    m_coin_type = coin_type;
    
    // Update all connected devices
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& device : m_devices) {
        if (device.second) {
            device.second->setCoinType(coin_type);
        }
    }
}

void HardwareWalletManager::setAccountIndex(uint32_t account_index) {
    m_account_index = account_index;
    
    // Update all connected devices
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& device : m_devices) {
        if (device.second) {
            device.second->setAccountIndex(account_index);
        }
    }
}

HardwareWalletType HardwareWalletManager::detectDeviceType(const std::string& device_path) {
    return HardwareWalletFactory::detectType(device_path);
}

std::string HardwareWalletManager::getDevicePath(const DeviceInfo& device) {
    return device.name;
}

bool HardwareWalletManager::isDeviceSupported(const DeviceInfo& device) {
    return device.type != HardwareWalletType::UNKNOWN && device.supports_dinero;
}

void HardwareWalletManager::addDevice(const DeviceInfo& device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_info[device.name] = device;
}

void HardwareWalletManager::removeDevice(const std::string& device_path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_device_info.erase(device_path);
}

void HardwareWalletManager::updateDeviceStatus(const std::string& device_path, ConnectionStatus status) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_device_info.find(device_path);
    if (it != m_device_info.end()) {
        // Update device status if needed
        g_logger.debug("Device status updated: " + device_path + " -> " + std::to_string(static_cast<int>(status)));
    }
}

void HardwareWalletManager::discoveryThread() {
    g_logger.info("Hardware wallet discovery thread started");
    
    while (m_discovery_running && m_running) {
        try {
            // Enumerate devices
            auto devices = enumerateDevices();
            
            // Check for new devices
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& device : devices) {
                auto it = m_device_info.find(device.name);
                if (it == m_device_info.end()) {
                    // New device found
                    g_logger.info("New hardware wallet device discovered: " + device.name);
                    addDevice(device);
                    
                    // Notify callback
                    if (m_device_connected_callback) {
                        m_device_connected_callback(device);
                    }
                }
            }
            
            // Check for disconnected devices
            std::vector<std::string> disconnected_devices;
            for (const auto& device : m_device_info) {
                bool found = false;
                for (const auto& current_device : devices) {
                    if (current_device.name == device.first) {
                        found = true;
                        break;
                    }
                }
                
                if (!found) {
                    disconnected_devices.append(device.first);
                }
            }
            
            // Remove disconnected devices
            for (const auto& device_path : disconnected_devices) {
                g_logger.info("Hardware wallet device disconnected: " + device_path);
                
                // Notify callback
                if (m_device_disconnected_callback) {
                    m_device_disconnected_callback(m_device_info[device_path]);
                }
                
                removeDevice(device_path);
            }
            
        } catch (const std::exception& e) {
            g_logger.error("Exception in discovery thread: " + std::string(e.what()));
            
            if (m_error_callback) {
                m_error_callback("Discovery error: " + std::string(e.what()));
            }
        }
        
        // Sleep for a bit before next discovery
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    
    g_logger.info("Hardware wallet discovery thread stopped");
}

void HardwareWalletManager::monitorDevices() {
    g_logger.info("Hardware wallet monitoring thread started");
    
    while (m_running) {
        try {
            std::lock_guard<std::mutex> lock(m_mutex);
            
            // Monitor connected devices
            for (auto& device : m_devices) {
                if (device.second) {
                    ConnectionStatus status = device.second->getStatus();
                    if (status == ConnectionStatus::ERROR || status == ConnectionStatus::DISCONNECTED) {
                        g_logger.warning("Device connection lost: " + device.first);
                        
                        // Notify callback
                        if (m_device_disconnected_callback) {
                            m_device_disconnected_callback(m_device_info[device.first]);
                        }
                        
                        // Remove device
                        device.second->disconnect();
                        m_devices.erase(device.first);
                        m_device_info.erase(device.first);
                    }
                }
            }
            
        } catch (const std::exception& e) {
            g_logger.error("Exception in monitoring thread: " + std::string(e.what()));
        }
        
        // Sleep for a bit before next monitoring
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    
    g_logger.info("Hardware wallet monitoring thread stopped");
}

} // namespace dinero 
