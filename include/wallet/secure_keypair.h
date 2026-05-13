#pragma once
/**
 * SecureKeypair — RAII wrapper around ml_dsa_65::Keypair that guarantees
 * the 4032-byte secret key is zeroized (via OPENSSL_cleanse) on every
 * path out of scope: normal destruction, exception, or move.
 *
 * Spec: docs/consensus/V7_WALLET_SCHEMA.md §2 "Zeroization".
 *
 * Usage pattern:
 *
 *     auto kp = wallet::pq::DerivePQKeypair(priv, chain, leaf);
 *     // kp is a SecureKeypair — secret is live in RAM here.
 *     auto sig = ml_dsa_65::Sign(msg, kp.secret());
 *     // kp goes out of scope; destructor wipes kp.secret().
 *
 * Design choices:
 *
 *  - **No copy.** Copying a secret key is an obvious footgun; construction
 *    is always via move from an ml_dsa_65::Keypair or from another
 *    SecureKeypair.
 *  - **Move zeroes the source.** After `SecureKeypair moved(std::move(src))`,
 *    `src.secret()` reads back as all-zero. Prevents the "I moved, but
 *    the old buffer still holds the key" bug.
 *  - **Pubkey is never scrubbed.** It's public data; wiping it would be
 *    busywork.
 *  - **OPENSSL_cleanse is the zeroize primitive.** Matches v5 convention
 *    (src/wallet/bip32_deriver.cpp, src/crypto/hd_keychain.cpp). The
 *    compiler cannot optimize OPENSSL_cleanse away the way a plain
 *    memset(0) can be.
 *
 * Wallet-layer only. Consensus code never handles secrets — verification
 * is the only consensus op and it takes the pubkey.
 */

#include "consensus/pq/ml_dsa_65.h"

#include <utility>

namespace dinero::wallet {

class SecureKeypair {
public:
    /** Adopt an existing keypair. std::array cannot truly move secret bytes,
     *  so construction copies into this owner and immediately scrubs the
     *  rvalue source buffer. */
    explicit SecureKeypair(dinero::consensus::pq::ml_dsa_65::Keypair&& kp) noexcept;

    /** No copy. Owning two copies of a secret is a foot-gun by design. */
    SecureKeypair(const SecureKeypair&) = delete;
    SecureKeypair& operator=(const SecureKeypair&) = delete;

    /** Move: the source is zeroed so the old scope cannot accidentally
     *  keep using the secret. */
    SecureKeypair(SecureKeypair&& other) noexcept;
    SecureKeypair& operator=(SecureKeypair&& other) noexcept;

    ~SecureKeypair();

    /** Read-only access to the secret and pubkey. Callers pass these by
     *  reference into ml_dsa_65::Sign() / Verify(); they should not keep
     *  long-lived copies. */
    const dinero::consensus::pq::ml_dsa_65::SecretKey& secret() const noexcept {
        return kp_.secret;
    }
    const dinero::consensus::pq::ml_dsa_65::PublicKey& pubkey() const noexcept {
        return kp_.pubkey;
    }

    /** Explicit early wipe. After calling, secret() reads as all-zero.
     *  Pubkey is preserved (see class-level comment). */
    void Wipe() noexcept;

private:
    dinero::consensus::pq::ml_dsa_65::Keypair kp_;
};

} // namespace dinero::wallet
