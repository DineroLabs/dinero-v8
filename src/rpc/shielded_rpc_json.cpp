/**
 * Shielded pool JSON-RPC adapters.
 *
 * Registers:
 *   wallet.shield          — transparent → private
 *   wallet.unshield        — private → transparent
 *   wallet.shieldedbalance — private pool balance + note count
 *   wallet.listshielded    — list unspent shielded notes
 *
 * Spending handlers require an unlocked wallet. Read/receive handlers stay
 * available while locked so wallets can receive and display already-indexed
 * shielded state without exposing spend authority.
 */

#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/wallet_service.h"
#include "dinero/daemon/execution_context.h"
#include "consensus/chainparams.h"
#include "consensus/pq/p2mr_consensus.h"
#include "primitives/transaction.h"
#include "wallet/canonical_wallet_utxo.h"
#include "wallet/transaction_builder.h"
#include "wallet/transaction_signer.h"
#include "wallet/unsigned_tx_builder.h"
#include "wallet/v7_p2mr_store.h"
#include "wallet/wallet_key_provider.h"
#include "wallet/wallet_manager.h"
#include "wallet/shielded_wallet_ops.h"
#include "wallet/shielded_derivation.h"

#include <openssl/crypto.h>

#include <climits>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace dinero;
namespace ops  = dinero::wallet::shielded_ops;
using din::Json;

// spec Fatal §3: gate shielded spend handlers.  Returns true when safe mode is
// active and err["error"] has been populated.  Call before AcquireWallet.
static bool ShieldedRefuseIfSafeMode(const ExecutionContext& ctx, Json& err) {
    if (!ctx.daemon || !ctx.daemon->chainstate) return false;
    auto cs = std::dynamic_pointer_cast<ChainstateService>(ctx.daemon->chainstate);
    if (cs && cs->IsInSafeMode()) {
        err["error"] = "disabled while node is in safe mode: " + cs->GetSafeModeReason();
        err["safe_mode"] = true;
        return true;
    }
    return false;
}

WalletManager* AcquireWallet(const ExecutionContext& ctx, Json& err, bool require_unlocked = true) {
    if (!ctx.daemon || !ctx.daemon->wallet) {
        err["error"] = "Wallet not available";
        return nullptr;
    }
    auto ws = std::dynamic_pointer_cast<WalletService>(ctx.daemon->wallet);
    if (!ws || !ws->hasActiveWallet()) {
        err["error"] = "No active wallet";
        return nullptr;
    }
    auto& wm = ws->get();
    if (require_unlocked && wm.isWalletLocked()) {
        err["error"] = "wallet_locked";
        return nullptr;
    }
    return &wm;
}

// Phase 1 activation gate. Returns true and writes a JSON-RPC error
// into `err` when the shielded pool is not yet active on this network
// (chainparams.shielded_activation_height == UINT32_MAX). Once Phase
// 4/5 sets a real activation height, this should additionally compare
// the active tip height — for now the chainparams sentinel is enough,
// since regtest sets it to 0 (active) and mainnet/testnet keep
// UINT32_MAX (parked).
bool RejectIfShieldedNotActive(Json& err) {
    if (Params().shielded_activation_height == UINT32_MAX) {
        err["error"]         = "shielded_not_active";
        err["error_message"] = "shielded pool not yet active on this network";
        return true;
    }
    return false;
}

// Hex helper.
std::string HashToHex(const consensus::shielded::Hash& h) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t b : h) {
        out << std::setw(2) << static_cast<int>(b);
    }
    return out.str();
}

std::string ShieldedAddressCacheKey(const std::string& hrp,
                                    uint32_t account,
                                    uint64_t j) {
    return "shielded_address:" + hrp + ":" + std::to_string(account) +
           ":" + std::to_string(j);
}

bool LoadCachedShieldedAddress(WalletManager& wm,
                               const std::string& hrp,
                               uint32_t account,
                               uint64_t j,
                               Json& result) {
    const std::string cached = wm.getSetting(ShieldedAddressCacheKey(hrp, account, j));
    if (cached.empty()) return false;

    const auto first = cached.find('|');
    const auto second = first == std::string::npos ? std::string::npos
                                                   : cached.find('|', first + 1);
    if (first == std::string::npos || second == std::string::npos) return false;

    result["address"]      = cached.substr(0, first);
    result["hrp"]          = hrp;
    result["account"]      = static_cast<int64_t>(account);
    result["j"]            = static_cast<int64_t>(j);
    result["d_hex"]        = cached.substr(first + 1, second - first - 1);
    result["pk_d_hex"]     = cached.substr(second + 1);
    result["cached"]       = true;
    return true;
}

void StoreCachedShieldedAddress(WalletManager& wm,
                                const std::string& hrp,
                                uint32_t account,
                                uint64_t j,
                                const std::string& address,
                                const std::string& d_hex,
                                const std::string& pk_d_hex) {
    wm.setSetting(ShieldedAddressCacheKey(hrp, account, j),
                  address + "|" + d_hex + "|" + pk_d_hex);
}

// ---------------------------------------------------------------------------
// wallet.shield
//
// Params: { "amount": <DIN as float>, "fee_una": <int|optional> }
//
// Builds a v5 transaction:
//   - vin   = transparent UTXOs covering (value_una + fee_una)
//   - vout  = optional change output (no recipient — bundle is the recipient)
//   - shielded_bundle_bytes = single-output bundle commiting to value_una
//   - explicit_fee = fee_una (committed to via the binding-sig sighash)
// Signs the transparent inputs with TransactionSigner and submits to mempool.
// ---------------------------------------------------------------------------
Json rpc_wallet_shield(const ExecutionContext& ctx, const Json& params) {
    Json result;
    if (ShieldedRefuseIfSafeMode(ctx, result)) return result;  // spec Fatal §3
    if (RejectIfShieldedNotActive(result)) return result;
    auto* wm = AcquireWallet(ctx, result);
    if (!wm) return result;

    std::string init_error;
    if (!ops::EnsureWalletRuntime(*wm, &init_error)) {
        result["error"] = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return result;
    }

    // ── Parse params ──────────────────────────────────────────────────
    // Issue #273: when the caller does NOT pass a fee, the provisional
    // default below is only a starting point — the handler measures the
    // final tx (v6 bundle counts in BASE serialization, so vsize is
    // kilobytes) and raises the fee to the mempool's size-based floor.
    // An explicitly-passed fee is always respected verbatim.
    double   amount_din   = 0;
    uint64_t fee_una      = 1000;  // provisional default (auto-sized below)
    bool     fee_explicit = false;
    if (params.isObject()) {
        if (params.isMember("amount"))  amount_din = params["amount"].asDouble();
        if (params.isMember("fee_una")) {
            fee_una      = static_cast<uint64_t>(params["fee_una"].asInt64());
            fee_explicit = true;
        }
    } else if (params.isArray()) {
        if (params.size() >= 1) amount_din = params[0].asDouble();
        if (params.size() >= 2) {
            fee_una      = static_cast<uint64_t>(params[1].asInt64());
            fee_explicit = true;
        }
    }
    if (amount_din <= 0) {
        result["error"] = "invalid_params";
        result["error_message"] = "amount must be positive";
        return result;
    }
    const uint64_t value_una = static_cast<uint64_t>(amount_din * 1e8);

    // ── Tip height for note metadata ──────────────────────────────────
    uint32_t tip_height = 0;
    if (ctx.daemon && ctx.daemon->chainstate) {
        if (auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate)) {
            tip_height = cs->getBlockHeight();
        }
    }

    // ── Mempool service (need it for both UTXO filtering and submit) ──
    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Mempool service unavailable";
        return result;
    }
    const auto& mempool = mempool_service->mempool();

    // ── Build + sign as a function of the fee ─────────────────────────
    // Issue #273 two-pass sizing: pass 1 (persist=false) builds and signs
    // a throwaway tx with the provisional fee purely to measure the final
    // vsize (the shielded bundle and the input witnesses both count).
    // Pass 2 rebuilds with the size-adequate fee and persists the note.
    // On failure, fills `result` and returns false.
    struct BuiltShieldTx {
        dinero::Transaction     signed_tx;
        ops::AttachShieldResult attach;
        uint64_t                change_una = 0;
        size_t                  n_inputs = 0;
    };
    auto build_signed_shield_tx = [&](uint64_t fee, bool persist,
                                      BuiltShieldTx& out) -> bool {
        const uint64_t needed = value_una + fee;

        // ── Greedy UTXO selection ─────────────────────────────────────
        auto utxos = wm->listUnspentUTXOs(1, 9999999);
        utxos.erase(std::remove_if(utxos.begin(), utxos.end(),
            [&](const dinero::WalletManager::WalletUTXO& u) {
                OutPoint op(dinero::TxId(uint256::FromHexUnsafe(u.txid)), u.vout);
                return mempool.isOutputSpentInMempool(op) ||
                       wm->isUTXOLocked(u.txid, u.vout);
            }), utxos.end());
        std::sort(utxos.begin(), utxos.end(),
                  [](const auto& a, const auto& b) { return a.amount_una > b.amount_una; });

        std::vector<dinero::WalletManager::WalletUTXO> selected;
        uint64_t total_in = 0;
        for (const auto& u : utxos) {
            selected.push_back(u);
            total_in += u.amount_una;
            if (total_in >= needed) break;
        }
        if (total_in < needed) {
            result["error"] = "insufficient_funds";
            result["error_message"] = "selected " + std::to_string(total_in) +
                " una < needed " + std::to_string(needed);
            return false;
        }
        const uint64_t change_una = total_in - needed;

        // ── Build canonical UTXO list + assemble unsigned transparent envelope ──
        std::vector<dinero::CanonicalWalletUTXO> canon_utxos;
        canon_utxos.reserve(selected.size());
        for (const auto& u : selected) {
            dinero::CanonicalWalletUTXO c;
            c.txid        = dinero::uint256::FromHexUnsafe(u.txid);
            c.vout        = u.vout;
            c.value       = dinero::AmountUna::Una(u.amount_una);
            c.path        = u.derivation_path;
            c.height      = u.height;
            c.is_coinbase = u.is_coinbase;
            if (!u.script_pubkey.empty()) {
                c.spk.reserve(u.script_pubkey.size() / 2);
                for (size_t i = 0; i + 1 < u.script_pubkey.size(); i += 2) {
                    c.spk.push_back(static_cast<uint8_t>(
                        std::stoi(u.script_pubkey.substr(i, 2), nullptr, 16)));
                }
            }
            canon_utxos.push_back(std::move(c));
        }

        dinero::Transaction tx;
        tx.version         = dinero::Transaction::TX_VERSION_SHIELDED_V2;
        tx.witness_version = 0;  // SegWit — TransactionSigner emits witness data
        tx.lockTime        = 0;
        for (const auto& c : canon_utxos) {
            dinero::TxInput in;
            in.prevout.txid = dinero::TxId(c.txid);
            in.prevout.vout = c.vout;
            in.sequence     = 0xfffffffe;  // RBF
            tx.vin.push_back(in);
        }
        std::string change_address;
        if (change_una > 0) {
            change_address = wm->getNewAddress("change", "taproot");
            if (change_address.empty()) {
                result["error"] = "no_change_address";
                return false;
            }
            dinero::TxOutput change_out;
            change_out.value = dinero::AmountUna::Una(change_una);
            change_out.scriptPubKey = dinero::TransactionBuilder::AddressToScriptPubKey(change_address);
            if (change_out.scriptPubKey.empty()) {
                result["error"] = "invalid_change_address";
                return false;
            }
            tx.vout.push_back(change_out);
        }
        tx.SetExplicitFee(fee);

        // ── Attach the shielded output bundle (one output, value_balance=+value_una) ──
        auto attach_rc = ops::AttachShieldOutputBundle(tx, value_una, *wm,
                                                       tip_height, persist);
        if (attach_rc.status != ops::OpStatus::Ok) {
            result["error"] = "attach_bundle_failed";
            result["error_message"] = attach_rc.error;
            return false;
        }

        // ── Sign transparent inputs ───────────────────────────────────
        dinero::UnsignedTransaction unsigned_tx;
        unsigned_tx.tx              = tx;
        unsigned_tx.fee             = fee;
        unsigned_tx.change_amount   = change_una;
        unsigned_tx.change_address  = change_address;
        unsigned_tx.selected_utxos  = canon_utxos;
        unsigned_tx.signals_rbf     = true;

        bool any_p2mr = false;
        std::map<std::string, std::string> path_to_key;
        for (const auto& c : canon_utxos) {
            if (dinero::consensus::pq::IsP2MRScript(c.spk)) {
                any_p2mr = true;
            } else {
                auto pk_opt = wm->deriveKeyForScriptPubKey([&]() {
                    std::ostringstream s;
                    s << std::hex << std::setfill('0');
                    for (uint8_t b : c.spk) s << std::setw(2) << static_cast<int>(b);
                    return s.str();
                }());
                if (pk_opt && !pk_opt->empty()) {
                    std::ostringstream hexs;
                    hexs << std::hex << std::setfill('0');
                    for (uint8_t b : *pk_opt) hexs << std::setw(2) << static_cast<int>(b);
                    path_to_key[c.path] = hexs.str();
                } else if (!c.path.empty()) {
                    std::string k = wm->getPrivateKeyForPath(c.path);
                    if (!k.empty()) path_to_key[c.path] = k;
                }
            }
        }

        std::unique_ptr<dinero::KeyProvider> kp;
        std::unique_ptr<dinero::wallet::V7P2MRStore> p2mr_store;
        if (any_p2mr) {
            auto master = wm->GetV7PqMasterKey();
            if (!master) {
                result["error"] = "wallet_locked_p2mr";
                return false;
            }
            const std::string store_path = wm->GetV7P2MRStorePath();
            if (store_path.empty()) {
                result["error"] = "p2mr_store_path_missing";
                return false;
            }
            p2mr_store = std::make_unique<dinero::wallet::V7P2MRStore>();
            if (p2mr_store->Open(store_path) != dinero::wallet::V7P2MRStore::OpenResult::Ok) {
                result["error"] = "p2mr_store_open_failed";
                return false;
            }
            dinero::wallet::WalletKeyProvider::Config cfg;
            cfg.legacy_keys_by_path = path_to_key;
            cfg.p2mr_store          = p2mr_store.get();
            cfg.wallet_id           = 1;
            std::memcpy(cfg.master_key.data(), master->data(), cfg.master_key.size());
            OPENSSL_cleanse(const_cast<uint8_t*>(master->data()), master->size());
            kp = std::make_unique<dinero::wallet::WalletKeyProvider>(std::move(cfg));
        } else {
            kp = std::make_unique<dinero::MapKeyProvider>(path_to_key);
        }

        auto sign_result = dinero::TransactionSigner::Sign(unsigned_tx, *kp);
        if (!sign_result.success) {
            result["error"] = "sign_failed";
            result["error_message"] = sign_result.error;
            return false;
        }

        out.signed_tx  = sign_result.signed_tx.tx;
        out.attach     = attach_rc;
        out.change_una = change_una;
        out.n_inputs   = canon_utxos.size();
        return true;
    };

    // ── Issue #273: size-aware fee when the caller didn't pass one ────
    uint64_t final_fee = fee_una;
    bool fee_autosized = false;
    if (!fee_explicit) {
        BuiltShieldTx probe;
        if (!build_signed_shield_tx(fee_una, /*persist=*/false, probe)) {
            return result;
        }
        const double min_fee_rate = mempool_service->mempool().getMinFeeRate();
        const uint64_t required = ops::RequiredFeeForTx(probe.signed_tx, min_fee_rate);
        if (required > final_fee) final_fee = required;
        fee_autosized = true;
    }

    BuiltShieldTx built;
    if (!build_signed_shield_tx(final_fee, /*persist=*/true, built)) {
        return result;
    }

    // ── Submit to mempool ─────────────────────────────────────────────
    const dinero::Transaction& signed_tx = built.signed_tx;
    auto submit = mempool_service->mempool().submitTransaction(
        signed_tx, "rpc:wallet.shield", true);
    if (!submit.accepted()) {
        result["error"] = "mempool_rejected";
        result["reject_code"] = TxRejectCodeToString(submit.code);
        result["reject_reason"] = submit.message;
        // Bundle is already attached; surfacing the txid helps debugging.
        result["txid"] = signed_tx.GetTxid().AsUint256().GetHex();
        result["fee_autosized"] = fee_autosized;
        result["vsize"] = static_cast<int64_t>(signed_tx.GetVirtualSize());
        return result;
    }

    result["status"]         = "shielded";
    result["txid"]           = signed_tx.GetTxid().AsUint256().GetHex();
    result["commitment_hex"] = HashToHex(built.attach.commitment);
    result["value_una"]      = static_cast<int64_t>(value_una);
    result["fee_una"]        = static_cast<int64_t>(final_fee);
    result["fee_autosized"]  = fee_autosized;
    result["vsize"]          = static_cast<int64_t>(signed_tx.GetVirtualSize());
    result["change_una"]     = static_cast<int64_t>(built.change_una);
    result["bundle_bytes"]   = static_cast<int64_t>(built.attach.bundle_bytes);
    result["inputs"]         = static_cast<int>(built.n_inputs);
    result["tree_size"]      = static_cast<int64_t>(ops::GetShieldedTreeSize(*wm));
    return result;
}

// ---------------------------------------------------------------------------
// wallet.unshield
//
// Params: { "amount": <DIN as float>, "fee_una": <int|optional> }
//
// Spends the smallest unspent confirmed shielded note with value >= amount,
// re-emerging note_value - fee_una as a fresh transparent UTXO controlled by
// the same wallet. Single-note only — multi-note coin selection (and change)
// is Wave 3d.
// ---------------------------------------------------------------------------
Json rpc_wallet_unshield(const ExecutionContext& ctx, const Json& params) {
    Json result;
    if (ShieldedRefuseIfSafeMode(ctx, result)) return result;  // spec Fatal §3
    if (RejectIfShieldedNotActive(result)) return result;
    auto* wm = AcquireWallet(ctx, result);
    if (!wm) return result;

    std::string init_error;
    if (!ops::EnsureWalletRuntime(*wm, &init_error)) {
        result["error"] = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return result;
    }

    // ── Parse params ──────────────────────────────────────────────────
    // Issue #273: no explicit fee → provisional default, auto-sized below
    // against the measured tx vsize. Explicit fee → respected verbatim.
    double   amount_din   = 0;
    uint64_t fee_una      = 1000;  // provisional default (auto-sized below)
    bool     fee_explicit = false;
    if (params.isObject()) {
        if (params.isMember("amount"))  amount_din = params["amount"].asDouble();
        if (params.isMember("fee_una")) {
            fee_una      = static_cast<uint64_t>(params["fee_una"].asInt64());
            fee_explicit = true;
        }
    } else if (params.isArray()) {
        if (params.size() >= 1) amount_din = params[0].asDouble();
        if (params.size() >= 2) {
            fee_una      = static_cast<uint64_t>(params[1].asInt64());
            fee_explicit = true;
        }
    }
    if (amount_din <= 0) {
        result["error"] = "invalid_params";
        result["error_message"] = "amount must be positive";
        return result;
    }
    const uint64_t requested_una = static_cast<uint64_t>(amount_din * 1e8);

    // ── Mempool service for submit ────────────────────────────────────
    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Mempool service unavailable";
        return result;
    }

    // ── Select the smallest unspent confirmed note >= requested ───────
    auto note_opt = ops::SelectUnshieldNote(*wm, requested_una);
    if (!note_opt) {
        result["error"] = "insufficient_single_note";
        result["error_message"] = "no single unspent confirmed shielded note >= requested amount; "
                                  "multi-note unshield is Wave 3d";
        return result;
    }
    const auto& note = *note_opt;
    const uint64_t note_value = note.value_una;

    constexpr uint64_t kDustThreshold = 546;
    // Dust + fee feasibility: transparent vout = note_value - fee.
    // Fills `result` and returns false when the fee doesn't fit the note.
    auto validate_fee = [&](uint64_t fee, bool autosized) -> bool {
        if (fee >= note_value) {
            result["error"] = "fee_too_large";
            result["error_message"] = autosized
                ? "note too small to cover size-based fee (need " +
                  std::to_string(fee) + " una, note " +
                  std::to_string(note_value) + " una)"
                : "fee >= note value";
            return false;
        }
        if (note_value - fee < kDustThreshold) {
            result["error"] = "dust_recipient";
            result["error_message"] = "note_value - fee_una < dust threshold (546 una)";
            return false;
        }
        return true;
    };
    if (!validate_fee(fee_una, false)) return result;

    // ── Allocate a fresh self-controlled recipient address ────────────
    const std::string recipient_addr = wm->getNewAddress("unshield-out", "taproot");
    if (recipient_addr.empty()) {
        result["error"] = "no_recipient_address";
        return result;
    }
    auto recipient_spk = dinero::TransactionBuilder::AddressToScriptPubKey(recipient_addr);
    if (recipient_spk.empty()) {
        result["error"] = "invalid_recipient_address";
        return result;
    }

    // ── Build unsigned envelope: empty vin, single transparent vout ───
    // Force SegWit-shape (witness_version=0) so the serializer emits the
    // 0x00 0x01 marker after version. Without it, an empty-vin tx
    // (vin_count=0x00) followed by vout_count=0x01 is structurally
    // indistinguishable from a SegWit-marked tx — the deserializer's
    // legacy/segwit detection collides with our wire bytes. With the
    // marker present, parse is unambiguous: marker is consumed, vin_count
    // is read as 0, vout_count is read as 1. Witness section emits zero
    // stacks (one per input → none).
    auto make_envelope = [&](uint64_t fee) -> dinero::Transaction {
        dinero::Transaction tx;
        tx.version         = dinero::Transaction::TX_VERSION_SHIELDED_V2;
        tx.witness_version = 0;
        tx.lockTime        = 0;
        dinero::TxOutput recipient_out;
        recipient_out.value = dinero::AmountUna::Una(note_value - fee);
        recipient_out.scriptPubKey = recipient_spk;
        tx.vout.push_back(std::move(recipient_out));
        tx.SetExplicitFee(fee);
        return tx;
    };

    // ── Issue #273: size-aware fee when the caller didn't pass one ────
    // Pass 1 (persist=false): throwaway build with the provisional fee to
    // measure the final vsize — the v6 bundle counts in BASE serialization,
    // so the spend proof alone pushes vsize into kilobytes. Then rebuild
    // once with the size-adequate fee (the explicit-fee field is fixed
    // 8 bytes, so the fee value cannot change the size).
    uint64_t final_fee = fee_una;
    bool fee_autosized = false;
    if (!fee_explicit) {
        dinero::Transaction probe = make_envelope(fee_una);
        auto probe_rc = ops::AttachUnshieldInputBundle(probe, note.leaf_index,
                                                       fee_una, *wm,
                                                       /*persist=*/false);
        if (probe_rc.status != ops::OpStatus::Ok) {
            result["error"] = "attach_unshield_failed";
            result["error_message"] = probe_rc.error;
            return result;
        }
        const double min_fee_rate = mempool_service->mempool().getMinFeeRate();
        const uint64_t required = ops::RequiredFeeForTx(probe, min_fee_rate);
        if (required > final_fee) final_fee = required;
        fee_autosized = true;
        if (!validate_fee(final_fee, true)) return result;
    }
    const uint64_t recipient_una = note_value - final_fee;

    dinero::Transaction tx = make_envelope(final_fee);

    // ── Attach the bundle (one spend, zero outputs, value_balance = -note_value) ──
    auto attach_rc = ops::AttachUnshieldInputBundle(tx, note.leaf_index,
                                                    final_fee, *wm);
    if (attach_rc.status != ops::OpStatus::Ok) {
        result["error"] = "attach_unshield_failed";
        result["error_message"] = attach_rc.error;
        return result;
    }

    // ── Submit. No transparent input signing needed — vin is empty. ───
    auto submit = mempool_service->mempool().submitTransaction(
        tx, "rpc:wallet.unshield", true);
    if (!submit.accepted()) {
        result["error"] = "mempool_rejected";
        result["reject_code"] = TxRejectCodeToString(submit.code);
        result["reject_reason"] = submit.message;
        result["txid"] = tx.GetTxid().AsUint256().GetHex();
        result["fee_autosized"] = fee_autosized;
        result["vsize"] = static_cast<int64_t>(tx.GetVirtualSize());
        return result;
    }

    result["status"]            = "unshielded";
    result["txid"]              = tx.GetTxid().AsUint256().GetHex();
    result["nullifier_hex"]     = HashToHex(attach_rc.nullifier);
    result["anchor_hex"]        = HashToHex(attach_rc.anchor);
    result["note_value_una"]    = static_cast<int64_t>(note_value);
    result["fee_una"]           = static_cast<int64_t>(final_fee);
    result["fee_autosized"]     = fee_autosized;
    result["vsize"]             = static_cast<int64_t>(tx.GetVirtualSize());
    result["recipient_address"] = recipient_addr;
    result["recipient_una"]     = static_cast<int64_t>(recipient_una);
    result["bundle_bytes"]      = static_cast<int64_t>(attach_rc.bundle_bytes);
    result["leaf_index"]        = static_cast<int64_t>(note.leaf_index);
    return result;
}

// ---------------------------------------------------------------------------
// wallet.transfer
//
// Params (object form): { "fee_una": <int>, "amount_una": <int|optional> }
// Params (array form):  [ fee_una, amount_una? ]
//
// Wave 3d (no amount): refresh smallest unspent confirmed note as a fresh
// self-note with value = note - fee. value_balance = -fee_una.
//
// Wave 3e (amount present): greedy-fill multi-note selection covering
// amount + fee, emit a recipient self-note of `amount_una` and (if any)
// a change self-note of `sum_inputs - amount_una - fee_una`. All outputs
// self-controlled (any-recipient transfer needs diversifier work parked
// in Phase 5).
//
// Both waves: empty transparent vin/vout, value_balance = -fee_una.
// ---------------------------------------------------------------------------
Json rpc_wallet_transfer(const ExecutionContext& ctx, const Json& params) {
    Json result;
    if (ShieldedRefuseIfSafeMode(ctx, result)) return result;  // spec Fatal §3
    if (RejectIfShieldedNotActive(result)) return result;
    auto* wm = AcquireWallet(ctx, result);
    if (!wm) return result;

    std::string init_error;
    if (!ops::EnsureWalletRuntime(*wm, &init_error)) {
        result["error"] = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return result;
    }

    // ── Parse params ──────────────────────────────────────────────────
    // Issue #273: no explicit fee → provisional default, auto-sized below
    // against the measured tx vsize. Explicit fee → respected verbatim.
    // Array form always carries the fee in slot 0; use the object form to
    // omit the fee and get auto-sizing.
    uint64_t fee_una      = 1000;  // provisional default (auto-sized below)
    bool     fee_explicit = false;
    bool     have_amount = false;
    uint64_t amount_una = 0;
    std::string recipient_address;
    std::string recipient_memo;
    if (params.isObject()) {
        if (params.isMember("fee_una")) {
            fee_una      = static_cast<uint64_t>(params["fee_una"].asInt64());
            fee_explicit = true;
        }
        if (params.isMember("amount_una")) {
            have_amount = true;
            amount_una = static_cast<uint64_t>(params["amount_una"].asInt64());
        }
        if (params.isMember("address")) {
            recipient_address = params["address"].asString();
        }
        if (params.isMember("memo")) {
            recipient_memo = params["memo"].asString();
        }
    } else if (params.isArray()) {
        if (params.size() >= 1) {
            fee_una      = static_cast<uint64_t>(params[0].asInt64());
            fee_explicit = true;
        }
        if (params.size() >= 2) {
            have_amount = true;
            amount_una = static_cast<uint64_t>(params[1].asInt64());
        }
        if (params.size() >= 3) recipient_address = params[2].asString();
        if (params.size() >= 4) recipient_memo    = params[3].asString();
    }
    if (fee_una == 0) {
        result["error"] = "invalid_params";
        result["error_message"] = "fee_una must be positive";
        return result;
    }
    if (have_amount && amount_una == 0) {
        result["error"] = "invalid_params";
        result["error_message"] = "amount_una must be positive when supplied";
        return result;
    }
    if (!recipient_address.empty() && !have_amount) {
        result["error"] = "invalid_params";
        result["error_message"] = "address requires amount_una";
        return result;
    }

    // ── Mempool service for submit ────────────────────────────────────
    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Mempool service unavailable";
        return result;
    }

    // ── Build empty-vin / empty-vout v6 envelope ──────────────────────
    // Empty-vin AND empty-vout: force the SegWit marker so the
    // serializer doesn't ambiguity-collide vin_count=0x00 with the
    // BIP141 0x00 0x01 marker. With marker present, parse is
    // unambiguous regardless of vin/vout cardinality.
    auto make_envelope = [](uint64_t fee) -> dinero::Transaction {
        dinero::Transaction tx;
        tx.version         = dinero::Transaction::TX_VERSION_SHIELDED_V2;
        tx.witness_version = 0;
        tx.lockTime        = 0;
        tx.SetExplicitFee(fee);
        return tx;
    };
    const double min_fee_rate = mempool_service->mempool().getMinFeeRate();

    if (!have_amount) {
        // ── Wave 3d fast path: refresh smallest single note. ──────────
        auto note_opt = ops::SelectTransferNote(*wm, fee_una + 1);
        if (!note_opt) {
            result["error"] = "insufficient_single_note";
            result["error_message"] = "no single unspent confirmed shielded note > fee_una; "
                                      "supply amount_una to use multi-note selection (Wave 3e)";
            return result;
        }

        // ── Issue #273: size-aware fee when the caller didn't pass one.
        // Pass 1 (persist=false): throwaway build with the provisional
        // fee to measure the final vsize, then re-select against the
        // size-adequate fee and rebuild once.
        uint64_t final_fee = fee_una;
        bool fee_autosized = false;
        if (!fee_explicit) {
            dinero::Transaction probe = make_envelope(fee_una);
            auto probe_rc = ops::AttachTransferInputBundle(
                probe, note_opt->leaf_index, fee_una, *wm, /*persist=*/false);
            if (probe_rc.status != ops::OpStatus::Ok) {
                result["error"] = "attach_transfer_failed";
                result["error_message"] = probe_rc.error;
                return result;
            }
            const uint64_t required = ops::RequiredFeeForTx(probe, min_fee_rate);
            if (required > final_fee) final_fee = required;
            fee_autosized = true;
            if (final_fee != fee_una) {
                note_opt = ops::SelectTransferNote(*wm, final_fee + 1);
                if (!note_opt) {
                    result["error"] = "insufficient_single_note";
                    result["error_message"] =
                        "no single unspent confirmed shielded note > size-based fee (" +
                        std::to_string(final_fee) + " una); supply amount_una to use "
                        "multi-note selection (Wave 3e)";
                    return result;
                }
            }
        }
        const auto& note = *note_opt;

        dinero::Transaction tx = make_envelope(final_fee);
        auto attach_rc = ops::AttachTransferInputBundle(tx, note.leaf_index,
                                                        final_fee, *wm);
        if (attach_rc.status != ops::OpStatus::Ok) {
            result["error"] = "attach_transfer_failed";
            result["error_message"] = attach_rc.error;
            return result;
        }
        auto submit = mempool_service->mempool().submitTransaction(
            tx, "rpc:wallet.transfer", true);
        if (!submit.accepted()) {
            result["error"] = "mempool_rejected";
            result["reject_code"] = TxRejectCodeToString(submit.code);
            result["reject_reason"] = submit.message;
            result["txid"] = tx.GetTxid().AsUint256().GetHex();
            result["fee_autosized"] = fee_autosized;
            result["vsize"] = static_cast<int64_t>(tx.GetVirtualSize());
            return result;
        }
        result["status"]                = "transferred";
        result["wave"]                  = "3d";
        result["txid"]                  = tx.GetTxid().AsUint256().GetHex();
        result["spend_nullifier_hex"]   = HashToHex(attach_rc.spend_nullifier);
        result["spend_anchor_hex"]      = HashToHex(attach_rc.spend_anchor);
        result["spend_leaf_index"]      = static_cast<int64_t>(note.leaf_index);
        result["spend_value_una"]       = static_cast<int64_t>(note.value_una);
        result["out_commitment_hex"]    = HashToHex(attach_rc.out_commitment);
        result["out_value_una"]         = static_cast<int64_t>(attach_rc.out_value_una);
        result["fee_una"]               = static_cast<int64_t>(final_fee);
        result["fee_autosized"]         = fee_autosized;
        result["vsize"]                 = static_cast<int64_t>(tx.GetVirtualSize());
        result["bundle_bytes"]          = static_cast<int64_t>(attach_rc.bundle_bytes);
        return result;
    }

    // ── Multi-spend path, possibly with change. Selects notes covering
    // ── amount + fee. If `address` was supplied, route through the
    // ── addressed-transfer helper; otherwise create self-controlled notes.
    // Selection is a function of the fee; re-run after auto-sizing.
    std::vector<dinero::wallet::ShieldedNote> picks;
    uint64_t spend_sum = 0;
    std::vector<uint64_t> leaf_indices;
    auto select_for_fee = [&](uint64_t fee, bool autosized) -> bool {
        auto picks_opt = ops::SelectTransferNotesForValue(*wm, amount_una + fee);
        if (!picks_opt) {
            result["error"] = "insufficient_balance";
            result["error_message"] = autosized
                ? "available unspent confirmed shielded balance < amount + "
                  "size-based fee (" + std::to_string(fee) + " una)"
                : "available unspent confirmed shielded balance < amount + fee";
            return false;
        }
        picks = std::move(*picks_opt);
        spend_sum = 0;
        leaf_indices.clear();
        leaf_indices.reserve(picks.size());
        for (const auto& n : picks) {
            spend_sum += n.value_una;
            leaf_indices.push_back(n.leaf_index);
        }
        return true;
    };
    if (!select_for_fee(fee_una, false)) return result;

    if (!recipient_address.empty()) {
        // ── Addressed transfer: route the requested amount to recipient.
        // Issue #273 two-pass sizing (see Wave 3d path above).
        uint64_t final_fee = fee_una;
        bool fee_autosized = false;
        if (!fee_explicit) {
            dinero::Transaction probe = make_envelope(fee_una);
            auto probe_rc = ops::AttachAddressedTransferInputBundle(
                probe, leaf_indices, recipient_address, amount_una, fee_una, *wm,
                recipient_memo.empty() ? nullptr : &recipient_memo,
                /*persist=*/false);
            if (probe_rc.status != ops::OpStatus::Ok) {
                result["error"] = "attach_transfer_failed";
                result["error_message"] = probe_rc.error;
                return result;
            }
            const uint64_t required = ops::RequiredFeeForTx(probe, min_fee_rate);
            if (required > final_fee) final_fee = required;
            fee_autosized = true;
            if (final_fee != fee_una && !select_for_fee(final_fee, true)) {
                return result;
            }
        }
        const uint64_t change = spend_sum - amount_una - final_fee;
        Json spend_indices = din::arr();
        Json spend_values  = din::arr();
        for (const auto& n : picks) {
            spend_indices.append(static_cast<int64_t>(n.leaf_index));
            spend_values.append(static_cast<int64_t>(n.value_una));
        }

        dinero::Transaction tx = make_envelope(final_fee);
        auto attach_rc = ops::AttachAddressedTransferInputBundle(
            tx, leaf_indices, recipient_address, amount_una, final_fee, *wm,
            recipient_memo.empty() ? nullptr : &recipient_memo);
        if (attach_rc.status != ops::OpStatus::Ok) {
            result["error"] = "attach_transfer_failed";
            result["error_message"] = attach_rc.error;
            return result;
        }
        auto submit = mempool_service->mempool().submitTransaction(
            tx, "rpc:wallet.transfer", true);
        if (!submit.accepted()) {
            result["error"] = "mempool_rejected";
            result["reject_code"] = TxRejectCodeToString(submit.code);
            result["reject_reason"] = submit.message;
            result["txid"] = tx.GetTxid().AsUint256().GetHex();
            result["fee_autosized"] = fee_autosized;
            result["vsize"] = static_cast<int64_t>(tx.GetVirtualSize());
            return result;
        }
        Json spend_nulls = din::arr();
        for (const auto& n : attach_rc.spend_nullifiers) spend_nulls.append(HashToHex(n));
        result["status"]                = "transferred";
        result["wave"]                  = "3d-addressed";
        result["txid"]                  = tx.GetTxid().AsUint256().GetHex();
        result["recipient_address"]     = recipient_address;
        result["recipient_commitment"]  = HashToHex(attach_rc.recipient_commitment);
        result["amount_una"]            = static_cast<int64_t>(amount_una);
        result["fee_una"]               = static_cast<int64_t>(final_fee);
        result["fee_autosized"]         = fee_autosized;
        result["vsize"]                 = static_cast<int64_t>(tx.GetVirtualSize());
        result["change_una"]            = static_cast<int64_t>(change);
        result["had_change"]            = attach_rc.had_change;
        if (attach_rc.had_change) {
            result["change_commitment"] = HashToHex(attach_rc.change_commitment);
        }
        result["spend_count"]           = static_cast<int64_t>(picks.size());
        result["spend_leaf_indices"]    = spend_indices;
        result["spend_values_una"]      = spend_values;
        result["spend_nullifiers"]      = spend_nulls;
        result["bundle_bytes"]          = static_cast<int64_t>(attach_rc.bundle_bytes);
        return result;
    }

    // ── Self-controlled multi-transfer (Wave 3e). Issue #273 two-pass
    // sizing: output values depend on the fee (change shrinks as the fee
    // grows), so recompute them per pass.
    auto output_values_for_fee = [&](uint64_t fee) -> std::vector<uint64_t> {
        std::vector<uint64_t> output_values{amount_una};
        const uint64_t change = spend_sum - amount_una - fee;
        if (change > 0) {
            output_values.push_back(change);
        }
        return output_values;
    };

    uint64_t final_fee = fee_una;
    bool fee_autosized = false;
    if (!fee_explicit) {
        dinero::Transaction probe = make_envelope(fee_una);
        auto probe_rc = ops::AttachMultiTransferInputBundle(
            probe, leaf_indices, output_values_for_fee(fee_una), fee_una, *wm,
            /*persist=*/false);
        if (probe_rc.status != ops::OpStatus::Ok) {
            result["error"] = "attach_transfer_failed";
            result["error_message"] = probe_rc.error;
            return result;
        }
        const uint64_t required = ops::RequiredFeeForTx(probe, min_fee_rate);
        if (required > final_fee) final_fee = required;
        fee_autosized = true;
        if (final_fee != fee_una && !select_for_fee(final_fee, true)) {
            return result;
        }
    }
    const uint64_t change = spend_sum - amount_una - final_fee;
    Json spend_indices = din::arr();
    Json spend_values  = din::arr();
    for (const auto& n : picks) {
        spend_indices.append(static_cast<int64_t>(n.leaf_index));
        spend_values.append(static_cast<int64_t>(n.value_una));
    }

    const std::vector<uint64_t> output_values = output_values_for_fee(final_fee);

    dinero::Transaction tx = make_envelope(final_fee);
    auto attach_rc = ops::AttachMultiTransferInputBundle(tx, leaf_indices,
                                                        output_values, final_fee, *wm);
    if (attach_rc.status != ops::OpStatus::Ok) {
        result["error"] = "attach_transfer_failed";
        result["error_message"] = attach_rc.error;
        return result;
    }
    auto submit = mempool_service->mempool().submitTransaction(
        tx, "rpc:wallet.transfer", true);
    if (!submit.accepted()) {
        result["error"] = "mempool_rejected";
        result["reject_code"] = TxRejectCodeToString(submit.code);
        result["reject_reason"] = submit.message;
        result["txid"] = tx.GetTxid().AsUint256().GetHex();
        result["fee_autosized"] = fee_autosized;
        result["vsize"] = static_cast<int64_t>(tx.GetVirtualSize());
        return result;
    }

    Json out_commits = din::arr();
    Json out_values  = din::arr();
    for (const auto& m : attach_rc.outputs) {
        out_commits.append(HashToHex(m.commitment));
        out_values.append(static_cast<int64_t>(m.value_una));
    }
    Json spend_nulls = din::arr();
    for (const auto& n : attach_rc.spend_nullifiers) {
        spend_nulls.append(HashToHex(n));
    }
    result["status"]              = "transferred";
    result["wave"]                = "3e";
    result["txid"]                = tx.GetTxid().AsUint256().GetHex();
    result["spend_count"]         = static_cast<int64_t>(picks.size());
    result["spend_leaf_indices"]  = spend_indices;
    result["spend_values_una"]    = spend_values;
    result["spend_nullifiers"]    = spend_nulls;
    result["out_count"]           = static_cast<int64_t>(output_values.size());
    result["out_commitments_hex"] = out_commits;
    result["out_values_una"]      = out_values;
    result["amount_una"]          = static_cast<int64_t>(amount_una);
    result["change_una"]          = static_cast<int64_t>(change);
    result["fee_una"]             = static_cast<int64_t>(final_fee);
    result["fee_autosized"]       = fee_autosized;
    result["vsize"]               = static_cast<int64_t>(tx.GetVirtualSize());
    result["bundle_bytes"]        = static_cast<int64_t>(attach_rc.bundle_bytes);
    return result;
}

// ---------------------------------------------------------------------------
// wallet.shieldedbalance
// ---------------------------------------------------------------------------
Json rpc_wallet_shieldedbalance(const ExecutionContext& ctx, const Json& params) {
    (void)params;
    Json result;
    if (RejectIfShieldedNotActive(result)) return result;
    auto* wm = AcquireWallet(ctx, result, false);
    if (!wm) return result;

    std::string init_error;
    if (!ops::EnsureWalletRuntime(*wm, &init_error)) {
        result["error"] = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return result;
    }

    uint64_t bal = ops::GetShieldedBalance(*wm);
    auto notes = ops::ListShieldedNotes(*wm, true);
    int64_t pending_count = 0;
    int64_t confirmed_count = 0;
    for (const auto& note : notes) {
        if (note.confirmed && !note.spent) {
            ++confirmed_count;
        } else if (!note.confirmed && !note.spent) {
            ++pending_count;
        }
    }

    result["balance_una"]  = static_cast<int64_t>(bal);
    result["balance_din"]  = static_cast<double>(bal) / 1e8;
    result["note_count"]   = confirmed_count;
    result["pending_note_count"] = pending_count;
    result["tree_size"]    = static_cast<int64_t>(ops::GetShieldedTreeSize(*wm));

    return result;
}

// ---------------------------------------------------------------------------
// wallet.listshielded
// ---------------------------------------------------------------------------
Json rpc_wallet_listshielded(const ExecutionContext& ctx, const Json& params) {
    (void)params;
    Json result;
    if (RejectIfShieldedNotActive(result)) return result;
    auto* wm = AcquireWallet(ctx, result, false);
    if (!wm) return result;

    std::string init_error;
    if (!ops::EnsureWalletRuntime(*wm, &init_error)) {
        result["error"] = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return result;
    }

    auto notes = ops::ListShieldedNotes(*wm, true);
    Json arr = din::arr();

    for (const auto& n : notes) {
        Json note;
        note["confirmed"]  = n.confirmed;
        note["spent"]      = n.spent;
        if (n.confirmed) {
            note["leaf_index"] = static_cast<int64_t>(n.leaf_index);
            std::string nf_hex;
            for (uint8_t b : n.nullifier) {
                char buf[3];
                snprintf(buf, sizeof(buf), "%02x", b);
                nf_hex += buf;
            }
            note["nullifier_hex"] = nf_hex;
            note["confirmed_height"] = static_cast<int64_t>(n.confirmed_height);
        }
        note["value_una"]  = static_cast<int64_t>(n.value_una);
        note["value_din"]  = static_cast<double>(n.value_una) / 1e8;
        note["created_height"] = static_cast<int64_t>(n.created_height);
        if (n.spent) {
            note["spent_height"] = static_cast<int64_t>(n.spent_height);
        }
        std::string cm_hex;
        for (uint8_t b : n.commitment) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", b);
            cm_hex += buf;
        }
        note["commitment_hex"] = cm_hex;
        arr.append(note);
    }

    result["notes"] = arr;
    result["count"] = static_cast<int64_t>(notes.size());
    return result;
}

// ---------------------------------------------------------------------------
// wallet.getshieldedaddress
//
// Params (object): { "account": <int|optional, default 0>,
//                    "j":       <int|optional, default 0> }
// Params (array):  [ account?, j? ]
//
// Returns the canonical shielded receive address for the wallet at
// (account, j) on the active network's HRP. Pure derivation — does not
// reserve `j` (caller manages diversifier-index issuance until Phase 5
// adds persistent issuance tracking). Available pre-activation: the
// activation gate only restricts on-chain transactions, not local
// address derivation.
// ---------------------------------------------------------------------------
namespace shielded_wallet = ::dinero::wallet::shielded;

const char* HrpForActiveChain() {
    switch (dinero::GetActiveChain()) {
        case dinero::Chain::MAINNET: return shielded_wallet::kHrpMainnet;
        case dinero::Chain::TESTNET: return shielded_wallet::kHrpTestnet;
        case dinero::Chain::REGTEST: return shielded_wallet::kHrpRegtest;
    }
    return shielded_wallet::kHrpMainnet;
}

Json rpc_wallet_getshieldedaddress(const ExecutionContext& ctx, const Json& params) {
    Json result;
    auto* wm = AcquireWallet(ctx, result, false);
    if (!wm) return result;

    uint32_t account = 0;
    uint64_t j       = 0;
    if (params.isObject()) {
        if (params.isMember("account")) {
            account = static_cast<uint32_t>(params["account"].asInt64());
        }
        if (params.isMember("j")) {
            j = static_cast<uint64_t>(params["j"].asInt64());
        }
    } else if (params.isArray()) {
        if (params.size() >= 1) account = static_cast<uint32_t>(params[0].asInt64());
        if (params.size() >= 2) j       = static_cast<uint64_t>(params[1].asInt64());
    }

    const std::string hrp = HrpForActiveChain();
    if (wm->isWalletLocked()) {
        if (LoadCachedShieldedAddress(*wm, hrp, account, j, result)) {
            return result;
        }
        result["error"]         = "wallet_locked_receive_cache_miss";
        result["error_message"] = "unlock once to derive and cache this shielded receive address";
        return result;
    }

    auto seed_opt = wm->GetMasterSeed();
    if (!seed_opt) {
        result["error"]         = "no_master_seed";
        result["error_message"] = "wallet has no master seed (not initialised)";
        return result;
    }
    if (seed_opt->size() != 64) {
        result["error"]         = "bad_seed_length";
        result["error_message"] = "shielded derivation requires 64-byte BIP32 seed";
        OPENSSL_cleanse(seed_opt->data(), seed_opt->size());
        return result;
    }

    shielded_wallet::ShieldedAccountKeys keys;
    try {
        keys = shielded_wallet::DeriveShieldedAccount(seed_opt->data(),
                                                      seed_opt->size(),
                                                      account);
    } catch (const std::exception& e) {
        OPENSSL_cleanse(seed_opt->data(), seed_opt->size());
        result["error"]         = "derive_failed";
        result["error_message"] = e.what();
        return result;
    }
    OPENSSL_cleanse(seed_opt->data(), seed_opt->size());

    shielded_wallet::DiversifiedAddress addr;
    try {
        addr = shielded_wallet::DeriveDiversifiedAddress(keys, j, hrp);
    } catch (const std::exception& e) {
        result["error"]         = "address_failed";
        result["error_message"] = e.what();
        return result;
    }

    std::string d_hex;
    d_hex.reserve(22);
    for (uint8_t b : addr.d) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", b);
        d_hex += buf;
    }

    result["address"]      = addr.address;
    result["hrp"]          = hrp;
    result["account"]      = static_cast<int64_t>(account);
    result["j"]            = static_cast<int64_t>(j);
    result["d_hex"]        = d_hex;
    const std::string pk_d_hex = HashToHex(addr.pk_d);
    result["pk_d_hex"]     = pk_d_hex;
    result["cached"]       = false;
    StoreCachedShieldedAddress(*wm, hrp, account, j, addr.address, d_hex,
                               pk_d_hex);
    return result;
}

} // close anonymous namespace

// ---------------------------------------------------------------------------
// Registration — external linkage (called from rpc_context_wiring.cpp)
// ---------------------------------------------------------------------------
void registerShieldedWalletMethods() {
    g_rpcRegistry.registerHandler("wallet.shield",
                                  rpc_wallet_shield,
                                  RegisterMode::Overwrite,
                                  "v7-shielded");
    g_rpcRegistry.registerHandler("wallet.unshield",
                                  rpc_wallet_unshield,
                                  RegisterMode::Overwrite,
                                  "v7-shielded");
    g_rpcRegistry.registerHandler("wallet.transfer",
                                  rpc_wallet_transfer,
                                  RegisterMode::Overwrite,
                                  "v7-shielded");
    g_rpcRegistry.registerHandler("wallet.shieldedbalance",
                                  rpc_wallet_shieldedbalance,
                                  RegisterMode::Overwrite,
                                  "v7-shielded");
    g_rpcRegistry.registerHandler("wallet.listshielded",
                                  rpc_wallet_listshielded,
                                  RegisterMode::Overwrite,
                                  "v7-shielded");
    g_rpcRegistry.registerHandler("wallet.getshieldedaddress",
                                  rpc_wallet_getshieldedaddress,
                                  RegisterMode::Overwrite,
                                  "v7-shielded");
}
