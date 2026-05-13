/*
 * V7 PQ wallet JSON-RPC adapters (Phase 4c.3 Commit 2/3).
 *
 * Registers three safe, read-write RPCs that wire the v7 PQ wallet
 * handler library (src/rpc/v7_pq_handlers.cpp) into Dinero's
 * JSON-RPC server via the existing ExecutionContext pattern:
 *
 *   wallet.getnewp2mraddress
 *   wallet.listp2mraddresses
 *   wallet.signp2mr
 *
 * Deliberately NOT registered here:
 *
 *   wallet.exportp2mrseed
 *   wallet.importp2mrseed
 *
 * Those return raw 32-byte seed material. They stay dark until after a
 * dedicated review pass — see V7_WALLET_SCHEMA.md §4 and the plan
 * agreed for Phase 4c.3.1.
 *
 * Secret-handling discipline
 * --------------------------
 *
 *   - Master key is sourced from WalletManager::GetV7PqMasterKey()
 *     every call. Never travels over the JSON-RPC wire.
 *   - BIP-32 material from WalletManager::DeriveV7Bip32Material().
 *     Never travels over the wire either.
 *   - Handler results are mapped from typed structs to JSON with
 *     only the public-side fields (address, pubkey, merkle_root,
 *     signature). Seed bytes stay in-process.
 *   - All error paths return a JSON object with `error` set —
 *     clients treat any non-empty error as failure.
 */

#include "rpc/v7_pq_handlers.h"
#include "wallet/v7_p2mr_store.h"
#include "wallet/wallet_manager.h"
#include "wallet/retired_coin_type_guard.h"
#include "util/hex.h"

#include "daemon/daemon_context.h"
#include "daemon/services/wallet_service.h"
#include "dinero/daemon/execution_context.h"
#include "rpc/rpc_registry.h"

// Phase 6 Commit 2: debug.computesighash uses the consensus BIP-341
// sighash implementation (same one ValidateTaprootSpend/ValidateP2MRSpend
// invoke) so test harnesses can produce the exact 32-byte sighash the
// validator will check.
#include "primitives/transaction.h"
#include "consensus/script_interpreter.h"

#include <chrono>
#include <cstring>
#include <optional>
#include <string>

#include <openssl/crypto.h>

namespace {

namespace wlt  = dinero::wallet;
namespace v7rpc = dinero::rpc::v7;
namespace mldsa = dinero::consensus::pq::ml_dsa_65;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Map a HandlerStatus to a short error string consumed by JSON-RPC clients. */
const char* StatusString(v7rpc::HandlerStatus s) {
    switch (s) {
        case v7rpc::HandlerStatus::Ok:                 return "ok";
        case v7rpc::HandlerStatus::InvalidParams:      return "invalid_params";
        case v7rpc::HandlerStatus::StoreError:         return "store_error";
        case v7rpc::HandlerStatus::UniqueConflict:     return "unique_conflict";
        case v7rpc::HandlerStatus::AddressNotFound:    return "address_not_found";
        case v7rpc::HandlerStatus::DecryptFailed:      return "decrypt_failed";
        case v7rpc::HandlerStatus::DerivationMismatch: return "derivation_mismatch";
        case v7rpc::HandlerStatus::InternalError:      return "internal_error";
    }
    return "unknown";
}

/** Open a V7P2MRStore at the wallet's v7 store path. Returns nullptr on I/O fail. */
std::unique_ptr<wlt::V7P2MRStore> OpenWalletStore(dinero::WalletManager& wm) {
    const std::string path = wm.GetV7P2MRStorePath();
    if (path.empty()) return nullptr;
    auto store = std::make_unique<wlt::V7P2MRStore>();
    if (store->Open(path) != wlt::V7P2MRStore::OpenResult::Ok) {
        return nullptr;
    }
    return store;
}

/**
 * Common preamble every v7 RPC runs: validate wallet service is ready,
 * wallet is open, wallet is unlocked. Returns a ptr to WalletManager on
 * success, populates `err_out` on failure.
 */
dinero::WalletManager* AcquireUnlockedWallet(const ExecutionContext& ctx,
                                          din::Json&              err_out) {
    if (!ctx.daemon || !ctx.daemon->wallet) {
        err_out["error"] = "Wallet service not available";
        return nullptr;
    }
    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        err_out["error"] = "No active wallet";
        return nullptr;
    }
    auto& wm = wallet_service->get();
    if (wm.isWalletLocked()) {
        err_out["error"] = "wallet_locked";
        return nullptr;
    }
    return &wm;
}

/**
 * Copy an AeadKey out of WalletManager. Logs nothing. Caller MUST scrub
 * the returned key after use.
 */
bool LoadMasterKey(dinero::WalletManager& wm, wlt::AeadKey& out) {
    auto master = wm.GetV7PqMasterKey();
    if (!master) return false;
    std::memcpy(out.data(), master->data(), out.size());
    OPENSSL_cleanse(const_cast<uint8_t*>(master->data()), master->size());
    return true;
}

/** Fixed-size blob → hex string. */
template <std::size_t N>
std::string HexOfArray(const std::array<uint8_t, N>& a) {
    std::vector<uint8_t> v(a.begin(), a.end());
    return util::hex(v);
}

// ---------------------------------------------------------------------------
// wallet.getnewp2mraddress
//
// Params: { "account": int=0, "change": int=0, "address_index": int=0,
//           "leaf_index": int=0, "label": string="", "hrp": string="din" }
//
// Returns: { "address", "merkle_root_hex", "pubkey_hex",
//            "derivation_path", "leaf_index" }
// ---------------------------------------------------------------------------
din::Json rpc_wallet_getnewp2mraddress(const ExecutionContext& ctx,
                                       const din::Json&        params) {
    din::Json result;
    auto* wm = AcquireUnlockedWallet(ctx, result);
    if (!wm) return result;

    // Parse params. All are optional with documented defaults.
    auto get_int = [&](const char* key, int def) -> int {
        if (!params.isMember(key)) return def;
        const auto& v = params[key];
        if (v.is<int>())    return v.as<int>();
        if (v.is<int64_t>()) return static_cast<int>(v.as<int64_t>());
        return def;
    };
    auto get_str = [&](const char* key, const std::string& def) -> std::string {
        if (!params.isMember(key)) return def;
        const auto& v = params[key];
        return v.is<std::string>() ? v.as<std::string>() : def;
    };

    v7rpc::GetNewP2MRAddressParams p{};
    p.wallet_id     = /*single-wallet today*/ 1;
    p.hrp           = get_str("hrp", "din");
    p.account       = get_int("account", 0);
    p.change        = get_int("change", 0);
    p.address_index = get_int("address_index", 0);
    p.leaf_index    = static_cast<uint32_t>(get_int("leaf_index", 0));
    p.label         = get_str("label", "");
    p.now_unix      = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

    // Source BIP-32 material + master key from the unlocked wallet manager.
    auto bip32 = wm->DeriveV7Bip32Material(p.account, p.change, p.address_index);
    if (!bip32) {
        result["error"] = "bip32_derivation_failed";
        return result;
    }
    p.bip32_priv  = bip32->private_key;
    p.bip32_chain = bip32->chain_code;

    if (!LoadMasterKey(*wm, p.master_key)) {
        result["error"] = "v7_master_key_not_loaded";
        OPENSSL_cleanse(p.bip32_priv.data(),  p.bip32_priv.size());
        OPENSSL_cleanse(p.bip32_chain.data(), p.bip32_chain.size());
        return result;
    }

    auto store_ptr = OpenWalletStore(*wm);
    if (!store_ptr) {
        result["error"] = "v7_store_open_failed";
        OPENSSL_cleanse(p.master_key.data(),  p.master_key.size());
        OPENSSL_cleanse(p.bip32_priv.data(),  p.bip32_priv.size());
        OPENSSL_cleanse(p.bip32_chain.data(), p.bip32_chain.size());
        return result;
    }

    auto r = v7rpc::GetNewP2MRAddress(*store_ptr, p);
    // Handler scrubs p.master_key / bip32 internally on every path.

    if (r.status != v7rpc::HandlerStatus::Ok) {
        result["error"]         = StatusString(r.status);
        result["error_message"] = r.error_message;
        return result;
    }

    // Register the P2MR scriptPubKey (0x53 0x20 || merkle_root) with the
    // wallet's UTXOIndex so incoming P2MR outputs get indexed as "mine"
    // and wallet-scoped consumers (template builder's WalletUTXOAdapter,
    // wallet.getbalance, wallet.listunspent) can see the UTXO. Without
    // this, getblocktemplate's FilterChainBackedTemplateTransactions
    // defers any P2MR spend as "non-chain-backed" because the wallet
    // UTXOIndex never learned about the prevout.
    std::vector<uint8_t> p2mr_spk;
    p2mr_spk.reserve(34);
    p2mr_spk.push_back(0x53);  // OP_3 (witness v3)
    p2mr_spk.push_back(0x20);  // PUSH 32 bytes
    p2mr_spk.insert(p2mr_spk.end(), r.merkle_root.begin(), r.merkle_root.end());
    wm->registerP2MRAddress(p2mr_spk, r.derivation_path);

    result["address"]         = r.address;
    result["merkle_root_hex"] = HexOfArray(r.merkle_root);
    result["pubkey_hex"]      = HexOfArray(r.pubkey);
    result["derivation_path"] = r.derivation_path;
    result["leaf_index"]      = static_cast<int64_t>(r.leaf_index);
    return result;
}

// ---------------------------------------------------------------------------
// wallet.listp2mraddresses
//
// Params: {} (ignored — single-wallet-per-session today)
// Returns: { "addresses": [{ "address", "merkle_root_hex", "pubkey_hex",
//                            "derivation_path", "leaf_index", "label",
//                            "created_at" }, ...] }
// ---------------------------------------------------------------------------
din::Json rpc_wallet_listp2mraddresses(const ExecutionContext& ctx,
                                       const din::Json&        params) {
    (void)params;
    din::Json result;
    auto* wm = AcquireUnlockedWallet(ctx, result);
    if (!wm) return result;

    auto store_ptr = OpenWalletStore(*wm);
    if (!store_ptr) {
        result["error"] = "v7_store_open_failed";
        return result;
    }

    v7rpc::ListP2MRAddressesParams p{};
    p.wallet_id = 1;

    auto r = v7rpc::ListP2MRAddresses(*store_ptr, p);
    if (r.status != v7rpc::HandlerStatus::Ok) {
        result["error"]         = StatusString(r.status);
        result["error_message"] = r.error_message;
        return result;
    }

    din::Json arr = din::Json(Json::arrayValue);
    for (const auto& e : r.entries) {
        din::Json row;
        row["address"]         = e.address;
        row["merkle_root_hex"] = HexOfArray(e.merkle_root);
        row["pubkey_hex"]      = HexOfArray(e.pubkey);
        row["derivation_path"] = e.derivation_path;
        row["leaf_index"]      = static_cast<int64_t>(e.leaf_index);
        row["label"]           = e.label;
        row["created_at"]      = static_cast<int64_t>(e.created_at_unix);
        arr.append(row);
    }
    result["addresses"] = arr;
    return result;
}

// ---------------------------------------------------------------------------
// wallet.signp2mr
//
// Params: { "address": "din1r...", "sighash_hex": "<64 hex chars>" }
// Returns: { "scheme_id", "pubkey_hex", "signature_hex" }
// ---------------------------------------------------------------------------
din::Json rpc_wallet_signp2mr(const ExecutionContext& ctx,
                              const din::Json&        params) {
    din::Json result;
    auto* wm = AcquireUnlockedWallet(ctx, result);
    if (!wm) return result;

    if (!params.isMember("address") || !params["address"].is<std::string>()) {
        result["error"] = "missing_address";
        return result;
    }
    if (!params.isMember("sighash_hex") || !params["sighash_hex"].is<std::string>()) {
        result["error"] = "missing_sighash_hex";
        return result;
    }

    v7rpc::SignP2MRParams p{};
    p.wallet_id = 1;
    p.address   = params["address"].as<std::string>();

    const std::string sighash_hex = params["sighash_hex"].as<std::string>();
    std::vector<uint8_t> sighash_bytes;
    if (!util::unhex(sighash_hex, sighash_bytes) || sighash_bytes.size() != p.sighash.size()) {
        result["error"]         = "invalid_params";
        result["error_message"] = "sighash_hex must be exactly 64 hex characters (32 bytes)";
        return result;
    }
    std::memcpy(p.sighash.data(), sighash_bytes.data(), p.sighash.size());

    if (!LoadMasterKey(*wm, p.master_key)) {
        result["error"] = "v7_master_key_not_loaded";
        return result;
    }

    auto store_ptr = OpenWalletStore(*wm);
    if (!store_ptr) {
        result["error"] = "v7_store_open_failed";
        OPENSSL_cleanse(p.master_key.data(), p.master_key.size());
        return result;
    }

    auto r = v7rpc::SignP2MR(*store_ptr, p);
    // Handler zeroizes p.master_key + internal SecureSeed on every path.

    if (r.status != v7rpc::HandlerStatus::Ok) {
        result["error"]         = StatusString(r.status);
        result["error_message"] = r.error_message;
        return result;
    }

    result["scheme_id"]     = static_cast<int64_t>(r.scheme_id);
    result["pubkey_hex"]    = HexOfArray(r.pubkey);
    result["signature_hex"] = HexOfArray(r.signature);
    return result;
}

// ---------------------------------------------------------------------------
// wallet.exportp2mrseed
//
// Returns the raw 32-byte ML-DSA seed for a P2MR address the wallet
// owns. Cold-wallet backup primitive. **DANGEROUS** — this is the only
// v7 RPC that puts seed material on the JSON wire.
//
// Params:  { "address": "din1r..." }
// Returns: { "seed_hex": "<64 hex chars>" }
//
// Preconditions:
//   - Wallet must be open AND unlocked (same gate as wallet.signp2mr;
//     sign requires decrypting the seed anyway, so we're not widening
//     the attack surface by requiring the same state here).
//   - The address must be in this wallet's v7_p2mr_addresses table. We
//     don't look up on merkle_root — the caller names the address they
//     want, and a mismatch returns address_not_found.
//
// Discipline:
//   - The master key is loaded into a local then scrubbed on every path
//     (same pattern used by sign/getnew).
//   - The seed is NOT logged. The only place the 32 bytes appear is
//     the `seed_hex` field in the returned JSON.
//   - Caller (wallet manager, Qt UI, operator) MUST scrub after use;
//     this handler trusts the caller per the header comment on
//     ExportP2MRSeed. We do NOT print the address or seed even at
//     debug level — error paths use a generic status string only.
// ---------------------------------------------------------------------------
din::Json rpc_wallet_exportp2mrseed(const ExecutionContext& ctx,
                                    const din::Json&        params) {
    din::Json result;
    auto* wm = AcquireUnlockedWallet(ctx, result);
    if (!wm) return result;

    if (!params.isMember("address") || !params["address"].is<std::string>()) {
        result["error"] = "missing_address";
        return result;
    }

    v7rpc::ExportP2MRSeedParams p{};
    p.wallet_id = 1;
    p.address   = params["address"].as<std::string>();

    if (!LoadMasterKey(*wm, p.master_key)) {
        result["error"] = "v7_master_key_not_loaded";
        return result;
    }

    auto store_ptr = OpenWalletStore(*wm);
    if (!store_ptr) {
        result["error"] = "v7_store_open_failed";
        OPENSSL_cleanse(p.master_key.data(), p.master_key.size());
        return result;
    }

    auto r = v7rpc::ExportP2MRSeed(*store_ptr, p);
    // Handler scrubs p.master_key on every path; we have nothing further
    // to scrub on this side. The seed lives on r.pq_seed and must be
    // zeroized after we serialize it to hex.

    if (r.status != v7rpc::HandlerStatus::Ok) {
        result["error"]         = StatusString(r.status);
        result["error_message"] = r.error_message;
        OPENSSL_cleanse(r.pq_seed.data(), r.pq_seed.size());
        return result;
    }

    // Serialize to hex, then scrub the raw bytes on the result struct.
    result["seed_hex"] = HexOfArray(r.pq_seed);
    OPENSSL_cleanse(r.pq_seed.data(), r.pq_seed.size());
    return result;
}

// ---------------------------------------------------------------------------
// wallet.importp2mrseed
//
// Accepts a raw 32-byte ML-DSA seed (typically from a cold-wallet
// backup) plus a derivation_path + leaf_index for record-keeping, runs
// KeygenFromSeed → derives the pubkey, encrypts the seed under the
// wallet master key, inserts the row into v7_p2mr_addresses. Does NOT
// walk BIP-32 — the caller is asserting they already derived the seed
// whatever way they wished.
//
// Params: {
//   "seed_hex":        "<64 hex chars>",          // required
//   "derivation_path": "m/88'/1448'/...",          // required for UNIQUE indexing
//   "leaf_index":      <int>,                      // default 0
//   "label":           "<string>",                 // default ""
//   "hrp":             "din" | "rdin" | "tdin"     // default "din"
// }
// Returns: { "address", "merkle_root_hex", "pubkey_hex" }
//
// Preconditions:
//   - Wallet must be open AND unlocked (same as export).
//
// Discipline:
//   - seed_hex is parsed into a local buffer, immediately copied into
//     p.pq_seed, then the intermediate buffer is scrubbed. The only
//     place the raw 32 bytes exist post-parse is p.pq_seed, which the
//     underlying handler scrubs internally on every path.
//   - On any error path after seed parsing, p.pq_seed is scrubbed
//     defensively in case the handler short-circuits before it would.
// ---------------------------------------------------------------------------
din::Json rpc_wallet_importp2mrseed(const ExecutionContext& ctx,
                                    const din::Json&        params) {
    din::Json result;
    auto* wm = AcquireUnlockedWallet(ctx, result);
    if (!wm) return result;

    auto require_str = [&](const char* key) -> const din::Json* {
        if (!params.isMember(key) || !params[key].is<std::string>()) {
            result["error"] = std::string("missing_") + key;
            return nullptr;
        }
        return &params[key];
    };

    const din::Json* seed_field            = require_str("seed_hex");
    if (!seed_field) return result;
    const din::Json* derivation_path_field = require_str("derivation_path");
    if (!derivation_path_field) return result;

    v7rpc::ImportP2MRSeedParams p{};
    p.wallet_id       = 1;
    p.hrp             = params.isMember("hrp") && params["hrp"].is<std::string>()
                            ? params["hrp"].as<std::string>() : std::string("din");
    p.derivation_path = (*derivation_path_field).as<std::string>();
    try {
        dinero::wallet::RejectRetiredLegacyCoinTypeText(p.derivation_path, "wallet.importp2mrseed");
    } catch (const std::exception& e) {
        result["error"] = "invalid_params";
        result["error_message"] = e.what();
        return result;
    }
    p.leaf_index      = (params.isMember("leaf_index") && params["leaf_index"].is<int>())
                            ? static_cast<uint32_t>(params["leaf_index"].as<int>())
                            : 0;
    p.label           = (params.isMember("label") && params["label"].is<std::string>())
                            ? params["label"].as<std::string>() : std::string("");
    p.now_unix        = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();

    // Parse seed_hex → 32-byte buffer. Scrub the intermediate vector
    // before returning on any failure path.
    const std::string seed_hex = (*seed_field).as<std::string>();
    std::vector<uint8_t> seed_bytes;
    if (!util::unhex(seed_hex, seed_bytes) || seed_bytes.size() != p.pq_seed.size()) {
        OPENSSL_cleanse(seed_bytes.data(), seed_bytes.size());
        result["error"]         = "invalid_params";
        result["error_message"] = "seed_hex must be exactly 64 hex characters (32 bytes)";
        return result;
    }
    std::memcpy(p.pq_seed.data(), seed_bytes.data(), p.pq_seed.size());
    OPENSSL_cleanse(seed_bytes.data(), seed_bytes.size());

    if (!LoadMasterKey(*wm, p.master_key)) {
        OPENSSL_cleanse(p.pq_seed.data(), p.pq_seed.size());
        result["error"] = "v7_master_key_not_loaded";
        return result;
    }

    auto store_ptr = OpenWalletStore(*wm);
    if (!store_ptr) {
        OPENSSL_cleanse(p.pq_seed.data(),    p.pq_seed.size());
        OPENSSL_cleanse(p.master_key.data(), p.master_key.size());
        result["error"] = "v7_store_open_failed";
        return result;
    }

    auto r = v7rpc::ImportP2MRSeed(*store_ptr, p);
    // Handler zeroizes p.pq_seed + p.master_key on every path.

    if (r.status != v7rpc::HandlerStatus::Ok) {
        result["error"]         = StatusString(r.status);
        result["error_message"] = r.error_message;
        return result;
    }

    // Register the new P2MR script with the wallet's UTXOIndex so
    // future incoming funds get indexed as mine — same post-create
    // step wallet.getnewp2mraddress does.
    std::vector<uint8_t> p2mr_spk;
    p2mr_spk.reserve(34);
    p2mr_spk.push_back(0x53);
    p2mr_spk.push_back(0x20);
    p2mr_spk.insert(p2mr_spk.end(), r.merkle_root.begin(), r.merkle_root.end());
    wm->registerP2MRAddress(p2mr_spk, p.derivation_path);

    result["address"]         = r.address;
    result["merkle_root_hex"] = HexOfArray(r.merkle_root);
    result["pubkey_hex"]      = HexOfArray(r.pubkey);
    return result;
}

// ---------------------------------------------------------------------------
// debug.computesighash
//
// Thin test-harness helper around the consensus BIP-341 sighash used by
// ValidateTaprootSpend and ValidateP2MRSpend. Lets the regtest build a raw
// tx and get back the exact 32-byte sighash that the v7 P2MR validator
// will compute at acceptance time, without reimplementing BIP-341 in bash.
//
// Params: {
//   "raw_tx_hex":   string (hex of the tx being signed),
//   "input_index":  int,
//   "prevouts":     [ { "scriptPubKey": "<hex>", "amount_sat": <int> }, ... ]
// }
//
// Returns: { "sighash_hex": "<64 hex chars>" }
//
// Does NOT require an open wallet. Pure computation over caller-supplied
// inputs. Intended for test harnesses and low-level diagnostic flows;
// do NOT expose to public RPC users without review.
// ---------------------------------------------------------------------------
din::Json rpc_debug_computesighash(const ExecutionContext& ctx,
                                   const din::Json&        params) {
    (void)ctx;
    din::Json result;

    if (!params.isObject()) {
        result["error"] = "invalid_params";
        result["error_message"] = "params must be an object";
        return result;
    }
    if (!params.isMember("raw_tx_hex") || !params["raw_tx_hex"].is<std::string>()) {
        result["error"] = "missing_raw_tx_hex";
        return result;
    }
    if (!params.isMember("input_index") ||
        !(params["input_index"].is<int>() || params["input_index"].is<int64_t>())) {
        result["error"] = "missing_input_index";
        return result;
    }
    if (!params.isMember("prevouts") || !params["prevouts"].isArray()) {
        result["error"] = "missing_prevouts";
        return result;
    }

    const std::string raw_tx_hex = params["raw_tx_hex"].as<std::string>();
    const size_t      input_index = params["input_index"].is<int>()
                                      ? static_cast<size_t>(params["input_index"].as<int>())
                                      : static_cast<size_t>(params["input_index"].as<int64_t>());

    dinero::Transaction tx;
    if (!dinero::TransactionSerializer::Deserialize(tx, raw_tx_hex)) {
        result["error"] = "invalid_params";
        result["error_message"] = "raw_tx_hex failed to deserialize";
        return result;
    }
    if (input_index >= tx.vin.size()) {
        result["error"] = "invalid_params";
        result["error_message"] = "input_index out of range";
        return result;
    }

    const auto& prevouts = params["prevouts"];
    if (prevouts.size() != tx.vin.size()) {
        result["error"] = "invalid_params";
        result["error_message"] = "prevouts.length must equal tx.vin.length";
        return result;
    }

    dinero::consensus::ScriptExecutionContext sctx(&tx,
                                                   static_cast<uint32_t>(input_index),
                                                   /*amount=*/0,
                                                   /*flags=*/0);
    sctx.all_amounts.reserve(prevouts.size());
    sctx.all_scriptpubkeys.reserve(prevouts.size());
    sctx.all_confidential_flags.reserve(prevouts.size());
    sctx.all_input_commitments.reserve(prevouts.size());

    for (Json::ArrayIndex i = 0; i < prevouts.size(); ++i) {
        const auto& p = prevouts[i];
        if (!p.isObject() ||
            !p.isMember("scriptPubKey") || !p["scriptPubKey"].is<std::string>() ||
            !p.isMember("amount_sat")) {
            result["error"] = "invalid_params";
            result["error_message"] = "each prevout must have scriptPubKey (hex) and amount_sat";
            return result;
        }

        std::vector<uint8_t> spk;
        if (!util::unhex(p["scriptPubKey"].as<std::string>(), spk)) {
            result["error"] = "invalid_params";
            result["error_message"] = "prevout.scriptPubKey is not valid hex";
            return result;
        }

        uint64_t amount_sat = 0;
        if (p["amount_sat"].is<int64_t>())       amount_sat = static_cast<uint64_t>(p["amount_sat"].as<int64_t>());
        else if (p["amount_sat"].is<uint64_t>()) amount_sat = p["amount_sat"].as<uint64_t>();
        else if (p["amount_sat"].is<int>())      amount_sat = static_cast<uint64_t>(p["amount_sat"].as<int>());
        else {
            result["error"] = "invalid_params";
            result["error_message"] = "prevout.amount_sat must be integer";
            return result;
        }

        if (i == input_index) {
            sctx.amount = amount_sat;
        }
        sctx.all_amounts.push_back(amount_sat);
        sctx.all_scriptpubkeys.push_back(std::move(spk));
        sctx.all_confidential_flags.push_back(0);
        sctx.all_input_commitments.push_back({});
    }

    std::vector<uint8_t> leaf_hash;
    std::vector<uint8_t> sighash =
        dinero::consensus::SignatureHashTaproot(sctx, /*hash_type=*/0x00, leaf_hash);
    if (sighash.size() != 32) {
        result["error"] = "internal_error";
        result["error_message"] = "SignatureHashTaproot returned unexpected length";
        return result;
    }

    result["sighash_hex"] = util::hex(sighash);
    return result;
}

// ---------------------------------------------------------------------------
// debug.attachwitness
//
// Parses a serialized transaction (with or without existing witness), writes
// a single witness-stack element into tx.vin[input_index].witness, and
// returns the full with-witness re-serialization. Companion to
// debug.computesighash: a test harness can assemble a canonical P2MR
// witness blob off-node, call this to attach it, and hand the result to
// sendrawtransaction.
//
// Params: {
//   "raw_tx_hex":      string,
//   "input_index":     int,
//   "witness_hex":     string   // single stack element; overwrites any existing
//                                // witness on that input
// }
//
// Returns: { "hex": "<full-tx-with-witness>" }
//
// No secret material. Pure transform. Not a wallet-facing primitive.
// ---------------------------------------------------------------------------
din::Json rpc_debug_attachwitness(const ExecutionContext& ctx,
                                  const din::Json&        params) {
    (void)ctx;
    din::Json result;

    if (!params.isObject() ||
        !params.isMember("raw_tx_hex")  || !params["raw_tx_hex"].is<std::string>() ||
        !params.isMember("input_index") ||
        !params.isMember("witness_hex") || !params["witness_hex"].is<std::string>()) {
        result["error"] = "invalid_params";
        result["error_message"] = "params: {raw_tx_hex, input_index, witness_hex}";
        return result;
    }

    const size_t input_index = params["input_index"].is<int>()
                                 ? static_cast<size_t>(params["input_index"].as<int>())
                                 : static_cast<size_t>(params["input_index"].as<int64_t>());

    dinero::Transaction tx;
    if (!dinero::TransactionSerializer::Deserialize(tx, params["raw_tx_hex"].as<std::string>())) {
        result["error"] = "invalid_params";
        result["error_message"] = "raw_tx_hex failed to deserialize";
        return result;
    }
    if (input_index >= tx.vin.size()) {
        result["error"] = "invalid_params";
        result["error_message"] = "input_index out of range";
        return result;
    }

    std::vector<uint8_t> witness_bytes;
    if (!util::unhex(params["witness_hex"].as<std::string>(), witness_bytes)) {
        result["error"] = "invalid_params";
        result["error_message"] = "witness_hex is not valid hex";
        return result;
    }

    tx.vin[input_index].witness.clear();
    tx.vin[input_index].witness.push_back(std::move(witness_bytes));

    // Transaction::HasWitness() is gated on the tx-level witness_version
    // field. Deserialize() of an unsigned tx leaves that at 0xFF (legacy),
    // so SerializeHex(true) would still emit the non-witness wire format
    // and drop the witness we just attached. Flip it to a non-legacy
    // value so the marker/flag + witness block is written.
    if (tx.witness_version == 0xFF) {
        tx.witness_version = 0;  // any value != 0xFF triggers HasWitness()
    }

    result["hex"] = tx.SerializeHex(/*include_witness=*/true);
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Explicit registrar — called from src/daemon/rpc_context_wiring.cpp
// alongside registerWalletMethodsContext(). Static-init registration won't
// work because dinero_rpc_handlers isn't a whole-archive dependency of
// dinerod, so constructor-ordering tricks would silently drop these.
// ---------------------------------------------------------------------------
void registerV7PqWalletMethods() {
    g_rpcRegistry.registerHandler("wallet.getnewp2mraddress",
                                  rpc_wallet_getnewp2mraddress,
                                  RegisterMode::Overwrite,
                                  "v7-pq");
    g_rpcRegistry.registerHandler("wallet.listp2mraddresses",
                                  rpc_wallet_listp2mraddresses,
                                  RegisterMode::Overwrite,
                                  "v7-pq");
    g_rpcRegistry.registerHandler("wallet.signp2mr",
                                  rpc_wallet_signp2mr,
                                  RegisterMode::Overwrite,
                                  "v7-pq");
    // Phase 4c.3.1.1: previously "dark" — landed after their security
    // review. Both require an unlocked wallet and never log seed bytes.
    g_rpcRegistry.registerHandler("wallet.exportp2mrseed",
                                  rpc_wallet_exportp2mrseed,
                                  RegisterMode::Overwrite,
                                  "v7-pq");
    g_rpcRegistry.registerHandler("wallet.importp2mrseed",
                                  rpc_wallet_importp2mrseed,
                                  RegisterMode::Overwrite,
                                  "v7-pq");
    g_rpcRegistry.registerHandler("debug.computesighash",
                                  rpc_debug_computesighash,
                                  RegisterMode::Overwrite,
                                  "v7-pq");
    g_rpcRegistry.registerHandler("debug.attachwitness",
                                  rpc_debug_attachwitness,
                                  RegisterMode::Overwrite,
                                  "v7-pq");
}
