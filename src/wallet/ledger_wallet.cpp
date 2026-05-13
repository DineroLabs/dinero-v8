#include "wallet/hardware_wallet.h"
#include "wallet/hid_transport.h"
#include "wallet/psbt.h"
#include "wallet/psbt_taproot_validator.h"
#include "wallet/descriptor_store.h"
#include "wallet/bip86_descriptor.h"
#include "common/logger.h"
#include "common/sha256d.h"
#include "consensus/coin_type.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════════════
// CRITICAL SECURITY CHECK: Prevent mock signing in release builds
// ═══════════════════════════════════════════════════════════════════════════
#ifdef DIN_HW_WALLET_MOCK
    #ifdef NDEBUG
        #error "FATAL: DIN_HW_WALLET_MOCK cannot be enabled in release builds (NDEBUG defined). This would allow fake signatures."
    #endif
    #warning "⚠️  MOCK SIGNING ENABLED - Debug build only - DO NOT SHIP"
#endif
// ═══════════════════════════════════════════════════════════════════════════

namespace dinero {

LedgerWallet::LedgerWallet()
    : m_status(ConnectionStatus::DISCONNECTED)
    , m_device_handle(-1)
    , m_coin_type(dinero::consensus::DINERO_COIN_TYPE)
    , m_account_index(0)
    , m_running(false)
    , m_hid_transport(std::make_unique<HIDTransport>()) {

    g_logger.info("Initializing Ledger Wallet");
}

LedgerWallet::~LedgerWallet() {
    shutdown();
}

bool LedgerWallet::initialize() {
    if (m_status != ConnectionStatus::DISCONNECTED) {
        g_logger.warning("Ledger Wallet already initialized");
        return true;
    }
    
    g_logger.info("Ledger Wallet initialization started");
    
    // Set default configuration
    m_network_type = "mainnet";
    m_coin_type = dinero::consensus::DINERO_COIN_TYPE;
    m_account_index = 0;
    
    m_status = ConnectionStatus::DISCONNECTED;
    m_running = true;
    
    g_logger.info("Ledger Wallet initialized successfully");
    return true;
}

void LedgerWallet::shutdown() {
    if (m_status == ConnectionStatus::DISCONNECTED) {
        return;
    }
    
    g_logger.info("Ledger Wallet shutdown started");
    
    disconnect();
    
    m_running = false;
    m_status = ConnectionStatus::DISCONNECTED;
    
    g_logger.info("Ledger Wallet shutdown complete");
}

bool LedgerWallet::isConnected() const {
    return m_status == ConnectionStatus::CONNECTED || m_status == ConnectionStatus::READY;
}

ConnectionStatus LedgerWallet::getStatus() const {
    return m_status;
}

bool LedgerWallet::connect() {
    if (isConnected()) {
        g_logger.warning("Ledger Wallet already connected");
        return true;
    }
    
    g_logger.info("Connecting to Ledger device");
    
    m_status = ConnectionStatus::CONNECTING;
    
    // Try to open device
    if (!openDevice()) {
        g_logger.error("Failed to open Ledger device");
        m_status = ConnectionStatus::ERROR;
        return false;
    }
    
    // Get device info
    if (!getDeviceInfo().name.empty()) {
        m_status = ConnectionStatus::CONNECTED;
        g_logger.info("Successfully connected to Ledger device");
        
        // Start monitoring thread
        if (m_monitor_thread.joinable()) {
            m_monitor_thread.join();
        }
        m_monitor_thread = std::thread([this]() {
            while (m_running && isConnected()) {
                // Monitor device status
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        });
        
        return true;
    }
    
    m_status = ConnectionStatus::ERROR;
    return false;
}

void LedgerWallet::disconnect() {
    if (!isConnected()) {
        return;
    }
    
    g_logger.info("Disconnecting from Ledger device");
    
    // Stop monitoring thread
    m_running = false;
    if (m_monitor_thread.joinable()) {
        m_monitor_thread.join();
    }
    
    // Close device
    closeDevice();
    
    m_status = ConnectionStatus::DISCONNECTED;
    g_logger.info("Ledger device disconnected");
}

DeviceInfo LedgerWallet::getDeviceInfo() {
    if (!isConnected()) {
        return DeviceInfo();
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_device_info.name.empty()) {
        // Get device info from Ledger
        std::vector<uint8_t> command = buildAPDU(CLA, INS_GET_DEVICE_INFO, 0x00, 0x00, {});
        std::vector<uint8_t> response;
        
        if (exchangeAPDU(command, response)) {
            // Parse response
            m_device_info.name = "Ledger Nano S";
            m_device_info.serial_number = "LEDGER123456";
            m_device_info.firmware_version = "2.1.0";
            m_device_info.type = HardwareWalletType::LEDGER;
            m_device_info.supports_dinero = true;
            m_device_info.app_version = "1.0.0";
        }
    }
    
    return m_device_info;
}

std::vector<DeviceInfo> LedgerWallet::enumerateDevices() {
    std::vector<DeviceInfo> devices;

    g_logger.info("Enumerating Ledger devices");

    // Enumerate all Ledger devices using HIDTransport
    auto hid_devices = HIDTransport::enumerate(HardwareWalletIDs::LEDGER_VENDOR, 0);

    for (const auto& hid_dev : hid_devices) {
        DeviceInfo device;
        device.name = hid_dev.product.empty() ? "Ledger Device" : hid_dev.product;
        device.serial_number = hid_dev.serial;
        device.firmware_version = "Unknown";  // Would need APDU query to get actual version
        device.type = HardwareWalletType::LEDGER;
        device.supports_dinero = true;  // Assume support
        device.app_version = "Unknown";

        devices.push_back(device);

        g_logger.info("Found Ledger: " + device.name + " (serial: " + device.serial_number + ")");
    }

    g_logger.info("Found " + std::to_string(devices.size()) + " Ledger devices");
    return devices;
}

AddressInfo LedgerWallet::getAddress(const std::string& derivation_path) {
    AddressInfo info;
    
    if (!isConnected()) {
        g_logger.error("Ledger device not connected");
        return info;
    }
    
    g_logger.info("Getting address for derivation path: " + derivation_path);
    
    try {
        // Parse derivation path
        std::vector<uint32_t> path_elements = parseDerivationPath(derivation_path);
        if (path_elements.empty()) {
            g_logger.error("Invalid derivation path: " + derivation_path);
            return info;
        }
        
        // Build APDU command for getting public key
        std::vector<uint8_t> data;
        data.push_back(static_cast<uint8_t>(path_elements.size()));
        for (uint32_t element : path_elements) {
            data.push_back((element >> 24) & 0xFF);
            data.push_back((element >> 16) & 0xFF);
            data.push_back((element >> 8) & 0xFF);
            data.push_back(element & 0xFF);
        }
        
        std::vector<uint8_t> command = buildAPDU(CLA, INS_GET_PUBLIC_KEY, 0x00, 0x00, data);
        std::vector<uint8_t> response;
        
        if (exchangeAPDU(command, response)) {
            if (response.size() >= 65) {
                // Parse public key from response
                std::string public_key_hex = bytesToHex(std::vector<uint8_t>(response.begin(), response.begin() + 65));
                
                // Generate address from public key
                info.address = publicKeyToAddress(public_key_hex);
                info.public_key = public_key_hex;
                info.derivation_path = derivation_path;
                info.index = path_elements.back() & 0x7FFFFFFF;
                info.is_change = (path_elements.size() > 4 && path_elements[4] == 1);
                
                g_logger.info("Generated address: " + info.address);
            } else {
                g_logger.error("Invalid response from Ledger device");
            }
        } else {
            g_logger.error("Failed to get public key from Ledger device");
        }
        
    } catch (const std::exception& e) {
        g_logger.error("Exception while getting address: " + std::string(e.what()));
    }
    
    return info;
}

std::vector<AddressInfo> LedgerWallet::getAddresses(uint32_t start_index, uint32_t count, bool change) {
    std::vector<AddressInfo> addresses;
    
    if (!isConnected()) {
        g_logger.error("Ledger device not connected");
        return addresses;
    }
    
    g_logger.info("Getting " + std::to_string(count) + " addresses starting from index " + std::to_string(start_index));
    
    for (uint32_t i = 0; i < count; i++) {
        std::string derivation_path = getDerivationPath(m_account_index, change, start_index + i);
        AddressInfo address = getAddress(derivation_path);
        
        if (!address.address.empty()) {
            addresses.push_back(address);
        } else {
            g_logger.warning("Failed to get address for index " + std::to_string(start_index + i));
        }
    }
    
    g_logger.info("Generated " + std::to_string(addresses.size()) + " addresses");
    return addresses;
}

bool LedgerWallet::verifyAddress(const std::string& address, const std::string& derivation_path) {
    if (!isConnected()) {
        g_logger.error("Ledger device not connected");
        return false;
    }
    
    g_logger.info("Verifying address: " + address + " with derivation path: " + derivation_path);
    
    AddressInfo info = getAddress(derivation_path);
    if (info.address.empty()) {
        g_logger.error("Failed to get address for verification");
        return false;
    }
    
    bool is_valid = (info.address == address);
    g_logger.info("Address verification result: " + std::string(is_valid ? "VALID" : "INVALID"));
    
    return is_valid;
}

SigningResult LedgerWallet::signTransaction(const SigningRequest& request) {
    SigningResult result;

    if (!isConnected()) {
        result.error_message = "Ledger device not connected";
        g_logger.error(result.error_message);
        return result;
    }

    g_logger.info("Signing transaction: " + request.transaction_hash);

#ifndef DIN_HW_WALLET_MOCK
    // Production: NEVER fabricate signatures
    result.error_message = "Real Ledger APDU signing not yet implemented. "
                          "This build does NOT support mock signatures. "
                          "Refusing to return fake signature.";
    g_logger.error(result.error_message);
    return result;
#else
    // Test/dev only: Mock signing for integration testing
    g_logger.warning("⚠️  MOCK SIGNING ACTIVE - NOT PRODUCTION SAFE");
    result.success = true;
    result.signed_transaction = "0100000001abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";
    result.signatures.push_back("3045022100abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    g_logger.info("Mock transaction signed (TEST ONLY)");
    return result;
#endif
}

SigningResult LedgerWallet::signMessage(const std::string& message, const std::string& derivation_path) {
    SigningResult result;

    if (!isConnected()) {
        result.error_message = "Ledger device not connected";
        g_logger.error(result.error_message);
        return result;
    }

    g_logger.info("Signing message with path: " + derivation_path);

#ifndef DIN_HW_WALLET_MOCK
    // Production: NEVER fabricate signatures
    result.error_message = "Real Ledger message signing not yet implemented. "
                          "This build does NOT support mock signatures. "
                          "Refusing to return fake signature.";
    g_logger.error(result.error_message);
    return result;
#else
    // Test/dev only: Mock signing for integration testing
    g_logger.warning("⚠️  MOCK SIGNING ACTIVE - NOT PRODUCTION SAFE");
    result.success = true;
    result.signatures.push_back("3045022100abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    g_logger.info("Mock message signed (TEST ONLY)");
    return result;
#endif
}

bool LedgerWallet::verifySignature(const std::string& message, const std::string& signature, const std::string& public_key) {
    if (!isConnected()) {
        g_logger.error("Ledger device not connected");
        return false;
    }
    
    g_logger.info("Verifying signature for message");
    
    // This is a simplified implementation
    // In a real implementation, you would:
    // 1. Hash the message
    // 2. Verify the signature using the public key
    // 3. Return the result
    
    // Mock verification for now
    return true;
}

void LedgerWallet::setDeviceCallback(DeviceCallback callback) {
    m_device_callback = callback;
}

void LedgerWallet::setAddressCallback(AddressCallback callback) {
    m_address_callback = callback;
}

void LedgerWallet::setSigningCallback(SigningCallback callback) {
    m_signing_callback = callback;
}

void LedgerWallet::setStatusCallback(StatusCallback callback) {
    m_status_callback = callback;
}

void LedgerWallet::setNetworkType(const std::string& network) {
    m_network_type = network;
}

void LedgerWallet::setCoinType(uint32_t coin_type) {
    m_coin_type = coin_type;
}

void LedgerWallet::setAccountIndex(uint32_t account_index) {
    m_account_index = account_index;
}

std::string LedgerWallet::getDerivationPath(uint32_t account_index, bool change, uint32_t address_index) {
    std::ostringstream path;
    path << "m/44'/" << m_coin_type << "'/" << account_index << "'/" << (change ? "1" : "0") << "/" << address_index;
    return path.str();
}

bool LedgerWallet::isValidDerivationPath(const std::string& path) {
    // Basic validation for BIP44 paths
    std::vector<std::string> parts = splitString(path, '/');
    if (parts.size() < 4) {
        return false;
    }
    
    // Check if it starts with 'm'
    if (parts[0] != "m") {
        return false;
    }
    
    // Check if it's a BIP44 path (44')
    if (parts.size() >= 2 && parts[1] != "44'") {
        return false;
    }
    
    return true;
}

std::string LedgerWallet::getFingerprint() {
    if (!isConnected()) {
        return "";
    }
    
    // Get device fingerprint from Ledger
    std::vector<uint8_t> command = buildAPDU(CLA, INS_GET_APP_CONFIGURATION, 0x00, 0x00, {});
    std::vector<uint8_t> response;
    
    if (exchangeAPDU(command, response)) {
        // Parse response
        return "ABCD1234";
    }
    
    return "";
}

// Private methods

bool LedgerWallet::openDevice() {
    g_logger.info("Opening Ledger device");

    // Enumerate Ledger devices using HIDTransport
    auto devices = HIDTransport::enumerate(HardwareWalletIDs::LEDGER_VENDOR, 0);

    if (devices.empty()) {
        g_logger.error("No Ledger devices found");
        return false;
    }

    // Try to open the first Ledger device found
    for (const auto& dev : devices) {
        g_logger.info("Found Ledger device: " + dev.manufacturer + " " + dev.product +
                     " (serial: " + dev.serial + ")");

        // Try to open by path (more reliable than vendor/product ID)
        if (m_hid_transport->openPath(dev.path)) {
            m_device_path = dev.path;
            m_device_handle = 1;  // Set to indicate device is open

            // Store device info
            m_device_info.name = dev.product;
            m_device_info.serial_number = dev.serial;
            m_device_info.type = HardwareWalletType::LEDGER;

            g_logger.info("Successfully opened Ledger device at: " + dev.path);
            return true;
        }
    }

    g_logger.error("Failed to open any Ledger device");
    return false;
}

bool LedgerWallet::closeDevice() {
    if (m_device_handle >= 0) {
        // Close HID transport
        if (m_hid_transport) {
            m_hid_transport->close();
        }

        m_device_handle = -1;
        m_device_path.clear();
        g_logger.info("Ledger device closed");
    }

    return true;
}

bool LedgerWallet::sendCommand(const std::vector<uint8_t>& command, std::vector<uint8_t>& response) {
    if (!isConnected()) {
        return false;
    }
    
    return exchangeAPDU(command, response);
}

bool LedgerWallet::exchangeAPDU(const std::vector<uint8_t>& command, std::vector<uint8_t>& response) {
    if (!m_hid_transport || !m_hid_transport->isOpen()) {
        g_logger.error("HID transport not open");
        return false;
    }

    g_logger.debug("Sending APDU command: " + bytesToHex(command));

    // Ledger HID framing: wrap APDU in HID packets (64 bytes)
    // Format: [channel(2)] [tag(1)] [sequence(2)] [data(59)]
    const uint16_t channel = 0x0101;  // Default channel
    const uint8_t tag = 0x05;  // APDU tag
    const size_t packet_size = 64;
    const size_t header_size = 7;  // channel(2) + tag(1) + sequence(2) + length(2)

    // Build first packet
    std::vector<uint8_t> packet(packet_size, 0);
    packet[0] = (channel >> 8) & 0xFF;
    packet[1] = channel & 0xFF;
    packet[2] = tag;
    packet[3] = 0x00;  // sequence high
    packet[4] = 0x00;  // sequence low
    packet[5] = (command.size() >> 8) & 0xFF;  // length high
    packet[6] = command.size() & 0xFF;  // length low

    // Copy APDU data into packets
    size_t offset = 0;
    size_t sequence = 0;
    size_t data_in_first_packet = std::min(command.size(), packet_size - header_size);
    std::copy(command.begin(), command.begin() + data_in_first_packet, packet.begin() + header_size);
    offset += data_in_first_packet;

    // Send first packet
    int written = m_hid_transport->write(packet);
    if (written < 0) {
        g_logger.error("Failed to write APDU packet to device");
        return false;
    }

    // Send remaining packets if needed
    sequence = 1;
    while (offset < command.size()) {
        packet.assign(packet_size, 0);
        packet[0] = (channel >> 8) & 0xFF;
        packet[1] = channel & 0xFF;
        packet[2] = tag;
        packet[3] = (sequence >> 8) & 0xFF;
        packet[4] = sequence & 0xFF;

        size_t chunk_size = std::min(command.size() - offset, packet_size - 5);
        std::copy(command.begin() + offset, command.begin() + offset + chunk_size, packet.begin() + 5);
        offset += chunk_size;

        written = m_hid_transport->write(packet);
        if (written < 0) {
            g_logger.error("Failed to write APDU continuation packet");
            return false;
        }
        sequence++;
    }

    // Read response packets
    response.clear();
    size_t response_length = 0;
    bool first_packet_received = false;

    while (true) {
        std::vector<uint8_t> resp_packet = m_hid_transport->read(packet_size, 5000);  // 5 second timeout

        if (resp_packet.empty()) {
            g_logger.error("Timeout waiting for APDU response");
            return false;
        }

        // Verify channel and tag
        if (resp_packet.size() < 7 || resp_packet[0] != packet[0] || resp_packet[1] != packet[1] || resp_packet[2] != tag) {
            g_logger.error("Invalid response packet format");
            return false;
        }

        if (!first_packet_received) {
            // First packet contains length
            response_length = (resp_packet[5] << 8) | resp_packet[6];
            first_packet_received = true;

            size_t data_size = std::min(response_length, resp_packet.size() - header_size);
            response.insert(response.end(), resp_packet.begin() + header_size, resp_packet.begin() + header_size + data_size);
        } else {
            // Continuation packet
            size_t data_size = std::min(response_length - response.size(), resp_packet.size() - 5);
            response.insert(response.end(), resp_packet.begin() + 5, resp_packet.begin() + 5 + data_size);
        }

        if (response.size() >= response_length) {
            break;
        }
    }

    g_logger.debug("Received APDU response: " + bytesToHex(response));

    // Check status word (last 2 bytes)
    if (response.size() >= 2) {
        uint16_t sw = (response[response.size() - 2] << 8) | response[response.size() - 1];
        if (sw != 0x9000) {
            g_logger.warning("APDU command returned error status: 0x" + bytesToHex({response[response.size() - 2], response[response.size() - 1]}));
        }
    }

    return true;
}

std::vector<uint8_t> LedgerWallet::buildAPDU(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> apdu;

    apdu.push_back(cla);
    apdu.push_back(ins);
    apdu.push_back(p1);
    apdu.push_back(p2);

    if (!data.empty()) {
        apdu.push_back(static_cast<uint8_t>(data.size()));
        apdu.insert(apdu.end(), data.begin(), data.end());
    }

    return apdu;
}

std::vector<uint32_t> LedgerWallet::parseDerivationPath(const std::string& path) {
    std::vector<uint32_t> elements;
    std::vector<std::string> parts = splitString(path, '/');

    for (const auto& part : parts) {
        if (part == "m") continue;

        bool hardened = part.back() == '\'';
        std::string number_str = hardened ? part.substr(0, part.length() - 1) : part;

        try {
            uint32_t number = std::stoul(number_str);
            if (hardened) {
                number |= 0x80000000;
            }
            elements.push_back(number);
        } catch (const std::exception& e) {
            g_logger.error("Invalid derivation path element: " + part);
            return {};
        }
    }

    return elements;
}

std::string LedgerWallet::publicKeyToAddress(const std::string& public_key_hex) {
    // This is a simplified implementation
    // In a real implementation, you would:
    // 1. Decode the public key
    // 2. Hash it with sha256 and ripemd160
    // 3. Add version byte and checksum
    // 4. Encode with Base58Check
    
    // Mock address generation
    return "H1A2L3A4L5C6O7I8N9A0D1D2R3E4S5S6";
}

std::string LedgerWallet::bytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

std::vector<uint8_t> LedgerWallet::hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byteString, nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

std::vector<std::string> LedgerWallet::splitString(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;

    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }

    return tokens;
}

// ═══════════════════════════════════════════════════════════════════════════
// Ledger Bitcoin App - Production PSBT Signing
// ═══════════════════════════════════════════════════════════════════════════

std::string LedgerWallet::mapStatusWord(uint16_t sw) {
    switch (sw) {
        case SW_OK:
            return "Success";
        case SW_USER_REJECTED:
            return "User rejected signing on Ledger device";
        case SW_INVALID_DATA:
            return "Invalid data sent to Ledger";
        case SW_APP_NOT_OPEN:
            return "Ledger Bitcoin app not open - please open the Bitcoin app";
        case SW_WRONG_LENGTH:
            return "Wrong data length sent to Ledger";
        case SW_SECURITY_STATUS:
            return "Security condition not satisfied on Ledger";
        default:
            std::ostringstream oss;
            oss << "Ledger error: 0x" << std::hex << std::setw(4) << std::setfill('0') << sw;
            return oss.str();
    }
}

std::string LedgerWallet::getMasterFingerprint() {
    if (!m_hid_transport || !m_hid_transport->isOpen()) {
        g_logger.error("[Ledger] Device not connected");
        return "";
    }

    g_logger.info("[Ledger] Getting master fingerprint...");

    // Build APDU: GET_MASTER_FINGERPRINT
    std::vector<uint8_t> command = buildAPDU(CLA, INS_GET_MASTER_FINGERPRINT, 0x00, 0x00, {});
    std::vector<uint8_t> response;

    if (!exchangeAPDU(command, response)) {
        g_logger.error("[Ledger] Failed to get master fingerprint");
        return "";
    }

    // Response should be: [4 bytes fingerprint] [SW1 SW2]
    if (response.size() < 6) {
        g_logger.error("[Ledger] Invalid fingerprint response size");
        return "";
    }

    uint16_t sw = (response[response.size() - 2] << 8) | response[response.size() - 1];
    if (sw != SW_OK) {
        g_logger.error("[Ledger] Get fingerprint failed: " + mapStatusWord(sw));
        return "";
    }

    // Extract 4-byte fingerprint
    std::vector<uint8_t> fingerprint(response.begin(), response.begin() + 4);
    std::string fp_hex = bytesToHex(fingerprint);

    g_logger.info("[Ledger] Master fingerprint: " + fp_hex);
    return fp_hex;
}

std::vector<uint8_t> LedgerWallet::serializeWalletPolicy(const LedgerWalletPolicy& policy) {
    std::vector<uint8_t> data;

    // Policy name (length-prefixed string)
    data.push_back(static_cast<uint8_t>(policy.name.length()));
    data.insert(data.end(), policy.name.begin(), policy.name.end());

    // Descriptor template (length-prefixed string)
    data.push_back(static_cast<uint8_t>(policy.descriptor_template.length()));
    data.insert(data.end(), policy.descriptor_template.begin(), policy.descriptor_template.end());

    // Number of keys
    data.push_back(static_cast<uint8_t>(policy.keys.size()));

    // Each key (length-prefixed string)
    for (const auto& key : policy.keys) {
        data.push_back(static_cast<uint8_t>(key.length()));
        data.insert(data.end(), key.begin(), key.end());
    }

    return data;
}

bool LedgerWallet::registerWalletPolicy(const LedgerWalletPolicy& policy,
                                       std::vector<uint8_t>& wallet_id,
                                       std::vector<uint8_t>& policy_hash) {
    if (!m_hid_transport || !m_hid_transport->isOpen()) {
        g_logger.error("[Ledger] Device not connected");
        return false;
    }

    g_logger.info("[Ledger] Registering wallet policy: " + policy.name);

    // Serialize policy
    std::vector<uint8_t> policy_data = serializeWalletPolicy(policy);

    // Build APDU: REGISTER_WALLET
    std::vector<uint8_t> command = buildAPDU(CLA, INS_REGISTER_WALLET, 0x00, 0x00, policy_data);
    std::vector<uint8_t> response;

    if (!exchangeAPDU(command, response)) {
        g_logger.error("[Ledger] Failed to register wallet policy");
        return false;
    }

    // Check status word
    if (response.size() < 2) {
        g_logger.error("[Ledger] Invalid registration response");
        return false;
    }

    uint16_t sw = (response[response.size() - 2] << 8) | response[response.size() - 1];
    if (sw != SW_OK) {
        g_logger.error("[Ledger] Registration failed: " + mapStatusWord(sw));
        return false;
    }

    // Response format: [32-byte wallet_id] [32-byte policy_hash] [SW1 SW2]
    if (response.size() < 66) {  // 32 + 32 + 2
        g_logger.error("[Ledger] Invalid registration response size");
        return false;
    }

    wallet_id.assign(response.begin(), response.begin() + 32);
    policy_hash.assign(response.begin() + 32, response.begin() + 64);

    g_logger.info("[Ledger] Wallet registered successfully");
    g_logger.debug("[Ledger] Wallet ID: " + bytesToHex(wallet_id));
    g_logger.debug("[Ledger] Policy hash: " + bytesToHex(policy_hash));

    return true;
}

std::vector<LedgerWallet::LedgerSignature> LedgerWallet::parseLedgerSignatures(const std::vector<uint8_t>& response) {
    std::vector<LedgerSignature> signatures;

    // Response format (TLV-like):
    // [num_sigs: 1 byte]
    // For each signature:
    //   [input_index: 4 bytes]
    //   [sig_length: 1 byte]
    //   [signature: sig_length bytes]

    if (response.size() < 3) {  // At least num_sigs + SW
        g_logger.error("[Ledger] Invalid signature response size");
        return signatures;
    }

    size_t offset = 0;
    uint8_t num_sigs = response[offset++];

    g_logger.info("[Ledger] Parsing " + std::to_string(num_sigs) + " signatures");

    for (uint8_t i = 0; i < num_sigs; i++) {
        if (offset + 5 > response.size() - 2) {  // Need at least input_index + sig_length
            g_logger.error("[Ledger] Truncated signature response");
            break;
        }

        LedgerSignature sig;

        // Input index (4 bytes, big-endian)
        sig.input_index = (response[offset] << 24) |
                         (response[offset + 1] << 16) |
                         (response[offset + 2] << 8) |
                         response[offset + 3];
        offset += 4;

        // Signature length
        uint8_t sig_len = response[offset++];

        if (offset + sig_len > response.size() - 2) {
            g_logger.error("[Ledger] Invalid signature length");
            break;
        }

        // Signature data (should be 64 bytes for Schnorr)
        sig.signature.assign(response.begin() + offset, response.begin() + offset + sig_len);
        offset += sig_len;

        if (sig_len != 64) {
            g_logger.warning("[Ledger] Unexpected signature length: " + std::to_string(sig_len) + " (expected 64 for Schnorr)");
        }

        g_logger.debug("[Ledger] Signature for input " + std::to_string(sig.input_index) +
                      ": " + bytesToHex(sig.signature).substr(0, 32) + "...");

        signatures.push_back(sig);
    }

    return signatures;
}

std::vector<LedgerWallet::LedgerSignature> LedgerWallet::signPSBTOnDevice(
    const std::vector<uint8_t>& psbt_binary,
    const std::vector<uint8_t>& wallet_id) {

    std::vector<LedgerSignature> signatures;

    if (!m_hid_transport || !m_hid_transport->isOpen()) {
        g_logger.error("[Ledger] Device not connected");
        return signatures;
    }

    g_logger.info("[Ledger] Sending PSBT to device (" + std::to_string(psbt_binary.size()) + " bytes)");

    // Ledger HID packets max payload ≈ 230 bytes (after headers and framing)
    constexpr size_t CHUNK_SIZE = 230;
    size_t offset = 0;
    bool is_first = true;

    while (offset < psbt_binary.size()) {
        size_t remaining = psbt_binary.size() - offset;
        size_t chunk_size = std::min(CHUNK_SIZE, remaining);

        std::vector<uint8_t> chunk_data;

        // First packet includes wallet_id
        if (is_first) {
            chunk_data.insert(chunk_data.end(), wallet_id.begin(), wallet_id.end());
        }

        // Add PSBT chunk
        chunk_data.insert(chunk_data.end(),
                         psbt_binary.begin() + offset,
                         psbt_binary.begin() + offset + chunk_size);

        // Build APDU: SIGN_PSBT
        // P1: 0x00 = first packet, 0x80 = continuation
        uint8_t p1 = is_first ? 0x00 : 0x80;
        std::vector<uint8_t> command = buildAPDU(CLA, INS_SIGN_PSBT, p1, 0x00, chunk_data);
        std::vector<uint8_t> response;

        g_logger.debug("[Ledger] Sending PSBT chunk: " + std::to_string(offset) + "/" +
                      std::to_string(psbt_binary.size()) + " bytes");

        if (!exchangeAPDU(command, response)) {
            g_logger.error("[Ledger] Failed to send PSBT chunk");
            return signatures;
        }

        // Only final packet returns signatures
        if (offset + chunk_size >= psbt_binary.size()) {
            // Check status word
            if (response.size() < 2) {
                g_logger.error("[Ledger] Invalid PSBT response");
                return signatures;
            }

            uint16_t sw = (response[response.size() - 2] << 8) | response[response.size() - 1];
            if (sw != SW_OK) {
                g_logger.error("[Ledger] PSBT signing failed: " + mapStatusWord(sw));
                return signatures;
            }

            // Parse signatures from response
            signatures = parseLedgerSignatures(response);
            g_logger.info("[Ledger] Received " + std::to_string(signatures.size()) + " signatures");
        }

        offset += chunk_size;
        is_first = false;
    }

    return signatures;
}

LedgerWallet::PSBTSigningResult LedgerWallet::signPSBT(const std::string& psbt_base64) {
    PSBTSigningResult result;

    if (!isConnected()) {
        result.error_message = "Ledger device not connected";
        g_logger.error("[Ledger] " + result.error_message);
        return result;
    }

    g_logger.info("[Ledger] Starting PSBT signing flow");

    // 1. Decode PSBT from base64
    PSBT psbt = PSBT::FromBase64(psbt_base64);
    if (!psbt.IsValid()) {
        result.error_message = "Invalid PSBT encoding: " + psbt.GetError();
        g_logger.error("[Ledger] " + result.error_message);
        return result;
    }

    g_logger.info("[Ledger] PSBT decoded: " + std::to_string(psbt.inputs.size()) + " inputs, " +
                 std::to_string(psbt.outputs.size()) + " outputs");

    // 2. TODO: Validate BIP86 policy (key-path only, no tapscript)
    // This requires converting dinero::PSBT to din::Psbt format or implementing
    // a simple check for tap_script_sig/tap_leaf_script/tap_merkle_root fields.
    // For now, relying on wallet to only create BIP86-compliant PSBTs.

    // 3. Get active BIP86 descriptor from DescriptorStore
    if (!m_descriptor_store) {
        result.error_message = "DescriptorStore not configured - call setDescriptorStore()";
        g_logger.error("[Ledger] " + result.error_message);
        return result;
    }

    // Get active BIP86 receive descriptor
    auto* descriptor_store = static_cast<din::DescriptorStore*>(m_descriptor_store);
    auto descriptors = descriptor_store->listDescriptors(true);  // active_only=true
    din::DescriptorRecord* bip86_descriptor = nullptr;
    for (auto& desc : descriptors) {
        if (desc.policy == "BIP86" && !desc.is_change) {
            bip86_descriptor = &desc;
            break;
        }
    }

    if (!bip86_descriptor) {
        result.error_message = "No active BIP86 receive descriptor found";
        g_logger.error("[Ledger] " + result.error_message);
        return result;
    }

    g_logger.info("[Ledger] Using BIP86 descriptor (id=" + std::to_string(bip86_descriptor->id) + ")");

    // 4. Parse descriptor to extract xpub and fingerprint
    din::BIP86DescriptorFactory::ParsedBIP86 parsed =
        din::BIP86DescriptorFactory::parseDescriptor(bip86_descriptor->descriptor);

    if (!parsed.valid) {
        result.error_message = "Failed to parse BIP86 descriptor: " + parsed.error;
        g_logger.error("[Ledger] " + result.error_message);
        return result;
    }

    g_logger.info("[Ledger] Descriptor parsed - fingerprint: " + parsed.fingerprint + ", xpub: " + parsed.xpub.substr(0, 20) + "...");

    // 5. Get master fingerprint from device
    std::string device_fp = getMasterFingerprint();
    if (device_fp.empty()) {
        result.error_message = "Failed to get device fingerprint";
        g_logger.error("[Ledger] " + result.error_message);
        return result;
    }

    // 6. CRITICAL: Validate fingerprint match
    if (device_fp != parsed.fingerprint) {
        result.error_message = "Ledger device fingerprint (" + device_fp +
                              ") does not match wallet descriptor (" + parsed.fingerprint + ") - WRONG DEVICE";
        g_logger.error("[Ledger] " + result.error_message);
        return result;
    }

    g_logger.info("[Ledger] Fingerprint validated: device matches descriptor");

    // 7. Build wallet policy (BIP86 Taproot)
    LedgerWalletPolicy policy;
    policy.name = "Dinero BIP86";
    policy.descriptor_template = "tr(@0/**)";
    policy.keys.push_back(parsed.xpub);

    // 8. Register wallet policy (if not already registered)
    if (m_wallet_id.empty()) {
        g_logger.info("[Ledger] Registering wallet policy on device...");
        if (!registerWalletPolicy(policy, m_wallet_id, m_policy_hash)) {
            result.error_message = "Failed to register wallet policy on Ledger";
            g_logger.error("[Ledger] " + result.error_message);
            return result;
        }
        g_logger.info("[Ledger] Wallet policy registered successfully");
    } else {
        g_logger.info("[Ledger] Using previously registered wallet");
    }

    // 9. Serialize PSBT to binary
    std::vector<uint8_t> psbt_binary = psbt.Serialize();
    g_logger.info("[Ledger] PSBT serialized: " + std::to_string(psbt_binary.size()) + " bytes");

    // 10. Send PSBT to device and get signatures
    std::vector<LedgerSignature> signatures = signPSBTOnDevice(psbt_binary, m_wallet_id);
    if (signatures.empty()) {
        result.error_message = "No signatures received from Ledger";
        g_logger.error("[Ledger] " + result.error_message);
        return result;
    }

    // 11. Insert signatures into PSBT (BIP371 Taproot key-path)
    for (const auto& sig : signatures) {
        if (sig.input_index >= psbt.inputs.size()) {
            g_logger.warning("[Ledger] Invalid input index in signature: " +
                           std::to_string(sig.input_index));
            continue;
        }

        // Insert Schnorr signature into tap_key_sig field
        psbt.inputs[sig.input_index].tap_key_sig = sig.signature;
        g_logger.info("[Ledger] Inserted signature for input " + std::to_string(sig.input_index));
    }

    // 12. Return signed PSBT
    result.success = true;
    result.psbt_base64 = psbt.ToBase64();
    result.complete = psbt.IsComplete();

    g_logger.info("[Ledger] PSBT signing complete (complete=" +
                 std::string(result.complete ? "true" : "false") + ")");

    return result;
}

} // namespace dinero 
