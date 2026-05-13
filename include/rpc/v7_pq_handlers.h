#pragma once
/**
 * V7 PQ wallet RPC handlers — pure C++ entry points.
 *
 * Spec: docs/consensus/V7_WALLET_SCHEMA.md §4 "RPC Surface".
 *
 * These are library-level functions with typed inputs and typed outputs.
 * The JSON-RPC dispatcher adaptation (wire these into Dinero's rpc_server)
 * is intentionally a separate phase — keeping this file free of JSON
 * serialization means:
 *
 *   1. The handlers are unit-testable end-to-end without a running daemon.
 *   2. Phase 7 (Qt) can consume them directly via FFI without going
 *      through a JSON round-trip.
 *   3. A future alternative RPC protocol (gRPC, MessagePack, etc.) plugs
 *      in with a thin adapter instead of a rewrite.
 *
 * Ownership / security discipline
 * -------------------------------
 *
 * - Callers provide a 32-byte master key for every call that touches
 *   secret material. The key is treated as consumed-by-value in local
 *   scope: handlers pass it to SealSeed/OpenSeed and wipe the local
 *   copy before returning.
 * - BIP-32 material (priv + chain_code) and a raw pq_seed (for import)
 *   are passed in the same way. They are scrubbed before the handler
 *   returns.
 * - A V7P2MRStore instance is borrowed by reference and is NOT taken
 *   ownership of.
 * - No handler persists the master key. Callers are responsible for
 *   only holding the key while the wallet is unlocked.
 *
 * Handlers return a typed `Result` / output struct — callers can check
 * the status enum to know what happened and format any user-facing
 * message themselves.
 */

#include "consensus/pq/ml_dsa_65.h"
#include "wallet/aead_seed.h"
#include "wallet/p2mr_address.h"
#include "wallet/pq_derivation.h"
#include "wallet/v7_p2mr_store.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
    DerivationMismatch  = 6,   ///< re-derived pubkey != stored pubkey
    InternalError       = 7,
};

// ---------------------------------------------------------------------------
// wallet.getnewp2mraddress
// ---------------------------------------------------------------------------

struct GetNewP2MRAddressParams {
    int64_t                      wallet_id       = 0;
    std::string                  hrp             = "din";  ///< "din" / "tdin" / "rdin"
    int                          account         = 0;
    int                          change          = 0;
    int                          address_index   = 0;
    uint32_t                     leaf_index      = 0;       ///< single-leaf = 0 at v7 genesis
    std::string                  label;                     ///< optional free-form label
    int64_t                      now_unix        = 0;       ///< caller-provided for testability

    dinero::wallet::pq::Bip32PrivKey   bip32_priv{};
    dinero::wallet::pq::Bip32ChainCode bip32_chain{};
    dinero::wallet::AeadKey            master_key{};
};

struct GetNewP2MRAddressResult {
    HandlerStatus                     status          = HandlerStatus::InternalError;
    std::string                       address;                  ///< "din1r..."
    std::array<uint8_t, 32>           merkle_root{};
    std::array<uint8_t, dinero::consensus::pq::ml_dsa_65::PUBKEY_BYTES> pubkey{};
    std::string                       derivation_path;
    uint32_t                          leaf_index      = 0;
    std::string                       error_message;
};

/**
 * Derive a fresh P2MR address from BIP-32 material, encrypt the pq_seed
 * with the master key, and persist the row. Typical call site:
 * the wallet UI asks for "a new address", which triggers BIP-32 walk
 * in the wallet manager → this handler.
 */
GetNewP2MRAddressResult GetNewP2MRAddress(
    dinero::wallet::V7P2MRStore&   store,
    GetNewP2MRAddressParams        params);

// ---------------------------------------------------------------------------
// wallet.listp2mraddresses
// ---------------------------------------------------------------------------

struct ListP2MRAddressesParams {
    int64_t wallet_id = 0;
};

struct ListP2MRAddressesResult {
    HandlerStatus                                          status = HandlerStatus::InternalError;
    std::vector<dinero::wallet::P2MRStoredAddress>         entries;
    std::string                                            error_message;
};

/** Read-only. Returns public rows; never returns secret material. */
ListP2MRAddressesResult ListP2MRAddresses(
    const dinero::wallet::V7P2MRStore& store,
    ListP2MRAddressesParams            params);

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

// ---------------------------------------------------------------------------
// wallet.importp2mrseed
// ---------------------------------------------------------------------------

struct ImportP2MRSeedParams {
    int64_t                                  wallet_id  = 0;
    std::string                              hrp        = "din";
    dinero::consensus::pq::ml_dsa_65::Seed   pq_seed{};     ///< 32-byte raw seed
    std::string                              derivation_path;  ///< for record-keeping; no re-derivation
    uint32_t                                 leaf_index = 0;
    std::string                              label;
    int64_t                                  now_unix   = 0;
    dinero::wallet::AeadKey                  master_key{};
};

struct ImportP2MRSeedResult {
    HandlerStatus                     status = HandlerStatus::InternalError;
    std::string                       address;           ///< "din1r..."
    std::array<uint8_t, 32>           merkle_root{};
    std::array<uint8_t, dinero::consensus::pq::ml_dsa_65::PUBKEY_BYTES> pubkey{};
    std::string                       error_message;
};

/**
 * Import a raw 32-byte pq_seed (e.g. from a cold-wallet dump), run
 * KeygenFromSeed to get the pubkey, encrypt the seed, persist. Does
 * NOT walk BIP-32 — the caller is asserting they already derived the
 * seed by whatever path they wish.
 */
ImportP2MRSeedResult ImportP2MRSeed(
    dinero::wallet::V7P2MRStore&   store,
    ImportP2MRSeedParams           params);

// ---------------------------------------------------------------------------
// wallet.exportp2mrseed
// ---------------------------------------------------------------------------

struct ExportP2MRSeedParams {
    int64_t                 wallet_id = 0;
    std::string             address;                 ///< "din1r..."
    dinero::wallet::AeadKey master_key{};
};

struct ExportP2MRSeedResult {
    HandlerStatus                            status = HandlerStatus::InternalError;
    dinero::consensus::pq::ml_dsa_65::Seed   pq_seed{};     ///< 32-byte seed (plaintext)
    std::string                              error_message;
};

/**
 * Decrypt and return the 32-byte pq_seed for cold-wallet backup.
 *
 * DANGEROUS: the caller receives the raw seed. They MUST NOT log it,
 * MUST scrub it after use, and MUST NOT expose it over network RPC
 * without an explicit local-only gate. This handler does not enforce
 * those things — it trusts the caller (wallet manager / Qt UI).
 *
 * The `pq_seed` in the result is a raw std::array; destructing the
 * result struct does NOT zeroize it. Callers should copy into a
 * SecureSeed immediately.
 */
ExportP2MRSeedResult ExportP2MRSeed(
    const dinero::wallet::V7P2MRStore& store,
    ExportP2MRSeedParams               params);

} // namespace dinero::rpc::v7
