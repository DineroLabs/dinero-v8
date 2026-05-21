#include "daemon/node_identity.h"
#include "dinero/core/crypto/dinero_crypto_minimal.h"
#include "daemon/crypto_utils.h"
#include "common/logger.h"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <sys/stat.h>

namespace dinero {
namespace daemon {

bool NodeIdentity::initialize(const std::string& data_dir) {
    std::string filepath = data_dir + "/node_identity.dat";

    // Try to load existing identity
    if (load_identity(filepath)) {
        dinero::g_logger.info("[NodeIdentity] Loaded existing node identity");
        dinero::g_logger.info("[NodeIdentity] Node ID: " + get_node_id());
        initialized_ = true;
        return true;
    }

    // Generate new identity
    dinero::g_logger.info("[NodeIdentity] No existing identity found, generating new keypair...");
    if (generate_and_save_identity(filepath)) {
        dinero::g_logger.info("[NodeIdentity] Generated new node identity");
        dinero::g_logger.info("[NodeIdentity] Node ID: " + get_node_id());
        dinero::g_logger.info("[NodeIdentity] Identity saved to: " + filepath);
        initialized_ = true;
        return true;
    }

    dinero::g_logger.error("[NodeIdentity] Failed to initialize node identity");
    return false;
}

std::string NodeIdentity::sign_message(const std::string& message) const {
    if (!initialized_) {
        return "";
    }

    // Hash the message with SHA256
    uint8_t msg_hash[32];
    ::sha256(reinterpret_cast<const uint8_t*>(message.data()), message.size(), msg_hash);

    // Sign the hash
    unsigned char signature[72];  // Max DER signature size
    size_t sig_len = 72;

    if (!::CF_SignDER(private_key_.data(), msg_hash, signature, sig_len, 72)) {
        dinero::g_logger.error("[NodeIdentity] Failed to sign message");
        return "";
    }

    return bytes_to_hex(signature, sig_len);
}

std::string NodeIdentity::get_pubkey_hex() const {
    if (!initialized_) {
        return "";
    }
    return bytes_to_hex(public_key_.data(), 33);
}

std::string NodeIdentity::get_node_id() const {
    if (!initialized_) {
        return "";
    }

    // Node ID = HASH160(pubkey) for privacy
    uint8_t hash[20];
    ::HASH160(public_key_.data(), 33, hash);
    return bytes_to_hex(hash, 20);
}

bool NodeIdentity::verify_signature(
    const std::string& message,
    const std::string& signature_hex,
    const std::string& pubkey_hex
) {
    // Convert hex strings to bytes
    std::vector<uint8_t> signature_bytes;
    std::vector<uint8_t> pubkey_bytes;

    // Parse hex
    for (size_t i = 0; i < signature_hex.length(); i += 2) {
        std::string byte_str = signature_hex.substr(i, 2);
        signature_bytes.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
    }

    for (size_t i = 0; i < pubkey_hex.length(); i += 2) {
        std::string byte_str = pubkey_hex.substr(i, 2);
        pubkey_bytes.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
    }

    if (pubkey_bytes.size() != 33) {
        return false;
    }

    // Hash the message
    uint8_t msg_hash[32];
    ::sha256(reinterpret_cast<const uint8_t*>(message.data()), message.size(), msg_hash);

    // Verify signature
    return ::CF_VerifyDER(pubkey_bytes.data(), msg_hash, signature_bytes.data(), signature_bytes.size());
}

std::vector<uint8_t> NodeIdentity::sign_bytes(const uint8_t* msg, size_t msg_len) const {
    if (!initialized_ || !msg) {
        return {};
    }
    uint8_t msg_hash[32];
    ::sha256(msg, msg_len, msg_hash);

    unsigned char signature[72];
    size_t sig_len = 72;
    if (!::CF_SignDER(private_key_.data(), msg_hash, signature, sig_len, 72)) {
        return {};
    }
    return std::vector<uint8_t>(signature, signature + sig_len);
}

bool NodeIdentity::verify_bytes(const uint8_t* msg, size_t msg_len,
                                const uint8_t* sig, size_t sig_len,
                                const uint8_t* pubkey_33) {
    if (!msg || !sig || !pubkey_33 || sig_len == 0 || sig_len > 72) {
        return false;
    }
    uint8_t msg_hash[32];
    ::sha256(msg, msg_len, msg_hash);
    return ::CF_VerifyDER(pubkey_33, msg_hash, sig, sig_len);
}

std::array<uint8_t, 20> NodeIdentity::get_node_id_bytes() const {
    std::array<uint8_t, 20> id{};
    if (initialized_) {
        ::HASH160(public_key_.data(), 33, id.data());
    }
    return id;
}

bool NodeIdentity::load_identity(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    // Read private key (32 bytes)
    file.read(reinterpret_cast<char*>(private_key_.data()), 32);
    if (!file.good()) {
        return false;
    }

    // Derive public key from private key
    if (!::CF_GetCompressedPubkey(private_key_.data(), public_key_.data())) {
        dinero::g_logger.error("[NodeIdentity] Failed to derive public key from loaded private key");
        return false;
    }

    return true;
}

bool NodeIdentity::generate_and_save_identity(const std::string& filepath) {
    // Generate new private key
    if (!::CF_GeneratePrivKey(private_key_.data())) {
        dinero::g_logger.error("[NodeIdentity] Failed to generate private key");
        return false;
    }

    // Derive public key
    if (!::CF_GetCompressedPubkey(private_key_.data(), public_key_.data())) {
        dinero::g_logger.error("[NodeIdentity] Failed to derive public key");
        return false;
    }

    // Save to file
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        dinero::g_logger.error("[NodeIdentity] Failed to open file for writing: " + filepath);
        return false;
    }

    file.write(reinterpret_cast<const char*>(private_key_.data()), 32);
    if (!file.good()) {
        dinero::g_logger.error("[NodeIdentity] Failed to write identity file");
        return false;
    }

    file.close();

    // Set restrictive permissions (0600 = owner read/write only)
#ifndef _WIN32
    chmod(filepath.c_str(), 0600);
#endif

    return true;
}

std::string NodeIdentity::bytes_to_hex(const uint8_t* bytes, size_t len) const {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return oss.str();
}

std::vector<uint8_t> NodeIdentity::hex_to_bytes(const std::string& hex) const {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        bytes.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
    }
    return bytes;
}

} // namespace daemon
} // namespace dinero
