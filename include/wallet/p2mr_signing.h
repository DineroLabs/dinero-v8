#pragma once
/**
 * P2MR signing entry point, owned by the wallet library.
 *
 * Moved verbatim from include/rpc/v7_pq_handlers.h (wallet/node separation
 * Task 2.5). Namespace and signature are unchanged; v7_pq_handlers.h keeps a
 * forwarding include so existing includers compile unchanged.
 */

#include "consensus/pq/ml_dsa_65.h"
#include "wallet/aead_seed.h"
#include "wallet/v7_p2mr_store.h"

#include <array>
#include <cstdint>
#include <string>

namespace dinero::rpc::v7 {

// ---------------------------------------------------------------------------
// Common status codes
// ---------------------------------------------------------------------------

enum class HandlerStatus : uint8_t {
    Ok                  = 0,
    InvalidParams       = 1,   ///< empty / wrong-length inputs
    StoreError          = 2,   ///< sqlite / migration failure
    UniqueConflict      = 3,   ///< (wallet_id, path, leaf_index) already exists
    AddressNotFound     = 4,   ///< no such P2MR address in this wallet
    DecryptFailed       = 5,   ///< master key didn't open the seed
    DerivationMismatch  = 6,   ///< seed/public key/root/address binding mismatch
    InternalError       = 7,
};

// ---------------------------------------------------------------------------
// wallet.signp2mr
// ---------------------------------------------------------------------------

struct SignP2MRParams {
    int64_t                             wallet_id = 0;
    std::string                         address;                 ///< "din1r..."
    std::array<uint8_t, 32>             sighash{};               ///< BIP341-style 32-byte sighash
    dinero::wallet::AeadKey             master_key{};            ///< to decrypt seed
};

struct SignP2MRResult {
    HandlerStatus                                                                 status          = HandlerStatus::InternalError;
    uint8_t                                                                       scheme_id       = 0;
    std::array<uint8_t, dinero::consensus::pq::ml_dsa_65::PUBKEY_BYTES>            pubkey{};
    std::array<uint8_t, dinero::consensus::pq::ml_dsa_65::SIGNATURE_BYTES>         signature{};
    std::string                                                                   error_message;
};

/**
 * Produce an ML-DSA-65 signature over the given sighash, using the
 * secret rooted at the stored address. Re-derives the keypair from the
 * encrypted seed; nothing is written to disk and the SecureKeypair is
 * destroyed before this function returns.
 */
SignP2MRResult SignP2MR(
    const dinero::wallet::V7P2MRStore& store,
    SignP2MRParams                     params);

} // namespace dinero::rpc::v7
