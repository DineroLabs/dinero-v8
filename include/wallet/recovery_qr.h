#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dinero {

/**
 * Recovery QR — encrypted envelope for wallet backup via QR code.
 *
 * Format: DINERO-RECOVERY-1
 * Payload: salt(16) || nonce(12) || AES-256-GCM(key, mnemonic || profile_data)
 * Key derivation: Argon2id(passphrase, salt, t=3, m=64MB, p=1)
 *
 * If passphrase is empty, a WARNING should be displayed to the user.
 * The QR data is base64-encoded for transport.
 */
class RecoveryQR {
public:
    /**
     * Encode wallet backup as encrypted QR payload.
     *
     * @param mnemonic BIP39 mnemonic phrase
     * @param profile_data Serialized SafetyProfile (may be empty for STANDARD)
     * @param passphrase Encryption passphrase (empty = no passphrase, WARNING)
     * @return Base64-encoded encrypted payload with "DINERO-RECOVERY-1\n" prefix
     */
    static std::string Encode(
        const std::string& mnemonic,
        const std::vector<uint8_t>& profile_data,
        const std::string& passphrase
    );

    /**
     * Decode and decrypt recovery QR payload.
     *
     * @param qr_data Base64-encoded payload (with header prefix)
     * @param passphrase Decryption passphrase
     * @param mnemonic_out Recovered mnemonic
     * @param profile_data_out Recovered profile data (may be empty)
     * @return true on success, false on authentication failure or malformed data
     */
    static bool Decode(
        const std::string& qr_data,
        const std::string& passphrase,
        std::string& mnemonic_out,
        std::vector<uint8_t>& profile_data_out
    );

    static constexpr const char* HEADER = "DINERO-RECOVERY-1";
};

} // namespace dinero
