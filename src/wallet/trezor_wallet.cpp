#include "wallet/hardware_wallet.h"
#include "wallet/trezor_transport.h"
#include "wallet/hid_transport.h"
#include "wallet/psbt.h"
#include "wallet/bip84_descriptor.h"
#include "wallet/descriptor_store.h"
#include "wallet/bip86_descriptor.h"
#include "wallet/retired_coin_type_guard.h"
#include "crypto/extended_pubkey.h"
#include "wallet/taproot_keys.h"
#include "address/addr_codec.h"
#include "consensus/chainparams.h"
#include "common/logger.h"
#include "consensus/coin_type.h"
#include "crypto/hash.h"
#include "crypto/sha256.h"
#include "bech32/bech32.hpp"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <map>

// Trezor protobuf messages
#include "messages.pb.h"
#include "messages-bitcoin.pb.h"
#include "messages-common.pb.h"
#include "messages-management.pb.h"

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

extern Logger g_logger;  // Defined in main.cpp

namespace {

bool IsRootDerivationPath(const std::string& path) {
    return path == "m" || path == "m/";
}

bool ParseDerivationPath(const std::string& path, std::vector<uint32_t>& out, std::string& error) {
    out.clear();

    if (path.empty() || path[0] != 'm') {
        error = "Derivation path must start with m";
        return false;
    }
    if (IsRootDerivationPath(path)) {
        return true;
    }
    if (path.size() < 3 || path[1] != '/') {
        error = "Derivation path must be in m/... form";
        return false;
    }

    std::stringstream ss(path.substr(2));
    std::string token;
    while (std::getline(ss, token, '/')) {
        if (token.empty()) {
            error = "Derivation path contains an empty element";
            return false;
        }

        bool hardened = false;
        if (!token.empty() && (token.back() == '\'' || token.back() == 'h' || token.back() == 'H')) {
            hardened = true;
            token.pop_back();
        }
        if (token.empty()) {
            error = "Derivation path contains an invalid hardened marker";
            return false;
        }

        try {
            uint32_t value = static_cast<uint32_t>(std::stoul(token));
            if (hardened) {
                value |= 0x80000000U;
            }
            out.push_back(value);
        } catch (const std::exception&) {
            error = "Invalid derivation element: " + token;
            return false;
        }
    }

    return true;
}

std::string FormatTrezorFailure(const hw::trezor::messages::common::Failure& failure) {
    if (failure.has_code()) {
        return hw::trezor::messages::common::Failure::FailureType_Name(failure.code()) +
               ": " + failure.message();
    }
    return failure.message();
}

hw::trezor::messages::bitcoin::InputScriptType ScriptTypeForPath(const std::vector<uint32_t>& path) {
    if (!path.empty()) {
        const uint32_t purpose = path[0] & 0x7fffffffU;
        if (purpose == 84U) {
            return hw::trezor::messages::bitcoin::SPENDWITNESS;
        }
        if (purpose == 86U) {
            return hw::trezor::messages::bitcoin::SPENDTAPROOT;
        }
    }
    return hw::trezor::messages::bitcoin::SPENDADDRESS;
}

bool ValidateCanonicalCoinTypePath(const std::vector<uint32_t>& path,
                                   const std::string& context,
                                   std::string& error) {
    if (path.size() < 2) {
        error = context + " requires a derivation path containing coin_type 1448";
        return false;
    }
    if ((path[1] & 0x80000000U) == 0U) {
        error = context + " requires a hardened coin_type element: 1448'";
        return false;
    }
    try {
        dinero::wallet::RejectRetiredLegacyCoinTypePath(path, context);
        dinero::wallet::RejectNonCanonicalCoinTypePath(path, context);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

std::string CompressPubkeyHex(const std::string& public_key_hex) {
    if (public_key_hex.size() == 66) {
        return public_key_hex;
    }
    if (public_key_hex.size() != 130 || public_key_hex.rfind("04", 0) != 0) {
        return {};
    }

    const std::string y_last_byte = public_key_hex.substr(public_key_hex.size() - 2);
    const uint8_t parity = static_cast<uint8_t>(std::strtoul(y_last_byte.c_str(), nullptr, 16) & 0x01);
    return std::string(parity ? "03" : "02") + public_key_hex.substr(2, 64);
}

std::string FingerprintFromCompressedPubkey(const std::string& compressed_pubkey_hex) {
    if (compressed_pubkey_hex.empty()) {
        return {};
    }

    std::vector<uint8_t> pubkey_bytes;
    pubkey_bytes.reserve(compressed_pubkey_hex.size() / 2);
    for (size_t i = 0; i < compressed_pubkey_hex.size(); i += 2) {
        pubkey_bytes.push_back(static_cast<uint8_t>(
            std::strtoul(compressed_pubkey_hex.substr(i, 2).c_str(), nullptr, 16)));
    }

    const auto hash160 = din::crypto::HASH160(pubkey_bytes);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(2) << static_cast<int>(hash160[0])
        << std::setw(2) << static_cast<int>(hash160[1])
        << std::setw(2) << static_cast<int>(hash160[2])
        << std::setw(2) << static_cast<int>(hash160[3]);
    return oss.str();
}

std::string NormalizeDescriptorPolicy(std::string policy) {
    std::transform(policy.begin(), policy.end(), policy.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return policy;
}

std::string PolicyFromPurpose(uint32_t purpose) {
    if (purpose == 84U) {
        return "bip84";
    }
    if (purpose == 86U) {
        return "bip86";
    }
    return {};
}

constexpr uint32_t kBip86HostDiscoveryLimit = 4096;

std::string ActiveHrp() {
    std::string hrp = dinero::HrpForActiveNetworkRef();
    if (hrp.empty()) {
        hrp = "din";
    }
    return hrp;
}

std::string HexString(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

bool TryExtractOpReturnData(const std::vector<uint8_t>& script, std::vector<uint8_t>& data_out) {
    data_out.clear();
    if (script.empty() || script[0] != 0x6a) {  // OP_RETURN
        return false;
    }
    if (script.size() == 1) {
        return true;
    }

    const uint8_t opcode = script[1];
    if (opcode <= 75 && script.size() == static_cast<size_t>(opcode) + 2) {
        data_out.assign(script.begin() + 2, script.end());
        return true;
    }
    if (opcode == 0x4c && script.size() >= 3) {  // OP_PUSHDATA1
        const uint8_t len = script[2];
        if (script.size() == static_cast<size_t>(len) + 3) {
            data_out.assign(script.begin() + 3, script.end());
            return true;
        }
    }

    return false;
}

bool EncodeAddressFromScript(const std::vector<uint8_t>& script, std::string& address_out) {
    address_out.clear();
    const std::string hrp = ActiveHrp();

    if (script.size() == 22 && script[0] == 0x00 && script[1] == 0x14) {
        address_out = bech32::Encode(hrp,
                                     0,
                                     std::vector<uint8_t>(script.begin() + 2, script.end()));
        return !address_out.empty();
    }
    if (script.size() == 34 && script[0] == 0x00 && script[1] == 0x20) {
        address_out = bech32::Encode(hrp,
                                     0,
                                     std::vector<uint8_t>(script.begin() + 2, script.end()));
        return !address_out.empty();
    }
    if (script.size() == 34 && script[0] == 0x51 && script[1] == 0x20) {
        address_out = bech32::Encode(hrp,
                                     1,
                                     std::vector<uint8_t>(script.begin() + 2, script.end()),
                                     bech32::Encoding::BECH32M);
        return !address_out.empty();
    }

    return false;
}

bool BuildBip86ScriptPubKey(const dinero::crypto::ExtendedPubKey& account_xpub,
                            uint32_t chain,
                            uint32_t index,
                            std::vector<uint8_t>& script_out,
                            std::string& error) {
    script_out.clear();
    error.clear();

    try {
        auto chain_key = account_xpub.Derive(chain);
        auto child_key = chain_key.Derive(index);
        const auto compressed_pubkey = child_key.GetPublicKey();
        if (compressed_pubkey.size() != 33) {
            error = "Derived compressed pubkey has unexpected size";
            return false;
        }

        std::array<uint8_t, 32> internal_xonly{};
        std::copy(compressed_pubkey.begin() + 1, compressed_pubkey.end(), internal_xonly.begin());

        std::array<uint8_t, 32> output_key{};
        if (!dinero::TaprootKeys::ComputeTweakedPubkey(internal_xonly, output_key)) {
            error = "Failed to compute tweaked Taproot output key";
            return false;
        }

        script_out = dinero::CreateP2TRScriptPubKey(std::vector<uint8_t>(output_key.begin(), output_key.end()));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool BuildBip86ScriptIndex(const dinero::crypto::ExtendedPubKey& account_xpub,
                           const std::vector<uint32_t>& account_path,
                           std::map<std::string, std::vector<uint32_t>>& script_index_out,
                           std::string& error) {
    script_index_out.clear();
    error.clear();

    if (account_path.size() != 3) {
        error = "BIP86 account path must contain purpose/coin/account";
        return false;
    }

    for (uint32_t chain = 0; chain <= 1; ++chain) {
        for (uint32_t index = 0; index < kBip86HostDiscoveryLimit; ++index) {
            std::vector<uint8_t> script_pubkey;
            if (!BuildBip86ScriptPubKey(account_xpub, chain, index, script_pubkey, error)) {
                return false;
            }

            std::vector<uint32_t> full_path = account_path;
            full_path.push_back(chain);
            full_path.push_back(index);
            script_index_out.emplace(HexString(script_pubkey), std::move(full_path));
        }
    }

    return true;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════════════

TrezorWallet::TrezorWallet()
    : m_status(ConnectionStatus::DISCONNECTED)
    , m_trezor_transport(std::make_unique<TrezorTransport>())
    , m_coin_type(dinero::consensus::DINERO_COIN_TYPE)
    , m_account_index(0)
    , m_running(false)
{
    m_network_type = "mainnet";
}

TrezorWallet::~TrezorWallet() {
    shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Core Functionality
// ═══════════════════════════════════════════════════════════════════════════

bool TrezorWallet::initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);

    g_logger.info("[TrezorWallet] Initializing Trezor Wallet");
    m_last_error.clear();

    m_running = true;
    m_status = ConnectionStatus::DISCONNECTED;

    g_logger.info("[TrezorWallet] Trezor Wallet initialized successfully");
    return true;
}

void TrezorWallet::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);

    g_logger.info("[TrezorWallet] Shutting down Trezor Wallet");

    m_running = false;
    m_last_error.clear();

    if (m_trezor_transport && m_trezor_transport->isOpen()) {
        m_trezor_transport->close();
    }

    m_status = ConnectionStatus::DISCONNECTED;

    g_logger.info("[TrezorWallet] Trezor Wallet shutdown complete");
}

bool TrezorWallet::isConnected() const {
    return m_status == ConnectionStatus::CONNECTED ||
           m_status == ConnectionStatus::READY;
}

ConnectionStatus TrezorWallet::getStatus() const {
    return m_status;
}

// ═══════════════════════════════════════════════════════════════════════════
// Device Management
// ═══════════════════════════════════════════════════════════════════════════

bool TrezorWallet::connect() {
    std::lock_guard<std::mutex> lock(m_mutex);

    g_logger.info("[TrezorWallet] Connecting to Trezor device");
    m_last_error.clear();

    m_status = ConnectionStatus::CONNECTING;

    if (!openDevice()) {
        m_status = ConnectionStatus::ERROR;
        if (m_last_error.empty()) {
            m_last_error = "Failed to open Trezor device";
        }
        g_logger.error("[TrezorWallet] " + m_last_error);
        return false;
    }

    if (!initializeSessionLocked()) {
        closeDevice();
        m_status = ConnectionStatus::ERROR;
        if (m_last_error.empty()) {
            m_last_error = "Failed to initialize Trezor session";
        }
        g_logger.error("[TrezorWallet] " + m_last_error);
        return false;
    }

    m_status = ConnectionStatus::READY;
    g_logger.info("[TrezorWallet] Connected to Trezor device");
    return true;
}

void TrezorWallet::disconnect() {
    std::lock_guard<std::mutex> lock(m_mutex);

    g_logger.info("[TrezorWallet] Disconnecting from Trezor device");

    if (m_trezor_transport && m_trezor_transport->isOpen()) {
        m_trezor_transport->close();
    }

    m_status = ConnectionStatus::DISCONNECTED;
    g_logger.info("[TrezorWallet] Disconnected from Trezor device");
}

DeviceInfo TrezorWallet::getDeviceInfo() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!isConnected()) {
        m_last_error = "Device not connected";
        g_logger.error("[TrezorWallet] " + m_last_error);
        return DeviceInfo();
    }

    return m_device_info;
}

std::vector<DeviceInfo> TrezorWallet::enumerateDevices() {
    g_logger.info("[TrezorWallet] Enumerating Trezor devices");

    std::vector<DeviceInfo> devices;

    // Enumerate Trezor devices via HID
    auto hid_devices = HIDTransport::enumerate(TrezorIDs::TREZOR_VENDOR_1, 0);

    for (const auto& hid_dev : hid_devices) {
        DeviceInfo dev;
        dev.name = hid_dev.product;
        dev.serial_number = hid_dev.serial;
        dev.type = HardwareWalletType::TREZOR;
        dev.supports_dinero = true;  // Trezor supports generic Bitcoin signing
        devices.push_back(dev);
    }

    g_logger.info("[TrezorWallet] Found " + std::to_string(devices.size()) + " Trezor devices");
    return devices;
}

// ═══════════════════════════════════════════════════════════════════════════
// Address Management
// ═══════════════════════════════════════════════════════════════════════════

AddressInfo TrezorWallet::getAddress(const std::string& derivation_path) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AddressInfo info;
    info.derivation_path = derivation_path;

#ifndef DIN_HW_WALLET_MOCK
    if (!isConnected()) {
        m_last_error = "Trezor device not connected";
        g_logger.error("[TrezorWallet] Trezor device not connected");
        return info;
    }

    std::vector<uint32_t> path;
    std::string parse_error;
    if (!ParseDerivationPath(derivation_path, path, parse_error)) {
        m_last_error = parse_error;
        g_logger.error("[TrezorWallet] " + m_last_error);
        return info;
    }
    std::string coin_type_error;
    if (!ValidateCanonicalCoinTypePath(path, "Trezor address retrieval", coin_type_error)) {
        m_last_error = coin_type_error;
        g_logger.error("[TrezorWallet] " + m_last_error);
        return info;
    }

    std::string address;
    if (!requestAddressLocked(derivation_path, /*show_display=*/false, address)) {
        g_logger.error("[TrezorWallet] " + m_last_error);
        return info;
    }

    std::string public_key_hex;
    if (!requestPublicKeyLocked(derivation_path, public_key_hex, nullptr)) {
        g_logger.warning("[TrezorWallet] Address fetched without public key metadata: " + m_last_error);
    }

    info.address = address;
    info.public_key = public_key_hex;
    if (!path.empty()) {
        info.index = path.back() & 0x7fffffffU;
        if (path.size() >= 2) {
            info.is_change = ((path[path.size() - 2] & 0x7fffffffU) == 1U);
        }
    }
    return info;
#else
    // TEST ONLY: Mock address generation
    g_logger.warning("⚠️  MOCK: Returning test Trezor address (NOT PRODUCTION SAFE)");
    info.address = "bc1p_trezor_mock_" + derivation_path;
    info.public_key = "02" + std::string(64, '0');
    return info;
#endif
}

std::vector<AddressInfo> TrezorWallet::getAddresses(uint32_t start_index, uint32_t count, bool change) {
    std::vector<AddressInfo> addresses;

    for (uint32_t i = 0; i < count; i++) {
        std::string path = getDerivationPath(m_account_index, change, start_index + i);
        AddressInfo info = getAddress(path);
        if (!info.address.empty()) {
            addresses.push_back(info);
        }
    }

    return addresses;
}

bool TrezorWallet::verifyAddress(const std::string& address, const std::string& derivation_path) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!isConnected()) {
        m_last_error = "Trezor device not connected";
        g_logger.error("[TrezorWallet] Trezor device not connected");
        return false;
    }

    std::string displayed_address;
    if (!requestAddressLocked(derivation_path, /*show_display=*/true, displayed_address)) {
        g_logger.error("[TrezorWallet] " + m_last_error);
        return false;
    }

    if (!address.empty() && displayed_address != address) {
        m_last_error = "Trezor returned address " + displayed_address +
                       " which does not match expected address " + address;
        g_logger.error("[TrezorWallet] " + m_last_error);
        return false;
    }

    return !displayed_address.empty();
}

// ═══════════════════════════════════════════════════════════════════════════
// Transaction Signing (Legacy Methods - Use signPSBT for Production)
// ═══════════════════════════════════════════════════════════════════════════

SigningResult TrezorWallet::signTransaction(const SigningRequest& request) {
    SigningResult result;

#ifndef DIN_HW_WALLET_MOCK
    // Production: Refuse to sign without device
    result.success = false;
    result.error_message = "Trezor device not connected";
    g_logger.error("[TrezorWallet] " + result.error_message);
    return result;
#else
    // TEST ONLY: Mock signing
    g_logger.warning("⚠️  MOCK SIGNING ACTIVE - NOT PRODUCTION SAFE");
    result.success = true;
    result.signatures.push_back("mock_trezor_signature_" + request.transaction_hash);
    return result;
#endif
}

SigningResult TrezorWallet::signMessage(const std::string& message, const std::string& derivation_path) {
    SigningResult result;

#ifndef DIN_HW_WALLET_MOCK
    // Production: Refuse to sign without device
    result.success = false;
    result.error_message = "Trezor device not connected";
    g_logger.error("[TrezorWallet] " + result.error_message);
    return result;
#else
    // TEST ONLY: Mock signing
    g_logger.warning("⚠️  MOCK SIGNING ACTIVE - NOT PRODUCTION SAFE");
    result.success = true;
    result.signatures.push_back("mock_trezor_msg_sig_" + message.substr(0, 10));
    return result;
#endif
}

bool TrezorWallet::verifySignature(const std::string& message, const std::string& signature, const std::string& public_key) {
    // Bitcoin message signature verification
    // Signature format: base64-encoded 65-byte compact signature (1 byte header + 64 byte signature)
    // Public key format: hex-encoded compressed (33 bytes) or uncompressed (65 bytes) public key

    // 1. Decode base64 signature
    static const std::string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    auto base64_decode = [&](const std::string& encoded) -> std::vector<uint8_t> {
        std::vector<uint8_t> decoded;
        int val = 0, valb = -8;
        for (unsigned char c : encoded) {
            if (c == '=') break;
            size_t pos = base64_chars.find(c);
            if (pos == std::string::npos) continue;
            val = (val << 6) + static_cast<int>(pos);
            valb += 6;
            if (valb >= 0) {
                decoded.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return decoded;
    };

    std::vector<uint8_t> sig_bytes = base64_decode(signature);
    if (sig_bytes.size() != 65) {
        g_logger.error("[TrezorWallet] Invalid signature length: " + std::to_string(sig_bytes.size()));
        return false;
    }

    // 2. Decode hex public key
    auto hex_decode = [](const std::string& hex) -> std::vector<uint8_t> {
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i < hex.length(); i += 2) {
            bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        }
        return bytes;
    };

    std::vector<uint8_t> pubkey_bytes = hex_decode(public_key);
    if (pubkey_bytes.size() != 33 && pubkey_bytes.size() != 65) {
        g_logger.error("[TrezorWallet] Invalid public key length: " + std::to_string(pubkey_bytes.size()));
        return false;
    }

    // 3. Create Bitcoin signed message hash
    // Format: "\x18Bitcoin Signed Message:\n" + varint(len) + message
    std::string magic = "\x18" "Bitcoin Signed Message:\n";
    std::vector<uint8_t> msg_data;
    msg_data.insert(msg_data.end(), magic.begin(), magic.end());
    // Varint for message length
    if (message.length() < 253) {
        msg_data.push_back(static_cast<uint8_t>(message.length()));
    } else {
        msg_data.push_back(0xfd);
        msg_data.push_back(message.length() & 0xff);
        msg_data.push_back((message.length() >> 8) & 0xff);
    }
    msg_data.insert(msg_data.end(), message.begin(), message.end());

    // Double SHA256
    uint8_t hash1[32], hash2[32];
    dinero::crypto::CSHA256().Write(msg_data.data(), msg_data.size()).Finalize(hash1);
    dinero::crypto::CSHA256().Write(hash1, 32).Finalize(hash2);

    // 4. Verify signature using secp256k1
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) {
        g_logger.error("[TrezorWallet] Failed to create secp256k1 context");
        return false;
    }

    // Parse the public key
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_parse(ctx, &pubkey, pubkey_bytes.data(), pubkey_bytes.size())) {
        g_logger.error("[TrezorWallet] Failed to parse public key");
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Extract recovery id from signature header (first byte)
    int recid = (sig_bytes[0] - 27) & 3;
    bool compressed = (sig_bytes[0] - 27) >= 4;
    (void)compressed;  // Not used for verification

    // Parse recoverable signature
    secp256k1_ecdsa_recoverable_signature rsig;
    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(ctx, &rsig, sig_bytes.data() + 1, recid)) {
        g_logger.error("[TrezorWallet] Failed to parse signature");
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Recover public key from signature and compare
    secp256k1_pubkey recovered_pubkey;
    if (!secp256k1_ecdsa_recover(ctx, &recovered_pubkey, &rsig, hash2)) {
        g_logger.error("[TrezorWallet] Failed to recover public key from signature");
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Compare recovered pubkey with provided pubkey
    unsigned char recovered_bytes[65];
    size_t recovered_len = 65;
    secp256k1_ec_pubkey_serialize(ctx, recovered_bytes, &recovered_len, &recovered_pubkey, SECP256K1_EC_UNCOMPRESSED);

    unsigned char provided_bytes[65];
    size_t provided_len = 65;
    secp256k1_ec_pubkey_serialize(ctx, provided_bytes, &provided_len, &pubkey, SECP256K1_EC_UNCOMPRESSED);

    secp256k1_context_destroy(ctx);

    bool match = (recovered_len == provided_len &&
                  std::memcmp(recovered_bytes, provided_bytes, recovered_len) == 0);

    if (match) {
        g_logger.info("[TrezorWallet] Signature verified successfully");
    } else {
        g_logger.warning("[TrezorWallet] Signature verification failed - public key mismatch");
    }

    return match;
}

// ═══════════════════════════════════════════════════════════════════════════
// Production PSBT Signing (BIP174 + BIP371 Taproot)
// ═══════════════════════════════════════════════════════════════════════════

TrezorWallet::PSBTSigningResult TrezorWallet::signPSBT(const std::string& psbt_base64) {
    PSBTSigningResult result;
    std::lock_guard<std::mutex> lock(m_mutex);

    g_logger.info("[TrezorWallet] PSBT signing requested");

    if (!isConnected()) {
        result.error_message = "Trezor device not connected";
        g_logger.error("[TrezorWallet] " + result.error_message);
        return result;
    }

    // 1. Decode PSBT from base64
    PSBT psbt = PSBT::FromBase64(psbt_base64);
    if (!psbt.IsValid()) {
        result.error_message = "Invalid PSBT encoding: " + psbt.GetError();
        g_logger.error("[TrezorWallet] " + result.error_message);
        return result;
    }

    g_logger.info("[TrezorWallet] PSBT decoded: " + std::to_string(psbt.inputs.size()) + " inputs, " +
                 std::to_string(psbt.outputs.size()) + " outputs");

    // 2. TODO: Validate BIP86 policy (key-path only, no tapscript)
    // Same as Ledger - relying on wallet to only create BIP86-compliant PSBTs for now

    // 3. Get active BIP86 descriptor from DescriptorStore
    if (!m_descriptor_store) {
        result.error_message = "DescriptorStore not configured - call setDescriptorStore()";
        g_logger.error("[TrezorWallet] " + result.error_message);
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
        g_logger.error("[TrezorWallet] " + result.error_message);
        return result;
    }

    g_logger.info("[TrezorWallet] Using BIP86 descriptor (id=" + std::to_string(bip86_descriptor->id) + ")");

    // 4. Parse descriptor to extract xpub and fingerprint
    din::BIP86DescriptorFactory::ParsedBIP86 parsed =
        din::BIP86DescriptorFactory::parseDescriptor(bip86_descriptor->descriptor);

    if (!parsed.valid) {
        result.error_message = "Failed to parse BIP86 descriptor: " + parsed.error;
        g_logger.error("[TrezorWallet] " + result.error_message);
        return result;
    }

    g_logger.info("[TrezorWallet] Descriptor parsed - fingerprint: " + parsed.fingerprint +
                 ", xpub: " + parsed.xpub.substr(0, 20) + "...");

    // 5. Get master fingerprint from device without re-entering the public API lock path.
    uint32_t root_fingerprint = 0;
    std::string root_pubkey_hex;
    if (!requestPublicKeyLocked("m", root_pubkey_hex, &root_fingerprint)) {
        result.error_message = m_last_error.empty()
            ? "Failed to query Trezor root public key"
            : m_last_error;
        g_logger.error("[TrezorWallet] " + result.error_message);
        return result;
    }

    std::string device_fp;
    if (root_fingerprint != 0) {
        std::ostringstream oss;
        oss << std::hex << std::setw(8) << std::setfill('0') << root_fingerprint;
        device_fp = oss.str();
    } else {
        const std::string compressed_root = CompressPubkeyHex(root_pubkey_hex);
        device_fp = FingerprintFromCompressedPubkey(compressed_root.empty() ? root_pubkey_hex : compressed_root);
    }
    if (device_fp.empty()) {
        result.error_message = "Failed to derive master fingerprint from Trezor root public key";
        g_logger.error("[TrezorWallet] " + result.error_message);
        return result;
    }

    // 6. CRITICAL: Validate fingerprint match
    if (device_fp != parsed.fingerprint) {
        result.error_message = "Trezor device fingerprint (" + device_fp +
                              ") does not match wallet descriptor (" + parsed.fingerprint + ") - WRONG DEVICE";
        g_logger.error("[TrezorWallet] " + result.error_message);
        return result;
    }

    g_logger.info("[TrezorWallet] Fingerprint validated: device matches descriptor");

    // 7. Build a bounded BIP86 script → derivation index so we can infer key paths
    // without relying on PSBT derivation metadata, which the current PSBT stack does
    // not persist yet.
    try {
        const dinero::crypto::ExtendedPubKey account_xpub = dinero::crypto::ExtendedPubKey::FromString(parsed.xpub);
        std::map<std::string, std::vector<uint32_t>> known_scripts;
        std::string discovery_error;
        if (!BuildBip86ScriptIndex(account_xpub, parsed.derivation_path, known_scripts, discovery_error)) {
            result.error_message = "Failed to derive BIP86 address index for Trezor signing: " + discovery_error;
            g_logger.error("[TrezorWallet] " + result.error_message);
            return result;
        }

        std::vector<std::vector<uint32_t>> input_paths(psbt.inputs.size());
        for (size_t i = 0; i < psbt.inputs.size(); ++i) {
            const auto& input = psbt.inputs[i];
            if (!input.final_script_sig.empty() || !input.final_script_witness.empty()) {
                result.error_message = "Input " + std::to_string(i) + " is already finalized; Trezor USB signing expects an unsigned PSBT.";
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }
            if (!input.tap_key_sig.empty()) {
                result.error_message = "Input " + std::to_string(i) + " already contains a Taproot signature.";
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }
            if (input.witness_utxo_script.empty()) {
                result.error_message = "Input " + std::to_string(i) + " is missing witness_utxo_script.";
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }
            if (input.witness_utxo_amount == 0) {
                result.error_message = "Input " + std::to_string(i) + " is missing witness_utxo_amount.";
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }
            if (!(input.witness_utxo_script.size() == 34 &&
                  input.witness_utxo_script[0] == 0x51 &&
                  input.witness_utxo_script[1] == 0x20)) {
                result.error_message = "Input " + std::to_string(i) + " is not a BIP86 P2TR witness UTXO. This Trezor path only supports key-path Taproot spends.";
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }

            const auto match = known_scripts.find(HexString(input.witness_utxo_script));
            if (match == known_scripts.end()) {
                result.error_message =
                    "Input " + std::to_string(i) +
                    " does not match the active BIP86 descriptor within the host scan range. This Trezor path currently supports only the active account's BIP86 Taproot inputs.";
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }

            input_paths[i] = match->second;
        }

        std::vector<std::vector<uint32_t>> change_output_paths(psbt.tx.vout.size());
        for (size_t i = 0; i < psbt.tx.vout.size(); ++i) {
            const auto match = known_scripts.find(HexString(psbt.tx.vout[i].scriptPubKey));
            if (match != known_scripts.end() && match->second.size() >= 4 && match->second[3] == 1U) {
                change_output_paths[i] = match->second;
            }
        }

        // 8. Send SignTx protobuf message
        if (!isConnected()) {
            result.error_message = "Trezor device not connected";
            g_logger.error("[TrezorWallet] " + result.error_message);
            return result;
        }

        hw::trezor::messages::bitcoin::SignTx sign_request;
        sign_request.set_outputs_count(static_cast<uint32_t>(psbt.tx.vout.size()));
        sign_request.set_inputs_count(static_cast<uint32_t>(psbt.tx.vin.size()));
        sign_request.set_coin_name("Bitcoin");
        sign_request.set_version(static_cast<uint32_t>(psbt.tx.version));
        sign_request.set_lock_time(psbt.tx.lockTime);
        sign_request.set_serialize(false);

        if (!m_trezor_transport->sendMessage(hw::trezor::messages::MessageType_SignTx, sign_request)) {
            result.error_message = "Failed to send SignTx to Trezor: " + m_trezor_transport->getLastError();
            g_logger.error("[TrezorWallet] " + result.error_message);
            return result;
        }

        std::map<uint32_t, std::vector<uint8_t>> signatures;
        bool finished = false;
        while (!finished) {
            uint16_t response_type = 0;
            std::vector<uint8_t> payload;
            if (!m_trezor_transport->recvMessage(response_type, payload)) {
                result.error_message = "Failed to receive SignTx response from Trezor: " + m_trezor_transport->getLastError();
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }

            if (response_type == hw::trezor::messages::MessageType_Failure) {
                hw::trezor::messages::common::Failure failure;
                if (failure.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
                    result.error_message = "Trezor rejected SignTx: " + FormatTrezorFailure(failure);
                } else {
                    result.error_message = "Trezor rejected SignTx with an unreadable failure response";
                }
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }

            if (response_type == hw::trezor::messages::MessageType_ButtonRequest) {
                hw::trezor::messages::common::ButtonAck ack;
                if (!m_trezor_transport->sendMessage(hw::trezor::messages::MessageType_ButtonAck, ack)) {
                    result.error_message = "Failed to acknowledge Trezor SignTx button request: " + m_trezor_transport->getLastError();
                    g_logger.error("[TrezorWallet] " + result.error_message);
                    return result;
                }
                continue;
            }

            if (response_type == hw::trezor::messages::MessageType_PinMatrixRequest) {
                result.error_message = "Trezor requested PIN entry during SignTx, which is not supported in this backend path.";
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }
            if (response_type == hw::trezor::messages::MessageType_PassphraseRequest) {
                result.error_message = "Trezor requested a passphrase during SignTx, which is not supported in this backend path.";
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }
            if (response_type != hw::trezor::messages::MessageType_TxRequest) {
                result.error_message = "Unexpected Trezor response type during SignTx: " + std::to_string(response_type);
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }

            hw::trezor::messages::bitcoin::TxRequest request;
            if (!request.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
                result.error_message = "Failed to parse Trezor TxRequest response";
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }

            if (request.has_serialized() &&
                request.serialized().has_signature_index() &&
                request.serialized().has_signature() &&
                !request.serialized().signature().empty()) {
                signatures[request.serialized().signature_index()] = std::vector<uint8_t>(
                    request.serialized().signature().begin(),
                    request.serialized().signature().end());
            }

            const auto request_type = request.has_request_type()
                ? request.request_type()
                : hw::trezor::messages::bitcoin::TxRequest::TXFINISHED;
            if (request_type == hw::trezor::messages::bitcoin::TxRequest::TXFINISHED) {
                finished = true;
                continue;
            }

            if (!request.has_details() || !request.details().has_request_index()) {
                result.error_message = "Trezor TxRequest did not include a request_index";
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }

            const uint32_t request_index = request.details().request_index();
            const bool has_tx_hash = request.details().has_tx_hash() && !request.details().tx_hash().empty();

            switch (request_type) {
                case hw::trezor::messages::bitcoin::TxRequest::TXINPUT: {
                    if (has_tx_hash) {
                        result.error_message =
                            "Trezor requested previous-transaction input data. This backend path only supports BIP86 witness_utxo PSBTs that do not require prev-tx fetches.";
                        g_logger.error("[TrezorWallet] " + result.error_message);
                        return result;
                    }
                    if (request_index >= psbt.tx.vin.size() || request_index >= input_paths.size()) {
                        result.error_message = "Trezor requested an out-of-range input index: " + std::to_string(request_index);
                        g_logger.error("[TrezorWallet] " + result.error_message);
                        return result;
                    }

                    hw::trezor::messages::bitcoin::TxAckInput ack;
                    auto* tx = ack.mutable_tx();
                    auto* input = tx->mutable_input();
                    const auto& txin = psbt.tx.vin[request_index];
                    const auto& psbt_input = psbt.inputs[request_index];
                    for (uint32_t path_element : input_paths[request_index]) {
                        input->add_address_n(path_element);
                    }
                    input->set_prev_hash(txin.prevout.txid.AsUint256().data, 32);
                    input->set_prev_index(txin.prevout.vout);
                    input->set_sequence(txin.sequence);
                    input->set_script_type(hw::trezor::messages::bitcoin::SPENDTAPROOT);
                    input->set_amount(psbt_input.witness_utxo_amount);

                    if (!m_trezor_transport->sendMessage(hw::trezor::messages::MessageType_TxAck, ack)) {
                        result.error_message = "Failed to send TxAckInput to Trezor: " + m_trezor_transport->getLastError();
                        g_logger.error("[TrezorWallet] " + result.error_message);
                        return result;
                    }
                    break;
                }

                case hw::trezor::messages::bitcoin::TxRequest::TXOUTPUT: {
                    if (has_tx_hash) {
                        result.error_message =
                            "Trezor requested previous-transaction output data. This backend path only supports witness_utxo-based BIP86 spends.";
                        g_logger.error("[TrezorWallet] " + result.error_message);
                        return result;
                    }
                    if (request_index >= psbt.tx.vout.size()) {
                        result.error_message = "Trezor requested an out-of-range output index: " + std::to_string(request_index);
                        g_logger.error("[TrezorWallet] " + result.error_message);
                        return result;
                    }

                    hw::trezor::messages::bitcoin::TxAckOutput ack;
                    auto* tx = ack.mutable_tx();
                    auto* output = tx->mutable_output();
                    const auto& txout = psbt.tx.vout[request_index];
                    output->set_amount(txout.value.GetUna());

                    if (!change_output_paths[request_index].empty()) {
                        for (uint32_t path_element : change_output_paths[request_index]) {
                            output->add_address_n(path_element);
                        }
                        output->set_script_type(hw::trezor::messages::bitcoin::PAYTOTAPROOT);
                    } else {
                        std::vector<uint8_t> op_return_data;
                        if (TryExtractOpReturnData(txout.scriptPubKey, op_return_data)) {
                            output->set_script_type(hw::trezor::messages::bitcoin::PAYTOOPRETURN);
                            output->set_op_return_data(op_return_data.data(), static_cast<int>(op_return_data.size()));
                        } else {
                            std::string address;
                            if (!EncodeAddressFromScript(txout.scriptPubKey, address)) {
                                result.error_message =
                                    "Output " + std::to_string(request_index) +
                                    " has a script type that this Trezor USB path cannot describe to the device.";
                                g_logger.error("[TrezorWallet] " + result.error_message);
                                return result;
                            }
                            output->set_script_type(hw::trezor::messages::bitcoin::PAYTOADDRESS);
                            output->set_address(address);
                        }
                    }

                    if (!m_trezor_transport->sendMessage(hw::trezor::messages::MessageType_TxAck, ack)) {
                        result.error_message = "Failed to send TxAckOutput to Trezor: " + m_trezor_transport->getLastError();
                        g_logger.error("[TrezorWallet] " + result.error_message);
                        return result;
                    }
                    break;
                }

                case hw::trezor::messages::bitcoin::TxRequest::TXMETA: {
                    if (has_tx_hash) {
                        result.error_message =
                            "Trezor requested previous-transaction metadata. This backend path only supports witness_utxo-based BIP86 spends.";
                        g_logger.error("[TrezorWallet] " + result.error_message);
                        return result;
                    }

                    hw::trezor::messages::bitcoin::TxAckPrevMeta ack;
                    auto* meta = ack.mutable_tx();
                    meta->set_version(static_cast<uint32_t>(psbt.tx.version));
                    meta->set_lock_time(psbt.tx.lockTime);
                    meta->set_inputs_count(static_cast<uint32_t>(psbt.tx.vin.size()));
                    meta->set_outputs_count(static_cast<uint32_t>(psbt.tx.vout.size()));
                    if (!m_trezor_transport->sendMessage(hw::trezor::messages::MessageType_TxAck, ack)) {
                        result.error_message = "Failed to send TxAckPrevMeta to Trezor: " + m_trezor_transport->getLastError();
                        g_logger.error("[TrezorWallet] " + result.error_message);
                        return result;
                    }
                    break;
                }

                case hw::trezor::messages::bitcoin::TxRequest::TXEXTRADATA:
                case hw::trezor::messages::bitcoin::TxRequest::TXORIGINPUT:
                case hw::trezor::messages::bitcoin::TxRequest::TXORIGOUTPUT:
                case hw::trezor::messages::bitcoin::TxRequest::TXPAYMENTREQ:
                default:
                    result.error_message =
                        "Trezor requested a transaction-signing data type that this backend path does not provide yet.";
                    g_logger.error("[TrezorWallet] " + result.error_message);
                    return result;
            }
        }

        if (signatures.empty()) {
            result.error_message = "Trezor completed SignTx without returning any Taproot signatures";
            g_logger.error("[TrezorWallet] " + result.error_message);
            return result;
        }

        for (size_t i = 0; i < psbt.inputs.size(); ++i) {
            const auto it = signatures.find(static_cast<uint32_t>(i));
            if (it == signatures.end()) {
                result.error_message = "Trezor did not return a signature for input " + std::to_string(i);
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }
            if (it->second.size() != 64 && it->second.size() != 65) {
                result.error_message = "Trezor returned an invalid Schnorr signature size for input " + std::to_string(i);
                g_logger.error("[TrezorWallet] " + result.error_message);
                return result;
            }
            psbt.inputs[i].tap_key_sig = it->second;
        }

        result.success = true;
        result.psbt_base64 = psbt.ToBase64();
        result.complete = psbt.IsComplete();
        g_logger.info("[TrezorWallet] PSBT signing complete (complete=" +
                      std::string(result.complete ? "true" : "false") + ")");
        return result;
    } catch (const std::exception& e) {
        result.error_message = std::string("Failed to decode descriptor xpub: ") + e.what();
        g_logger.error("[TrezorWallet] " + result.error_message);
        return result;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Callbacks
// ═══════════════════════════════════════════════════════════════════════════

void TrezorWallet::setDeviceCallback(DeviceCallback callback) {
    m_device_callback = callback;
}

void TrezorWallet::setAddressCallback(AddressCallback callback) {
    m_address_callback = callback;
}

void TrezorWallet::setSigningCallback(SigningCallback callback) {
    m_signing_callback = callback;
}

void TrezorWallet::setStatusCallback(StatusCallback callback) {
    m_status_callback = callback;
}

// ═══════════════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════════

void TrezorWallet::setNetworkType(const std::string& network) {
    m_network_type = network;
}

void TrezorWallet::setCoinType(uint32_t coin_type) {
    m_coin_type = coin_type;
}

void TrezorWallet::setAccountIndex(uint32_t account_index) {
    m_account_index = account_index;
}

// ═══════════════════════════════════════════════════════════════════════════
// Utility Methods
// ═══════════════════════════════════════════════════════════════════════════

std::string TrezorWallet::getDerivationPath(uint32_t account_index, bool change, uint32_t address_index) {
    // BIP86 Taproot: m/86'/coin_type'/account'/change/address_index
    std::stringstream ss;
    ss << "m/86'/" << m_coin_type << "'/" << account_index << "'/"
       << (change ? 1 : 0) << "/" << address_index;
    return ss.str();
}

bool TrezorWallet::isValidDerivationPath(const std::string& path) {
    // Basic validation: must start with m/ and contain valid BIP32 path
    if (path.empty()) {
        return false;
    }
    if (IsRootDerivationPath(path)) {
        return true;
    }
    return path.substr(0, 2) == "m/";
}

bool TrezorWallet::initializeSessionLocked() {
    hw::trezor::messages::management::Initialize request;
    if (!m_trezor_transport->sendMessage(hw::trezor::messages::MessageType_Initialize, request)) {
        m_last_error = "Failed to send Initialize to Trezor: " + m_trezor_transport->getLastError();
        return false;
    }

    uint16_t response_type = 0;
    std::vector<uint8_t> payload;
    if (!m_trezor_transport->recvMessage(response_type, payload)) {
        m_last_error = "Failed to receive Features from Trezor: " + m_trezor_transport->getLastError();
        return false;
    }

    if (response_type == hw::trezor::messages::MessageType_Failure) {
        hw::trezor::messages::common::Failure failure;
        if (failure.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
            m_last_error = "Trezor initialization failed: " + FormatTrezorFailure(failure);
        } else {
            m_last_error = "Trezor initialization failed with an unreadable failure response";
        }
        return false;
    }

    if (response_type == hw::trezor::messages::MessageType_ButtonRequest) {
        m_last_error = "Trezor requested on-device confirmation during initialization, which is not supported in this backend path";
        return false;
    }
    if (response_type == hw::trezor::messages::MessageType_PinMatrixRequest) {
        m_last_error = "Trezor requested PIN entry during initialization, which is not supported in this backend path";
        return false;
    }
    if (response_type == hw::trezor::messages::MessageType_PassphraseRequest) {
        m_last_error = "Trezor requested a passphrase during initialization, which is not supported in this backend path";
        return false;
    }
    if (response_type != hw::trezor::messages::MessageType_Features) {
        m_last_error = "Unexpected Trezor response type during initialization: " + std::to_string(response_type);
        return false;
    }

    hw::trezor::messages::management::Features features;
    if (!features.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        m_last_error = "Failed to parse Trezor Features response";
        return false;
    }

    if (features.has_label() && !features.label().empty()) {
        m_device_info.name = features.label();
    } else if (features.has_model() && !features.model().empty()) {
        m_device_info.name = "Trezor " + features.model();
    } else if (features.has_vendor() && !features.vendor().empty()) {
        m_device_info.name = features.vendor();
    } else {
        m_device_info.name = "Trezor";
    }

    if (features.has_device_id() && !features.device_id().empty()) {
        m_device_info.serial_number = features.device_id();
    }
    if (features.has_major_version() && features.has_minor_version() && features.has_patch_version()) {
        m_device_info.firmware_version =
            std::to_string(features.major_version()) + "." +
            std::to_string(features.minor_version()) + "." +
            std::to_string(features.patch_version());
    }
    m_device_info.type = HardwareWalletType::TREZOR;
    m_device_info.supports_dinero = true;
    m_device_info.app_version = features.has_model() ? features.model() : "";

    return true;
}

bool TrezorWallet::requestAddressLocked(const std::string& derivation_path,
                                        bool show_display,
                                        std::string& address_out) {
    address_out.clear();

    if (!m_trezor_transport || !m_trezor_transport->isOpen()) {
        m_last_error = "Trezor transport is not open";
        return false;
    }

    std::vector<uint32_t> path;
    std::string parse_error;
    if (!ParseDerivationPath(derivation_path, path, parse_error)) {
        m_last_error = parse_error;
        return false;
    }
    std::string coin_type_error;
    if (!ValidateCanonicalCoinTypePath(path, "Trezor address retrieval", coin_type_error)) {
        m_last_error = coin_type_error;
        return false;
    }

    hw::trezor::messages::bitcoin::GetAddress request;
    for (uint32_t element : path) {
        request.add_address_n(element);
    }
    request.set_coin_name("Bitcoin");
    request.set_show_display(show_display);
    request.set_script_type(ScriptTypeForPath(path));

    if (!m_trezor_transport->sendMessage(hw::trezor::messages::MessageType_GetAddress, request)) {
        m_last_error = "Failed to send GetAddress to Trezor: " + m_trezor_transport->getLastError();
        return false;
    }

    while (true) {
        uint16_t response_type = 0;
        std::vector<uint8_t> payload;
        if (!m_trezor_transport->recvMessage(response_type, payload)) {
            m_last_error = "Failed to receive Address from Trezor: " + m_trezor_transport->getLastError();
            return false;
        }

        if (response_type == hw::trezor::messages::MessageType_Failure) {
            hw::trezor::messages::common::Failure failure;
            if (failure.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
                m_last_error = "Trezor rejected GetAddress: " + FormatTrezorFailure(failure);
            } else {
                m_last_error = "Trezor rejected GetAddress with an unreadable failure response";
            }
            return false;
        }
        if (response_type == hw::trezor::messages::MessageType_ButtonRequest) {
            hw::trezor::messages::common::ButtonAck ack;
            if (!m_trezor_transport->sendMessage(hw::trezor::messages::MessageType_ButtonAck, ack)) {
                m_last_error = "Failed to acknowledge Trezor button request: " + m_trezor_transport->getLastError();
                return false;
            }
            continue;
        }
        if (response_type == hw::trezor::messages::MessageType_PinMatrixRequest) {
            m_last_error = "Trezor requested PIN entry for GetAddress, which is not supported in this backend path";
            return false;
        }
        if (response_type == hw::trezor::messages::MessageType_PassphraseRequest) {
            m_last_error = "Trezor requested a passphrase for GetAddress, which is not supported in this backend path";
            return false;
        }
        if (response_type != hw::trezor::messages::MessageType_Address) {
            m_last_error = "Unexpected Trezor response type for GetAddress: " + std::to_string(response_type);
            return false;
        }

        hw::trezor::messages::bitcoin::Address response;
        if (!response.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
            m_last_error = "Failed to parse Trezor Address response";
            return false;
        }

        if (!response.has_address() || response.address().empty()) {
            m_last_error = "Trezor returned an empty address";
            return false;
        }

        address_out = response.address();
        return true;
    }
}

bool TrezorWallet::requestPublicKeyLocked(const std::string& derivation_path,
                                          std::string& public_key_hex,
                                          uint32_t* root_fingerprint,
                                          std::string* xpub_out) {
    public_key_hex.clear();
    if (root_fingerprint) {
        *root_fingerprint = 0;
    }
    if (xpub_out) {
        xpub_out->clear();
    }

    if (!m_trezor_transport || !m_trezor_transport->isOpen()) {
        m_last_error = "Trezor transport is not open";
        return false;
    }

    std::vector<uint32_t> path;
    std::string parse_error;
    if (!ParseDerivationPath(derivation_path, path, parse_error)) {
        m_last_error = parse_error;
        return false;
    }

    hw::trezor::messages::bitcoin::GetPublicKey request;
    for (uint32_t element : path) {
        request.add_address_n(element);
    }
    request.set_coin_name("Bitcoin");
    request.set_show_display(false);
    request.set_ignore_xpub_magic(true);
    request.set_script_type(ScriptTypeForPath(path));

    if (!m_trezor_transport->sendMessage(hw::trezor::messages::MessageType_GetPublicKey, request)) {
        m_last_error = "Failed to send GetPublicKey to Trezor: " + m_trezor_transport->getLastError();
        return false;
    }

    uint16_t response_type = 0;
    std::vector<uint8_t> payload;
    if (!m_trezor_transport->recvMessage(response_type, payload)) {
        m_last_error = "Failed to receive PublicKey from Trezor: " + m_trezor_transport->getLastError();
        return false;
    }

    if (response_type == hw::trezor::messages::MessageType_Failure) {
        hw::trezor::messages::common::Failure failure;
        if (failure.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
            m_last_error = "Trezor rejected GetPublicKey: " + FormatTrezorFailure(failure);
        } else {
            m_last_error = "Trezor rejected GetPublicKey with an unreadable failure response";
        }
        return false;
    }
    if (response_type == hw::trezor::messages::MessageType_ButtonRequest) {
        m_last_error = "Trezor requested on-device confirmation for GetPublicKey, which is not supported in this backend path";
        return false;
    }
    if (response_type == hw::trezor::messages::MessageType_PinMatrixRequest) {
        m_last_error = "Trezor requested PIN entry for GetPublicKey, which is not supported in this backend path";
        return false;
    }
    if (response_type == hw::trezor::messages::MessageType_PassphraseRequest) {
        m_last_error = "Trezor requested a passphrase for GetPublicKey, which is not supported in this backend path";
        return false;
    }
    if (response_type != hw::trezor::messages::MessageType_PublicKey) {
        m_last_error = "Unexpected Trezor response type for GetPublicKey: " + std::to_string(response_type);
        return false;
    }

    hw::trezor::messages::bitcoin::PublicKey response;
    if (!response.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        m_last_error = "Failed to parse Trezor PublicKey response";
        return false;
    }

    if (response.has_root_fingerprint() && root_fingerprint) {
        *root_fingerprint = response.root_fingerprint();
    }
    if (xpub_out && response.has_xpub()) {
        *xpub_out = response.xpub();
    }

    if (response.has_node() && response.node().has_public_key()) {
        public_key_hex = bytesToHex(std::vector<uint8_t>(
            response.node().public_key().begin(),
            response.node().public_key().end()));
    }

    if (public_key_hex.empty()) {
        m_last_error = "Trezor returned no public key bytes";
        return false;
    }

    return true;
}

bool TrezorWallet::exportAccountDescriptors(const std::string& derivation_path,
                                            const std::string& requested_policy,
                                            AccountDescriptorExport& export_out) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!isConnected()) {
        m_last_error = "Trezor device not connected";
        g_logger.error("[TrezorWallet] " + m_last_error);
        return false;
    }

    std::vector<uint32_t> path;
    std::string parse_error;
    if (!ParseDerivationPath(derivation_path, path, parse_error)) {
        m_last_error = parse_error;
        return false;
    }
    if (path.size() != 3) {
        m_last_error = "Account descriptor export requires an account-level path like m/86'/1448'/0'";
        return false;
    }
    for (uint32_t element : path) {
        if ((element & 0x80000000U) == 0U) {
            m_last_error = "Account descriptor export requires hardened purpose/coin/account elements";
            return false;
        }
    }

    const uint32_t purpose = path[0] & 0x7fffffffU;
    const uint32_t coin_type = path[1] & 0x7fffffffU;
    const uint32_t account = path[2] & 0x7fffffffU;
    std::string coin_type_error;
    if (!ValidateCanonicalCoinTypePath(path, "Trezor account descriptor export", coin_type_error)) {
        m_last_error = coin_type_error;
        return false;
    }
    const std::string inferred_policy = PolicyFromPurpose(purpose);
    if (inferred_policy.empty()) {
        m_last_error = "Unsupported account path purpose for descriptor export. Use m/84'/coin_type'/account' or m/86'/coin_type'/account'.";
        return false;
    }

    const std::string normalized_policy = requested_policy.empty()
        ? inferred_policy
        : NormalizeDescriptorPolicy(requested_policy);
    if (normalized_policy != "bip84" && normalized_policy != "bip86") {
        m_last_error = "Unsupported descriptor policy. Use bip84 or bip86.";
        return false;
    }
    if (normalized_policy != inferred_policy) {
        m_last_error = "Requested descriptor policy does not match the account derivation path purpose.";
        return false;
    }

    uint32_t root_fingerprint = 0;
    std::string account_pubkey_hex;
    std::string account_xpub;
    if (!requestPublicKeyLocked(derivation_path, account_pubkey_hex, &root_fingerprint, &account_xpub)) {
        if (m_last_error.empty()) {
            m_last_error = "Failed to query Trezor account public key";
        }
        return false;
    }
    if (account_xpub.empty()) {
        m_last_error = "Trezor did not return an account xpub for descriptor export";
        return false;
    }

    if (root_fingerprint == 0) {
        std::string root_pubkey_hex;
        if (!requestPublicKeyLocked("m", root_pubkey_hex, &root_fingerprint, nullptr)) {
            if (m_last_error.empty()) {
                m_last_error = "Failed to query Trezor root fingerprint";
            }
            return false;
        }
        if (root_fingerprint == 0) {
            const std::string compressed_root = CompressPubkeyHex(root_pubkey_hex);
            const std::string fingerprint_hex = FingerprintFromCompressedPubkey(compressed_root.empty() ? root_pubkey_hex : compressed_root);
            if (fingerprint_hex.empty()) {
                m_last_error = "Unable to derive master fingerprint from Trezor root public key";
                return false;
            }
            root_fingerprint = static_cast<uint32_t>(std::stoul(fingerprint_hex, nullptr, 16));
        }
    }

    std::ostringstream fingerprint_stream;
    fingerprint_stream << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << root_fingerprint;
    const std::string master_fingerprint = fingerprint_stream.str();

    export_out = AccountDescriptorExport{};
    export_out.derivation_path = derivation_path;
    export_out.policy = normalized_policy;
    export_out.master_fingerprint = master_fingerprint;
    export_out.account_xpub = account_xpub;
    export_out.coin_type = coin_type;
    export_out.account = account;

    if (normalized_policy == "bip84") {
        din::BIP84DescriptorFactory::BIP84Config config;
        config.master_fingerprint = master_fingerprint;
        config.account_xpub = account_xpub;
        config.coin_type = coin_type;
        config.account = account;
        export_out.receive_descriptor = din::BIP84DescriptorFactory::createReceiveDescriptor(config);
        export_out.change_descriptor = din::BIP84DescriptorFactory::createChangeDescriptor(config);
    } else {
        din::BIP86DescriptorFactory::BIP86Config config;
        config.master_fingerprint = master_fingerprint;
        config.account_xpub = account_xpub;
        config.coin_type = coin_type;
        config.account = account;
        export_out.receive_descriptor = din::BIP86DescriptorFactory::createReceiveDescriptor(config);
        export_out.change_descriptor = din::BIP86DescriptorFactory::createChangeDescriptor(config);
    }

    return true;
}

std::string TrezorWallet::getFingerprint() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!isConnected()) {
        m_last_error = "Device not connected";
        g_logger.error("[TrezorWallet] " + m_last_error);
        return "";
    }

    uint32_t root_fingerprint = 0;
    std::string public_key_hex;
    if (!requestPublicKeyLocked("m", public_key_hex, &root_fingerprint)) {
        if (m_last_error.empty()) {
            m_last_error = "Failed to query Trezor root public key";
        }
        g_logger.error("[TrezorWallet] " + m_last_error);
        return "";
    }

    if (root_fingerprint != 0) {
        std::ostringstream oss;
        oss << std::hex << std::setw(8) << std::setfill('0') << root_fingerprint;
        return oss.str();
    }

    const std::string compressed_pubkey = CompressPubkeyHex(public_key_hex);
    const std::string derived_fingerprint = FingerprintFromCompressedPubkey(compressed_pubkey);
    if (derived_fingerprint.empty()) {
        m_last_error = "Trezor returned a public key shape that cannot produce a fingerprint";
        g_logger.error("[TrezorWallet] " + m_last_error);
    }
    return derived_fingerprint;
}

// ═══════════════════════════════════════════════════════════════════════════
// Private Implementation
// ═══════════════════════════════════════════════════════════════════════════

bool TrezorWallet::openDevice() {
    if (!m_trezor_transport) {
        m_trezor_transport = std::make_unique<TrezorTransport>();
    }

    if (!m_device_path.empty()) {
        if (m_trezor_transport->openPath(m_device_path)) {
            g_logger.info("[TrezorWallet] Trezor device opened successfully at requested path");
            return true;
        }
    }

    // Try both known Trezor vendor IDs when no explicit path was provided.
    if (m_trezor_transport->open(TrezorIDs::TREZOR_VENDOR_1, 0) ||
        m_trezor_transport->open(TrezorIDs::TREZOR_VENDOR_2, 0)) {
        g_logger.info("[TrezorWallet] Trezor device opened successfully");
        return true;
    }

    m_last_error = m_trezor_transport ? m_trezor_transport->getLastError() : "Failed to open Trezor device";
    g_logger.error("[TrezorWallet] Failed to open Trezor device");
    return false;
}

bool TrezorWallet::closeDevice() {
    if (m_trezor_transport && m_trezor_transport->isOpen()) {
        m_trezor_transport->close();
        return true;
    }
    return false;
}

std::vector<std::string> TrezorWallet::splitString(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;

    while (std::getline(ss, token, delimiter)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }

    return tokens;
}

std::string TrezorWallet::bytesToHex(const std::vector<uint8_t>& bytes) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

std::vector<uint8_t> TrezorWallet::hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::strtol(byteString.c_str(), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

} // namespace dinero
