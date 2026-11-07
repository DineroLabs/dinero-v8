/*
 * P2MR signing — implementation (moved from src/rpc/v7_pq_handlers.cpp,
 * wallet/node separation Task 2.5). SignP2MR's body is moved verbatim.
 * The file-local helpers it uses are shared with v7_pq_handlers.cpp via
 * p2mr_signing_helpers.inc (single text source; stage-2 review follow-up).
 */

#include "wallet/p2mr_signing.h"

#include <exception>

#include "consensus/pq/ml_dsa_65.h"
#include "consensus/pq/scheme_registry.h"
#include "crypto/sha256.h"
#include "wallet/aead_seed.h"
#include "wallet/p2mr_address.h"
#include "wallet/pq_derivation.h"
#include "wallet/secure_keypair.h"
#include "wallet/v7_p2mr_store.h"

#include <cstring>
#include <openssl/crypto.h>
#include <optional>
#include <string>

namespace dinero::rpc::v7 {

namespace {
#include "wallet/p2mr_signing_helpers.inc"
} // namespace

// ---------------------------------------------------------------------------
// wallet.signp2mr
// ---------------------------------------------------------------------------
SignP2MRResult SignP2MR(const wlt::V7P2MRStore& store, SignP2MRParams params) {
    SignP2MRResult out;
    ScopedMasterKey master_guard{params.master_key};
    wlt::SecureSeed seed;
    std::optional<wlt::SecureKeypair> key;
    try {
        out.status = OpenBoundKey(store, params.wallet_id, params.address,
                                  params.master_key, seed, key);
        if (out.status != HandlerStatus::Ok) {
            out.error_message = "P2MR signing key or stored address binding unavailable";
            return out;
        }
        auto signature = mldsa::Sign(params.sighash.data(), params.sighash.size(), key->secret());
        out.scheme_id = pq::SCHEME_ID_ML_DSA_65;
        out.pubkey = key->pubkey();
        out.signature = signature;
    } catch (const std::exception&) {
        out.status = HandlerStatus::InternalError;
        out.error_message = "P2MR signing failed";
    }
    return out;
}

} // namespace dinero::rpc::v7
