/*
 * SecureKeypair — RAII zeroization for ML-DSA-65 secret keys.
 * See include/wallet/secure_keypair.h.
 */

#include "wallet/secure_keypair.h"

#include <openssl/crypto.h>  // OPENSSL_cleanse

namespace dinero::wallet {

namespace {

// Scrub only the secret array. The pubkey is public; there's no hygiene
// reason to wipe it, and tests rely on being able to read it back from
// moved-from objects.
void ScrubSecret(dinero::consensus::pq::ml_dsa_65::SecretKey& s) noexcept {
    OPENSSL_cleanse(s.data(), s.size());
}

} // namespace

SecureKeypair::SecureKeypair(dinero::consensus::pq::ml_dsa_65::Keypair&& kp) noexcept
    : kp_(kp) {
    // std::array "move" is a byte copy. Scrub the adopted raw keypair so the
    // SecureKeypair is the only remaining owner of the ML-DSA secret.
    ScrubSecret(kp.secret);
}

SecureKeypair::SecureKeypair(SecureKeypair&& other) noexcept
    : kp_(other.kp_) {
    // kp_ copy-construction above duplicated the secret bytes. Now scrub
    // the source so only one live copy exists.
    ScrubSecret(other.kp_.secret);
}

SecureKeypair& SecureKeypair::operator=(SecureKeypair&& other) noexcept {
    if (this != &other) {
        // Scrub our current secret before we overwrite.
        ScrubSecret(kp_.secret);
        kp_ = other.kp_;
        ScrubSecret(other.kp_.secret);
    }
    return *this;
}

SecureKeypair::~SecureKeypair() {
    ScrubSecret(kp_.secret);
}

void SecureKeypair::Wipe() noexcept {
    ScrubSecret(kp_.secret);
}

} // namespace dinero::wallet
