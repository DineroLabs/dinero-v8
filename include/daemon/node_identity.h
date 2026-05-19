#pragma once

#include <string>
#include <array>
#include <vector>
#include <optional>

namespace dinero {
namespace daemon {

/**
 * NodeIdentity - Persistent cryptographic identity for the daemon
 *
 * Each node has a unique secp256k1 keypair stored in node_identity.dat
 * Used for signing serverinfo.json and proving node authenticity
 */
class NodeIdentity {
public:
    /**
     * Load or generate node identity
     *
     * @param data_dir Directory to store node_identity.dat
     * @return true if successful
     */
    bool initialize(const std::string& data_dir);

    /**
     * Sign a message with the node's private key
     *
     * @param message Message to sign (will be SHA256 hashed)
     * @return DER-encoded signature in hex, or empty if failed
     */
    std::string sign_message(const std::string& message) const;

    /**
     * Get the node's public key (compressed, 33 bytes)
     *
     * @return Hex-encoded public key
     */
    std::string get_pubkey_hex() const;

    /**
     * Get node ID (hash of public key for privacy)
     *
     * @return Hex-encoded HASH160 of public key
     */
    std::string get_node_id() const;

    /**
     * Verify a signature
     *
     * @param message Original message
     * @param signature_hex DER-encoded signature in hex
     * @param pubkey_hex Public key in hex
     * @return true if signature is valid
     */
    static bool verify_signature(
        const std::string& message,
        const std::string& signature_hex,
        const std::string& pubkey_hex
    );

    // ─── Binary-friendly API (added Phase 1A for `dineroid` P2P message) ─────
    // Avoids hex round-trips when signing / verifying on the wire.
    // Returns empty vector on failure.
    std::vector<uint8_t> sign_bytes(const uint8_t* msg, size_t msg_len) const;
    static bool verify_bytes(const uint8_t* msg, size_t msg_len,
                             const uint8_t* sig, size_t sig_len,
                             const uint8_t* pubkey_33);
    const std::array<uint8_t, 33>& get_pubkey_bytes() const { return public_key_; }
    std::array<uint8_t, 20> get_node_id_bytes() const;

private:
    bool load_identity(const std::string& filepath);
    bool generate_and_save_identity(const std::string& filepath);
    std::string bytes_to_hex(const uint8_t* bytes, size_t len) const;
    std::vector<uint8_t> hex_to_bytes(const std::string& hex) const;

    std::array<uint8_t, 32> private_key_;
    std::array<uint8_t, 33> public_key_;
    bool initialized_ = false;
};

} // namespace daemon
} // namespace dinero
