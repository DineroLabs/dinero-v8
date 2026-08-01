/**
 * Wallet RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file migrates all 39 wallet RPC methods from legacy globals to DaemonContext.
 * Compare with methods_wallet.cpp to see the difference.
 *
 * OLD PATTERN (legacy):
 *   extern std::unique_ptr<WalletManager> g_wallet_manager;
 *   double balance = dinero::legacy::g_wallet_manager()->getBalance().confirmed;
 *
 * NEW PATTERN (context-aware):
 *   auto wallet = ctx.daemon->wallet;
 *   double balance = wallet->get().getBalance().confirmed;
 *
 * Benefits:
 * - No dependency on global variables
 * - Testable with mock wallet services
 * - Clear dependency tracking
 * - Thread-safe service access
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "rpc/proof_bundle_consistency.h"
#include "daemon/daemon_context.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/interfaces/tx_ingress.h"
#include "wallet/wallet_manager.h"
#include "wallet/hd_wallet.h"
#include "wallet/transaction_builder.h"  // Phase 33: Transaction building
#include "consensus/utreexo_maturity_leaf_activation.h"
#include <iostream>  // For std::cerr debug logging
#include "wallet/unsigned_tx_builder.h"   // v0.14.0: Unsigned TX construction
#include "wallet/transaction_signer.h"    // v0.14.0: Transaction signing (Taproot, PSBT, HW wallet ready)
#include "wallet/wallet_key_provider.h"   // Phase 10: hybrid ECDSA + v7 P2MR provider
#include "wallet/v7_p2mr_store.h"         // Phase 10: P2MR SQLite store
#include "consensus/pq/p2mr_consensus.h"  // Phase 10: IsP2MRScript
#include "consensus/covenants.h"          // PrecomputedTransactionData
#include "consensus/script_validation.h"  // canonical completion validation
#include "consensus/script_interpreter.h" // Phase 10: SignatureHashTaproot, ScriptExecutionContext
#include "wallet/p2mr_address.h"          // Phase 10: DecodeP2MRAddress for address validation
#include "address/addr_codec.h"           // Taproot/bech32m-aware validateaddress decode
#include <openssl/crypto.h>               // Phase 10: OPENSSL_cleanse
#include <cstring>                        // Phase 10: std::memcpy for master key stamp
#include "wallet/coin_selection.h"        // Phase 1.1: Frozen coin selection engine
#include "wallet/canonical_wallet_utxo.h" // Phase M.3: Canonical UTXO type
#include "wallet/transaction.h"           // STEP 2: Transaction construction
#include "mempool/mempool.h"              // STEP 2: Mempool policy testing
#include "storage/archival_block_reader.h"
#include "mempool/fee_estimator.h"        // Phase 35.1: Fee introspection
#include "external/bech32/bech32.hpp"     // STEP 2: Address decoding
#include "storage/chain_db.h"             // STEP 2: ChainDB for UTXO lookups
#include "storage/chain_direct.h"         // For GetBlockHash()
#include "primitives/uint256.h"           // Phase M.0: uint256 type
#include "consensus/outpoint.h"           // Phase M.0: OutPoint type
#include "consensus/coin_type.h"

using dinero::uint256;                    // Phase M.0: Make uint256 available without namespace prefix
using dinero::OutPoint;                   // Phase M.0: Make OutPoint available without namespace prefix
#include "wallet/psbt.h"                  // Phase 33: PSBT support
#include "common/logger.h"
#include "common/ilogger.h"
#include "daemon/rpc/wallet_gui_handlers.h"
#include "daemon/rpc/wallet_import_handlers.h"  // Phase W.1.1: For RpcImportMnemonic
#include "src/core/rpc/wallet_descriptor_rpc_handlers.h"  // Phase 1: Descriptor wallet RPCs
#include "address/addr_codec.h"
#include "consensus/chainparams.h"
#include "consensus/utreexo_accumulator.h"
#include "rpc/methods_utreexo.h"
#include "rpc/wallet_sync_aggregator.h"
#include "assets/asset_registry.h"       // Phase 33.6: Multi-asset balance
#include "wallet/wallet_sync_status.h"
#include "external/bech32/bech32.hpp"    // Bech32 address decoding
#include <memory>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <chrono>
#include <ctime>

// Forward declarations for cross-handler delegation.
din::Json rpc_context_wallet_dumpwallet(const ExecutionContext& ctx, const din::Json& params);
din::Json rpc_context_wallet_rescanblockchain(const ExecutionContext& ctx, const din::Json& params);
namespace din {
din::Json rpc_getaddressbalance(const ExecutionContext& ctx, const din::Json& params);
din::Json rpc_getaddresshistory(const ExecutionContext& ctx, const din::Json& params);
}

namespace {
static dinero::StatusOr<dinero::Block> ReadWalletRpcBlock(
    dinero::ChainstateService* chainstate,
    dinero::ChainDB* chain_db,
    dinero::BlockStorage* block_storage,
    const uint256& block_hash)
{
    if (chainstate) {
        return chainstate->getBlockByHash(block_hash);
    }
    if (chain_db) {
        return dinero::storage::ReadArchivalBlock(*chain_db, block_storage, block_hash);
    }
    return dinero::Status::Internal;
}

std::string BuildStandardDerivationPath(uint32_t purpose, int account, int change, int index) {
    return "m/" + std::to_string(purpose) + "'/" +
           std::to_string(dinero::consensus::DINERO_COIN_TYPE) + "'/" +
           std::to_string(account) + "'/" +
           std::to_string(change) + "/" +
           std::to_string(index);
}

// AddressRowIsTaproot — authoritative taproot test for an address row. Decodes
// the address as bech32m (witness version 1 = P2TR) instead of the old
// string-prefix shortcut (rfind("din1p")), which guessed the type from the
// encoding char — the same fragile pattern that previously mis-handled taproot.
// Honors the explicit stored type first, then falls back to a real decode.
bool AddressRowIsTaproot(const dinero::AddressRow& addr_row) {
    if (addr_row.type == "p2tr" || addr_row.type == "taproot") {
        return true;
    }
    const auto info = dinero::DecodeWitnessAddress(
        addr_row.address, dinero::HrpForActiveNetworkRef());
    return info.is_valid && info.witness_version == 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// RefuseIfSafeMode — safe-mode gate for all send/spend handlers.
// Spec: docs/design/assumeutxo-fatal-state-machine.md, Fatal Mismatch
// Semantics §3: "Stop accepting new foreground wallet/payment/mining decisions
// that depend on the assumed state."
//
// Returns true when safe mode is active and result["error"] has been set.
// MUST be invoked as the very first statement in each gated handler (before any
// wallet-availability, params, or balance checks) so the refusal fires even
// with no active wallet and zero balance.
// ─────────────────────────────────────────────────────────────────────────────
static bool RefuseIfSafeMode(const ExecutionContext& ctx, din::Json& result) {
    if (!ctx.daemon || !ctx.daemon->chainstate) return false;
    auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (cs && cs->IsInSafeMode()) {
        result["error"] = "disabled while node is in safe mode: " + cs->GetSafeModeReason();
        result["safe_mode"] = true;
        return true;
    }
    return false;
}

struct RescanReadiness {
    bool ok;
    std::string reason;
};

RescanReadiness CheckRescanReadiness(const std::shared_ptr<dinero::ChainstateService>& chainstate) {
    if (!chainstate) {
        return {false, "Chainstate service not available"};
    }

    if (chainstate->IsInSafeMode()) {
        return {false, "Rescan blocked while node is in safe mode: " + chainstate->GetSafeModeReason()};
    }

    auto* utxo_index = chainstate->utxoIndex();
    if (!utxo_index) {
        return {false, "Rescan blocked: UTXO index unavailable"};
    }

    const auto reorg_marker = utxo_index->GetMetadata("reorg_in_progress");
    if (reorg_marker.has_value() && !reorg_marker->empty()) {
        return {
            false,
            "Rescan blocked: chain reorganization is currently in progress (marker=" + *reorg_marker + ")"
        };
    }

    const auto last_error = utxo_index->GetMetadata("activation_last_error");
    if (last_error.has_value() && !last_error->empty()) {
        uint64_t streak = 1;
        if (const auto streak_meta = utxo_index->GetMetadata("activation_failure_streak")) {
            try {
                streak = std::stoull(*streak_meta);
            } catch (...) {
                streak = 1;
            }
        }

        bool recent_failure = true;
        if (const auto ts_meta = utxo_index->GetMetadata("activation_last_error_time")) {
            try {
                const auto ts = static_cast<uint64_t>(std::stoull(*ts_meta));
                const auto now = static_cast<uint64_t>(std::time(nullptr));
                if (now >= ts) {
                    // Fail fast only for recent instability to avoid stale permanent blocks.
                    recent_failure = (now - ts) <= 600;
                }
            } catch (...) {
                recent_failure = true;
            }
        }

        if (recent_failure && streak > 0) {
            return {
                false,
                "Rescan blocked due to recent chain activation failure: " + *last_error +
                ". Resolve reorg/connect-tip instability first."
            };
        }
    }

    return {true, ""};
}

bool WalletUtxoIsPresentInLiveUtreexoForest(
    const dinero::WalletManager::WalletUTXO& utxo,
    const std::shared_ptr<dinero::ChainstateService>& chainstate,
    std::string* reason = nullptr)
{
    if (!chainstate) {
        return true;
    }

    auto* forest = chainstate->utreexoForest();
    if (!forest) {
        return true;
    }

    if (utxo.txid.empty() || utxo.script_pubkey.empty()) {
        if (reason) {
            *reason = "missing txid or scriptPubKey";
        }
        return false;
    }

    try {
        dinero::uint256 txid_u256 = dinero::uint256::FromHexUnsafe(utxo.txid);
        std::vector<uint8_t> script_pubkey;
        script_pubkey.reserve(utxo.script_pubkey.size() / 2);
        for (size_t i = 0; i + 1 < utxo.script_pubkey.size(); i += 2) {
            script_pubkey.push_back(static_cast<uint8_t>(
                std::stoi(utxo.script_pubkey.substr(i, 2), nullptr, 16)));
        }

        const auto leaf_hash = dinero::consensus::HashUTXOForCreationHeight(
            txid_u256,
            utxo.vout,
            static_cast<uint64_t>(utxo.amount_una),
            script_pubkey,
            utxo.height,
            utxo.is_coinbase);

        if (!forest->findLeafPosition(leaf_hash).has_value()) {
            if (reason) {
                *reason = "leaf missing from live Utreexo forest";
            }
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        if (reason) {
            *reason = std::string("leaf check failed: ") + e.what();
        }
        return false;
    }
}

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string NormalizeWalletTxType(const dinero::WalletManager::TransactionInfo& tx) {
    const std::string category = ToLowerCopy(tx.category);

    if (category == "shield" || category == "shielded") {
        return "shield";
    }
    if (category == "unshield" || category == "unshielded") {
        return "unshield";
    }
    if (tx.is_coinbase || category == "generate" || category == "mining" ||
        category == "coinbase" || category == "immature") {
        return "mined";
    }
    if (category == "confsend" || category == "send" || tx.amount < 0.0) {
        return "sent";
    }
    if (category == "confreceive" || category == "receive" || tx.amount >= 0.0) {
        return "received";
    }
    return category.empty() ? "unknown" : category;
}

std::string WalletCategoryToPrivacyFlow(const std::string& raw_category) {
    const std::string category = ToLowerCopy(raw_category);
    if (category == "shield" || category == "shielded") {
        return "shield";
    }
    if (category == "unshield" || category == "unshielded") {
        return "unshield";
    }
    if (category == "confsend" || category == "confreceive" || category == "ct_transfer") {
        return "ct_transfer";
    }
    return "transparent";
}

bool WalletCategoryHasConfidentialActivity(const std::string& raw_category) {
    return WalletCategoryToPrivacyFlow(raw_category) != "transparent";
}

void PopulateWalletHistoryMetadata(din::Json& tx_obj,
                                   const dinero::WalletManager::TransactionInfo& tx) {
    const std::string privacy_flow = WalletCategoryToPrivacyFlow(tx.category);
    tx_obj["privacy_flow"] = privacy_flow;
    tx_obj["classification"] = privacy_flow;
    tx_obj["has_confidential_activity"] = privacy_flow != "transparent";
    tx_obj["amount_hidden"] = false;
}

std::string BytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        stream << std::setw(2) << static_cast<int>(byte);
    }
    return stream.str();
}

void AppendWalletProofRootsSnapshot(din::Json& result,
                                    const dinero::consensus::UtreexoForest& forest) {
    const auto roots = forest.getRoots();
    const uint64_t num_leaves = forest.getNumLeaves();

    din::Json roots_array = din::arr();
    for (const auto& root : roots) {
        roots_array.append(BytesToHex(root));
    }

    result["stump_num_leaves"] = static_cast<Json::Int64>(num_leaves);
    result["stump_roots"] = roots_array;
    result["num_leaves"] = static_cast<Json::Int64>(num_leaves);
    result["num_roots"] = static_cast<Json::Int64>(roots.size());
    result["roots"] = roots_array;
}

template <typename OutputLike>
void PopulateWalletOutputDisplay(din::Json& output_obj, const OutputLike& output) {
    output_obj["is_confidential"] = output.is_confidential;
    output_obj["amount_hidden"] = output.is_confidential;
    if (output.is_confidential) {
        output_obj["display_amount"] = "confidential";
        output_obj["commitment"] = BytesToHex(output.commitment);
        output_obj["range_proof_bytes"] = static_cast<Json::UInt64>(output.range_proof.size());
        output_obj["nonce_bytes"] = static_cast<Json::UInt64>(output.nonce.size());
    } else {
        output_obj["value"] = static_cast<double>(output.value.GetUna()) / 1e8;
        output_obj["display_amount"] = static_cast<double>(output.value.GetUna()) / 1e8;
    }
}

bool ParseWalletTxTypeFilter(const din::Json& value, std::string& canonical_type) {
    if (!value.is<std::string>()) {
        return false;
    }

    const std::string normalized = ToLowerCopy(value.as<std::string>());
    if (normalized.empty() || normalized == "all" || normalized == "*" || normalized == "any") {
        canonical_type = "all";
        return true;
    }
    if (normalized == "sent" || normalized == "send") {
        canonical_type = "sent";
        return true;
    }
    if (normalized == "received" || normalized == "receive") {
        canonical_type = "received";
        return true;
    }
    if (normalized == "mined" || normalized == "mine" || normalized == "mining" ||
        normalized == "generate" || normalized == "generated" || normalized == "coinbase" ||
        normalized == "immature") {
        canonical_type = "mined";
        return true;
    }
    if (normalized == "shield" || normalized == "shielded") {
        canonical_type = "shield";
        return true;
    }
    if (normalized == "unshield" || normalized == "unshielded") {
        canonical_type = "unshield";
        return true;
    }

    return false;
}

bool ParseVoutParam(const din::Json& value, uint32_t& out_vout) {
    if (value.isUInt()) {
        out_vout = value.asUInt();
        return true;
    }
    if (value.isInt()) {
        int v = value.asInt();
        if (v >= 0) {
            out_vout = static_cast<uint32_t>(v);
            return true;
        }
    }
    return false;
}

bool ParseUtxoRefParams(const din::Json& params, std::string& txid, uint32_t& vout, size_t& consumed, std::string& error) {
    consumed = 0;
    error.clear();

    if (params.empty() || !params[0].is<std::string>()) {
        error = "Invalid UTXO identifier";
        return false;
    }

    std::string first_param = params[0].as<std::string>();
    size_t colon_pos = first_param.find(':');

    if (colon_pos != std::string::npos) {
        try {
            txid = first_param.substr(0, colon_pos);
            vout = static_cast<uint32_t>(std::stoul(first_param.substr(colon_pos + 1)));
            consumed = 1;
            return true;
        } catch (const std::exception&) {
            error = "Invalid UTXO identifier";
            return false;
        }
    }

    if (params.size() < 2 || !ParseVoutParam(params[1], vout)) {
        error = "Invalid UTXO identifier";
        return false;
    }

    txid = first_param;
    consumed = 2;
    return true;
}

std::string ExtractRpcErrorMessage(const din::Json& rpc_result) {
    if (!rpc_result.isMember("error")) {
        return "unknown error";
    }

    const din::Json& error = rpc_result["error"];
    if (error.isString()) {
        return error.asString();
    }
    if (error.isObject() && error.isMember("message") && error["message"].isString()) {
        return error["message"].asString();
    }
    return error.toStyledString();
}

const din::Json* ResolveProofObject(const din::Json& value) {
    if (!value.isObject()) {
        return nullptr;
    }
    if (value.isMember("utreexo_proof") && value["utreexo_proof"].isObject()) {
        return &value["utreexo_proof"];
    }
    if (value.isMember("proof") && value["proof"].isObject()) {
        return &value["proof"];
    }
    return &value;
}

bool IsBatchProofShape(const din::Json& proof_obj) {
    return proof_obj.isObject() &&
           proof_obj.isMember("siblings") &&
           proof_obj["siblings"].isArray() &&
           proof_obj.isMember("position") &&
           proof_obj.isMember("num_leaves");
}

std::string MakeUtxoKey(const std::string& txid, uint32_t vout) {
    return txid + ":" + std::to_string(vout);
}

uint64_t ParseUnsignedUnaField(const din::Json& value) {
    if (value.isUInt64()) {
        return value.asUInt64();
    }
    if (value.isUInt()) {
        return static_cast<uint64_t>(value.asUInt());
    }
    if (value.isInt64()) {
        return value.asInt64() > 0 ? static_cast<uint64_t>(value.asInt64()) : 0;
    }
    if (value.isInt()) {
        return value.asInt() > 0 ? static_cast<uint64_t>(value.asInt()) : 0;
    }
    if (value.isDouble()) {
        return value.asDouble() > 0.0 ? static_cast<uint64_t>(value.asDouble()) : 0;
    }
    if (value.isString()) {
        try {
            return static_cast<uint64_t>(std::stoull(value.asString()));
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

int64_t ParseSignedUnaField(const din::Json& value) {
    if (value.isInt64()) {
        return value.asInt64();
    }
    if (value.isInt()) {
        return static_cast<int64_t>(value.asInt());
    }
    if (value.isUInt64()) {
        const auto unsigned_value = value.asUInt64();
        return unsigned_value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            ? std::numeric_limits<int64_t>::max()
            : static_cast<int64_t>(unsigned_value);
    }
    if (value.isUInt()) {
        return static_cast<int64_t>(value.asUInt());
    }
    if (value.isDouble()) {
        return static_cast<int64_t>(value.asDouble());
    }
    if (value.isString()) {
        try {
            return std::stoll(value.asString());
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

uint64_t ApplySignedUnaDelta(uint64_t base, int64_t delta) {
    if (delta >= 0) {
        return base + static_cast<uint64_t>(delta);
    }

    const auto abs_delta = static_cast<uint64_t>(-(delta + 1)) + 1;
    return base >= abs_delta ? (base - abs_delta) : 0;
}

struct SnapshotScopedAddressEntry {
    std::string address;
    din::Json metadata;
};

std::vector<SnapshotScopedAddressEntry> ParseScopedSnapshotAddresses(const din::Json& params) {
    std::vector<SnapshotScopedAddressEntry> results;
    std::set<std::string> seen;

    if (!params.isObject() || !params.isMember("addresses") || !params["addresses"].isArray()) {
        return results;
    }

    for (const auto& entry : params["addresses"]) {
        std::string address;
        din::Json metadata;

        if (entry.isString()) {
            address = entry.asString();
            metadata["address"] = address;
        } else if (entry.isObject() && entry.isMember("address") && entry["address"].isString()) {
            address = entry["address"].asString();
            metadata = entry;
        } else {
            continue;
        }

        if (address.empty()) {
            continue;
        }

        const auto normalized = ToLowerCopy(address);
        if (!seen.insert(normalized).second) {
            continue;
        }

        if (!metadata.isMember("address")) {
            metadata["address"] = address;
        }
        results.push_back({address, metadata});
    }

    return results;
}
}  // namespace

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE WALLET RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

/**
 * wallet.getbalance - Get wallet balance
 *
 * OLD: dinero::legacy::g_wallet_manager()->getBalance()
 * NEW: ctx.daemon->wallet->get().getBalance()
 */
din::Json rpc_context_wallet_getbalance(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    if (!wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        auto& mgr = wallet_service->get();

        // Sync gate: wait for wallet worker to catch up to chain tip.
        if (ctx.daemon->chainstate) {
            auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
            if (cs) {
                mgr.WaitForHeight(cs->getBlockHeight(), std::chrono::milliseconds(5000));
            }
        }

        auto balance = mgr.getBalance();

        // Phase 35.3: Compute locked balance
        double locked_balance = mgr.getLockedBalance();

        // Phase 35: Enhanced balance breakdown
        result["confirmed"] = balance.confirmed;
        result["unconfirmed"] = balance.unconfirmed;
        result["immature"] = balance.immature;
        result["locked"] = locked_balance;  // Phase 35.3: Actual locked balance
        result["total"] = balance.total;
        result["spendable"] = balance.spendable;
        result["utxo_count"] = balance.utxo_count;

        // PQ health ratio: what fraction of the wallet's confirmed balance
        // is held in quantum-resistant (P2MR) UTXOs. Read-only metric —
        // no enforcement, just visibility for the user or a future UI.
        {
            auto utxos = mgr.listUnspentUTXOs(1, 9999999);
            int64_t p2mr_una = 0;
            int64_t total_una = 0;
            for (const auto& u : utxos) {
                if (!u.spendable || !u.is_mature) continue;
                total_una += u.amount_una;
                const auto& spk = u.script_pubkey;
                if (spk.length() == 68 && spk.rfind("5320", 0) == 0) {
                    p2mr_una += u.amount_una;
                }
            }
            double pq_ratio = (total_una > 0)
                ? static_cast<double>(p2mr_una) / static_cast<double>(total_una)
                : 0.0;
            result["pq_ratio"]        = pq_ratio;
            result["pq_balance_din"]  = static_cast<double>(p2mr_una) / 1e8;
            result["pq_balance_una"]  = static_cast<int64_t>(p2mr_una);
        }

        // Phase 35: Detailed breakdown for verification
        din::Json breakdown;
        breakdown["spendable"] = balance.spendable;  // confirmed - locked
        breakdown["pending"] = balance.unconfirmed;  // unconfirmed
        breakdown["unspendable"] = balance.immature + locked_balance;  // immature + locked
        result["breakdown"] = breakdown;

        // Phase F: Add confidential balance if chainstate/UTXOIndex available
        if (ctx.daemon->chainstate) {
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
            if (chainstate && chainstate->utxoIndex()) {
                try {
                    int64_t conf_balance_una = chainstate->utxoIndex()->GetConfidentialBalance().GetInt64();  // Phase M.6.2
                    double conf_balance_din = static_cast<double>(conf_balance_una) / 1e8;
                    int64_t total_with_conf_una = static_cast<int64_t>(balance.total * 1e8) + conf_balance_una;
                    double total_with_conf = static_cast<double>(total_with_conf_una) / 1e8;

                    result["confidential"] = conf_balance_din;
                    result["total_with_confidential"] = total_with_conf;
                } catch (const std::exception& e) {
                    // Silently skip confidential balance if error occurs
                    // Transparent balance still works
                }
            }
        }

        // Phase 33.6: Add multi-asset balances
        try {
            auto& asset_registry = dinero::assets::GetAssetRegistry();
            auto addresses = wallet_service->get().getWalletAddresses();

            // Aggregate asset balances across all wallet addresses
            std::map<std::string, uint64_t> asset_totals;  // asset_id_hex -> total balance

            for (const auto& addr : addresses) {
                auto asset_balances = asset_registry.getAddressBalances(addr);
                for (const auto& ab : asset_balances) {
                    std::string asset_id_hex = dinero::assets::AssetIDToHex(ab.asset_id);
                    asset_totals[asset_id_hex] += ab.confirmed_balance;
                }
            }

            // Add assets to result
            if (!asset_totals.empty()) {
                din::Json assets_obj;
                for (const auto& [asset_id, amount] : asset_totals) {
                    // Get asset metadata for display
                    auto id_opt = dinero::assets::AssetIDFromHex(asset_id);
                    if (!id_opt.has_value()) continue;
                    auto summary = asset_registry.getAssetSummary(*id_opt);

                    din::Json asset_entry;
                    asset_entry["balance"] = static_cast<double>(amount);
                    if (summary.has_value()) {
                        asset_entry["name"] = summary->metadata.name;
                        asset_entry["ticker"] = summary->metadata.ticker;
                        asset_entry["decimals"] = static_cast<int>(summary->metadata.decimals);
                        // Format balance with decimals
                        double formatted = static_cast<double>(amount) /
                            std::pow(10, summary->metadata.decimals);
                        asset_entry["formatted"] = formatted;
                    }
                    assets_obj[asset_id] = asset_entry;
                }
                result["assets"] = assets_obj;
                result["asset_count"] = static_cast<int>(asset_totals.size());
            }
        } catch (const std::exception& e) {
            // Asset registry may not be initialized - silently skip
            // DIN balance still works
        }

        result["rpc_schema"] = "din.wallet.v1";
    } catch (const std::exception& e) {
        result["error"] = std::string("Balance query error: ") + e.what();
    }

    return result;
}

/**
 * wallet.getwalletinfo - Get wallet status information
 *
 * Phase 35.1: Wallet Introspection & UX
 *
 * Returns comprehensive wallet state aggregation:
 * - Wallet name
 * - Balance breakdown (confirmed, unconfirmed, immature, locked)
 * - Transaction count
 * - Address count
 * - HD wallet status
 *
 * Read-only operation - no behavioral changes.
 */
din::Json rpc_context_wallet_getwalletinfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    if (!wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        auto& mgr = wallet_service->get();

        // Wallet name
        result["walletname"] = mgr.current();

        // Balance aggregation (read-only)
        auto balance = mgr.getBalance();
        result["balance"] = balance.total;
        result["transparent_balance"] = balance.total;
        result["confirmed_balance"] = balance.confirmed;
        result["unconfirmed_balance"] = balance.unconfirmed;
        result["immature_balance"] = balance.immature;
        result["confidential_balance"] = 0.0;
        result["total_with_confidential"] = balance.total;

        if (ctx.daemon->chainstate) {
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
            if (chainstate && chainstate->utxoIndex()) {
                try {
                    const int64_t conf_balance_una = chainstate->utxoIndex()->GetConfidentialBalance().GetInt64();
                    const double conf_balance_din = static_cast<double>(conf_balance_una) / 1e8;
                    result["confidential_balance"] = conf_balance_din;
                    result["total_with_confidential"] = balance.total + conf_balance_din;
                } catch (const std::exception&) {
                    // Keep wallet info available even if the confidential index is unavailable.
                }
            }
        }

        // Locked balance (Phase 35.3)
        double locked_balance = mgr.getLockedBalance();
        result["locked_balance"] = locked_balance;

        // Spendable balance
        result["spendable_balance"] = balance.spendable;

        // Transaction count (read-only - no limit to get full count)
        auto tx_history = mgr.getTransactionHistory(999999, 0);
        result["txcount"] = static_cast<int>(tx_history.size());

        // Address count (read-only)
        auto addresses = mgr.listAddresses(false);  // Don't need labels for count
        result["address_count"] = static_cast<int>(addresses.size());

        // UTXO count
        result["utxo_count"] = balance.utxo_count;
        result["immature_utxo_count"] = balance.immature_utxo_count;

        // HD wallet status (read-only check)
        result["hd_enabled"] = (mgr.getHDWallet() != nullptr);

        // Primary transparent address: first from address list
        try {
            auto addrs = mgr.listAddresses(false);
            if (!addrs.empty()) {
                result["primary_address"] = addrs[0].address;
            }
        } catch (const std::exception& e) {
            dinero::g_logger.warning("[wallet.getinfo] listAddresses failed: " + std::string(e.what()));
        }

        // Wallet encryption status (read-only)
        result["encrypted"] = mgr.isWalletEncrypted();
        result["locked"] = mgr.isWalletLocked();
        result["unlocked"] = !mgr.isWalletLocked();

        // Scanning status (future: rescan progress tracking)
        result["scanning"] = false;

        // Wallet version/schema marker
        result["walletversion"] = 120000;  // Phase 1.1: Wallet introspection

        // Virtual keypool: HD wallet derives keys on demand
        result["keypoolsize"] = 1000;

    } catch (const std::exception& e) {
        result["error"] = std::string("Wallet info query error: ") + e.what();
    }

    return result;
}

/**
 * wallet.snapshot - Get a stable wallet state snapshot for light clients
 *
 * Returns a single read-only snapshot that aggregates:
 * - wallet identity and lock/encryption state
 * - sync progress
 * - balance breakdown
 * - current receive address and funded addresses
 * - recent typed history
 * - proof context (tip hash + current Utreexo commitment)
 *
 * Parameters (optional object or array):
 *   {
 *     "history_count": 20,
 *     "funded_address_limit": 20
 *   }
 */
din::Json rpc_context_wallet_snapshot(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    result["rpc_schema"] = "din.wallet.snapshot.v1";

    int history_count = 20;
    int funded_address_limit = 20;
    if (params.isObject()) {
        if (params.isMember("history_count") && params["history_count"].isInt()) {
            history_count = params["history_count"].asInt();
        }
        if (params.isMember("funded_address_limit") && params["funded_address_limit"].isInt()) {
            funded_address_limit = params["funded_address_limit"].asInt();
        }
    } else if (params.isArray()) {
        if (params.size() >= 1 && params[0].isInt()) {
            history_count = params[0].asInt();
        }
        if (params.size() >= 2 && params[1].isInt()) {
            funded_address_limit = params[1].asInt();
        }
    }

    history_count = std::max(1, std::min(history_count, 200));
    funded_address_limit = std::max(1, std::min(funded_address_limit, 200));
    const auto scoped_addresses = ParseScopedSnapshotAddresses(params);

    din::Json wallet_obj;
    wallet_obj["loaded"] = false;
    wallet_obj["name"] = "";
    wallet_obj["encrypted"] = false;
    wallet_obj["locked"] = false;
    wallet_obj["unlocked"] = false;
    wallet_obj["hd_enabled"] = false;
    wallet_obj["scope"] = scoped_addresses.empty() ? "wallet" : "address_index";

    din::Json sync_obj;
    try {
        const auto sync_status = dinero::BuildWalletSyncStatusFromContext(ctx);
        std::string phase = "unknown";
        switch (sync_status.phase) {
            case dinero::SyncPhase::IBD:
                phase = "ibd";
                break;
            case dinero::SyncPhase::CATCHING_UP:
                phase = "catching_up";
                break;
            case dinero::SyncPhase::STEADY_STATE:
                phase = "steady_state";
                break;
            default:
                break;
        }

        sync_obj["phase"] = phase;
        sync_obj["phase_name"] = sync_status.GetPhaseName();
        sync_obj["is_synced"] = sync_status.IsFullySynced();
        sync_obj["overall_progress"] = sync_status.overall_progress;
        sync_obj["overall_progress_percent"] = sync_status.overall_progress * 100.0;
        sync_obj["chain_height"] = static_cast<Json::UInt64>(sync_status.chain_height);
        sync_obj["headers_synced"] = static_cast<Json::UInt64>(sync_status.headers_synced);
        sync_obj["headers_total"] = static_cast<Json::UInt64>(sync_status.headers_total);
        sync_obj["blocks_synced"] = static_cast<Json::UInt64>(sync_status.blocks_synced);
        sync_obj["blocks_total"] = static_cast<Json::UInt64>(sync_status.blocks_total);
        sync_obj["wallet_scan_height"] = static_cast<Json::UInt64>(sync_status.wallet_scan_height);
        sync_obj["wallet_scan_progress"] = sync_status.wallet_scan_progress();
    } catch (const std::exception& e) {
        sync_obj["error"] = std::string("Failed to build sync snapshot: ") + e.what();
    }

    din::Json proof_context;
    std::string tip_hash;
    if (ctx.daemon->chainstate) {
        tip_hash = ctx.daemon->chainstate->getBestBlockHash();
        proof_context["tip_height"] = static_cast<Json::UInt64>(ctx.daemon->chainstate->getBlockHeight());
    } else {
        proof_context["tip_height"] = static_cast<Json::UInt64>(0);
    }
    proof_context["tip_hash"] = tip_hash;

    din::Json commitment_result = din::rpc_getutreexocommitment(ctx, din::arr());
    if (!commitment_result.isMember("error") &&
        commitment_result.isMember("commitment") &&
        commitment_result["commitment"].isString()) {
        proof_context["utreexo_root"] = commitment_result["commitment"].asString();
    } else {
        proof_context["utreexo_root"] = "";
    }
    proof_context["available"] =
        proof_context["tip_hash"].isString() &&
        !proof_context["tip_hash"].asString().empty() &&
        proof_context["utreexo_root"].isString() &&
        !proof_context["utreexo_root"].asString().empty();

    din::Json balances;
    balances["confirmed"] = 0.0;
    balances["unconfirmed"] = 0.0;
    balances["immature"] = 0.0;
    balances["locked"] = 0.0;
    balances["total"] = 0.0;
    balances["spendable"] = 0.0;
    balances["utxo_count"] = 0;

    din::Json balances_breakdown;
    balances_breakdown["spendable"] = 0.0;
    balances_breakdown["pending"] = 0.0;
    balances_breakdown["unspendable"] = 0.0;
    balances["breakdown"] = balances_breakdown;

    din::Json receive_obj;
    receive_obj["current_address"] = "";
    receive_obj["current_path"] = "";
    receive_obj["known_address_count"] = 0;
    receive_obj["funded_address_count"] = 0;

    din::Json addresses_obj;
    addresses_obj["funded"] = din::arr();

    din::Json history_obj;
    history_obj["count"] = 0;
    history_obj["items"] = din::arr();

    if (!scoped_addresses.empty()) {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"] = "Daemon/chainstate not available";
            return result;
        }
        auto* chain_db = ctx.daemon->chainstate->GetChainDB();
        if (!chain_db) {
            result["error"] = "ChainDB not initialized";
            return result;
        }

        if (params.isObject() &&
            params.isMember("current_address") &&
            params["current_address"].isString()) {
            receive_obj["current_address"] = params["current_address"].asString();
        }

        receive_obj["known_address_count"] = static_cast<Json::UInt64>(scoped_addresses.size());
        wallet_obj["address_count"] = static_cast<Json::UInt64>(scoped_addresses.size());

        int64_t aggregate_unconfirmed_una = 0;
        uint64_t aggregate_confirmed_una = 0;
        uint64_t aggregate_total_una = 0;
        uint64_t funded_count = 0;

        struct FundedAddressEntry {
            uint64_t total_una = 0;
            din::Json json;
        };

        std::vector<FundedAddressEntry> funded_entries;

        std::unordered_set<std::string> outgoing_txids;
        std::vector<din::Json> history_items;

        for (const auto& scoped_entry : scoped_addresses) {
            din::Json balance_params = din::arr();
            balance_params.append(scoped_entry.address);
            auto balance_response = din::rpc_getaddressbalance(ctx, balance_params);
            if (balance_response.isMember("error")) {
                result["error"] = "Failed to aggregate balance for address " + scoped_entry.address;
                result["detail"] = balance_response["error"];
                return result;
            }

            const uint64_t confirmed_una = ParseUnsignedUnaField(balance_response["confirmed"]);
            const int64_t unconfirmed_una = ParseSignedUnaField(balance_response["unconfirmed"]);
            const uint64_t total_una = balance_response.isMember("estimated_balance")
                ? ParseUnsignedUnaField(balance_response["estimated_balance"])
                : ApplySignedUnaDelta(confirmed_una, unconfirmed_una);

            aggregate_confirmed_una += confirmed_una;
            aggregate_unconfirmed_una += unconfirmed_una;
            aggregate_total_una += total_una;

            if (total_una > 0 || confirmed_una > 0) {
                din::Json addr_obj = scoped_entry.metadata;
                addr_obj["address"] = scoped_entry.address;
                addr_obj["balance_una"] = static_cast<Json::UInt64>(total_una);
                addr_obj["confirmed_una"] = static_cast<Json::UInt64>(confirmed_una);
                addr_obj["unconfirmed_una"] = static_cast<Json::Int64>(unconfirmed_una);
                addr_obj["spendable_una"] = static_cast<Json::UInt64>(confirmed_una);
                addr_obj["balance"] = static_cast<double>(total_una) /
                    static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
                addr_obj["confirmed"] = static_cast<double>(confirmed_una) /
                    static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
                addr_obj["unconfirmed"] = static_cast<double>(unconfirmed_una) /
                    static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
                addr_obj["spendable"] = static_cast<double>(confirmed_una) /
                    static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
                funded_entries.push_back({total_una, addr_obj});
                ++funded_count;
            }

            if (receive_obj["current_address"].isString() &&
                receive_obj["current_address"].asString() == scoped_entry.address &&
                scoped_entry.metadata.isMember("path") &&
                scoped_entry.metadata["path"].isString()) {
                receive_obj["current_path"] = scoped_entry.metadata["path"].asString();
            }

            din::Json history_params = din::arr();
            history_params.append(scoped_entry.address);
            history_params.append(history_count);
            auto history_response = din::rpc_getaddresshistory(ctx, history_params);
            if (history_response.isMember("error")) {
                result["error"] = "Failed to aggregate history for address " + scoped_entry.address;
                result["detail"] = history_response["error"];
                return result;
            }

            const auto txs = history_response["transactions"];
            if (!txs.isArray()) {
                continue;
            }

            for (const auto& tx : txs) {
                const std::string txid = tx.isMember("txid") && tx["txid"].isString()
                    ? tx["txid"].asString()
                    : "";
                std::string category = tx.isMember("type") && tx["type"].isString()
                    ? tx["type"].asString()
                    : "receive";
                const int confirmations =
                    tx.isMember("confirmations") && tx["confirmations"].isInt()
                        ? tx["confirmations"].asInt()
                        : 0;
                const bool is_outgoing = category == "send" || category == "spend";
                const std::string privacy_flow =
                    tx.isMember("privacy_flow") && tx["privacy_flow"].isString()
                        ? tx["privacy_flow"].asString()
                        : "transparent";
                const bool amount_hidden =
                    tx.isMember("amount_hidden") && tx["amount_hidden"].isBool()
                        ? tx["amount_hidden"].asBool()
                        : false;

                // Prefer the coinbase flag the getaddresshistory entry now carries
                // (derived from the block, no txindex). Fall back to a chain lookup
                // for nodes/entries that predate the flag (only works with txindex).
                bool is_coinbase = false;
                if (!is_outgoing) {
                    if (tx.isMember("is_coinbase") && tx["is_coinbase"].isBool()) {
                        is_coinbase = tx["is_coinbase"].asBool();
                    } else if (txid.size() == 64) {
                        auto tx_result = chain_db->getTransaction(dinero::uint256::FromHexUnsafe(txid));
                        if (tx_result.ok()) {
                            is_coinbase = tx_result.value().IsCoinbase();
                        }
                    }
                }

                std::string normalized_category;
                std::string normalized_type;
                if (is_outgoing) {
                    normalized_category = "send";
                    normalized_type = "sent";
                    outgoing_txids.insert(txid);
                } else if (is_coinbase || category == "generate" || category == "immature" ||
                           category == "mining" || category == "coinbase") {
                    normalized_category = confirmations >= 100 ? "generate" : "immature";
                    normalized_type = "mined";
                } else {
                    normalized_category = "receive";
                    normalized_type = "received";
                }

                din::Json tx_obj;
                tx_obj["txid"] = tx["txid"];
                tx_obj["address"] = scoped_entry.address;
                tx_obj["privacy_flow"] = privacy_flow;
                tx_obj["classification"] = privacy_flow;
                tx_obj["has_confidential_activity"] =
                    tx.isMember("has_confidential_activity") && tx["has_confidential_activity"].isBool()
                        ? tx["has_confidential_activity"].asBool()
                        : (privacy_flow != "transparent");
                tx_obj["amount_hidden"] = amount_hidden;
                if (amount_hidden) {
                    tx_obj["display_amount"] =
                        tx.isMember("display_amount") && tx["display_amount"].isString()
                            ? tx["display_amount"].asString()
                            : "confidential";
                } else {
                    tx_obj["amount_una"] = tx["amount"];
                    tx_obj["amount"] = static_cast<double>(ParseUnsignedUnaField(tx["amount"])) /
                        static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
                    if (tx.isMember("amount_din")) {
                        tx_obj["display_amount"] = tx["amount_din"];
                    }
                }
                tx_obj["confirmations"] = tx["confirmations"];
                tx_obj["category"] = normalized_category;
                tx_obj["type"] = normalized_type;
                tx_obj["is_coinbase"] = is_coinbase;
                if (tx.isMember("height")) {
                    tx_obj["height"] = tx["height"];
                }
                if (tx.isMember("time")) {
                    tx_obj["time"] = tx["time"];
                }
                history_items.push_back(tx_obj);
            }
        }

        std::sort(funded_entries.begin(), funded_entries.end(),
                  [](const FundedAddressEntry& lhs, const FundedAddressEntry& rhs) {
                      if (lhs.total_una != rhs.total_una) {
                          return lhs.total_una > rhs.total_una;
                      }
                      return lhs.json["address"].asString() < rhs.json["address"].asString();
                  });

        receive_obj["funded_address_count"] = static_cast<Json::UInt64>(funded_count);
        for (size_t i = 0; i < funded_entries.size() &&
                           i < static_cast<size_t>(funded_address_limit); ++i) {
            addresses_obj["funded"].append(funded_entries[i].json);
        }

        std::sort(history_items.begin(), history_items.end(),
                  [](const din::Json& lhs, const din::Json& rhs) {
                      const auto lhs_height = lhs.isMember("height") && lhs["height"].isInt()
                          ? lhs["height"].asInt()
                          : 0;
                      const auto rhs_height = rhs.isMember("height") && rhs["height"].isInt()
                          ? rhs["height"].asInt()
                          : 0;
                      if (lhs_height != rhs_height) {
                          return lhs_height > rhs_height;
                      }
                      const auto lhs_time = lhs.isMember("time") && lhs["time"].isNumeric()
                          ? lhs["time"].asDouble()
                          : 0.0;
                      const auto rhs_time = rhs.isMember("time") && rhs["time"].isNumeric()
                          ? rhs["time"].asDouble()
                          : 0.0;
                      if (lhs_time != rhs_time) {
                          return lhs_time > rhs_time;
                      }
                      const auto lhs_txid = lhs.isMember("txid") && lhs["txid"].isString()
                          ? lhs["txid"].asString()
                          : "";
                      const auto rhs_txid = rhs.isMember("txid") && rhs["txid"].isString()
                          ? rhs["txid"].asString()
                          : "";
                      return lhs_txid < rhs_txid;
                  });

        std::set<std::string> seen_history_keys;
        for (const auto& item : history_items) {
            const auto txid = item["txid"].asString();
            const auto address = item["address"].asString();
            const auto category = item["category"].asString();
            if (category != "send" && outgoing_txids.count(txid) > 0) {
                continue;
            }

            const std::string key =
                ToLowerCopy(txid) + "|" + ToLowerCopy(address) + "|" + ToLowerCopy(category);
            if (!seen_history_keys.insert(key).second) {
                continue;
            }

            history_obj["items"].append(item);
            if (static_cast<int>(history_obj["items"].size()) >= history_count) {
                break;
            }
        }

        history_obj["count"] = static_cast<Json::UInt64>(history_obj["items"].size());
        balances["confirmed_una"] = static_cast<Json::UInt64>(aggregate_confirmed_una);
        balances["unconfirmed_una"] = static_cast<Json::Int64>(aggregate_unconfirmed_una);
        balances["total_una"] = static_cast<Json::UInt64>(aggregate_total_una);
        balances["spendable_una"] = static_cast<Json::UInt64>(aggregate_confirmed_una);
        balances["confirmed"] = static_cast<double>(aggregate_confirmed_una) /
            static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
        balances["unconfirmed"] = static_cast<double>(aggregate_unconfirmed_una) /
            static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
        balances["immature"] = 0.0;
        balances["locked"] = 0.0;
        balances["total"] = static_cast<double>(aggregate_total_una) /
            static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
        balances["spendable"] = static_cast<double>(aggregate_confirmed_una) /
            static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
        balances["utxo_count"] = static_cast<Json::UInt64>(0);
        balances["funded_address_count"] = static_cast<Json::UInt64>(funded_count);
        balances["breakdown"]["spendable"] = balances["spendable"];
        balances["breakdown"]["pending"] = balances["unconfirmed"];
        balances["breakdown"]["unspendable"] = 0.0;

        result["wallet"] = wallet_obj;
        result["sync"] = sync_obj;
        result["balances"] = balances;
        result["receive"] = receive_obj;
        result["addresses"] = addresses_obj;
        result["history"] = history_obj;
        result["proof_context"] = proof_context;
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    wallet_obj["loaded"] = wallet_service->hasActiveWallet();
    wallet_obj["name"] = wallet_service->hasActiveWallet() ? wallet_service->getCurrentWalletName() : "";

    if (!wallet_service->hasActiveWallet()) {
        result["wallet"] = wallet_obj;
        result["sync"] = sync_obj;
        result["balances"] = balances;
        result["receive"] = receive_obj;
        result["addresses"] = addresses_obj;
        result["history"] = history_obj;
        result["proof_context"] = proof_context;
        return result;
    }

    try {
        auto& mgr = wallet_service->get();
        wallet_obj["name"] = mgr.current();
        wallet_obj["encrypted"] = mgr.isWalletEncrypted();
        wallet_obj["locked"] = mgr.isWalletLocked();
        wallet_obj["unlocked"] = !mgr.isWalletLocked();
        wallet_obj["hd_enabled"] = (mgr.getHDWallet() != nullptr);

        const auto balance = mgr.getBalance();
        const double locked_balance = mgr.getLockedBalance();
        balances["confirmed"] = balance.confirmed;
        balances["unconfirmed"] = balance.unconfirmed;
        balances["immature"] = balance.immature;
        balances["locked"] = locked_balance;
        balances["total"] = balance.total;
        balances["spendable"] = balance.spendable;
        balances["utxo_count"] = balance.utxo_count;
        balances["immature_utxo_count"] = balance.immature_utxo_count;
        balances["breakdown"]["spendable"] = balance.spendable;
        balances["breakdown"]["pending"] = balance.unconfirmed;
        balances["breakdown"]["unspendable"] = balance.immature + locked_balance;

        const auto addresses = mgr.listAddresses(true);
        receive_obj["known_address_count"] = static_cast<Json::UInt64>(addresses.size());

        struct FundedAddressEntry {
            double total = 0.0;
            din::Json json;
        };

        std::vector<FundedAddressEntry> funded_entries;
        bool have_receive_address = false;
        int best_receive_account = -1;
        int best_receive_index = -1;
        int best_receive_change = 99;

        auto build_path = [](const dinero::AddressRow& addr_row) -> std::string {
            if (addr_row.account < 0) {
                return "imported";
            }
            const bool is_taproot = AddressRowIsTaproot(addr_row);
            const int purpose = is_taproot ? 86 : 84;
            return BuildStandardDerivationPath(
                static_cast<uint32_t>(purpose),
                addr_row.account,
                addr_row.change,
                addr_row.index);
        };

        for (const auto& addr_row : addresses) {
            if (addr_row.account >= 0 && addr_row.change == 0) {
                if (!have_receive_address ||
                    addr_row.account > best_receive_account ||
                    (addr_row.account == best_receive_account && addr_row.index > best_receive_index) ||
                    (addr_row.account == best_receive_account &&
                     addr_row.index == best_receive_index &&
                     addr_row.change < best_receive_change)) {
                    have_receive_address = true;
                    best_receive_account = addr_row.account;
                    best_receive_index = addr_row.index;
                    best_receive_change = addr_row.change;
                    receive_obj["current_address"] = addr_row.address;
                    receive_obj["current_path"] = build_path(addr_row);
                }
            }

            auto addr_balance = addr_row.script_pubkey.empty()
                ? mgr.getAddressBalance(addr_row.address)
                : mgr.getScriptPubKeyBalance(addr_row.script_pubkey);

            if (addr_balance.total <= 0.0) {
                continue;
            }

            din::Json addr_obj;
            addr_obj["address"] = addr_row.address;
            if (addr_row.label) {
                addr_obj["label"] = *addr_row.label;
            }
            addr_obj["account"] = addr_row.account;
            addr_obj["change"] = addr_row.change;
            addr_obj["index"] = addr_row.index;
            addr_obj["external"] = addr_row.external;
            addr_obj["type"] = addr_row.type;
            addr_obj["path"] = build_path(addr_row);
            if (!addr_row.script_pubkey.empty()) {
                addr_obj["scriptPubKey"] = addr_row.script_pubkey;
            }
            addr_obj["balance"] = addr_balance.total;
            addr_obj["confirmed"] = addr_balance.confirmed;
            addr_obj["unconfirmed"] = addr_balance.unconfirmed;
            addr_obj["immature"] = addr_balance.immature;
            addr_obj["spendable"] = addr_balance.spendable;
            addr_obj["utxo_count"] = addr_balance.utxo_count;

            funded_entries.push_back(FundedAddressEntry{
                addr_balance.total,
                addr_obj
            });
        }

        std::sort(funded_entries.begin(), funded_entries.end(),
                  [](const FundedAddressEntry& lhs, const FundedAddressEntry& rhs) {
                      if (lhs.total != rhs.total) {
                          return lhs.total > rhs.total;
                      }
                      return lhs.json["address"].asString() < rhs.json["address"].asString();
                  });

        receive_obj["funded_address_count"] = static_cast<Json::UInt64>(funded_entries.size());
        for (size_t i = 0; i < funded_entries.size() &&
                           i < static_cast<size_t>(funded_address_limit); ++i) {
            addresses_obj["funded"].append(funded_entries[i].json);
        }

        const auto tx_history = mgr.getTransactionHistory(history_count, 0);
        history_obj["count"] = static_cast<Json::UInt64>(tx_history.size());
        for (const auto& tx : tx_history) {
            din::Json tx_obj;
            tx_obj["txid"] = tx.txid;
            tx_obj["address"] = tx.address;
            tx_obj["amount"] = tx.amount;
            tx_obj["confirmations"] = tx.confirmations;
            tx_obj["category"] = tx.category;
            tx_obj["type"] = NormalizeWalletTxType(tx);
            tx_obj["time"] = static_cast<double>(tx.time);
            tx_obj["label"] = tx.label;
            tx_obj["is_coinbase"] = tx.is_coinbase;
            PopulateWalletHistoryMetadata(tx_obj, tx);
            history_obj["items"].append(tx_obj);
        }
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to build wallet snapshot: ") + e.what();
        return result;
    }

    result["wallet"] = wallet_obj;
    result["sync"] = sync_obj;
    result["balances"] = balances;
    result["receive"] = receive_obj;
    result["addresses"] = addresses_obj;
    result["history"] = history_obj;
    result["proof_context"] = proof_context;
    return result;
}

/**
 * wallet.estimatefee - Estimate transaction fee for confirmation target
 *
 * Phase 35.1: Wallet Introspection & UX - Fee introspection helper
 *
 * Convenience wrapper for mempool fee estimation from wallet context.
 * Pure read-only operation - delegates to mempool fee estimator.
 *
 * Parameters:
 * - conf_target (optional): Target confirmation blocks (default: 6)
 *
 * Returns:
 * {
 *   "feerate": <una/vB>,
 *   "feerate_din_kb": <DIN/kB>,
 *   "blocks": <confirmation target>,
 *   "confidence": "high|medium|low",
 *   "source": "historical_data|mempool_analysis|fallback"
 * }
 */
din::Json rpc_context_wallet_estimatefee(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Parse confirmation target
    int conf_target = 6;  // Default: medium priority
    if (!params.empty() && params[0].is<int>()) {
        conf_target = params[0].as<int>();
        if (conf_target < 1) conf_target = 1;
        if (conf_target > 1008) conf_target = 1008;
    }

    // Validate mempool service available
    if (!ctx.daemon || !ctx.daemon->mempool) {
        result["error"] = "Mempool service not available";
        result["feerate"] = 1.0;  // Fallback minimum
        result["source"] = "fallback";
        return result;
    }

    try {
        auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
        if (!mempool_service) {
            result["error"] = "Failed to cast mempool service";
            result["feerate"] = 1.0;
            result["source"] = "fallback";
            return result;
        }

        auto& mempool = mempool_service->mempool();
        auto& fee_estimator = mempool.getFeeEstimator();

        // Try to get estimate from historical data
        auto estimate = fee_estimator.estimateFee(static_cast<uint32_t>(conf_target));

        if (estimate.has_value()) {
            // Got reliable estimate from confirmation history
            double feerate_una_vb = estimate.value();

            // Convert to DIN/kB for Bitcoin Core compatibility
            double feerate_din_kb = feerate_una_vb * 1000.0 / 1e8;

            result["feerate"] = feerate_una_vb;           // una/vB (preferred)
            result["feerate_din_kb"] = feerate_din_kb;    // DIN/kB (Bitcoin compat)
            result["blocks"] = conf_target;
            result["confidence"] = "high";
            result["source"] = "historical_data";
        } else {
            // Insufficient historical data - use mempool analysis
            auto stats = mempool.getStats();
            double feerate_una_vb = 1.0;  // Minimum fallback

            if (stats.tx_count > 0 && stats.total_fees > 0) {
                // Estimate based on current mempool
                double avg_fee_rate = static_cast<double>(stats.total_fees) / static_cast<double>(stats.total_size);
                feerate_una_vb = std::max(1.0, avg_fee_rate);
            }

            double feerate_din_kb = feerate_una_vb * 1000.0 / 1e8;

            result["feerate"] = feerate_una_vb;
            result["feerate_din_kb"] = feerate_din_kb;
            result["blocks"] = conf_target;
            result["confidence"] = (stats.tx_count > 10) ? "medium" : "low";
            result["source"] = "mempool_analysis";
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Fee estimation failed: ") + e.what();
        result["feerate"] = 1.0;
        result["source"] = "fallback";
    }

    return result;
}

/**
 * wallet.getnewaddress - Generate new receiving address
 *
 * Supports "taproot"/"p2tr" (Taproot P2TR) and "p2mr" (v7 post-quantum).
 * P2MR dispatches through the RPC registry to wallet.getnewp2mraddress
 * with an auto-incremented address_index.
 */
din::Json rpc_context_wallet_getnewaddress(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    if (!wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        std::string label = "";
        std::string address_type = "taproot";

        // Parse params: wallet.getnewaddress [address_type] [label]
        std::string normalized;
        if (!params.empty() && params[0].is<std::string>()) {
            std::string requested = params[0].as<std::string>();
            for (char c : requested) {
                normalized += std::tolower(static_cast<unsigned char>(c));
            }
        }
        if (params.size() >= 2 && params[1].is<std::string>()) {
            label = params[1].as<std::string>();
        }

        // Receive-scheme policy: if no explicit type was requested, consult
        // the wallet's "receive_default" setting. Values: "taproot" (default),
        // "p2mr", "split". "split" alternates between Taproot and P2MR on
        // successive calls — simple hedging without user intervention.
        if (normalized.empty()) {
            std::string policy = wallet_service->get().getSetting("receive_default", "");
            if (policy == "p2mr") {
                normalized = "p2mr";
            } else if (policy == "split") {
                // Alternate: even call count → taproot, odd → p2mr.
                // Count is derived from the parity of total addresses.
                auto store = std::make_unique<dinero::wallet::V7P2MRStore>();
                const std::string sp = wallet_service->get().GetV7P2MRStorePath();
                size_t p2mr_count = 0;
                if (!sp.empty()) {
                    if (store->Open(sp) == dinero::wallet::V7P2MRStore::OpenResult::Ok) {
                        p2mr_count = store->ListByWallet(1).size();
                    }
                }
                int tap_count = wallet_service->get().getNextAddressIndex(0, 0);
                normalized = ((tap_count + p2mr_count) % 2 == 0) ? "taproot" : "p2mr";
            }
            // else: default "taproot" — normalized stays empty, falls through
        }

        // v7 P2MR (post-quantum): dispatch through the RPC registry to
        // wallet.getnewp2mraddress with auto-incremented address_index.
        // The store's row count gives the next index.
        if (normalized == "p2mr" || normalized == "p2mr_v3") {
            auto store = std::make_unique<dinero::wallet::V7P2MRStore>();
            const std::string store_path = wallet_service->get().GetV7P2MRStorePath();
            if (store_path.empty() ||
                store->Open(store_path) != dinero::wallet::V7P2MRStore::OpenResult::Ok) {
                result["error"] = "v7 P2MR store not available";
                return result;
            }
            int next_idx = static_cast<int>(store->ListByWallet(1).size());

            din::Json p2mr_params;
            p2mr_params["account"]       = 0;
            p2mr_params["change"]        = 0;
            p2mr_params["address_index"] = next_idx;
            p2mr_params["leaf_index"]    = 0;
            p2mr_params["label"]         = label;

            auto* handler = g_rpcRegistry.lookup("wallet.getnewp2mraddress");
            if (!handler) {
                result["error"] = "wallet.getnewp2mraddress handler not registered";
                return result;
            }
            return (*handler)(ctx, p2mr_params);
        }

        // Taproot (default): accept taproot aliases, reject legacy/segwit.
        if (!normalized.empty() &&
            normalized != "taproot" &&
            normalized != "p2tr" &&
            normalized != "bech32m" &&
            normalized != "witness_v1_taproot") {
            result["error"] = "Unsupported address_type. Use 'taproot', 'p2tr', or 'p2mr'";
            return result;
        }

        std::string address = wallet_service->get().getNewAddress(label, address_type);
        if (address.empty()) {
            const bool locked = wallet_service->get().isWalletLocked();
            result["error"] = locked
                ? "Failed to generate address: wallet is locked"
                : "Failed to generate address: wallet returned empty address (seed unavailable or wallet not initialized)";
            result["address_type"] = address_type;
            result["rpc_schema"] = "din.wallet.v1";
            if (ctx.logger) {
                ctx.logger->error("[wallet.getnewaddress] Failed: empty address returned (locked=" +
                                  std::string(locked ? "true" : "false") + ")");
            }
            return result;
        }

        result["address"] = address;
        result["address_type"] = address_type;
        result["rpc_schema"] = "din.wallet.v1";

        // Use context logger instead of global logger
        if (ctx.logger) {
            ctx.logger->info("[wallet.getnewaddress] Generated new " + address_type + " address: " + address);
        }
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to generate address: ") + e.what();
        // Log error via context logger
        if (ctx.logger) {
            ctx.logger->error("[wallet.getnewaddress] Failed: " + std::string(e.what()));
        }
    }

    return result;
}

/**
 * wallet.listaddresses - List all wallet addresses
 *
 * OLD: dinero::legacy::g_wallet_manager()->listAddresses()
 * NEW: ctx.daemon->wallet->get().listAddresses()
 */
din::Json rpc_context_wallet_listaddresses(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    if (!wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        auto addresses = wallet_service->get().listAddresses(true);
        din::Json addr_array = din::arr();

        auto build_path = [](const dinero::AddressRow& addr_row) -> std::string {
            if (addr_row.account < 0) {
                return "imported";
            }
            const bool is_taproot = AddressRowIsTaproot(addr_row);
            const int purpose = is_taproot ? 86 : 84;
            return BuildStandardDerivationPath(
                static_cast<uint32_t>(purpose),
                addr_row.account,
                addr_row.change,
                addr_row.index);
        };

        for (const auto& addr_row : addresses) {
            din::Json addr_obj;
            auto balance = addr_row.script_pubkey.empty()
                               ? wallet_service->get().getAddressBalance(addr_row.address)
                               : wallet_service->get().getScriptPubKeyBalance(addr_row.script_pubkey);

            addr_obj["address"] = addr_row.address;
            if (addr_row.label) {
                addr_obj["label"] = *addr_row.label;
            }
            addr_obj["account"] = addr_row.account;
            addr_obj["change"] = addr_row.change;
            addr_obj["index"] = addr_row.index;
            addr_obj["external"] = addr_row.external;
            addr_obj["type"] = addr_row.type;
            if (!addr_row.script_pubkey.empty()) {
                addr_obj["scriptPubKey"] = addr_row.script_pubkey;
            }
            addr_obj["path"] = build_path(addr_row);
            addr_obj["balance"] = balance.total;
            addr_obj["confirmed"] = balance.confirmed;
            addr_obj["unconfirmed"] = balance.unconfirmed;
            addr_obj["immature"] = balance.immature;
            addr_obj["spendable"] = balance.spendable;
            addr_obj["utxo_count"] = balance.utxo_count;
            addr_array.append(addr_obj);
        }

        // v7: P2MR addresses live in V7P2MRStore (separate SQLite DB) and are
        // not in the legacy `addresses` table. Merge them in so wallet.listaddresses
        // is the single source of truth for the wallet's receive set.
        const std::string p2mr_store_path = wallet_service->get().GetV7P2MRStorePath();
        if (!p2mr_store_path.empty()) {
            dinero::wallet::V7P2MRStore p2mr_store;
            if (p2mr_store.Open(p2mr_store_path) ==
                dinero::wallet::V7P2MRStore::OpenResult::Ok) {
                for (const auto& p2mr : p2mr_store.ListByWallet(1)) {
                    din::Json addr_obj;
                    auto balance = wallet_service->get().getAddressBalance(p2mr.address);
                    addr_obj["address"]    = p2mr.address;
                    if (!p2mr.label.empty()) {
                        addr_obj["label"]  = p2mr.label;
                    }
                    // P2MR rows don't carry HD account/change in the store; we
                    // expose the leaf_index as `index` and zero the rest so the
                    // existing UI sort/group logic keeps working.
                    addr_obj["account"]    = 0;
                    addr_obj["change"]     = 0;
                    addr_obj["index"]      = static_cast<int64_t>(p2mr.leaf_index);
                    addr_obj["external"]   = true;
                    addr_obj["type"]       = "p2mr";
                    addr_obj["path"]       = p2mr.derivation_path;
                    addr_obj["balance"]    = balance.total;
                    addr_obj["confirmed"]  = balance.confirmed;
                    addr_obj["unconfirmed"]= balance.unconfirmed;
                    addr_obj["immature"]   = balance.immature;
                    addr_obj["spendable"]  = balance.spendable;
                    addr_obj["utxo_count"] = balance.utxo_count;
                    addr_array.append(addr_obj);
                }
            }
        }

        result = addr_array;
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to list addresses: ") + e.what();
    }

    return result;
}

/**
 * wallet.listunspent - List unspent transaction outputs
 *
 * OLD: dinero::legacy::g_wallet_manager()->listUnspentUTXOs()
 * NEW: ctx.daemon->wallet->get().listUnspentUTXOs()
 */
din::Json rpc_context_wallet_listunspent(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    if (!wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        int min_conf = 1;
        int max_conf = 9999999;

        if (params.size() >= 1 && params[0].is<int>()) {
            min_conf = params[0].as<int>();
        }
        if (params.size() >= 2 && params[1].is<int>()) {
            max_conf = params[1].as<int>();
        }

        const dinero::Mempool* mempool = nullptr;
        if (ctx.daemon->mempool) {
            if (auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
                mempool_service && mempool_service->isInitialized()) {
                mempool = &mempool_service->mempool();
            }
        }

        auto& mgr = wallet_service->get();

        // Sync gate: wait for wallet worker to catch up to chain tip.
        if (ctx.daemon->chainstate) {
            auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
            if (cs) {
                mgr.WaitForHeight(cs->getBlockHeight(), std::chrono::milliseconds(5000));
            }
        }

        auto utxos = mgr.listUnspentUTXOs(min_conf, max_conf, mempool);
        din::Json utxo_array = din::arr();

        for (const auto& utxo : utxos) {
            din::Json utxo_obj;
            const bool solvable = mgr.hasSigningMaterialForScriptPubKey(utxo.script_pubkey);
            const bool spendable = utxo.spendable && solvable;

            // Phase 35: Enhanced UTXO metadata
            utxo_obj["txid"] = utxo.txid;
            utxo_obj["vout"] = static_cast<int>(utxo.vout);
            utxo_obj["address"] = utxo.address;  // Display only - NOT authoritative
            utxo_obj["scriptPubKey"] = utxo.script_pubkey;  // AUTHORITATIVE for ownership
            utxo_obj["amount"] = utxo.amount_din;  // DIN
            utxo_obj["amount_una"] = static_cast<int64_t>(utxo.amount_una);  // una
            utxo_obj["confirmations"] = utxo.confirmations;
            utxo_obj["spendable"] = spendable;
            utxo_obj["solvable"] = solvable;
            utxo_obj["safe"] = (utxo.confirmations > 0) && solvable;  // Confirmed + signable = safe
            utxo_obj["is_coinbase"] = utxo.is_coinbase;
            utxo_obj["is_mature"] = utxo.is_mature;
            utxo_obj["locked"] = mgr.isUTXOLocked(utxo.txid, utxo.vout);  // Phase 35.3: Check lock status

            // Parse witness version from scriptPubKey
            uint8_t witness_version = 0xFF;  // Default: legacy (non-witness)
            if (!utxo.script_pubkey.empty() && utxo.script_pubkey.size() >= 4) {
                std::string spk = utxo.script_pubkey;
                if (spk.size() >= 4) {
                    // Check first byte (witness version opcode)
                    if (spk.substr(0, 2) == "00") {
                        witness_version = 0;  // SegWit v0 (P2WPKH/P2WSH)
                    } else if (spk.substr(0, 2) == "51" && spk.size() == 68) {
                        witness_version = 1;  // Taproot (OP_1 + 32 bytes)
                    }
                }
            }
            utxo_obj["witness_version"] = static_cast<int>(witness_version);

            if (!utxo.label.empty()) {
                utxo_obj["label"] = utxo.label;
            }
            if (!utxo.derivation_path.empty()) {
                utxo_obj["derivation_path"] = utxo.derivation_path;
            }
            utxo_array.append(utxo_obj);
        }

        result = utxo_array;
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to list UTXOs: ") + e.what();
    }

    return result;
}

/**
 * wallet.setreceivepolicy — set the default address scheme for getnewaddress.
 * Params: ["taproot"|"p2mr"|"split"]
 * "split" alternates Taproot and P2MR on successive calls.
 */
din::Json rpc_context_wallet_setreceivepolicy(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    if (!ctx.daemon || !ctx.daemon->wallet) { result["error"] = "Wallet not available"; return result; }
    auto ws = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!ws || !ws->hasActiveWallet()) { result["error"] = "No active wallet"; return result; }

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.setreceivepolicy <taproot|p2mr|split>";
        return result;
    }
    std::string policy = params[0].as<std::string>();
    for (auto& c : policy) c = std::tolower(static_cast<unsigned char>(c));

    if (policy != "taproot" && policy != "p2mr" && policy != "split") {
        result["error"] = "Invalid policy. Must be 'taproot', 'p2mr', or 'split'";
        return result;
    }

    ws->get().setSetting("receive_default", policy);
    result["receive_default"] = policy;
    return result;
}

/**
 * wallet.getreceivepolicy — read the current default receive scheme.
 */
din::Json rpc_context_wallet_getreceivepolicy(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    din::Json result;
    if (!ctx.daemon || !ctx.daemon->wallet) { result["error"] = "Wallet not available"; return result; }
    auto ws = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!ws || !ws->hasActiveWallet()) { result["error"] = "No active wallet"; return result; }

    std::string policy = ws->get().getSetting("receive_default", "");
    result["receive_default"] = policy.empty() ? "taproot" : policy;
    return result;
}

/**
 * wallet.lockunspent - Lock/unlock specific UTXOs (Phase 35)
 *
 * Prevents automatic coin selection from using specific UTXOs.
 * Useful for:
 * - Preserving specific coins for future use
 * - Testing coin selection behavior
 * - Manual UTXO management
 */
din::Json rpc_context_wallet_lockunspent(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    if (!wallet_service->hasActiveWallet()) {
        result["error"] = "Wallet not loaded";
        return result;
    }

    try {
        auto& mgr = wallet_service->get();

        // Parse parameters
        bool unlock = false;
        bool has_outputs_param = false;
        din::Json outputs_array;

        if (!params.empty() && params[0].isBool()) {
            unlock = params[0].asBool();
        }

        if (params.size() > 1 && params[1].isArray()) {
            outputs_array = params[1];
            has_outputs_param = true;
        }

        // Special case: Query currently locked UTXOs (no params)
        if (params.empty()) {
            auto locked = mgr.getLockedUTXOs();
            din::Json locked_array = din::arr();
            for (const auto& outpoint : locked) {
                size_t colon_pos = outpoint.find(':');
                if (colon_pos != std::string::npos) {
                    din::Json utxo;
                    utxo["txid"] = outpoint.substr(0, colon_pos);
                    utxo["vout"] = std::stoi(outpoint.substr(colon_pos + 1));
                    locked_array.append(utxo);
                }
            }
            result["locked"] = locked_array;
            result["success"] = true;
            return result;
        }

        // Special case: Lock/unlock ALL UTXOs (empty outputs array)
        if (has_outputs_param && outputs_array.size() == 0) {
            if (unlock) {
                // Unlock all
                size_t count = mgr.unlockAllUTXOs();
                result["success"] = true;
                result["unlocked_count"] = static_cast<int>(count);
                result["locked"] = din::arr();  // Empty array
            } else {
                // Lock all - get all UTXOs and lock them
                auto utxos = mgr.listUnspentUTXOs(0, 9999999);  // All UTXOs
                for (const auto& utxo : utxos) {
                    mgr.lockUTXO(utxo.txid, utxo.vout);
                }
                result["success"] = true;
                result["locked_count"] = static_cast<int>(utxos.size());
            }

            auto locked = mgr.getLockedUTXOs();
            din::Json locked_array = din::arr();
            for (const auto& outpoint : locked) {
                size_t colon_pos = outpoint.find(':');
                if (colon_pos != std::string::npos) {
                    din::Json utxo;
                    utxo["txid"] = outpoint.substr(0, colon_pos);
                    utxo["vout"] = std::stoi(outpoint.substr(colon_pos + 1));
                    locked_array.append(utxo);
                }
            }
            result["locked"] = locked_array;
            return result;
        }

        // Normal case: Lock/unlock specific UTXOs
        if (!has_outputs_param) {
            result["error"] = "Usage: wallet.lockunspent <unlock> <outputs_array>";
            return result;
        }

        for (Json::ArrayIndex i = 0; i < outputs_array.size(); ++i) {
            auto output = outputs_array[i];

            if (!output.isObject()) {
                result["error"] = "Invalid output format";
                return result;
            }

            if (!output["txid"].isString() || !output["vout"].isInt()) {
                result["error"] = "Each output must have 'txid' (string) and 'vout' (int)";
                return result;
            }

            std::string txid = output["txid"].as<std::string>();
            uint32_t vout = static_cast<uint32_t>(output["vout"].as<int>());

            if (unlock) {
                mgr.unlockUTXO(txid, vout);
            } else {
                mgr.lockUTXO(txid, vout);
            }
        }

        // Return currently locked UTXOs
        auto locked = mgr.getLockedUTXOs();
        din::Json locked_array = din::arr();
        for (const auto& outpoint : locked) {
            size_t colon_pos = outpoint.find(':');
            if (colon_pos != std::string::npos) {
                din::Json utxo;
                utxo["txid"] = outpoint.substr(0, colon_pos);
                utxo["vout"] = std::stoi(outpoint.substr(colon_pos + 1));
                locked_array.append(utxo);
            }
        }
        result["locked"] = locked_array;
        result["success"] = true;

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to lock/unlock UTXOs: ") + e.what();
        result["success"] = false;
    }

    return result;
}

/**
 * wallet.abandontransaction - Mark an unconfirmed transaction as abandoned (Phase 35.4)
 *
 * Returns inputs to spendable set, allowing replacement transactions.
 * Critical for recovering from stuck transactions.
 */
din::Json rpc_context_wallet_abandontransaction(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    if (!wallet_service->hasActiveWallet()) {
        result["error"] = "Wallet not loaded";
        return result;
    }

    if (params.empty() || !params[0].isString()) {
        result["error"] = "Usage: wallet.abandontransaction <txid>";
        return result;
    }

    try {
        auto& mgr = wallet_service->get();
        std::string txid = params[0].as<std::string>();

        // Get info before abandoning
        auto info = mgr.getAbandonmentInfo(txid);

        if (!info.success) {
            result["error"] = info.error;
            result["success"] = false;
            return result;
        }

        // Abandon the transaction
        bool abandoned = mgr.abandonTransaction(txid);

        if (abandoned) {
            result["success"] = true;
            result["abandoned"] = txid;
            result["inputs_returned"] = info.inputs_returned;
            result["amount_returned"] = info.amount_returned;
        } else {
            result["success"] = false;
            result["error"] = "Failed to abandon transaction";
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to abandon transaction: ") + e.what();
        result["success"] = false;
    }

    return result;
}

/**
 * wallet.getinfo - Get wallet information
 *
 * OLD: dinero::legacy::g_wallet_manager()->getCurrentWalletName()
 * NEW: ctx.daemon->wallet->getCurrentWalletName()
 */
din::Json rpc_context_wallet_getinfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    try {
        result["has_active_wallet"] = wallet_service->hasActiveWallet();

        if (wallet_service->hasActiveWallet()) {
            result["wallet_name"] = wallet_service->getCurrentWalletName();

            auto& mgr = wallet_service->get();
            result["encrypted"] = mgr.isWalletEncrypted();
            result["locked"] = mgr.isWalletLocked();
            result["hd_enabled"] = true;  // All Dinero wallets are HD (BIP86)
            result["unlocked"] = !mgr.isWalletLocked();

            auto balance = mgr.getBalance();
            result["balance"] = balance.total;
            result["utxo_count"] = balance.utxo_count;

            // Primary transparent address
            try {
                auto addrs = mgr.listAddresses(false);
                if (!addrs.empty())
                    result["primary_address"] = addrs[0].address;
            } catch (...) {}

        } else {
            result["hd_enabled"] = false;
            result["unlocked"] = false;
        }

        auto wallets = wallet_service->listWallets();
        din::Json wallet_array = din::arr();
        for (const auto& w : wallets) {
            wallet_array.append(w);
        }
        result["available_wallets"] = wallet_array;

        result["rpc_schema"] = "din.wallet.v1";
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get wallet info: ") + e.what();
    }

    return result;
}

/**
 * wallet.validateaddress - Validate Dinero address format
 */
din::Json rpc_context_wallet_validateaddress(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.validateaddress <address>";
        return result;
    }

    std::string address = params[0].as<std::string>();

    // Proper bech32/bech32m decode for the active network. Handles SegWit v0
    // (P2WPKH/P2WSH) and v1 Taproot (BIP350/bech32m), populating scriptPubKey.
    const std::string& hrp = dinero::HrpForActiveNetworkRef();
    dinero::WitnessAddressInfo info = dinero::DecodeWitnessAddress(address, hrp);

    result["isvalid"] = info.is_valid;
    result["address"] = address;
    result["ismine"] = false;

    if (info.is_valid) {
        auto to_hex = [](const std::vector<uint8_t>& v) {
            std::string out;
            out.reserve(v.size() * 2);
            static constexpr char kHex[] = "0123456789abcdef";
            for (uint8_t b : v) { out.push_back(kHex[(b >> 4) & 0xF]); out.push_back(kHex[b & 0xF]); }
            return out;
        };
        result["iswitness"] = info.is_witness;
        result["isscript"] = (info.witness_version == 0 && info.witness_program.size() == 32);
        result["witness_version"] = info.witness_version;
        result["witness_program"] = to_hex(info.witness_program);
        // For taproot this is 5120<32-byte program>.
        result["scriptPubKey"] = to_hex(info.script_pubkey);
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (wallet_service && wallet_service->hasActiveWallet()) {
        try {
            result["ismine"] = wallet_service->get().isAddressMine(address);
        } catch (...) {
            result["ismine"] = false;
        }
    }

    return result;
}

/**
 * wallet.lock - Lock encrypted wallet
 */
din::Json rpc_context_wallet_lock(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        wallet_service->get().lockWallet();
        result["success"] = true;
        if (ctx.logger) {
            ctx.logger->info("[wallet.lock] Wallet locked");
        }
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to lock wallet: ") + e.what();
        if (ctx.logger) {
            ctx.logger->error("[wallet.lock] Failed: " + std::string(e.what()));
        }
    }

    return result;
}

/**
 * wallet.unlock - Unlock encrypted wallet
 */
din::Json rpc_context_wallet_unlock(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.unlock <password> [timeout_seconds]";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        std::string passphrase = params[0].as<std::string>();
        int timeout = 0;
        if (params.size() >= 2 && params[1].is<int>()) {
            timeout = params[1].as<int>();
        }

        wallet_service->get().unlockWallet(passphrase, timeout);
        result["success"] = true;
        if (ctx.logger) {
            ctx.logger->info("[wallet.unlock] Wallet unlocked");
        }
    } catch (const std::exception& e) {
        std::string error_text = e.what();
        if (error_text == "Invalid passphrase") {
            error_text = "Invalid password";
        } else if (error_text == "Passphrase cannot be empty") {
            error_text = "Password cannot be empty";
        }
        result["error"] = std::string("Failed to unlock wallet: ") + error_text;
        if (ctx.logger) {
            ctx.logger->error("[wallet.unlock] Failed: " + std::string(e.what()));
        }
    }

    return result;
}

/**
 * wallet.encrypt - Encrypt wallet with passphrase
 */
din::Json rpc_context_wallet_encrypt(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.encrypt <passphrase>";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        std::string passphrase = params[0].as<std::string>();
        wallet_service->get().encryptWallet(passphrase);
        result["success"] = true;
        result["message"] = "Wallet encrypted successfully. Please backup your wallet.";
        if (ctx.logger) {
            ctx.logger->info("[wallet.encrypt] Wallet encrypted");
        }
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to encrypt wallet: ") + e.what();
        if (ctx.logger) {
            ctx.logger->error("[wallet.encrypt] Failed: " + std::string(e.what()));
        }
    }

    return result;
}

/**
 * wallet.passphrasechange - Change wallet passphrase
 */
din::Json rpc_context_wallet_passphrasechange(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.size() < 2 || !params[0].is<std::string>() || !params[1].is<std::string>()) {
        result["error"] = "Usage: wallet.passphrasechange <old_passphrase> <new_passphrase>";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        std::string old_pass = params[0].as<std::string>();
        std::string new_pass = params[1].as<std::string>();
        wallet_service->get().changePassphrase(old_pass, new_pass);
        result["success"] = true;
        if (ctx.logger) {
            ctx.logger->info("[wallet.passphrasechange] Wallet passphrase changed");
        }
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to change passphrase: ") + e.what();
        if (ctx.logger) {
            ctx.logger->error("[wallet.passphrasechange] Failed: " + std::string(e.what()));
        }
    }

    return result;
}

/**
 * wallet.sendtoaddress - Send DIN to address
 *
 * Phase 33.4: Full transaction creation, signing, and broadcast
 */
din::Json rpc_context_wallet_sendtoaddress(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    if (RefuseIfSafeMode(ctx, result)) return result;  // spec Fatal §3

    const auto log_info = [&ctx](const std::string& msg) {
        if (ctx.logger) {
            ctx.logger->info(msg);
        } else {
            dinero::g_logger.info(msg);
        }
    };
    const auto log_debug = [&ctx](const std::string& msg) {
        if (ctx.logger) {
            ctx.logger->debug(msg);
        } else {
            dinero::g_logger.debug(msg);
        }
    };
    const auto log_error = [&ctx](const std::string& msg) {
        if (ctx.logger) {
            ctx.logger->error(msg);
        } else {
            dinero::g_logger.error(msg);
        }
    };

    if (params.size() < 2) {
        result["error"] = "Usage: wallet.sendtoaddress <address> <amount> [fee_rate] [comment]";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    // Check if wallet is locked
    if (wallet_service->get().isWalletLocked()) {
        result["error"] = "Wallet is locked. Use wallet.unlock first.";
        return result;
    }

    // Check chainstate for UTXOs
    if (!ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate_service || !chainstate_service->utxoIndex()) {
        result["error"] = "UTXO index not available";
        return result;
    }

    try {
        // Support both array and object parameter formats
        // Array: [address, amount, fee_rate, comment, broadcast]
        // Object: {"address": "...", "amount": 1.0, "preview": true} for dry-run
        std::string address;
        double amount_din = 0.0;
        double fee_rate = 0.0;  // Will auto-estimate if not provided
        bool fee_auto_estimated = false;
        bool broadcast = true;  // Default: sign and broadcast (production behavior)
        bool allow_unconfirmed_chain = false;  // Opt-in only for chain-building experiments
        std::string override_change_address;  // Phase 6: client-specified change address

        if (params.isObject()) {
            // Object format (named parameters)
            if (!params.isMember("address") || !params["address"].isString()) {
                result["error"] = "Missing or invalid 'address' parameter";
                return result;
            }
            if (!params.isMember("amount") || !params["amount"].isNumeric()) {
                result["error"] = "Missing or invalid 'amount' parameter";
                return result;
            }
            address = params["address"].asString();
            amount_din = params["amount"].asDouble();

            if (params.isMember("fee_rate") && params["fee_rate"].isNumeric()) {
                fee_rate = params["fee_rate"].asDouble();
            }
            if (params.isMember("preview") && params["preview"].isBool()) {
                broadcast = !params["preview"].asBool();
            } else if (params.isMember("test_mode") && params["test_mode"].isBool()) {
                broadcast = params["test_mode"].asBool();  // backward compat
            }
            if (params.isMember("allow_unconfirmed_chain") && params["allow_unconfirmed_chain"].isBool()) {
                allow_unconfirmed_chain = params["allow_unconfirmed_chain"].asBool();
            }
            // Phase 6: Optional client-specified change address
            if (params.isMember("change_address") && params["change_address"].isString()) {
                override_change_address = params["change_address"].asString();
            }
        } else if (params.isArray() && params.size() >= 2) {
            // Array format (positional parameters)
            address = params[0].asString();
            amount_din = params[1].asDouble();

            if (params.size() >= 3 && params[2].isNumeric()) {
                fee_rate = params[2].asDouble();
            }
            // Skip params[3] (comment) - not used
            if (params.size() >= 5 && params[4].isBool()) {
                broadcast = params[4].asBool();  // positional backward compat
            }
            if (params.size() >= 6 && params[5].isBool()) {
                allow_unconfirmed_chain = params[5].asBool();
            }
        } else {
            result["error"] = "Invalid parameters. Use array [address, amount, ...] or object {address, amount, ...}";
            return result;
        }

        // Phase 34a: Apply stored wallet.txfee_din_kb setting if no explicit fee_rate given
        if (fee_rate <= 0.0) {
            std::string stored_fee = wallet_service->get().getSetting("wallet.txfee_din_kb");
            if (!stored_fee.empty()) {
                try {
                    double din_kb = std::stod(stored_fee);
                    if (din_kb > 0.0) {
                        // Convert DIN/KB → una/byte (matches mempool lane-weight units)
                        // 1 DIN/KB = 1e8 una / 1000 bytes = 1e5 una/byte
                        fee_rate = din_kb * 1e8 / 1000.0;
                    }
                } catch (const std::exception& e) {
                    log_debug("[wallet.sendtoaddress] wallet.txfee_din_kb parse error: " + std::string(e.what()));
                }
            }
        }

        // Phase 34b: Auto-estimate fee from mempool if not provided
        if (fee_rate <= 0.0 && ctx.daemon->mempool) {
            auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
            if (mempool_service) {
                auto fee_estimator = mempool_service->getFeeEstimator();
                if (fee_estimator) {
                    // Use NORMAL target (6 blocks)
                    auto estimate = fee_estimator->estimateFee(dinero::policy::FeeTarget::NORMAL);
                    if (estimate.is_sufficient_data && estimate.fee_rate > 0) {
                        fee_rate = static_cast<double>(estimate.fee_rate) / 1000.0;  // Convert una/kB to una/vB
                        fee_auto_estimated = true;
                        log_debug("[wallet.sendtoaddress] Auto-estimated fee: " +
                                  std::to_string(fee_rate) + " una/vB (confidence: " +
                                  std::to_string(estimate.confidence * 100) + "%)");
                    }
                }

                // Fallback: use mempool average
                if (fee_rate <= 0.0) {
                    auto stats = mempool_service->mempool().getStats();
                    if (stats.avg_fee_rate > 0) {
                        fee_rate = stats.avg_fee_rate;
                        fee_auto_estimated = true;
                    }
                }
            }
        }

        // Final fallback: minimum relay fee (1.0 una/byte, matching mempool lane-weight check)
        if (fee_rate <= 0.0) {
            fee_rate = 1.0;  // 1 una/byte minimum
        }

        if (amount_din <= 0) {
            result["error"] = "Invalid amount: must be positive";
            return result;
        }

        if (!broadcast) {
            log_info("[wallet.sendtoaddress] Preview mode (preview=true)");
        }

        // Convert DIN to una (1 DIN = 1e8 una, 8 decimals like Bitcoin)
        int64_t amount_una = static_cast<int64_t>(amount_din * 1e8);

        // Get active network HRP
        const std::string& hrp = dinero::HrpForActiveNetworkRef();

        // Validate address format. DecodeAddressAuto handles P2WPKH, P2WSH,
        // P2TR. DecodeP2MRAddress handles witness v3 (BIP-360). Try both.
        {
            bool addr_ok = false;
            try {
                auto parsed = dinero::DecodeAddressAuto(address);
                addr_ok = dinero::IsValidDestination(parsed.dest);
            } catch (...) {}
            if (!addr_ok) {
                addr_ok = dinero::wallet::DecodeP2MRAddress(address).has_value();
            }
            if (!addr_ok) {
                result["error"] = "Invalid address: unsupported format";
                return result;
            }
        }

        log_info("[wallet.sendtoaddress] Preparing transaction to " + address +
                 " for " + std::to_string(amount_din) + " DIN");

        // Wait for the wallet worker to process blocks up to the chain tip.
        // The worker runs asynchronously — without this gate, a UTXO mined
        // in the latest block may not be in the wallet's utxos table yet,
        // causing "insufficient funds" or stale balances. The 5-second
        // timeout is generous; regtest blocks process in <1 ms each.
        if (ctx.daemon->chainstate) {
            auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
            if (cs) {
                const uint32_t chain_tip = cs->getBlockHeight();
                wallet_service->get().WaitForHeight(chain_tip, std::chrono::milliseconds(5000));
            }
        }

        // Get wallet UTXOs
        auto utxos = wallet_service->get().listUnspentUTXOs(1, 9999999);
        if (utxos.empty()) {
            result["error"] = "No confirmed UTXOs available";
            return result;
        }

        size_t mempool_spent_filtered = 0;
        size_t locked_filtered = 0;
        size_t live_forest_filtered = 0;

        // STEP 3: Filter out mempool-spent UTXOs (fix wallet infrastructure for honest policy testing)
        // Without this filter, wallet reuses UTXOs that are already spent in mempool
        auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
        if (mempool_service) {
            const auto& mempool = mempool_service->mempool();

            // Remove spent UTXOs
            utxos.erase(
                std::remove_if(utxos.begin(), utxos.end(),
                    [&mempool, &mempool_spent_filtered](const dinero::WalletManager::WalletUTXO& utxo) {  // Phase M.3: WalletUTXO
                        // Check if this UTXO is spent in mempool (Phase M.4: OutPoint takes TxId)
                        OutPoint outpoint(dinero::TxId(uint256::FromHexUnsafe(utxo.txid)), utxo.vout);
                        const bool spent_in_mempool = mempool.isOutputSpentInMempool(outpoint);
                        if (spent_in_mempool) {
                            ++mempool_spent_filtered;
                        }
                        return spent_in_mempool;
                    }),
                utxos.end()
            );

            // Phase 35.3: Remove locked UTXOs from selection
            utxos.erase(
                std::remove_if(utxos.begin(), utxos.end(),
                    [&wallet_service, &locked_filtered](const dinero::WalletManager::WalletUTXO& utxo) {  // Phase M.3: WalletUTXO
                        const bool locked = wallet_service->get().isUTXOLocked(utxo.txid, utxo.vout);
                        if (locked) {
                            ++locked_filtered;
                        }
                        return locked;
                    }),
                utxos.end()
            );

            // STEP 3.2: Optionally add mempool-created UTXOs for explicit
            // chain-building experiments. Real sends should default to confirmed
            // chain-backed inputs so a live unconfirmed tx graph cannot silently
            // absorb fresh payments.
            if (broadcast && allow_unconfirmed_chain) {
                // Phase M.0: Get txids directly as uint256 (never string→uint256 conversion)
                auto mempool_txids = mempool.getTransactionIds();
                size_t added_outputs = 0;
                std::unordered_map<std::string, std::string> script_to_path;

                // Build scriptPubKey -> BIP32 path lookup from wallet metadata.
                for (const auto& row : wallet_service->get().listAddresses(false)) {
                    if (row.script_pubkey.empty()) continue;

                    int purpose = 86;  // Taproot default
                    if (row.type == "p2wpkh" || row.type == "legacy") {
                        purpose = 84;
                    }

                    std::string path = BuildStandardDerivationPath(
                        static_cast<uint32_t>(purpose),
                        row.account,
                        row.change,
                        row.index);
                    script_to_path[row.script_pubkey] = path;
                }

                for (const auto& txid : mempool_txids) {
                    // Phase M.0: Get transaction using uint256 txid
                    auto tx_ptr = mempool.getTransaction(txid);
                    if (!tx_ptr) continue;
                    const auto& tx = *tx_ptr;

                    for (size_t vout = 0; vout < tx.vout.size(); ++vout) {
                        // Phase M.4: OutPoint takes TxId, not uint256
                        OutPoint outpoint(dinero::TxId(txid), static_cast<uint32_t>(vout));
                        if (mempool.isOutputSpentInMempool(outpoint)) {
                            continue;
                        }

                        // Only include mempool outputs we can sign:
                        // enforce script ownership and carry script identity forward.
                        const auto& txout = tx.vout[vout];
                        std::string script_pubkey_hex = dinero::TransactionSerializer::ToHex(txout.scriptPubKey);
                        if (script_pubkey_hex.empty() || !wallet_service->get().isScriptMine(script_pubkey_hex)) {
                            continue;
                        }
                        auto path_it = script_to_path.find(script_pubkey_hex);
                        if (path_it == script_to_path.end()) {
                            if (ctx.logger) {
                                ctx.logger->debug("[TEST_ONLY] Skipping mempool UTXO without known path: " +
                                                txid.GetHex().substr(0, 16) + ":" + std::to_string(vout));
                            }
                            continue;
                        }

                        dinero::WalletManager::WalletUTXO utxo;  // Phase M.3: WalletUTXO
                        utxo.txid = txid.GetHex();  // Convert to string ONLY at wallet boundary
                        utxo.vout = static_cast<uint32_t>(vout);
                        utxo.amount_una = txout.value.GetUna();  // Phase M.6.2
                        utxo.amount_din = static_cast<double>(utxo.amount_una) / 1e8;
                        utxo.spendable = true;
                        utxo.is_mature = true;  // Mempool outputs immediately spendable
                        utxo.confirmations = 0; // Unconfirmed
                        utxo.is_coinbase = false;
                        utxo.is_spent = false;
                        utxo.height = 0;
                        utxo.address = "";
                        utxo.script_pubkey = script_pubkey_hex;
                        utxo.derivation_path = path_it->second;
                        utxo.is_confidential = txout.is_confidential;
                        utxo.label = "mempool";

                        utxos.push_back(utxo);
                        ++added_outputs;
                    }
                }

                if (ctx.logger) {
                    ctx.logger->debug("[TEST_ONLY] Added " + std::to_string(added_outputs) +
                                    " signable mempool outputs to UTXO set for chain building");
                }
            } else if (ctx.logger && broadcast) {
                ctx.logger->debug("[wallet.sendtoaddress] Using confirmed chain-backed UTXOs only");
            }
        }

        utxos.erase(
            std::remove_if(utxos.begin(), utxos.end(),
                [&](const dinero::WalletManager::WalletUTXO& utxo) {
                    if (!utxo.spendable || !utxo.is_mature || utxo.is_confidential) {
                        return false;
                    }

                    std::string forest_reason;
                    if (WalletUtxoIsPresentInLiveUtreexoForest(utxo, chainstate_service, &forest_reason)) {
                        return false;
                    }

                    ++live_forest_filtered;
                    log_debug("[wallet.sendtoaddress] Skipping UTXO absent from live Utreexo forest: " +
                              utxo.txid.substr(0, 16) + ":" + std::to_string(utxo.vout) +
                              " (" + forest_reason + ")");
                    return true;
                }),
            utxos.end()
        );

        if (utxos.empty()) {
            std::ostringstream error;
            error << "No available transparent mature UTXOs after filtering";
            if (live_forest_filtered > 0 || mempool_spent_filtered > 0 || locked_filtered > 0) {
                error << " (missing_from_live_forest=" << live_forest_filtered
                      << ", spent_in_mempool=" << mempool_spent_filtered
                      << ", locked=" << locked_filtered << ")";
            }
            result["error"] = error.str();
            return result;
        }

        // Calculate total available balance for the active transparent wallet surface.
        int64_t total_available = 0;
        for (const auto& utxo : utxos) {
            if (utxo.spendable && utxo.is_mature && !utxo.is_confidential) {
                total_available += static_cast<int64_t>(utxo.amount_din * 1e8);
            }
        }

        if (total_available < amount_una) {
            result["error"] = "Insufficient funds. Available: " +
                            std::to_string(static_cast<double>(total_available) / 1e8) + " DIN";
            return result;
        }

        // Phase M.3: RPC boundary - convert WalletManager RPC type to wallet canonical type
        std::vector<dinero::CanonicalWalletUTXO> available_utxos;
        for (const auto& utxo : utxos) {
            if (!utxo.spendable || !utxo.is_mature) continue;
            // Retired legacy private-lane outputs are not spendable through the
            // active transparent wallet flow.
            if (utxo.is_confidential) continue;
            if (utxo.derivation_path.empty()) {
                log_debug("[wallet.sendtoaddress] Skipping UTXO without derivation path: " +
                          utxo.txid.substr(0, 16) + ":" + std::to_string(utxo.vout));
                continue;
            }

            dinero::CanonicalWalletUTXO converted;
            converted.txid = dinero::uint256::FromHexUnsafe(utxo.txid);
            converted.vout = utxo.vout;
            converted.value = dinero::AmountUna::Una(utxo.amount_una);  // Phase M.6.2
            converted.path = utxo.derivation_path;  // BIP32 path for key derivation
            converted.height = utxo.height;
            converted.is_coinbase = utxo.is_coinbase;

            // Convert hex script_pubkey to binary spk
            if (!utxo.script_pubkey.empty()) {
                std::string hex = utxo.script_pubkey;
                converted.spk.reserve(hex.size() / 2);
                for (size_t i = 0; i < hex.size(); i += 2) {
                    uint8_t byte = static_cast<uint8_t>(
                        std::stoi(hex.substr(i, 2), nullptr, 16)
                    );
                    converted.spk.push_back(byte);
                }
            }

            available_utxos.push_back(converted);
        }

        if (available_utxos.empty()) {
            result["error"] = "No spendable UTXOs available";
            return result;
        }

        // Phase M.3: CoinSelector now uses CanonicalWalletUTXO directly (no conversion needed)
        // Use frozen CoinSelector engine (BnB + privacy heuristics)
        // num_outputs = 2 (payment + potential change) matches current inline behavior
        auto coin_result = dinero::CoinSelector::SelectCoins(
            available_utxos,
            static_cast<uint64_t>(amount_una),
            static_cast<uint64_t>(fee_rate),
            2  // num_outputs: payment + potential change (matches inline code: 31 * 2)
        );

        if (!coin_result.success) {
            result["error"] = coin_result.error;
            return result;
        }

        // Convert selected coins back to WalletManager::WalletUTXO format
        std::vector<dinero::WalletManager::WalletUTXO> selected_utxos;
        for (const auto& coin : coin_result.selected_coins) {
            // Find original UTXO from WalletManager format
            for (const auto& orig_utxo : utxos) {
                if (orig_utxo.txid == coin.txid.GetHex() &&  // Phase M.3: uint256.GetHex()
                    orig_utxo.vout == coin.vout) {
                    selected_utxos.push_back(orig_utxo);
                    break;
                }
            }
        }

        int64_t selected_total = coin_result.total_value;
        int64_t estimated_fee = coin_result.fee;
        int64_t change_amount = coin_result.change_amount;

        // Phase 6: Use client-specified change address if provided, otherwise derive
        std::string change_address;
        if (!override_change_address.empty()) {
            change_address = override_change_address;
            if (ctx.logger) {
                ctx.logger->debug("[wallet.sendtoaddress] Using client-specified change address: " + change_address.substr(0, 20) + "...");
            }
        } else {
            change_address = wallet_service->get().getNewAddress("change", "taproot");
        }
        if (change_address.empty()) {
            // Fallback: use recipient address for change
            change_address = address;
            if (ctx.logger) {
                ctx.logger->debug("[wallet.sendtoaddress] Using recipient address for change (no change address derived)");
            }
        }

        // Get private keys for selected UTXOs (needed for signing in both normal and test mode)
        std::map<std::string, std::string> private_keys;
        for (const auto& utxo : selected_utxos) {
            std::string script_pubkey = utxo.script_pubkey;

            // Legacy safety net: backfill scriptPubKey from address metadata.
            if (script_pubkey.empty() && !utxo.address.empty()) {
                auto spk_opt = wallet_service->get().getScriptPubKeyForAddress(utxo.address);
                if (spk_opt.has_value() && !spk_opt->empty()) {
                    script_pubkey = *spk_opt;
                    log_debug("[wallet.sendtoaddress] Recovered scriptPubKey from address metadata for " +
                              utxo.txid.substr(0, 16) + ":" + std::to_string(utxo.vout));
                }
            }

            // Phase 34.3: Direct scriptPubKey → private key lookup
            // ⚠️ CRITICAL FIX: Use deriveKeyForScriptPubKey() (same as PSBT/raw tx signing)
            // Replaces legacy listAddresses() approach which fails for mempool-created UTXOs
            if (script_pubkey.empty()) {
                log_error("📤 ❌ UTXO has empty scriptPubKey: " + utxo.txid + ":" + std::to_string(utxo.vout));
                continue;
            }

            // Phase 10: P2MR (witness v3) inputs have no ECDSA private key.
            // The signing secret is a PQ seed resolved by WalletKeyProvider at
            // sign-time via the V7P2MRStore. Skip legacy key derivation here —
            // deriveKeyForScriptPubKey can't decode a 0x53 0x20 || merkle_root
            // script and would log a spurious error for every P2MR coin.
            {
                std::vector<uint8_t> spk_bytes;
                spk_bytes.reserve(script_pubkey.size() / 2);
                for (std::size_t i = 0; i + 1 < script_pubkey.size(); i += 2) {
                    spk_bytes.push_back(static_cast<uint8_t>(
                        std::stoi(script_pubkey.substr(i, 2), nullptr, 16)));
                }
                if (dinero::consensus::pq::IsP2MRScript(spk_bytes)) {
                    continue;
                }
            }

            // Direct scriptPubKey → private key resolution (Bitcoin Core semantics)
            auto privkey_bytes = wallet_service->get().deriveKeyForScriptPubKey(script_pubkey);
            if (privkey_bytes.has_value() && !privkey_bytes->empty()) {
                // Convert bytes to hex string for compatibility with existing signing code
                std::ostringstream priv_key_hex;
                for (uint8_t byte : privkey_bytes.value()) {
                    priv_key_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
                }
                // Store by derivation_path (not address) - TransactionSigner
                // looks up keys by utxo.path which is now the BIP32 derivation path
                private_keys[utxo.derivation_path] = priv_key_hex.str();
                log_info("📤 ✅ Retrieved private key for scriptPubKey: " + script_pubkey.substr(0, 16) + "...");
            } else {
                // Fallback path: if script-based path lookup is incomplete, derive by known selected path.
                if (!utxo.derivation_path.empty()) {
                    std::string priv_key_hex = wallet_service->get().getPrivateKeyForPath(utxo.derivation_path);
                    if (!priv_key_hex.empty()) {
                        private_keys[utxo.derivation_path] = priv_key_hex;
                        log_info("📤 ✅ Retrieved private key via derivation path fallback: " +
                                 utxo.derivation_path);
                        continue;
                    }
                }
                log_error("📤 ❌ Could not derive key for scriptPubKey/path: " + script_pubkey +
                          " / " + utxo.derivation_path);
            }
        }

        // Phase 10: empty private_keys is OK if every selected UTXO is P2MR —
        // those are signed via WalletKeyProvider::SignP2MR (PQ seed, not
        // secp256k1 private key). Only fail if we have selected UTXOs that
        // need ECDSA keys AND we couldn't derive any.
        if (private_keys.empty() && broadcast) {
            bool has_non_p2mr = false;
            for (const auto& utxo : selected_utxos) {
                const std::string& spkhex = utxo.script_pubkey;
                const bool is_p2mr = (spkhex.length() == 68 && spkhex.rfind("5320", 0) == 0);
                if (!is_p2mr) { has_non_p2mr = true; break; }
            }
            if (has_non_p2mr) {
                log_error("[wallet.sendtoaddress] No private keys available after script and path derivation attempts");
                result["error"] = "Could not retrieve private keys for signing";
                return result;
            }
        }

        // ========================================================================
        // Broadcast mode: Build, sign, and submit transaction
        // ========================================================================
        if (broadcast) {
            // Phase M.3: RPC boundary - convert to canonical wallet UTXO
            std::vector<dinero::CanonicalWalletUTXO> utxos_for_builder;
            for (const auto& utxo : selected_utxos) {
                dinero::CanonicalWalletUTXO converted;
                converted.txid = dinero::uint256::FromHexUnsafe(utxo.txid);
                converted.vout = utxo.vout;
                converted.value = dinero::AmountUna::Una(utxo.amount_una);  // Phase M.6.2
                converted.path = utxo.derivation_path;  // BIP32 path for key derivation
                converted.height = utxo.height;
                converted.is_coinbase = utxo.is_coinbase;

                // Convert hex script_pubkey to binary spk
                if (!utxo.script_pubkey.empty()) {
                    std::string hex = utxo.script_pubkey;
                    converted.spk.reserve(hex.size() / 2);
                    for (size_t i = 0; i < hex.size(); i += 2) {
                        uint8_t byte = static_cast<uint8_t>(
                            std::stoi(hex.substr(i, 2), nullptr, 16)
                        );
                        converted.spk.push_back(byte);
                    }
                }

                utxos_for_builder.push_back(converted);
            }

            // Step 1: Build unsigned transaction using UnsignedTxBuilder
            dinero::BuildOptions build_opts;
            build_opts.fee_rate = static_cast<uint64_t>(fee_rate);
            build_opts.enable_rbf = true;
            build_opts.change_address = change_address;

            std::vector<dinero::TxOutputRequest> outputs;
            outputs.push_back(dinero::TxOutputRequest(address, amount_una));

            auto build_result = dinero::UnsignedTxBuilder::Build(utxos_for_builder, outputs, build_opts);
            if (!build_result.success) {
                result["error"] = "Failed to build transaction: " + build_result.error;
                return result;
            }

            // Step 2: Sign transaction using TransactionSigner + existing key infrastructure
            // Build key provider from selected UTXOs
            // NOTE: Keys are indexed by derivation_path because TransactionSigner
            // looks up keys using utxo.path (which is now the BIP32 derivation path)
            std::map<std::string, std::string> path_to_key;
            for (const auto& utxo : selected_utxos) {
                // Get private key for this derivation path
                std::string priv_key_hex = private_keys[utxo.derivation_path];
                if (!priv_key_hex.empty()) {
                    path_to_key[utxo.derivation_path] = priv_key_hex;
                }
            }

            // Phase 10: if any selected UTXO is P2MR (witness v3), we must
            // use the hybrid provider that knows how to resolve v7 seeds
            // and produce ML-DSA-65 signatures. Otherwise the legacy
            // MapKeyProvider is sufficient (and cheaper — no store open).
            bool any_p2mr = false;
            for (const auto& cu : utxos_for_builder) {
                if (dinero::consensus::pq::IsP2MRScript(cu.spk)) {
                    any_p2mr = true;
                    break;
                }
            }

            std::unique_ptr<dinero::KeyProvider> key_provider_holder;
            std::unique_ptr<dinero::wallet::V7P2MRStore> p2mr_store_holder;

            if (any_p2mr) {
                // Wallet must be unlocked: v7 signing needs the AEAD master
                // key to decrypt the stored seed.
                auto master_opt = wallet_service->get().GetV7PqMasterKey();
                if (!master_opt) {
                    result["error"] = "Cannot spend P2MR coin: wallet locked or v7 master key unavailable";
                    return result;
                }
                const std::string store_path = wallet_service->get().GetV7P2MRStorePath();
                if (store_path.empty()) {
                    result["error"] = "Cannot spend P2MR coin: v7 P2MR store path not configured";
                    return result;
                }
                p2mr_store_holder = std::make_unique<dinero::wallet::V7P2MRStore>();
                if (p2mr_store_holder->Open(store_path) != dinero::wallet::V7P2MRStore::OpenResult::Ok) {
                    result["error"] = "Cannot spend P2MR coin: failed to open v7 P2MR store";
                    return result;
                }

                dinero::wallet::WalletKeyProvider::Config cfg;
                cfg.legacy_keys_by_path = path_to_key;
                cfg.p2mr_store          = p2mr_store_holder.get();
                cfg.wallet_id           = 1;  // single-wallet today, matches v7 RPC handlers
                std::memcpy(cfg.master_key.data(), master_opt->data(), cfg.master_key.size());
                // Scrub the caller-side copy after stamping into cfg.
                OPENSSL_cleanse(const_cast<uint8_t*>(master_opt->data()), master_opt->size());

                key_provider_holder = std::make_unique<dinero::wallet::WalletKeyProvider>(std::move(cfg));
            } else {
                key_provider_holder = std::make_unique<dinero::MapKeyProvider>(path_to_key);
            }

            auto sign_result = dinero::TransactionSigner::Sign(build_result.unsigned_tx, *key_provider_holder);

            if (!sign_result.success) {
                result["error"] = "Failed to sign transaction: " + sign_result.error;
                return result;
            }

            // Step 3: Submit to mempool
            if (!ctx.daemon->mempool) {
                result["error"] = "Mempool service not available";
                return result;
            }

            auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
            if (!mempool_service) {
                result["error"] = "Mempool service unavailable";
                return result;
            }

            const dinero::Transaction& signed_tx = sign_result.signed_tx.tx;

            if (ctx.logger) {
                ctx.logger->info("[wallet.sendtoaddress] Submitting signed TX: " + signed_tx.GetTxid().AsUint256().GetHex().substr(0, 16) + "...");
            }

            // Submit to mempool WITH relay (broadcast to network)
            auto submit_result = mempool_service->mempool().submitTransaction(signed_tx, "rpc:wallet.sendtoaddress", true);

            result["status"] = "signed_and_submitted";
            result["accepted"] = submit_result.accepted();
            if (submit_result.rejected()) {
                result["reject_code"] = TxRejectCodeToString(submit_result.code);
                result["reject_reason"] = submit_result.message;
            }
            std::string sent_txid = signed_tx.GetTxid().AsUint256().GetHex();
            double fee_din = static_cast<double>(sign_result.signed_tx.fee) / 1e8;

            result["txid"] = sent_txid;
            result["amount"] = amount_din;
            result["estimated_fee"] = fee_din;
            result["fee_rate"] = fee_rate;
            result["inputs"] = static_cast<int>(selected_utxos.size());
            result["outputs"] = static_cast<int>(signed_tx.vout.size());

            // Phase 6: Enhanced response for client-side tracking
            result["change_address"] = change_address;
            result["change_amount_una"] = static_cast<int64_t>(change_amount);
            result["fee_paid_una"] = static_cast<int64_t>(sign_result.signed_tx.fee);
            {
                din::Json inputs_arr = din::arr();
                for (const auto& utxo : selected_utxos) {
                    din::Json inp;
                    inp["txid"] = utxo.txid;
                    inp["vout"] = static_cast<int>(utxo.vout);
                    inp["amount_una"] = static_cast<int64_t>(utxo.amount_una);
                    inputs_arr.append(inp);
                }
                result["selected_inputs"] = inputs_arr;
            }

            // Record "send" entry in wallet transaction history
            if (submit_result.accepted()) {
                int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                const bool history_recorded = wallet_service->get().addTransaction(
                    sent_txid, address, -(amount_din + fee_din),
                    "send", false, "", now, 0);
                if (!history_recorded) {
                    result["history_warning"] = "Transaction broadcast succeeded but wallet send history entry could not be recorded";
                    if (ctx.logger) {
                        ctx.logger->warning("[wallet.sendtoaddress] Broadcast succeeded but addTransaction(send) failed for tx " + sent_txid);
                    }
                }
            }

            if (ctx.logger) {
                ctx.logger->info("[wallet.sendtoaddress] TX " + sent_txid +
                               " - accepted=" + (submit_result.accepted() ? "true" : "false"));
            }

            return result;
        }

        // Preview mode (preview=true)
        result["status"] = "preview";
        result["note"] = "Preview mode - omit preview=true to sign and broadcast";
        result["amount"] = amount_din;
        result["estimated_fee"] = static_cast<double>(estimated_fee) / 1e8;
        result["fee_rate"] = fee_rate;
        result["fee_auto_estimated"] = fee_auto_estimated;
        result["inputs"] = static_cast<int>(selected_utxos.size());
        result["outputs"] = 2;  // Payment + change
        result["to"] = address;

        if (change_amount > 546) {
            result["change"] = static_cast<double>(change_amount) / 1e8;
            result["change_address"] = change_address;
        }

        result["available_keys"] = static_cast<int>(private_keys.size());

        if (ctx.logger) {
            ctx.logger->info("[wallet.sendtoaddress] Preview: " + std::to_string(amount_din) +
                           " DIN to " + address);
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Send failed: ") + e.what();
        if (ctx.logger) {
            ctx.logger->error("[wallet.sendtoaddress] Exception: " + std::string(e.what()));
        }
    }

    return result;
}

/**
 * wallet.sendmany - Send to multiple addresses in a single transaction
 *
 * Phase 33.4: Batch payments support
 */
din::Json rpc_context_wallet_sendmany(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    if (RefuseIfSafeMode(ctx, result)) return result;  // spec Fatal §3

    // params[0] = { "address1": amount1, "address2": amount2, ... }
    // params[1] = optional fee_rate
    if (params.empty() || !params[0].isObject()) {
        result["error"] = "Usage: wallet.sendmany {\"address1\": amount1, \"address2\": amount2, ...} [fee_rate]";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    if (wallet_service->get().isWalletLocked()) {
        result["error"] = "Wallet is locked. Use wallet.unlock first.";
        return result;
    }

    if (!ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate_service || !chainstate_service->utxoIndex()) {
        result["error"] = "UTXO index not available";
        return result;
    }

    try {
        din::Json recipients_obj = params[0];
        double fee_rate = 1.0;

        if (params.size() >= 2 && params[1].is<double>()) {
            fee_rate = params[1].as<double>();
        }

        // Parse recipients using getMemberNames() for jsoncpp iteration
        std::vector<dinero::TransactionBuilder::Recipient> recipients;
        int64_t total_amount = 0;

        auto member_names = recipients_obj.getMemberNames();
        for (const auto& address : member_names) {
            double amount_din = recipients_obj[address].as<double>();

            if (amount_din <= 0) {
                result["error"] = "Invalid amount for address: " + address;
                return result;
            }

            int64_t amount_una = static_cast<int64_t>(amount_din * 1e8);
            recipients.push_back({address, amount_una});
            total_amount += amount_una;
        }

        if (recipients.empty()) {
            result["error"] = "No recipients specified";
            return result;
        }

        if (ctx.logger) {
            ctx.logger->info("[wallet.sendmany] Preparing batch transaction to " +
                           std::to_string(recipients.size()) + " recipients");
        }

        // Get wallet UTXOs — same path as sendtoaddress.
        // listUnspentUTXOs now excludes immature coinbase outputs (Bug Fix 1) and
        // stale/spent UTXOs not present in the chain UTXO index (Bug Fix 2).
        auto utxos = wallet_service->get().listUnspentUTXOs(1, 9999999);
        if (utxos.empty()) {
            result["error"] = "No confirmed UTXOs available";
            return result;
        }

        size_t sendmany_mempool_spent_filtered = 0;
        size_t sendmany_locked_filtered = 0;
        size_t sendmany_forest_filtered = 0;

        // Filter out mempool-spent and locked UTXOs (mirrors sendtoaddress STEP 3).
        auto mempool_service_sm = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
        if (mempool_service_sm) {
            const auto& mempool_sm = mempool_service_sm->mempool();
            utxos.erase(
                std::remove_if(utxos.begin(), utxos.end(),
                    [&mempool_sm, &sendmany_mempool_spent_filtered](const dinero::WalletManager::WalletUTXO& u) {
                        OutPoint op(dinero::TxId(uint256::FromHexUnsafe(u.txid)), u.vout);
                        const bool spent_in_mempool = mempool_sm.isOutputSpentInMempool(op);
                        if (spent_in_mempool) {
                            ++sendmany_mempool_spent_filtered;
                        }
                        return spent_in_mempool;
                    }),
                utxos.end()
            );
            utxos.erase(
                std::remove_if(utxos.begin(), utxos.end(),
                    [&wallet_service, &sendmany_locked_filtered](const dinero::WalletManager::WalletUTXO& u) {
                        const bool locked = wallet_service->get().isUTXOLocked(u.txid, u.vout);
                        if (locked) {
                            ++sendmany_locked_filtered;
                        }
                        return locked;
                    }),
                utxos.end()
            );
        }

        // Bug Fix 3: Derive private keys using deriveKeyForScriptPubKey() (same as
        // sendtoaddress Phase 34.3) instead of the old listAddresses() loop.
        // The old loop silently skipped UTXOs for which the wallet has no key,
        // causing "Missing private key for UTXO" failures during signing.
        // We also enforce the spendable + is_mature gate here so non-spendable
        // UTXOs are never passed to the transaction builder.
        // Bug Fix 4: Build candidate_utxos in parallel so the builder uses EXACTLY
        // the same pre-filtered set (avoids "Missing private key" when builder selects
        // a stale/CT UTXO from UTXOIndex that wasn't included in private_keys).
        std::map<std::string, std::string> private_keys;
        std::vector<dinero::CanonicalWalletUTXO> candidate_utxos;
        int signable_count = 0;
        for (const auto& utxo : utxos) {
            // Skip non-spendable (immature, out-of-confirmation-range, etc.)
            if (!utxo.spendable || !utxo.is_mature) continue;
            // Skip CT outputs — transparent sendmany cannot spend them
            if (utxo.is_confidential) continue;

            std::string forest_reason;
            if (!WalletUtxoIsPresentInLiveUtreexoForest(utxo, chainstate_service, &forest_reason)) {
                ++sendmany_forest_filtered;
                if (ctx.logger) {
                    ctx.logger->debug("[wallet.sendmany] Skipping UTXO absent from live Utreexo forest: " +
                                      utxo.txid.substr(0, 16) + ":" + std::to_string(utxo.vout) +
                                      " (" + forest_reason + ")");
                }
                continue;
            }

            // Skip UTXOs with no derivation path — we cannot sign for them.
            if (utxo.derivation_path.empty()) {
                if (ctx.logger) {
                    ctx.logger->debug("[wallet.sendmany] Skipping UTXO without derivation path: " +
                                      utxo.txid.substr(0, 16) + ":" + std::to_string(utxo.vout));
                }
                continue;
            }

            if (utxo.script_pubkey.empty()) {
                if (ctx.logger) {
                    ctx.logger->debug("[wallet.sendmany] Skipping UTXO with empty scriptPubKey: " +
                                      utxo.txid.substr(0, 16) + ":" + std::to_string(utxo.vout));
                }
                continue;
            }

            // Direct scriptPubKey → private key resolution (Bitcoin Core semantics).
            auto privkey_bytes = wallet_service->get().deriveKeyForScriptPubKey(utxo.script_pubkey);
            if (!privkey_bytes.has_value() || privkey_bytes->empty()) {
                // Fall back to the explicit derivation path for older wallet state
                // where direct scriptPubKey lookup is incomplete but ownership is known.
                std::string fallback_privkey = wallet_service->get().getPrivateKeyForPath(utxo.derivation_path);
                if (!fallback_privkey.empty()) {
                    private_keys[utxo.derivation_path] = fallback_privkey;
                } else {
                    // This UTXO belongs to a watch-only or foreign script — skip it.
                    if (ctx.logger) {
                        ctx.logger->debug("[wallet.sendmany] Skipping UTXO with no signing key: " +
                                          utxo.txid.substr(0, 16) + ":" + std::to_string(utxo.vout));
                    }
                    continue;
                }
            } else {
                // Convert raw bytes to hex string for the signing infrastructure.
                std::ostringstream priv_key_hex;
                for (uint8_t byte : privkey_bytes.value()) {
                    priv_key_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
                }
                private_keys[utxo.derivation_path] = priv_key_hex.str();
            }

            // Build CanonicalWalletUTXO for coin selection (mirrors sendtoaddress).
            // listUnspentUTXOs() is already filtered by the wallet's canonical view
            // of spendable confirmed outputs, so we intentionally do not hard-reject
            // coins that happen to be missing from UTXOPositionIndex metadata on
            // long-lived wallets. Live EpycOne testing on Apr 15 2026 showed that
            // this extra gate could discard valid mature coinbase UTXOs and make
            // sendmany unusable even though wallet.sendtoaddress still worked.
            if (!utxo.script_pubkey.empty()) {
                dinero::CanonicalWalletUTXO canonical;
                canonical.txid = dinero::uint256::FromHexUnsafe(utxo.txid);
                canonical.vout = utxo.vout;
                canonical.value = dinero::AmountUna::Una(utxo.amount_una);
                canonical.path = utxo.derivation_path;
                canonical.height = utxo.height;
                canonical.is_coinbase = utxo.is_coinbase;
                // Convert hex scriptPubKey to binary
                const std::string& hex = utxo.script_pubkey;
                canonical.spk.reserve(hex.size() / 2);
                for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                    canonical.spk.push_back(static_cast<uint8_t>(
                        std::stoi(hex.substr(i, 2), nullptr, 16)));
                }
                candidate_utxos.push_back(std::move(canonical));
            }
            ++signable_count;
        }

        if (signable_count == 0) {
            std::ostringstream error;
            error << "No spendable transparent mature UTXOs with signing keys available";
            if (sendmany_forest_filtered > 0 || sendmany_mempool_spent_filtered > 0 || sendmany_locked_filtered > 0) {
                error << " (missing_from_live_forest=" << sendmany_forest_filtered
                      << ", spent_in_mempool=" << sendmany_mempool_spent_filtered
                      << ", locked=" << sendmany_locked_filtered << ")";
            }
            result["error"] = error.str();
            return result;
        }

        // Build transaction using only our pre-filtered candidate UTXOs so that
        // the builder never selects stale/CT/immature inputs we have no key for.
        dinero::TransactionBuilder builder(chainstate_service->utxoIndex());
        std::string change_address = wallet_service->get().getNewAddress("change", "taproot");

        // The builder's vsize estimator can undercount the final serialized size
        // on some wallet states. Start conservatively, then rebuild with a higher
        // fee rate if mempool policy still rejects the transaction as too cheap.
        double effective_fee_rate = std::max(fee_rate * 6.0, fee_rate + 1.0);
        dinero::TransactionBuilder::BuildResult build_result;
        dinero::TxRejectCode last_reject_code = dinero::TxRejectCode::OK;
        std::string last_reject_reason;
        constexpr int kMaxFeeAttempts = 4;
        int fee_attempt = 0;

        // Broadcast through canonical ingress interface (Step 5).
        if (!ctx.daemon->tx_ingress) {
            result["error"] = "Transaction ingress not available";
            return result;
        }

        for (; fee_attempt < kMaxFeeAttempts; ++fee_attempt) {
            dinero::TransactionBuilder::BuildOptions options;
            options.fee_rate = effective_fee_rate;
            options.change_address = change_address;
            options.candidate_utxos = candidate_utxos;  // Bug Fix 4

            build_result = builder.BuildTransaction(recipients, private_keys, options);
            if (!build_result.success) {
                result["error"] = "Transaction build failed: " + build_result.error;
                return result;
            }

            auto submit_result = ctx.daemon->tx_ingress->Submit(
                build_result.transaction, dinero::TxOrigin::WALLET);
            if (!submit_result.rejected()) {
                break;
            }

            last_reject_code = submit_result.code;
            last_reject_reason = submit_result.message;

            if (submit_result.code != dinero::TxRejectCode::INSUFFICIENT_FEE ||
                fee_attempt + 1 >= kMaxFeeAttempts) {
                result["error"] = "Transaction rejected by mempool";
                result["reject_code"] = TxRejectCodeToString(last_reject_code);
                result["reject_reason"] = last_reject_reason;
                return result;
            }

            if (ctx.logger) {
                ctx.logger->warning(
                    "[wallet.sendmany] Rebuilding after insufficient-fee rejection at " +
                    std::to_string(effective_fee_rate) + " sat/vB: " + submit_result.message);
            }

            effective_fee_rate = std::max(effective_fee_rate * 2.0, effective_fee_rate + 1.0);
        }

        std::string txid = build_result.transaction.GetTxid().AsUint256().GetHex();

        result["txid"] = txid;
        result["recipients"] = static_cast<int>(recipients.size());
        result["total_amount"] = static_cast<double>(total_amount) / 1e8;
        result["fee"] = static_cast<double>(build_result.fee) / 1e8;
        result["fee_rate"] = effective_fee_rate;
        result["fee_attempts"] = fee_attempt + 1;

        if (ctx.logger) {
            ctx.logger->info("[wallet.sendmany] Batch transaction broadcast: " + txid +
                             " (fee_rate=" + std::to_string(effective_fee_rate) +
                             ", attempts=" + std::to_string(fee_attempt + 1) + ")");
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Sendmany failed: ") + e.what();
    }

    return result;
}

/**
 * wallet.utxoproof - Get Utreexo inclusion proof for a wallet UTXO
 *
 * This is a Dinero-specific extension that provides Utreexo proofs
 * for wallet UTXOs, enabling stateless verification.
 */
din::Json rpc_context_wallet_utxoproof(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty()) {
        result["error"] = "Usage: wallet.utxoproof <txid:vout> or wallet.utxoproof <txid> <vout>";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate_service || !chainstate_service->utxoIndex()) {
        result["error"] = "UTXO index not available";
        return result;
    }

    try {
        std::string txid;
        uint32_t vout = 0;
        size_t consumed = 0;
        std::string parse_error;
        if (!ParseUtxoRefParams(params, txid, vout, consumed, parse_error)) {
            result["error"] = parse_error;
            return result;
        }
        (void)consumed;

        // Get UTXO from index
        auto utxo = chainstate_service->utxoIndex()->GetUTXO(dinero::TxId(uint256::FromHexUnsafe(txid)), vout);  // Phase M.4: GetUTXO takes TxId
        if (!utxo.has_value()) {
            result["error"] = "UTXO not found: " + txid + ":" + std::to_string(vout);
            return result;
        }

        // Get current chain height to compute confirmations
        int current_height = chainstate_service->getBlockHeight();
        int confirmations = (current_height >= utxo->height) ? (current_height - utxo->height + 1) : 0;

        result["txid"] = txid;
        result["vout"] = static_cast<int>(vout);
        result["amount"] = static_cast<double>(utxo->value.GetUna()) / 1e8;
        result["height"] = utxo->height;
        result["confirmations"] = confirmations;
        result["is_coinbase"] = utxo->is_coinbase;
        result["tip_height"] = current_height;

        // Delegate to canonical proof generation path.
        din::Json proof_params = din::arr();
        proof_params.append(txid);
        proof_params.append(vout);
        din::Json proof_result = din::rpc_getutxoproof(ctx, proof_params);
        if (proof_result.isMember("error")) {
            result["error"] = "Failed to generate UTXO proof: " + ExtractRpcErrorMessage(proof_result);
            return result;
        }
        result["utreexo_proof"] = proof_result;

        // Bind proof bundle to current chain context for optional strict verification.
        din::Json commitment_params = din::arr();
        din::Json commitment_result = din::rpc_getutreexocommitment(ctx, commitment_params);
        if (commitment_result.isMember("error")) {
            result["error"] = "Failed to bind proof context: " + ExtractRpcErrorMessage(commitment_result);
            return result;
        }
        if (!commitment_result.isMember("commitment") || !commitment_result["commitment"].isString()) {
            result["error"] = "Failed to bind proof context: missing commitment";
            return result;
        }
        result["utreexo_root"] = commitment_result["commitment"].asString();

        std::string tip_hash = chainstate_service->getBestBlockHash();
        if (tip_hash.empty()) {
            result["error"] = "Failed to bind proof context: missing active tip hash";
            return result;
        }
        result["tip_hash"] = tip_hash;

        if (ctx.logger) {
            ctx.logger->debug("[wallet.utxoproof] Generated proof for " + txid + ":" + std::to_string(vout) +
                              " bound_root=" + result["utreexo_root"].asString().substr(0, 16) +
                              " tip=" + result["tip_hash"].asString().substr(0, 16));
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get UTXO proof: ") + e.what();
    }

    return result;
}

/**
 * wallet.getproofbundle - Batch-fetch Utreexo inclusion proofs for all wallet UTXOs.
 *
 * Returns a root-bound proof bundle: one proof per unspent UTXO plus the
 * accumulator root and chain context they are valid against. Clients store
 * the root and call wallet.proofstatus to detect staleness.
 *
 * Optional parameters (JSON object):
 *   min_confirmations (int, default 1)
 *   spendable_only (bool, default true)
 *   max_utxos (uint, default 500)
 *
 * Response:
 *   {
 *     "accumulator_root": hex,      // Root all proofs are valid against
 *     "block_hash": hex,            // Tip hash at generation time
 *     "height": uint,               // Tip height at generation time
 *     "stump_num_leaves": uint,     // Compact Utreexo state leaf count
 *     "stump_roots": [hex, ...],    // Compact Utreexo state roots
 *     "utxo_count": uint,           // Number of UTXOs proven
 *     "truncated": bool,            // true if max_utxos reached
 *     "proofs": [
 *       { "txid", "vout", "amount_una", "amount_unas", "script_pubkey",
 *         "leaf_hash", "position", "num_leaves", "siblings": [...],
 *         "success": bool }
 *     ]
 *   }
 */
din::Json rpc_context_wallet_getproofbundle(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    if (!ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate_service) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    auto* forest = chainstate_service->utreexoForest();
    if (!forest) {
        result["error"] = "Utreexo forest not available";
        return result;
    }

    // Parse options
    int min_confirmations = 1;
    bool spendable_only = true;
    uint32_t max_utxos = 500;

    if (!params.empty() && params[0].isObject()) {
        const din::Json& opts = params[0];
        if (opts.isMember("min_confirmations") && opts["min_confirmations"].isInt()) {
            min_confirmations = std::max(0, opts["min_confirmations"].asInt());
        }
        if (opts.isMember("spendable_only") && opts["spendable_only"].isBool()) {
            spendable_only = opts["spendable_only"].asBool();
        }
        if (opts.isMember("max_utxos") && opts["max_utxos"].isUInt()) {
            max_utxos = std::min(opts["max_utxos"].asUInt(), uint32_t(2000));
        }
    }

    try {
        auto& mgr = wallet_service->get();
        auto utxos = mgr.listUnspentUTXOs(min_confirmations, 9999999);

        if (spendable_only) {
            utxos.erase(
                std::remove_if(utxos.begin(), utxos.end(),
                    [](const dinero::WalletManager::WalletUTXO& u) { return !u.spendable; }),
                utxos.end());
        }

        bool truncated = false;
        if (utxos.size() > max_utxos) {
            utxos.resize(max_utxos);
            truncated = true;
        }

        // Batch-generate proofs via the canonical path
        din::Json proof_input = din::arr();
        for (const auto& utxo : utxos) {
            din::Json item;
            item["txid"] = utxo.txid;
            item["vout"] = utxo.vout;
            proof_input.append(item);
        }

        din::Json batch_params = din::arr();
        batch_params.append(proof_input);

        // The individual UTXO proofs and the stump (accumulator root + num_leaves)
        // must describe ONE Utreexo forest state, or a light client rejects the
        // bundle with "proof leaf count mismatch" (it requires every proof's
        // num_leaves == stump_num_leaves, since a proof only verifies against the
        // stump of the same forest size). Proof generation and the stump snapshot
        // are separate forest reads, so a block connecting on the sync thread in
        // between grows the forest and makes them diverge. Rather than hold a
        // consensus lock from this RPC thread (deadlock risk), assemble
        // optimistically and bracket the work with a forest-commitment read before
        // and after: getCommitment() reflects additions AND deletions (num_leaves
        // alone misses spends), so an unchanged commitment proves the whole bundle
        // came from one consistent state. If a block landed mid-assembly, retry.
        // Fails safe: on exhaustion return a transient error the client already
        // handles via seed failover/retry — never an internally inconsistent bundle.
        constexpr int kMaxAttempts = 4;
        bool consistent = false;
        std::string root_hex;

        for (int attempt = 0; attempt < kMaxAttempts && !consistent; ++attempt) {
            const auto commitment_before = forest->getCommitment();

            din::Json batch_result = din::rpc_getutxoproofs_batch(ctx, batch_params);
            if (batch_result.isMember("error")) {
                result["error"] = "Proof generation failed: " + ExtractRpcErrorMessage(batch_result);
                return result;
            }

            din::Json attempt_result;
            // Bind to the exact compact accumulator context used by the proofs.
            AppendWalletProofRootsSnapshot(attempt_result, *forest);
            const uint64_t stump_num_leaves = attempt_result["stump_num_leaves"].asUInt64();

            // commitment_after must be the LAST forest read so the whole window
            // (proof gen + stump snapshot) is provably inside the unchanged span.
            const auto commitment_after = forest->getCommitment();
            root_hex = BytesToHex(commitment_after);
            if (commitment_before != commitment_after) {
                continue;  // a block connected mid-assembly; re-assemble
            }

            attempt_result["accumulator_root"] = root_hex;
            attempt_result["block_hash"] = chainstate_service->getBestBlockHash();
            attempt_result["height"] = chainstate_service->getBlockHeight();
            attempt_result["truncated"] = truncated;

            // Transform batch result into proof bundle format
            din::Json proofs_out = din::arr();
            std::vector<uint64_t> proof_num_leaves;
            size_t success_count = 0;

            if (batch_result.isMember("proofs") && batch_result["proofs"].isArray()) {
                for (const auto& p : batch_result["proofs"]) {
                    din::Json entry;
                    entry["txid"] = p.isMember("txid") ? p["txid"].asString() : "";
                    entry["vout"] = p.isMember("vout") ? p["vout"].asUInt() : 0;

                    bool ok = p.isMember("success") && p["success"].isBool() && p["success"].asBool();
                    entry["success"] = ok;

                    if (ok && p.isMember("proof") && p["proof"].isObject()) {
                        const auto& proof = p["proof"];
                        if (proof.isMember("leaf_hash")) entry["leaf_hash"] = proof["leaf_hash"];
                        if (proof.isMember("position")) entry["position"] = proof["position"];
                        if (proof.isMember("num_leaves")) {
                            entry["num_leaves"] = proof["num_leaves"];
                            proof_num_leaves.push_back(proof["num_leaves"].asUInt64());
                        }
                        if (proof.isMember("siblings")) entry["siblings"] = proof["siblings"];
                        success_count++;
                    }

                    // Include amount for client-side balance verification
                    for (const auto& utxo : utxos) {
                        if (utxo.txid == entry["txid"].asString() &&
                            utxo.vout == entry["vout"].asUInt()) {
                            entry["amount_una"] = static_cast<int64_t>(utxo.amount_una);
                            entry["amount_unas"] = static_cast<int64_t>(utxo.amount_una);
                            entry["script_pubkey"] = utxo.script_pubkey;
                            entry["created_height"] = static_cast<uint64_t>(utxo.height);
                            entry["coinbase"] = utxo.is_coinbase;
                            break;
                        }
                    }

                    proofs_out.append(entry);
                }
            }

            // Defense in depth: the commitment gate above already guarantees this,
            // but assert the exact invariant the client validates before emitting.
            if (!dinero::rpc::ProofBundleLeafCountsConsistent(proof_num_leaves, stump_num_leaves)) {
                continue;
            }

            attempt_result["proofs"] = proofs_out;
            attempt_result["utxo_count"] = static_cast<int>(success_count);

            result = std::move(attempt_result);
            consistent = true;

            if (ctx.logger) {
                ctx.logger->info("[wallet.getproofbundle] Generated " +
                                 std::to_string(success_count) + "/" +
                                 std::to_string(utxos.size()) +
                                 " proofs, root=" + root_hex.substr(0, 16) + "...");
            }
        }

        if (!consistent) {
            result = din::Json();
            result["error"] = "Proof bundle could not be assembled from a stable "
                              "forest state (blocks connecting); please retry";
            result["retryable"] = true;
            return result;
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to generate proof bundle: ") + e.what();
    }

    return result;
}

/**
 * wallet.proofstatus - Check if a previously-fetched proof root is still current.
 *
 * Lightweight RPC for proof lifecycle management. Clients call this to know
 * whether their cached proofs need refreshing without fetching new proofs.
 *
 * Parameters:
 *   <accumulator_root> (hex string) - The root the client holds
 *
 * Response:
 *   {
 *     "stale": bool,              // true if root no longer matches tip
 *     "client_root": hex,         // Echo back for confirmation
 *     "current_root": hex,        // Current accumulator root
 *     "current_height": uint,     // Current tip height
 *     "current_block_hash": hex   // Current tip hash
 *   }
 */
din::Json rpc_context_wallet_proofstatus(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].isString()) {
        result["error"] = "Usage: wallet.proofstatus <accumulator_root_hex>";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate_service) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    try {
        std::string client_root = params[0].asString();

        din::Json commitment_result = din::rpc_getutreexocommitment(ctx, din::arr());
        std::string current_root;
        if (commitment_result.isMember("commitment") && commitment_result["commitment"].isString()) {
            current_root = commitment_result["commitment"].asString();
        }

        result["stale"] = (client_root != current_root);
        result["client_root"] = client_root;
        result["current_root"] = current_root;
        result["current_height"] = chainstate_service->getBlockHeight();
        result["current_block_hash"] = chainstate_service->getBestBlockHash();

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to check proof status: ") + e.what();
    }

    return result;
}

/**
 * wallet.verifyutxoproof - Verify a single Utreexo inclusion proof.
 *
 * Accepted input forms:
 *   wallet.verifyutxoproof <txid:vout> <proof_object>
 *   wallet.verifyutxoproof <txid> <vout> <proof_object>
 *   wallet.verifyutxoproof <object_with_{txid,vout,proof|utreexo_proof}>
 *
 * Optional context binding:
 *   - Pass {"enforce_bound_context":true} to enforce utreexo_root/tip_hash
 *     from the proof bundle envelope.
 *   - Pass expected_utreexo_root / expected_tip_hash explicitly to enforce
 *     against caller-specified chain context.
 */
din::Json rpc_context_wallet_verifyutxoproof(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty()) {
        result["error"] =
            "Usage: wallet.verifyutxoproof <txid:vout> <proof_object> "
            "or wallet.verifyutxoproof <txid> <vout> <proof_object> "
            "or wallet.verifyutxoproof {txid,vout,proof|utreexo_proof} [options]";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    std::string txid;
    uint32_t vout = 0;
    bool has_vout = false;
    const din::Json* proof_obj = nullptr;
    size_t options_index = params.size();
    bool enforce_bound_context = false;
    std::string expected_utreexo_root;
    std::string expected_tip_hash;
    std::string bundled_utreexo_root;
    std::string bundled_tip_hash;

    if (params[0].isObject()) {
        const din::Json& envelope = params[0];
        if (envelope.isMember("txid") && envelope["txid"].isString()) {
            txid = envelope["txid"].asString();
        }
        if (envelope.isMember("vout")) {
            has_vout = ParseVoutParam(envelope["vout"], vout);
        }
        if (envelope.isMember("enforce_bound_context") && envelope["enforce_bound_context"].isBool()) {
            enforce_bound_context = envelope["enforce_bound_context"].asBool();
        }
        if (envelope.isMember("expected_utreexo_root") && envelope["expected_utreexo_root"].isString()) {
            expected_utreexo_root = envelope["expected_utreexo_root"].asString();
        }
        if (envelope.isMember("expected_tip_hash") && envelope["expected_tip_hash"].isString()) {
            expected_tip_hash = envelope["expected_tip_hash"].asString();
        }
        if (envelope.isMember("utreexo_root") && envelope["utreexo_root"].isString()) {
            bundled_utreexo_root = envelope["utreexo_root"].asString();
        }
        if (envelope.isMember("tip_hash") && envelope["tip_hash"].isString()) {
            bundled_tip_hash = envelope["tip_hash"].asString();
        }
        proof_obj = ResolveProofObject(envelope);
        options_index = 1;
    } else {
        size_t consumed = 0;
        std::string parse_error;
        if (!ParseUtxoRefParams(params, txid, vout, consumed, parse_error)) {
            result["error"] = parse_error;
            return result;
        }
        has_vout = true;
        const Json::ArrayIndex proof_idx = static_cast<Json::ArrayIndex>(consumed);
        if (params.size() <= consumed || !params[proof_idx].isObject()) {
            result["error"] =
                "Usage: wallet.verifyutxoproof <txid:vout> <proof_object> "
                "or wallet.verifyutxoproof <txid> <vout> <proof_object>";
            return result;
        }
        proof_obj = ResolveProofObject(params[proof_idx]);
        options_index = consumed + 1;
    }

    if (proof_obj && txid.empty() && proof_obj->isMember("txid") && (*proof_obj)["txid"].isString()) {
        txid = (*proof_obj)["txid"].asString();
    }
    if (proof_obj && !has_vout && proof_obj->isMember("vout")) {
        has_vout = ParseVoutParam((*proof_obj)["vout"], vout);
    }

    if (txid.empty() || !has_vout) {
        result["error"] = "Invalid proof envelope: txid and vout are required";
        return result;
    }
    if (!proof_obj || !proof_obj->isObject()) {
        result["error"] = "Invalid proof envelope: missing proof object";
        return result;
    }
    if (!IsBatchProofShape(*proof_obj)) {
        result["error"] = "Invalid proof format: expected siblings[], position, num_leaves";
        return result;
    }

    if (proof_obj->isMember("utreexo_root") && (*proof_obj)["utreexo_root"].isString()) {
        bundled_utreexo_root = (*proof_obj)["utreexo_root"].asString();
    }
    if (proof_obj->isMember("tip_hash") && (*proof_obj)["tip_hash"].isString()) {
        bundled_tip_hash = (*proof_obj)["tip_hash"].asString();
    }

    if (options_index < params.size()) {
        const Json::ArrayIndex opts_idx = static_cast<Json::ArrayIndex>(options_index);
        if (params[opts_idx].isObject()) {
            const din::Json& opts = params[opts_idx];
            if (opts.isMember("enforce_bound_context") && opts["enforce_bound_context"].isBool()) {
                enforce_bound_context = opts["enforce_bound_context"].asBool();
            }
            if (opts.isMember("expected_utreexo_root") && opts["expected_utreexo_root"].isString()) {
                expected_utreexo_root = opts["expected_utreexo_root"].asString();
            }
            if (opts.isMember("expected_tip_hash") && opts["expected_tip_hash"].isString()) {
                expected_tip_hash = opts["expected_tip_hash"].asString();
            }
        }
    }

    if (enforce_bound_context) {
        if (expected_utreexo_root.empty() && !bundled_utreexo_root.empty()) {
            expected_utreexo_root = bundled_utreexo_root;
        }
        if (expected_tip_hash.empty() && !bundled_tip_hash.empty()) {
            expected_tip_hash = bundled_tip_hash;
        }
    }

    din::Json normalized_proof;
    normalized_proof["siblings"] = (*proof_obj)["siblings"];
    normalized_proof["position"] = (*proof_obj)["position"];
    normalized_proof["num_leaves"] = (*proof_obj)["num_leaves"];

    din::Json batch_entry;
    batch_entry["txid"] = txid;
    batch_entry["vout"] = vout;
    batch_entry["proof"] = normalized_proof;

    din::Json batch_array = din::arr();
    batch_array.append(batch_entry);

    din::Json verify_params = din::arr();
    verify_params.append(batch_array);

    din::Json verify_result = din::rpc_verifyutxoproofs_batch(ctx, verify_params);
    if (verify_result.isMember("error")) {
        result["error"] = "Proof verification failed: " + ExtractRpcErrorMessage(verify_result);
        return result;
    }

    if (!verify_result.isMember("results") || !verify_result["results"].isArray() || verify_result["results"].empty()) {
        result["error"] = "Proof verification failed: missing verification results";
        return result;
    }

    const din::Json& first_result = verify_result["results"][0];
    result["txid"] = txid;
    result["vout"] = static_cast<int>(vout);
    if (first_result.isMember("valid")) {
        result["valid"] = first_result["valid"];
    } else {
        result["valid"] = false;
    }
    if (verify_result.isMember("utreexo_root")) {
        result["utreexo_root"] = verify_result["utreexo_root"];
    }
    if (first_result.isMember("error_code")) {
        result["error_code"] = first_result["error_code"];
    }
    if (first_result.isMember("error")) {
        result["error_detail"] = first_result["error"];
    }

    auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    std::string tip_hash;
    if (chainstate_service) {
        result["tip_height"] = chainstate_service->getBlockHeight();
        tip_hash = chainstate_service->getBestBlockHash();
        if (!tip_hash.empty()) {
            result["tip_hash"] = tip_hash;
        }
    }

    if (enforce_bound_context || !expected_utreexo_root.empty() || !expected_tip_hash.empty()) {
        result["context_enforced"] = true;
    }

    if (enforce_bound_context &&
        expected_utreexo_root.empty() &&
        expected_tip_hash.empty()) {
        result["valid"] = false;
        result["error_code"] = "missing-bound-context";
        result["error_detail"] =
            "enforce_bound_context=true requires expected_utreexo_root/expected_tip_hash "
            "or a proof bundle containing utreexo_root/tip_hash";
    } else if (result.isMember("valid") &&
               result["valid"].isBool() &&
               result["valid"].asBool()) {
        if (!expected_utreexo_root.empty()) {
            result["expected_utreexo_root"] = expected_utreexo_root;
            if (!result.isMember("utreexo_root") ||
                !result["utreexo_root"].isString() ||
                result["utreexo_root"].asString() != expected_utreexo_root) {
                result["valid"] = false;
                result["error_code"] = "utreexo-root-mismatch";
                result["error_detail"] = "Observed utreexo_root does not match expected_utreexo_root";
            }
        }

        if (result["valid"].isBool() &&
            result["valid"].asBool() &&
            !expected_tip_hash.empty()) {
            result["expected_tip_hash"] = expected_tip_hash;
            if (tip_hash.empty() || tip_hash != expected_tip_hash) {
                result["valid"] = false;
                result["error_code"] = "tip-hash-mismatch";
                result["error_detail"] = "Observed tip_hash does not match expected_tip_hash";
            }
        }
    }

    if (ctx.logger) {
        ctx.logger->debug("[wallet.verifyutxoproof] " + txid + ":" + std::to_string(vout) +
                          " valid=" + (result["valid"].asBool() ? std::string("true") : std::string("false")));
    }

    return result;
}

/**
 * wallet.validatestatelessbalance - Pilot stateless wallet balance validation
 *
 * Verifies wallet balance by generating and validating Utreexo proofs for
 * selected wallet UTXOs.
 *
 * Options (optional object as first param):
 *   {
 *     "min_confirmations": 1,
 *     "max_confirmations": 9999999,
 *     "spendable_only": true,
 *     "max_utxos": 1000,
 *     "include_details": false
 *   }
 */
din::Json rpc_context_wallet_validatestatelessbalance(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    if (!ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate_service) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    int min_confirmations = 1;
    int max_confirmations = 9999999;
    bool spendable_only = true;
    uint32_t max_utxos = 1000;
    bool include_details = false;

    if (!params.empty() && params[0].isObject()) {
        const din::Json& opts = params[0];
        if (opts.isMember("min_confirmations") && opts["min_confirmations"].isInt()) {
            min_confirmations = std::max(0, opts["min_confirmations"].asInt());
        }
        if (opts.isMember("max_confirmations") && opts["max_confirmations"].isInt()) {
            max_confirmations = std::max(min_confirmations, opts["max_confirmations"].asInt());
        }
        if (opts.isMember("spendable_only") && opts["spendable_only"].isBool()) {
            spendable_only = opts["spendable_only"].asBool();
        }
        if (opts.isMember("max_utxos") && opts["max_utxos"].isUInt()) {
            max_utxos = opts["max_utxos"].asUInt();
        }
        if (opts.isMember("include_details") && opts["include_details"].isBool()) {
            include_details = opts["include_details"].asBool();
        }
    }

    if (max_utxos == 0) {
        max_utxos = 1000;
    }

    auto& mgr = wallet_service->get();
    auto wallet_balance = mgr.getBalance();
    auto utxos = mgr.listUnspentUTXOs(min_confirmations, max_confirmations);

    if (spendable_only) {
        utxos.erase(
            std::remove_if(
                utxos.begin(),
                utxos.end(),
                [](const dinero::WalletManager::WalletUTXO& utxo) {
                    return !utxo.spendable;
                }),
            utxos.end());
    }

    bool truncated = false;
    size_t selected_count = utxos.size();
    if (selected_count > max_utxos) {
        utxos.resize(max_utxos);
        selected_count = utxos.size();
        truncated = true;
    }

    uint64_t selected_una = 0;
    for (const auto& utxo : utxos) {
        selected_una += utxo.amount_una;
    }

    din::Json proof_input = din::arr();
    for (const auto& utxo : utxos) {
        din::Json item;
        item["txid"] = utxo.txid;
        item["vout"] = utxo.vout;
        proof_input.append(item);
    }

    din::Json proof_params = din::arr();
    proof_params.append(proof_input);
    din::Json proof_batch = din::rpc_getutxoproofs_batch(ctx, proof_params);
    if (proof_batch.isMember("error")) {
        result["error"] = "Failed to generate UTXO proofs: " + ExtractRpcErrorMessage(proof_batch);
        return result;
    }

    std::unordered_map<std::string, std::string> generation_failures;
    std::unordered_map<std::string, bool> verify_valid;
    std::unordered_map<std::string, std::string> verify_error_codes;
    din::Json verify_input = din::arr();
    size_t generated_count = 0;

    if (proof_batch.isMember("proofs") && proof_batch["proofs"].isArray()) {
        for (const auto& p : proof_batch["proofs"]) {
            if (!p.isObject() || !p.isMember("txid") || !p.isMember("vout")) {
                continue;
            }
            std::string txid = p["txid"].asString();
            uint32_t vout = p["vout"].asUInt();
            std::string key = MakeUtxoKey(txid, vout);

            if (p.isMember("success") && p["success"].isBool() && p["success"].asBool()) {
                if (p.isMember("proof") && p["proof"].isObject()) {
                    din::Json verify_item;
                    verify_item["txid"] = txid;
                    verify_item["vout"] = vout;
                    verify_item["proof"] = p["proof"];
                    verify_input.append(verify_item);
                    generated_count++;
                } else {
                    generation_failures[key] = "missing-proof-object";
                }
            } else {
                if (p.isMember("error_code") && p["error_code"].isString()) {
                    generation_failures[key] = p["error_code"].asString();
                } else {
                    generation_failures[key] = "proof-generation-failed";
                }
            }
        }
    }

    din::Json verify_batch;
    if (!verify_input.empty()) {
        din::Json verify_params = din::arr();
        verify_params.append(verify_input);
        verify_batch = din::rpc_verifyutxoproofs_batch(ctx, verify_params);
        if (verify_batch.isMember("error")) {
            result["error"] = "Failed to verify UTXO proofs: " + ExtractRpcErrorMessage(verify_batch);
            return result;
        }

        if (verify_batch.isMember("results") && verify_batch["results"].isArray()) {
            for (const auto& vr : verify_batch["results"]) {
                if (!vr.isObject() || !vr.isMember("txid") || !vr.isMember("vout")) {
                    continue;
                }
                std::string txid = vr["txid"].asString();
                uint32_t vout = vr["vout"].asUInt();
                std::string key = MakeUtxoKey(txid, vout);
                bool valid = vr.isMember("valid") && vr["valid"].isBool() && vr["valid"].asBool();
                verify_valid[key] = valid;
                if (!valid) {
                    if (vr.isMember("error_code") && vr["error_code"].isString()) {
                        verify_error_codes[key] = vr["error_code"].asString();
                    } else {
                        verify_error_codes[key] = "proof-invalid";
                    }
                }
            }
        }
    }

    uint64_t verified_una = 0;
    size_t verified_valid_count = 0;
    size_t verified_invalid_count = 0;
    din::Json failure_entries = din::arr();

    for (const auto& utxo : utxos) {
        std::string key = MakeUtxoKey(utxo.txid, utxo.vout);

        auto gen_fail_it = generation_failures.find(key);
        if (gen_fail_it != generation_failures.end()) {
            verified_invalid_count++;
            if (include_details) {
                din::Json entry;
                entry["txid"] = utxo.txid;
                entry["vout"] = utxo.vout;
                entry["error_code"] = gen_fail_it->second;
                entry["stage"] = "proof-generation";
                failure_entries.append(entry);
            }
            continue;
        }

        auto valid_it = verify_valid.find(key);
        if (valid_it != verify_valid.end() && valid_it->second) {
            verified_valid_count++;
            verified_una += utxo.amount_una;
        } else {
            verified_invalid_count++;
            if (include_details) {
                din::Json entry;
                entry["txid"] = utxo.txid;
                entry["vout"] = utxo.vout;
                entry["error_code"] =
                    (verify_error_codes.count(key) > 0) ? verify_error_codes[key] : "missing-verification-result";
                entry["stage"] = "proof-verification";
                failure_entries.append(entry);
            }
        }
    }

    std::string tip_hash = chainstate_service->getBestBlockHash();
    uint32_t tip_height = static_cast<uint32_t>(chainstate_service->getBlockHeight());

    std::string chain_root;
    {
        din::Json commitment_params = din::arr();
        din::Json commitment_result = din::rpc_getutreexocommitment(ctx, commitment_params);
        if (!commitment_result.isMember("error") &&
            commitment_result.isMember("commitment") &&
            commitment_result["commitment"].isString()) {
            chain_root = commitment_result["commitment"].asString();
        }
    }

    std::string proof_root;
    if (proof_batch.isMember("utreexo_root") && proof_batch["utreexo_root"].isString()) {
        proof_root = proof_batch["utreexo_root"].asString();
    }

    std::string verify_root;
    if (verify_batch.isMember("utreexo_root") && verify_batch["utreexo_root"].isString()) {
        verify_root = verify_batch["utreexo_root"].asString();
    }

    bool roots_match = true;
    if (!chain_root.empty()) {
        if (!proof_root.empty()) roots_match = roots_match && (proof_root == chain_root);
        if (!verify_root.empty()) roots_match = roots_match && (verify_root == chain_root);
    }

    const uint64_t delta_una = (selected_una >= verified_una)
        ? (selected_una - verified_una)
        : (verified_una - selected_una);
    const bool subset_consistent = (delta_una == 0 && verified_invalid_count == 0);

    const uint64_t wallet_spendable_una = static_cast<uint64_t>(
        std::llround(wallet_balance.spendable * static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN)));

    const bool full_wallet_scope =
        spendable_only &&
        min_confirmations <= 1 &&
        max_confirmations >= 9999999 &&
        !truncated;

    bool wallet_match = true;
    if (full_wallet_scope) {
        wallet_match = (wallet_spendable_una == selected_una);
    }

    const bool pilot_pass = subset_consistent && roots_match && (!full_wallet_scope || wallet_match);

    result["wallet"] = wallet_service->getCurrentWalletName();
    result["mode"] = "stateless-wallet-pilot";
    result["pilot_pass"] = pilot_pass;
    result["tip_height"] = tip_height;
    result["tip_hash"] = tip_hash;
    if (!chain_root.empty()) {
        result["utreexo_root"] = chain_root;
    }

    result["options"]["min_confirmations"] = min_confirmations;
    result["options"]["max_confirmations"] = max_confirmations;
    result["options"]["spendable_only"] = spendable_only;
    result["options"]["max_utxos"] = max_utxos;
    result["options"]["include_details"] = include_details;

    result["summary"]["selected_utxos"] = static_cast<Json::UInt64>(selected_count);
    result["summary"]["truncated"] = truncated;
    result["summary"]["subset_consistent"] = subset_consistent;
    result["summary"]["full_wallet_scope"] = full_wallet_scope;
    if (full_wallet_scope) {
        result["summary"]["wallet_spendable_match"] = wallet_match;
    }

    result["proofs"]["requested"] = static_cast<Json::UInt64>(selected_count);
    result["proofs"]["generated"] = static_cast<Json::UInt64>(generated_count);
    result["proofs"]["generation_failed"] = static_cast<Json::UInt64>(generation_failures.size());
    result["proofs"]["verified_valid"] = static_cast<Json::UInt64>(verified_valid_count);
    result["proofs"]["verified_invalid"] = static_cast<Json::UInt64>(verified_invalid_count);
    if (proof_batch.isMember("generation_time_ms")) {
        result["proofs"]["generation_time_ms"] = proof_batch["generation_time_ms"];
    }
    if (verify_batch.isMember("verification_time_ms")) {
        result["proofs"]["verification_time_ms"] = verify_batch["verification_time_ms"];
    }

    result["balances"]["selected_una"] = static_cast<Json::UInt64>(selected_una);
    result["balances"]["selected_din"] = static_cast<double>(selected_una) /
        static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
    result["balances"]["verified_una"] = static_cast<Json::UInt64>(verified_una);
    result["balances"]["verified_din"] = static_cast<double>(verified_una) /
        static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
    result["balances"]["delta_una"] = static_cast<Json::UInt64>(delta_una);
    result["balances"]["delta_din"] = static_cast<double>(delta_una) /
        static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
    result["balances"]["wallet_spendable_una"] = static_cast<Json::UInt64>(wallet_spendable_una);
    result["balances"]["wallet_spendable_din"] = wallet_balance.spendable;

    result["context"]["root_match"] = roots_match;
    if (!proof_root.empty()) {
        result["context"]["proof_root"] = proof_root;
    }
    if (!verify_root.empty()) {
        result["context"]["verify_root"] = verify_root;
    }
    if (!chain_root.empty()) {
        result["context"]["chain_root"] = chain_root;
    }

    if (include_details) {
        result["failures"] = failure_entries;
    }

    if (ctx.logger) {
        ctx.logger->info("[wallet.validatestatelessbalance] wallet=" + wallet_service->getCurrentWalletName() +
                         " selected_utxos=" + std::to_string(selected_count) +
                         " valid=" + std::to_string(verified_valid_count) +
                         " invalid=" + std::to_string(verified_invalid_count) +
                         " pilot_pass=" + std::string(pilot_pass ? "true" : "false"));
    }

    return result;
}

/**
 * wallet.listtransactions - List transaction history
 */
din::Json rpc_context_wallet_listtransactions(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        int limit = 100;
        int offset = 0;
        int min_conf = -1;
        std::string address_filter;
        std::string type_filter = "all";

        if (params.isObject()) {
            // Object params: {"count": N, "offset": M, "type": "all|mined|sent|received"}
            if (params.isMember("count") && params["count"].isInt())
                limit = params["count"].asInt();
            if (params.isMember("limit") && params["limit"].isInt())
                limit = params["limit"].asInt();
            if (params.isMember("offset") && params["offset"].isInt())
                offset = params["offset"].asInt();
            if (params.isMember("minconf") && params["minconf"].isInt())
                min_conf = params["minconf"].asInt();
            if (params.isMember("min_conf") && params["min_conf"].isInt())
                min_conf = params["min_conf"].asInt();
            if (params.isMember("address") && params["address"].is<std::string>())
                address_filter = params["address"].as<std::string>();

            if (params.isMember("type")) {
                if (!ParseWalletTxTypeFilter(params["type"], type_filter)) {
                    result["error"] = "Invalid type. Expected one of: all, mined, sent, received, shield, unshield";
                    return result;
                }
            } else if (params.isMember("category")) {
                if (!ParseWalletTxTypeFilter(params["category"], type_filter)) {
                    result["error"] = "Invalid category/type. Expected one of: all, mined, sent, received, shield, unshield";
                    return result;
                }
            }
        } else if (params.isArray()) {
            // Array params: [count, offset, type]
            if (params.size() >= 1 && params[0].isInt())
                limit = params[0].asInt();
            if (params.size() >= 2 && params[1].isInt())
                offset = params[1].asInt();
            if (params.size() >= 3) {
                if (!ParseWalletTxTypeFilter(params[2], type_filter)) {
                    result["error"] = "Invalid type. Expected one of: all, mined, sent, received, shield, unshield";
                    return result;
                }
            }
        }

        limit = std::max(1, std::min(limit, 5000));
        offset = std::max(0, offset);
        min_conf = std::max(-1, min_conf);

        // Fetch in bounded chunks and apply filters locally so offset applies
        // to filtered results, not raw history rows.
        const int target = offset + limit;
        const int fetch_chunk = std::max(200, limit * 3);
        int fetch_offset = 0;
        int scanned_rows = 0;
        constexpr int kMaxScanRows = 200000;

        std::vector<dinero::WalletManager::TransactionInfo> filtered_txs;
        while (static_cast<int>(filtered_txs.size()) < target && scanned_rows < kMaxScanRows) {
            auto batch = wallet_service->get().getTransactionHistory(fetch_chunk, fetch_offset);
            if (batch.empty()) {
                break;
            }

            fetch_offset += static_cast<int>(batch.size());
            scanned_rows += static_cast<int>(batch.size());

            for (const auto& tx : batch) {
                const std::string normalized_type = NormalizeWalletTxType(tx);
                if (type_filter != "all" && normalized_type != type_filter) {
                    continue;
                }
                if (!address_filter.empty() && tx.address != address_filter) {
                    continue;
                }
                if (min_conf >= 0 && tx.confirmations < min_conf) {
                    continue;
                }
                filtered_txs.push_back(tx);
                if (static_cast<int>(filtered_txs.size()) >= target) {
                    break;
                }
            }

            if (static_cast<int>(batch.size()) < fetch_chunk) {
                break;  // Reached end of history
            }
        }

        din::Json tx_array = din::arr();

        const int end = std::min(static_cast<int>(filtered_txs.size()), target);
        for (int i = offset; i < end; ++i) {
            const auto& tx = filtered_txs[i];
            din::Json tx_obj;
            tx_obj["txid"] = tx.txid;
            tx_obj["address"] = tx.address;
            tx_obj["amount"] = tx.amount;
            tx_obj["confirmations"] = tx.confirmations;
            tx_obj["category"] = tx.category;
            tx_obj["type"] = NormalizeWalletTxType(tx);
            tx_obj["time"] = static_cast<double>(tx.time);
            tx_obj["label"] = tx.label;
            tx_obj["is_coinbase"] = tx.is_coinbase;
            PopulateWalletHistoryMetadata(tx_obj, tx);
            tx_array.append(tx_obj);
        }

        result = tx_array;
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to list transactions: ") + e.what();
    }

    return result;
}

/**
 * GetPrevoutInfo - Helper to lookup previous output information
 *
 * Searches for a specific output from a previous transaction.
 * Tries multiple sources in order of efficiency:
 * 1. Block scan (currently the only method, but safe and reliable)
 *
 * @param chain_db Blockchain database
 * @param current_height Current chain height for search bounds
 * @param prev_txid Transaction ID of the previous transaction
 * @param vout Output index within the previous transaction
 * @param out_value [OUT] Value of the output in una (una)
 * @param out_script [OUT] scriptPubKey of the output
 * @return true if prevout was found, false otherwise
 */
template<typename ChainDBType>
static bool GetPrevoutInfo(
    dinero::ChainstateService* chainstate,
    ChainDBType* chain_db,
    dinero::BlockStorage* block_storage,
    uint32_t current_height,
    const uint256& prev_txid,
    uint32_t vout,
    int64_t& out_value,
    std::vector<uint8_t>& out_script)
{
    // Search through blocks to find the previous transaction
    for (int32_t height = current_height; height >= 0; --height) {
        std::string block_hash_str = dinero::storage::GetBlockHash(chain_db, height);
        if (block_hash_str.empty()) continue;

        uint256 block_hash = uint256::FromHexUnsafe(block_hash_str);
        auto block_result = ReadWalletRpcBlock(
            chainstate,
            chain_db,
            block_storage,
            block_hash);
        if (block_result.status() != dinero::Status::Ok) continue;

        const dinero::Block& block = block_result.value();

        // Check each transaction in the block
        for (const auto& tx : block.vtx) {
            if (tx.GetTxid().AsUint256() == prev_txid) {
                // Found the transaction, now get the specific output
                if (vout >= tx.vout.size()) {
                    // Invalid vout index
                    return false;
                }

                out_value = tx.vout[vout].value.GetInt64();  // Phase M.6.2
                out_script = tx.vout[vout].scriptPubKey;
                return true;
            }
        }
    }

    return false;  // Transaction not found
}

/**
 * wallet.gettransaction - Get detailed transaction breakdown
 *
 * Provides comprehensive view of a wallet transaction including:
 * - Spent UTXOs (inputs)
 * - Outputs (payment + change)
 * - Fee breakdown
 * - UTXO status (spent/change/unspent)
 *
 * This complements blockchain.gettransaction by adding wallet-specific context.
 */
din::Json rpc_context_wallet_gettransaction(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.gettransaction <txid>";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    std::string txid = params[0].as<std::string>();

    try {
        // Get transaction from wallet history
        auto txs = wallet_service->get().getTransactionHistory(10000, 0);
        bool found = false;
        dinero::WalletManager::TransactionInfo tx_info;

        for (const auto& tx : txs) {
            if (tx.txid == txid) {
                found = true;
                tx_info = tx;
                break;
            }
        }

        if (!found) {
            result["error"] = "Transaction not found in wallet";
            result["txid"] = txid;
            result["hint"] = "Use blockchain.gettransaction for chain-level lookup";
            return result;
        }

        // Build enhanced response
        result["txid"] = txid;
        result["status"] = "confirmed";
        result["confirmations"] = tx_info.confirmations;
        result["time"] = static_cast<double>(tx_info.time);
        result["is_coinbase"] = tx_info.is_coinbase;

        // Categorize transaction
        result["category"] = tx_info.category;
        result["amount"] = tx_info.amount;
        result["address"] = tx_info.address;
        result["label"] = tx_info.label;

        // Try to get detailed breakdown from blockchain
        if (ctx.daemon->chainstate) {
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
            if (chainstate) {
                auto* chain_db = chainstate->GetChainDB();
                if (chain_db) {
                    // Search for transaction in blockchain for detailed breakdown
                    uint32_t current_height = chainstate->getBlockHeight();
                    uint256 target_txid = uint256::FromHexUnsafe(txid);
                    bool found_in_chain = false;

                    for (int32_t height = current_height; height >= 0 && height > static_cast<int32_t>(current_height) - 1000; --height) {
                        std::string block_hash_str = dinero::storage::GetBlockHash(chain_db, height);
                        if (block_hash_str.empty()) continue;

                        uint256 block_hash = uint256::FromHexUnsafe(block_hash_str);
                        auto block_result = ReadWalletRpcBlock(
                            chainstate.get(),
                            chain_db,
                            ctx.daemon ? ctx.daemon->block_storage.get() : nullptr,
                            block_hash);
                        if (block_result.status() != dinero::Status::Ok) continue;

                        const dinero::Block& block = block_result.value();

                        for (const auto& tx : block.vtx) {
                            if (tx.GetTxid().AsUint256() == target_txid) {
                                found_in_chain = true;

                                // Add detailed breakdown
                                result["blockhash"] = block_hash.GetHex();
                                result["blockheight"] = static_cast<int>(height);

                                // Input breakdown
                                din::Json inputs_array = din::arr();
                                int64_t total_input = 0;
                                for (const auto& input : tx.vin) {
                                    din::Json input_obj;
                                    input_obj["txid"] = input.prevout.txid.AsUint256().GetHex();
                                    input_obj["vout"] = static_cast<int>(input.prevout.vout);

                                    // Lookup prevout amount (if not coinbase)
                                    if (!tx.IsCoinbase()) {
                                        int64_t prevout_value = 0;
                                        std::vector<uint8_t> prevout_script;

                                        if (GetPrevoutInfo(
                                                chainstate.get(),
                                                chain_db,
                                                ctx.daemon ? ctx.daemon->block_storage.get() : nullptr,
                                                current_height,
                                                input.prevout.txid.AsUint256(),
                                                input.prevout.vout,
                                                prevout_value,
                                                prevout_script)) {
                                            input_obj["value"] = static_cast<double>(prevout_value) / 1e8;
                                            total_input += prevout_value;
                                        }
                                    }

                                    inputs_array.append(input_obj);
                                }
                                result["inputs"] = inputs_array;
                                result["input_count"] = static_cast<int>(tx.vin.size());

                                // Output breakdown
                                din::Json outputs_array = din::arr();
                                uint64_t total_output = 0;
                                bool has_confidential_outputs = false;
                                for (size_t i = 0; i < tx.vout.size(); ++i) {
                                    const auto& output = tx.vout[i];
                                    din::Json output_obj;
                                    output_obj["vout"] = static_cast<int>(i);
                                    PopulateWalletOutputDisplay(output_obj, output);
                                    output_obj["type"] = output.IsTaproot() ? "taproot" : (output.IsSegWitV0() ? "segwit_v0" : "legacy");
                                    has_confidential_outputs = has_confidential_outputs || output.is_confidential;

                                    // Check if this is a change output by comparing with wallet addresses
                                    // For now, mark as "output" - wallet could enhance this
                                    output_obj["category"] = "output";

                                    outputs_array.append(output_obj);
                                    if (!output.is_confidential) {
                                        total_output += output.value.GetUna();  // Phase M.6.2
                                    }
                                }
                                result["outputs"] = outputs_array;
                                result["output_count"] = static_cast<int>(tx.vout.size());
                                result["has_confidential_outputs"] = has_confidential_outputs;

                                // Add fee calculation (if inputs were resolved)
                                if (!tx.IsCoinbase() && total_input > 0 && !has_confidential_outputs) {
                                    int64_t fee = total_input - static_cast<int64_t>(total_output);
                                    result["fee"] = static_cast<double>(fee) / 1e8;
                                    result["fee_una"] = static_cast<double>(fee);
                                } else if (has_confidential_outputs) {
                                    result["fee_hidden"] = true;
                                }

                                break;
                            }
                        }

                        if (found_in_chain) break;
                    }

                    if (!found_in_chain) {
                        result["note"] = "Transaction confirmed but detailed breakdown unavailable (block may be pruned)";
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get transaction: ") + e.what();
    }

    return result;
}

/**
 * wallet.backup - Backup wallet to file
 */
din::Json rpc_context_wallet_backup(const ExecutionContext& ctx, const din::Json& params) {
    // Canonical backup path is identical to wallet.dumpwallet output.
    return rpc_context_wallet_dumpwallet(ctx, params);
}

/**
 * wallet.deriveaddress - Derive address from BIP84 path
 */
din::Json rpc_context_wallet_deriveaddress(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    din::Json result;

    if (params.size() < 3) {
        result["error"] = "Usage: wallet.deriveaddress <account> <change> <index>";
        return result;
    }

    result["error"]["code"] = -32601;
    result["error"]["message"] = "wallet.deriveaddress is not supported by current WalletManager API";
    result["error"]["detail"] = "Use wallet.getnewaddress for allocation or wallet.deriveaddresses with descriptors";
    return result;
}

/**
 * wallet.dumpprivkey - Export private key for address
 */
din::Json rpc_context_wallet_dumpprivkey(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.dumpprivkey <address>";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    result["error"] = "Private key export is intentionally disabled for BIP86 Taproot HD wallets";
    result["reason"] = "Use mnemonic seed backup or PSBT signing with hardware wallet";
    result["design"] = "BIP39 seed is the source of truth - individual key export breaks recovery model";

    return result;
}

// ═══════════════════════════════════════════════════════════════
// PSBT Methods (Partially Signed Bitcoin Transactions)
// ═══════════════════════════════════════════════════════════════

/**
 * wallet.createfundedpsbt - Create a funded PSBT
 *
 * Params:
 *   [0] outputs: {"address": amount, ...} - Recipients
 *   [1] options: {fee_rate, change_address, ...} - Optional settings
 *
 * Returns: {psbt: "base64...", fee: amount, fee_paid_una: n, change_position: n,
 *           change_amount_una: n, change_address: "...", selected_inputs: [...]}
 */
din::Json rpc_context_wallet_createfundedpsbt(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    // spec Fatal §3: UTXO selection consults the assumed UTXO set; gate before
    // any wallet or chainstate access.  Note: the eventual broadcast path is
    // also gated at mempool.sendrawtransaction — this gate covers the funding
    // step itself so the unsigned PSBT is not built on untrusted state.
    if (RefuseIfSafeMode(ctx, result)) return result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    if (params.empty() || !params[0].isObject()) {
        result["error"] = "Usage: wallet.createfundedpsbt {\"address\": amount, ...} [options]";
        return result;
    }

    try {
        auto& wallet = wallet_service->get();

        // Parse outputs
        std::vector<dinero::TransactionBuilder::Recipient> recipients;
        int64_t total_amount = 0;

        const auto& outputs = params[0];
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            std::string address = it.key().asString();
            double amount_din = (*it).asDouble();
            int64_t amount_sat = static_cast<int64_t>(amount_din * 1e8);

            recipients.push_back({address, amount_sat});
            total_amount += amount_sat;
        }

        // Parse options
        dinero::TransactionBuilder::BuildOptions options;
        if (params.size() >= 2 && params[1].isObject()) {
            const auto& opts = params[1];
            if (opts.isMember("fee_rate")) options.fee_rate = opts["fee_rate"].asDouble();
            if (opts.isMember("change_address")) options.change_address = opts["change_address"].asString();
            if (opts.isMember("subtract_fee")) options.subtract_fee_from_amount = opts["subtract_fee"].asBool();
        }

        // Get chainstate for UTXO access
        auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate_service || !chainstate_service->utxoIndex()) {
            result["error"] = "UTXO index not available";
            return result;
        }

        // Build transaction preview
        dinero::TransactionBuilder builder(chainstate_service->utxoIndex());
        auto build_result = builder.PreviewTransaction(recipients, options);

        if (!build_result.success) {
            result["error"] = build_result.error;
            return result;
        }

        // Create PSBT from unsigned transaction
        dinero::PSBT psbt;
        psbt.tx = build_result.transaction;
        psbt.inputs.resize(build_result.transaction.vin.size());
        psbt.outputs.resize(build_result.transaction.vout.size());

        // Fill in witness UTXO data for each input
        for (size_t i = 0; i < build_result.selected_utxos.size(); ++i) {
            const auto& utxo = build_result.selected_utxos[i];
            psbt.inputs[i].witness_utxo_amount = utxo.value.GetUna();  // Phase M.6.2
            psbt.inputs[i].witness_utxo_script = utxo.spk;
        }

        // Serialize to base64
        result["psbt"] = psbt.ToBase64();
        result["fee"] = static_cast<double>(build_result.fee) / 1e8;
        result["fee_paid_una"] = static_cast<int64_t>(build_result.fee);
        result["change_amount_una"] = static_cast<int64_t>(build_result.change_amount);
        result["change_position"] = build_result.change_amount > 0 ?
            static_cast<int>(build_result.transaction.vout.size() - 1) : -1;
        if (build_result.change_amount > 0 && !build_result.change_address.empty()) {
            result["change_address"] = build_result.change_address;
        }

        Json::Value inputs_arr(Json::arrayValue);
        for (const auto& utxo : build_result.selected_utxos) {
            Json::Value input_obj(Json::objectValue);
            input_obj["txid"] = utxo.txid.GetHex();
            input_obj["vout"] = utxo.vout;
            input_obj["amount_una"] = static_cast<Json::Int64>(utxo.value.GetUna());
            if (!utxo.path.empty()) {
                input_obj["path"] = utxo.path;
            }
            inputs_arr.append(input_obj);
        }
        result["selected_inputs"] = inputs_arr;

        if (ctx.logger) {
            ctx.logger->info("[wallet.createfundedpsbt] Created PSBT with " +
                std::to_string(recipients.size()) + " outputs, fee: " +
                std::to_string(build_result.fee) + " sat");
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to create PSBT: ") + e.what();
    }

    return result;
}

/**
 * wallet.processpsbt (signpsbt) - Sign a PSBT with wallet keys
 *
 * Params:
 *   [0] psbt: "base64..." - PSBT to sign
 *   [1] sign: bool - Whether to sign (default: true)
 *   [2] sighash_type: string - Sighash type (default: "ALL")
 *
 * Returns: {psbt: "base64...", complete: bool}
 */
din::Json rpc_context_wallet_processpsbt(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.processpsbt \"psbt_base64\" [sign=true] [sighash_type]";
        return result;
    }

    try {
        std::string psbt_base64 = params[0].as<std::string>();
        bool do_sign = params.size() < 2 || params[1].asBool();

        // Decode PSBT
        dinero::PSBT psbt = dinero::PSBT::FromBase64(psbt_base64);
        if (!psbt.IsValid()) {
            result["error"] = "Invalid PSBT: " + psbt.GetError();
            return result;
        }

        if (do_sign) {
            auto& wallet = wallet_service->get();

            // Sign each input where we have the key
            size_t signed_count = 0;
            for (size_t i = 0; i < psbt.inputs.size(); ++i) {
                // Get the scriptPubKey for this input from the witness UTXO script
                // ⚠️ OWNERSHIP LOGIC - Uses scriptPubKey (consensus data), NOT address
                const auto& script = psbt.inputs[i].witness_utxo_script;
                if (script.size() >= 22 && script[0] == 0x00 && script[1] == 0x14) {
                    // P2WPKH - convert scriptPubKey to hex for wallet lookup
                    std::stringstream ss;
                    for (uint8_t byte : script) {
                        ss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
                    }
                    std::string script_pubkey_hex = ss.str();

                    // Try to get private key from wallet BY SCRIPTPUBKEY (not address)
                    auto privkey = wallet.deriveKeyForScriptPubKey(script_pubkey_hex);
                    if (privkey.has_value() && privkey->size() == 32) {
                        if (psbt.Sign(privkey.value(), i)) {
                            signed_count++;
                        }
                    }
                }
            }

            if (ctx.logger) {
                ctx.logger->info("[wallet.processpsbt] Signed " +
                    std::to_string(signed_count) + "/" +
                    std::to_string(psbt.inputs.size()) + " inputs");
            }
        }

        result["psbt"] = psbt.ToBase64();
        result["complete"] = psbt.IsComplete();

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to process PSBT: ") + e.what();
    }

    return result;
}

/**
 * wallet.finalizepsbt - Finalize a PSBT for broadcasting
 *
 * Params:
 *   [0] psbt: "base64..." - Signed PSBT to finalize
 *   [1] extract: bool - Whether to extract final tx (default: true)
 *
 * Returns: {psbt: "base64...", hex: "tx_hex...", complete: bool}
 */
din::Json rpc_context_wallet_finalizepsbt(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.finalizepsbt \"psbt_base64\" [extract=true]";
        return result;
    }

    try {
        std::string psbt_base64 = params[0].as<std::string>();
        bool extract = params.size() < 2 || params[1].asBool();

        // Decode PSBT
        dinero::PSBT psbt = dinero::PSBT::FromBase64(psbt_base64);
        if (!psbt.IsValid()) {
            result["error"] = "Invalid PSBT: " + psbt.GetError();
            return result;
        }

        // Finalize
        bool success = psbt.Finalize();
        result["complete"] = success;
        result["psbt"] = psbt.ToBase64();

        if (success && extract) {
            // Extract final transaction
            dinero::Transaction final_tx = psbt.ExtractTransaction();
            std::vector<uint8_t> tx_bytes = final_tx.Serialize();

            // Convert to hex
            std::ostringstream oss;
            for (uint8_t byte : tx_bytes) {
                oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
            }
            result["hex"] = oss.str();
        }

        if (ctx.logger) {
            ctx.logger->info("[wallet.finalizepsbt] Finalized: " +
                std::string(success ? "complete" : "incomplete"));
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to finalize PSBT: ") + e.what();
    }

    return result;
}

/**
 * wallet.combinepsbt - Combine multiple PSBTs
 *
 * Params:
 *   [0] psbts: ["base64...", ...] - Array of PSBTs to combine
 *
 * Returns: {psbt: "base64..."}
 */
din::Json rpc_context_wallet_combinepsbt(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].isArray() || params[0].size() < 2) {
        result["error"] = "Usage: wallet.combinepsbt [\"psbt1\", \"psbt2\", ...]";
        return result;
    }

    try {
        const auto& psbt_array = params[0];

        // Start with the first PSBT
        dinero::PSBT combined = dinero::PSBT::FromBase64(psbt_array[static_cast<Json::ArrayIndex>(0)].asString());
        if (!combined.IsValid()) {
            result["error"] = "Invalid first PSBT: " + combined.GetError();
            return result;
        }

        // Combine remaining PSBTs
        for (Json::ArrayIndex i = 1; i < psbt_array.size(); ++i) {
            dinero::PSBT other = dinero::PSBT::FromBase64(psbt_array[i].asString());
            if (!other.IsValid()) {
                result["error"] = "Invalid PSBT at index " + std::to_string(i) + ": " + other.GetError();
                return result;
            }

            if (!combined.Combine(other)) {
                result["error"] = "Failed to combine PSBT at index " + std::to_string(i);
                return result;
            }
        }

        result["psbt"] = combined.ToBase64();

        if (ctx.logger) {
            ctx.logger->info("[wallet.combinepsbt] Combined " +
                std::to_string(psbt_array.size()) + " PSBTs");
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to combine PSBTs: ") + e.what();
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// Transaction Methods
// ═══════════════════════════════════════════════════════════════

/**
 * wallet.createrawtransaction - Create an unsigned raw transaction
 *
 * Params:
 *   [0] inputs: [{"txid": "...", "vout": n}, ...]
 *   [1] outputs: {"address": amount, ...} or [{"address": amount}, {"data": "hex"}]
 *   [2] locktime: int (optional)
 *
 * Returns: hex-encoded unsigned transaction
 */
din::Json rpc_context_wallet_createrawtransaction(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.size() < 2) {
        result["error"] = "Usage: wallet.createrawtransaction [{\"txid\":\"...\",\"vout\":n},...] {\"address\":amount,...} [locktime]";
        return result;
    }

    try {
        // Parse inputs
        const auto& inputs_json = params[0];
        const auto& outputs_json = params[1];

        dinero::Transaction tx;
        tx.version = 2;
        tx.lockTime = (params.size() >= 3) ? params[2].asUInt() : 0;

        // Process inputs
        for (Json::ArrayIndex i = 0; i < inputs_json.size(); ++i) {
            const auto& inp = inputs_json[i];
            dinero::TxInput input;
            input.prevout.txid = dinero::TxId(uint256::FromHexUnsafe(inp["txid"].asString()));  // RPC→Consensus: hex to TxId
            input.prevout.vout = inp["vout"].asUInt();
            input.sequence = inp.isMember("sequence") ? inp["sequence"].asUInt() : 0xfffffffe;
            tx.vin.push_back(input);
        }

        auto append_output = [&](const std::vector<uint8_t>& script, uint64_t amount_sat) {
            dinero::TxOutput output;
            output.value = dinero::AmountUna::Una(amount_sat);  // Phase M.6.2
            output.scriptPubKey = script;
            tx.vout.push_back(output);
        };

        // Process outputs
        if (outputs_json.isArray()) {
            for (Json::ArrayIndex i = 0; i < outputs_json.size(); ++i) {
                const auto& output_obj = outputs_json[i];
                if (!output_obj.isObject() || output_obj.empty()) {
                    result["error"] = "Invalid output entry";
                    return result;
                }

                if (output_obj.isMember("data")) {
                    auto data_script = dinero::TransactionSerializer::FromHex(output_obj["data"].asString());
                    if (data_script.empty() && !output_obj["data"].asString().empty()) {
                        result["error"] = "Invalid data hex in output entry";
                        return result;
                    }
                    append_output(data_script, 0);
                    continue;
                }

                if (output_obj.isMember("scriptPubKey")) {
                    const auto script_hex = output_obj["scriptPubKey"].asString();
                    auto script = dinero::TransactionSerializer::FromHex(script_hex);
                    if (script.empty()) {
                        result["error"] = "Invalid scriptPubKey hex";
                        return result;
                    }
                    double amount_din = output_obj.get("amount", 0.0).asDouble();
                    uint64_t amount_sat = static_cast<uint64_t>(amount_din * 1e8);
                    append_output(script, amount_sat);
                    continue;
                }

                for (auto it = output_obj.begin(); it != output_obj.end(); ++it) {
                    std::string address = it.key().asString();
                    double amount_din = (*it).asDouble();
                    uint64_t amount_sat = static_cast<uint64_t>(amount_din * 1e8);

                    auto script = dinero::TransactionBuilder::AddressToScriptPubKey(address);
                    if (script.empty()) {
                        result["error"] = "Invalid address: " + address;
                        return result;
                    }
                    append_output(script, amount_sat);
                }
            }
        } else {
            for (auto it = outputs_json.begin(); it != outputs_json.end(); ++it) {
                std::string address = it.key().asString();
                double amount_din = (*it).asDouble();
                uint64_t amount_sat = static_cast<uint64_t>(amount_din * 1e8);

                // Convert address to scriptPubKey
                auto script = dinero::TransactionBuilder::AddressToScriptPubKey(address);
                if (script.empty()) {
                    result["error"] = "Invalid address: " + address;
                    return result;
                }

                append_output(script, amount_sat);
            }
        }

        // Serialize to hex
        result["hex"] = tx.SerializeHex(false);  // No witness for unsigned

        if (ctx.logger) {
            ctx.logger->debug("[wallet.createrawtransaction] Created tx with " +
                std::to_string(tx.vin.size()) + " inputs, " +
                std::to_string(tx.vout.size()) + " outputs");
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to create transaction: ") + e.what();
    }

    return result;
}

/**
 * wallet.signrawtransaction - Sign a raw transaction with wallet keys
 *
 * Params:
 *   [0] hex: raw transaction hex
 *   [1] prevtxs: [{"txid":..., "vout":n, "scriptPubKey":..., "amount":...}] (optional)
 *   [2] privkeys: ["key1", ...] (optional, uses wallet keys if not provided)
 *
 * Returns: {hex: "signed_tx", complete: bool}
 */
din::Json rpc_context_wallet_signrawtransaction(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.signrawtransaction \"hex\" [prevtxs] [privkeys]";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        std::string tx_hex = params[0].asString();

        // Deserialize transaction
        dinero::Transaction tx;
        if (!dinero::TransactionSerializer::Deserialize(tx, tx_hex)) {
            result["error"] = "Failed to decode transaction";
            return result;
        }

        // Get prevtxs info if provided
        std::map<std::string, std::pair<uint64_t, std::vector<uint8_t>>> prevouts;  // outpoint -> (amount, script)
        if (params.size() >= 2 && params[1].isArray()) {
            const auto& prevtxs = params[1];
            for (Json::ArrayIndex i = 0; i < prevtxs.size(); ++i) {
                const auto& prev = prevtxs[i];
                std::string outpoint = prev["txid"].asString() + ":" + std::to_string(prev["vout"].asUInt());
                uint64_t amount = static_cast<uint64_t>(prev["amount"].asDouble() * 1e8);
                auto script = dinero::TransactionSerializer::FromHex(prev["scriptPubKey"].asString());
                prevouts[outpoint] = {amount, script};
            }
        }

        auto& wallet = wallet_service->get();
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);

        std::unordered_map<std::string, std::string> script_to_path;
        for (const auto& row : wallet.listAddresses(false)) {
            if (row.script_pubkey.empty()) continue;

            const uint32_t purpose =
                (row.type == "p2tr" || row.type == "taproot") ? 86 : 84;
            script_to_path[row.script_pubkey] = BuildStandardDerivationPath(
                purpose,
                row.account,
                row.change,
                row.index);
        }

        std::vector<dinero::CanonicalWalletUTXO> input_utxos(tx.vin.size());
        std::vector<std::optional<std::vector<uint8_t>>> input_private_keys(tx.vin.size());
        std::vector<bool> have_prevout(tx.vin.size(), false);
        std::vector<bool> had_witness(tx.vin.size(), false);
        size_t signed_count = 0;

        // Gather prevout metadata for all inputs first.
        for (size_t i = 0; i < tx.vin.size(); ++i) {
            const auto& input = tx.vin[i];
            std::string outpoint = input.prevout.txid.AsUint256().GetHex() + ":" + std::to_string(input.prevout.vout);

            uint64_t amount = 0;
            std::vector<uint8_t> script;

            if (prevouts.count(outpoint)) {
                amount = prevouts[outpoint].first;
                script = prevouts[outpoint].second;
            } else if (chainstate && chainstate->utxoIndex()) {
                auto utxo = chainstate->utxoIndex()->GetUTXO(input.prevout.txid, input.prevout.vout);
                if (utxo.has_value()) {
                    amount = utxo->value.GetUna();  // Phase M.6.2
                    script = utxo->spk;
                    input_utxos[i].is_confidential = utxo->is_confidential;
                    input_utxos[i].commitment = utxo->commitment;
                }
            }

            if (script.empty()) {
                continue;
            }

            auto& wallet_utxo = input_utxos[i];
            wallet_utxo.txid = input.prevout.txid.AsUint256();  // Phase M.4: Unwrap TxId to uint256
            wallet_utxo.vout = input.prevout.vout;
            wallet_utxo.value = dinero::AmountUna::Una(amount);  // Phase M.6.2
            wallet_utxo.spk = script;
            wallet_utxo.is_coinbase = false;
            wallet_utxo.height = 0;

            const std::string script_pubkey_hex = dinero::TransactionSerializer::ToHex(script);
            if (auto path_it = script_to_path.find(script_pubkey_hex); path_it != script_to_path.end()) {
                wallet_utxo.path = path_it->second;
            }

            // Preserve any already-populated witness (notably a covenant
            // script path) and do not ask the HD wallet for a key it cannot
            // own. Completion is decided below by canonical consensus
            // validation, so malformed pre-populated witnesses do not get a
            // free "complete" result.
            had_witness[i] = !tx.vin[i].witness.empty();
            if (had_witness[i]) {
                signed_count++;
            } else {
                auto privkey =
                    wallet.deriveKeyForScriptPubKey(script_pubkey_hex);
                if (privkey.has_value() && privkey->size() == 32) {
                    input_private_keys[i] = *privkey;
                }
            }

            have_prevout[i] = true;
        }

        const bool have_all_prevouts = std::all_of(
            have_prevout.begin(),
            have_prevout.end(),
            [](bool present) { return present; });

        // Phase 10 Commit 2: lazy WalletKeyProvider for P2MR inputs. Opened
        // exactly once across all inputs in the tx (lives until the loop
        // finishes, then destructor scrubs the master key). No-op for
        // plain taproot-only txs — cheap to keep inert.
        std::unique_ptr<dinero::wallet::V7P2MRStore> p2mr_store;
        std::unique_ptr<dinero::wallet::WalletKeyProvider> p2mr_provider;
        auto ensure_p2mr_provider = [&]() -> dinero::wallet::WalletKeyProvider* {
            if (p2mr_provider) return p2mr_provider.get();
            auto master_opt = wallet.GetV7PqMasterKey();
            if (!master_opt) return nullptr;
            const std::string store_path = wallet.GetV7P2MRStorePath();
            if (store_path.empty()) return nullptr;
            p2mr_store = std::make_unique<dinero::wallet::V7P2MRStore>();
            if (p2mr_store->Open(store_path) != dinero::wallet::V7P2MRStore::OpenResult::Ok) {
                p2mr_store.reset();
                return nullptr;
            }
            dinero::wallet::WalletKeyProvider::Config cfg;
            cfg.p2mr_store = p2mr_store.get();
            cfg.wallet_id  = 1;  // single-wallet today, matches v7 RPC handlers
            std::memcpy(cfg.master_key.data(), master_opt->data(), cfg.master_key.size());
            OPENSSL_cleanse(const_cast<uint8_t*>(master_opt->data()), master_opt->size());
            p2mr_provider = std::make_unique<dinero::wallet::WalletKeyProvider>(std::move(cfg));
            return p2mr_provider.get();
        };

        // Sign the wallet-owned inputs we have enough metadata for.
        for (size_t i = 0; i < tx.vin.size(); ++i) {
            const auto& utxo = input_utxos[i];
            if (had_witness[i]) {
                continue;
            }
            const bool is_p2mr = have_prevout[i] &&
                dinero::consensus::pq::IsP2MRScript(utxo.spk);

            // P2MR inputs take a dedicated branch — no ECDSA private key is
            // needed (the PQ seed is resolved inside WalletKeyProvider by
            // merkle-root lookup). BIP-341 sighash is computed over the
            // full input set, same as consensus ValidateP2MRSpend.
            if (is_p2mr) {
                if (!have_all_prevouts) {
                    continue;
                }
                auto* provider = ensure_p2mr_provider();
                if (!provider) {
                    continue;  // wallet locked or store unavailable
                }

                dinero::consensus::ScriptExecutionContext sctx(
                    &tx, static_cast<uint32_t>(i),
                    utxo.value.GetUna(), /*flags=*/0);
                sctx.all_amounts.reserve(input_utxos.size());
                sctx.all_scriptpubkeys.reserve(input_utxos.size());
                sctx.all_confidential_flags.reserve(input_utxos.size());
                sctx.all_input_commitments.reserve(input_utxos.size());
                for (const auto& u : input_utxos) {
                    sctx.all_amounts.push_back(u.value.GetUna());
                    sctx.all_scriptpubkeys.push_back(u.spk);
                    sctx.all_confidential_flags.push_back(u.is_confidential ? 1 : 0);
                    sctx.all_input_commitments.push_back(u.commitment);
                }
                std::vector<uint8_t> leaf_hash;
                std::vector<uint8_t> sighash_vec =
                    dinero::consensus::SignatureHashTaproot(sctx, /*hash_type=*/0x00, leaf_hash);
                if (sighash_vec.size() != 32) {
                    continue;
                }
                std::array<uint8_t, 32> sighash32{};
                std::memcpy(sighash32.data(), sighash_vec.data(), 32);

                std::vector<uint8_t> witness_bytes = provider->SignP2MR(utxo.spk, sighash32);
                if (witness_bytes.empty()) {
                    continue;
                }
                tx.vin[i].scriptSig.clear();
                tx.vin[i].witness.clear();
                tx.vin[i].witness.push_back(std::move(witness_bytes));
                signed_count++;
                continue;
            }

            if (!have_prevout[i] || !input_private_keys[i].has_value()) {
                continue;
            }

            bool signed_input = false;
            if (dinero::TaprootTxSigner::IsTaprootUTXO(utxo)) {
                if (!have_all_prevouts || utxo.path.empty()) {
                    continue;
                }
                signed_input = dinero::TaprootTxSigner::SignInput(
                    tx,
                    i,
                    input_utxos,
                    *input_private_keys[i]);
            } else {
                signed_input = dinero::BIP143Signer::SignInput(
                    tx,
                    i,
                    utxo,
                    *input_private_keys[i]);
            }

            if (signed_input) {
                signed_count++;
            }
        }

        tx.DetectWitnessVersion();
        result["hex"] = tx.SerializeHex(true);

        // "complete" means every input passes the same validator used by
        // mempool and block connection. Counting witness elements would let a
        // malformed pre-populated covenant witness masquerade as complete.
        bool complete =
            have_all_prevouts && signed_count == tx.vin.size();
        if (complete) {
            std::vector<dinero::consensus::UTXOEntry> prevout_entries;
            prevout_entries.reserve(input_utxos.size());
            for (const auto& utxo : input_utxos) {
                prevout_entries.emplace_back(
                    utxo.value,
                    utxo.spk,
                    utxo.height,
                    utxo.is_coinbase,
                    utxo.is_confidential,
                    utxo.commitment);
            }
            uint32_t validation_height = 0;
            if (chainstate) {
                const uint32_t tip_height =
                    chainstate->getBlockHeight();
                validation_height =
                    tip_height == UINT32_MAX ? tip_height : tip_height + 1;
            } else {
                complete = false;
            }
            if (complete) {
                const dinero::consensus::PrecomputedTransactionData
                    precomputed(tx, prevout_entries);
                for (size_t i = 0; i < tx.vin.size(); ++i) {
                    if (dinero::consensus::ValidateSpend(
                            tx,
                            i,
                            prevout_entries[i],
                            validation_height,
                            prevout_entries,
                            &precomputed) !=
                        dinero::consensus::ScriptValidationResult::OK) {
                        complete = false;
                        break;
                    }
                }
            }
        }
        result["complete"] = complete;

        if (ctx.logger) {
            ctx.logger->info("[wallet.signrawtransaction] Signed " +
                std::to_string(signed_count) + "/" + std::to_string(tx.vin.size()) + " inputs");
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to sign transaction: ") + e.what();
    }

    return result;
}

/**
 * wallet.decoderawtransaction - Decode a raw transaction hex
 *
 * Params:
 *   [0] hex: raw transaction hex
 *
 * Returns: decoded transaction object
 */
din::Json rpc_context_wallet_decoderawtransaction(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.decoderawtransaction \"hex\"";
        return result;
    }

    try {
        std::string tx_hex = params[0].as<std::string>();

        dinero::Transaction tx;
        if (!dinero::TransactionSerializer::Deserialize(tx, tx_hex)) {
            result["error"] = "Failed to decode transaction";
            return result;
        }

        result["txid"] = tx.GetTxid().AsUint256().GetHex();
        result["version"] = tx.version;
        result["locktime"] = static_cast<int>(tx.lockTime);
        result["size"] = static_cast<int>(tx_hex.length() / 2);

        // Inputs
        din::Json vin_arr = din::arr();
        for (size_t i = 0; i < tx.vin.size(); ++i) {
            const auto& input = tx.vin[i];
            din::Json inp;
            inp["txid"] = input.prevout.txid.AsUint256().GetHex();
            inp["vout"] = static_cast<int>(input.prevout.vout);
            inp["sequence"] = static_cast<int64_t>(input.sequence);

            // ScriptSig hex
            std::ostringstream script_hex;
            for (uint8_t b : input.scriptSig) {
                script_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
            }
            inp["scriptSig"] = script_hex.str();

            // Witness
            if (!input.witness.empty()) {
                din::Json witness_arr = din::arr();
                for (const auto& w : input.witness) {
                    std::ostringstream w_hex;
                    for (uint8_t b : w) {
                        w_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
                    }
                    witness_arr.append(w_hex.str());
                }
                inp["txinwitness"] = witness_arr;
            }

            vin_arr.append(inp);
        }
        result["vin"] = vin_arr;

        // Outputs
        din::Json vout_arr = din::arr();
        for (size_t i = 0; i < tx.vout.size(); ++i) {
            const auto& output = tx.vout[i];
            din::Json outp;
            outp["n"] = static_cast<int>(i);
            PopulateWalletOutputDisplay(outp, output);

            // ScriptPubKey
            din::Json spk;
            std::ostringstream spk_hex;
            for (uint8_t b : output.scriptPubKey) {
                spk_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
            }
            spk["hex"] = spk_hex.str();

            // Determine type
            if (output.IsSegWitV0()) {
                spk["type"] = output.scriptPubKey.size() == 22 ? "witness_v0_keyhash" : "witness_v0_scripthash";
            } else if (output.IsTaproot()) {
                spk["type"] = "witness_v1_taproot";
            } else {
                spk["type"] = "unknown";
            }

            outp["scriptPubKey"] = spk;
            vout_arr.append(outp);
        }
        result["vout"] = vout_arr;

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to decode transaction: ") + e.what();
    }

    return result;
}

/**
 * wallet.getrawtransaction - Get raw transaction from mempool or blockchain
 *
 * Params:
 *   [0] txid: transaction hash
 *   [1] verbose: bool (default: false)
 *
 * Returns: hex string or decoded object
 */
din::Json rpc_context_wallet_getrawtransaction(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.getrawtransaction \"txid\" [verbose=false]";
        return result;
    }

    try {
        std::string txid = params[0].as<std::string>();
        bool verbose = params.size() >= 2 && params[1].asBool();

        std::shared_ptr<dinero::Transaction> tx;

        // Check mempool first (Phase M.0: Convert hex string to uint256)
        if (ctx.daemon && ctx.daemon->mempool) {
            auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
            if (mempool_service) {
                tx = mempool_service->getTransaction(uint256::FromHexUnsafe(txid));
            }
        }

        // Check chainstate/blockchain if not in mempool
        if (!tx && ctx.daemon && ctx.daemon->chainstate) {
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
            if (chainstate) {
                auto* chain_db = chainstate->GetChainDB();
                if (chain_db) {
                    auto tx_status = chain_db->getTransaction(uint256::FromHexUnsafe(txid));
                    if (tx_status.ok()) {
                        tx = std::make_shared<dinero::Transaction>(tx_status.value());
                    }
                }
            }
        }

        if (!tx) {
            result["error"] = "Transaction not found";
            return result;
        }

        if (verbose) {
            // Return decoded transaction
            din::Json decode_params = din::arr();
            decode_params.append(tx->SerializeHex(true));
            return rpc_context_wallet_decoderawtransaction(ctx, decode_params);
        } else {
            result["hex"] = tx->SerializeHex(true);
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get transaction: ") + e.what();
    }

    return result;
}

/**
 * wallet.sendrawtransaction - Submit a raw transaction to the network
 *
 * Phase 38: Routes through mempool.sendrawtransaction (wallet must not bypass mempool)
 *
 * Params:
 *   [0] hex: signed raw transaction hex
 *   [1] maxfeerate: max fee rate in una/vB (optional, default: 0.10 DIN/kvB)
 *
 * Returns: txid if accepted
 */
din::Json rpc_context_wallet_sendrawtransaction(const ExecutionContext& ctx, const din::Json& params) {
    // spec Fatal §3: gate before forwarding so the caller sees the safe-mode
    // error even though mempool.sendrawtransaction also gates internally.
    din::Json result;
    if (RefuseIfSafeMode(ctx, result)) return result;

    // Phase 38: Wallet MUST route through mempool.sendrawtransaction
    // This ensures wallet doesn't bypass mempool policy validation

    // Forward to mempool.sendrawtransaction (the real implementation)
    extern din::Json rpc_context_mempool_sendrawtransaction(const ExecutionContext&, const din::Json&);
    return rpc_context_mempool_sendrawtransaction(ctx, params);
}

/**
 * wallet.recordsend - Record a send transaction in wallet history
 *
 * Called by external signers (e.g. DineroDPI) that build and broadcast
 * transactions via sendrawtransaction, which doesn't record history.
 *
 * Params (object):
 *   txid:    transaction ID (required)
 *   address: recipient address (required)
 *   amount:  amount sent in DIN (required, positive)
 *   fee:     fee in DIN (optional, default 0)
 *
 * Returns: {success: true}
 */
din::Json rpc_context_wallet_recordsend(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    std::string txid, address;
    double amount_din = 0.0, fee_din = 0.0;

    if (params.isObject()) {
        if (params.isMember("txid") && params["txid"].is<std::string>())
            txid = params["txid"].asString();
        if (params.isMember("address") && params["address"].is<std::string>())
            address = params["address"].asString();
        if (params.isMember("amount"))
            amount_din = params["amount"].is<double>() ? params["amount"].asDouble()
                       : (params["amount"].isInt() ? static_cast<double>(params["amount"].asInt()) : 0.0);
        if (params.isMember("fee"))
            fee_din = params["fee"].is<double>() ? params["fee"].asDouble()
                    : (params["fee"].isInt() ? static_cast<double>(params["fee"].asInt()) : 0.0);
    } else if (params.isArray() && params.size() >= 3) {
        txid = params[0].asString();
        address = params[1].asString();
        amount_din = params[2].is<double>() ? params[2].asDouble()
                   : static_cast<double>(params[2].asInt());
        if (params.size() >= 4)
            fee_din = params[3].is<double>() ? params[3].asDouble()
                    : static_cast<double>(params[3].asInt());
    }

    if (txid.empty() || address.empty() || amount_din <= 0) {
        result["error"] = "Usage: wallet.recordsend {txid, address, amount [, fee]}";
        return result;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Record as negative amount (outgoing), same as wallet.sendtoaddress
    bool ok = wallet_service->get().addTransaction(
        txid, address, -(amount_din + fee_din),
        "send", false, "", now, 0);

    if (!ok) {
        result["error"] = "Failed to record send transaction";
        return result;
    }

    result["success"] = true;
    if (ctx.logger) {
        ctx.logger->info("[wallet.recordsend] Recorded send: " + txid +
                         " -> " + address + " (" + std::to_string(amount_din) + " DIN)");
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════
// Import/Export Methods
// ═══════════════════════════════════════════════════════════════

/**
 * wallet.importprivkey - Import a private key into the wallet
 *
 * Params:
 *   [0] privkey: WIF-encoded private key or hex
 *   [1] label: address label (optional)
 *   [2] rescan: whether to rescan blockchain (default: true)
 *
 * Returns: {address, success}
 */
din::Json rpc_context_wallet_importprivkey(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.importprivkey \"privkey\" [label] [rescan=true]";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        std::string privkey_str = params[0].asString();
        std::string label = params.size() >= 2 ? params[1].asString() : "";
        bool rescan = params.size() < 3 || params[2].asBool();

        auto& wallet = wallet_service->get();

        // Decode private key (WIF or hex)
        std::vector<uint8_t> privkey_bytes;
        if (privkey_str.length() == 64) {
            // Hex format
            privkey_bytes = dinero::TransactionSerializer::FromHex(privkey_str);
        } else {
            // WIF format - decode
            privkey_bytes = wallet.decodeWIF(privkey_str);
            if (privkey_bytes.empty()) {
                result["error"] = "Invalid private key format";
                return result;
            }
        }

        if (privkey_bytes.size() != 32) {
            result["error"] = "Invalid private key length";
            return result;
        }

        // Import the key
        std::string address = wallet.importPrivateKey(privkey_bytes, label);
        if (address.empty()) {
            result["error"] = "Failed to import private key";
            return result;
        }

        result["address"] = address;
        result["success"] = true;

        if (ctx.logger) {
            ctx.logger->info("[wallet.importprivkey] Imported key for address: " + address);
        }

        // Optionally rescan
        if (rescan) {
            result["note"] = "Rescan recommended - use wallet.rescanblockchain";
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to import key: ") + e.what();
    }

    return result;
}

/**
 * wallet.exportseed - Export the HD wallet mnemonic seed phrase
 *
 * Params: none
 *
 * Returns: {mnemonic: "word1 word2 ..."}
 */
din::Json rpc_context_wallet_exportseed(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        auto& mgr = wallet_service->get();
        auto* hd = mgr.getHDWallet();
        if (!hd) {
            result["error"] = "HD wallet not available";
            return result;
        }

        std::string mnemonic = hd->GetMnemonic();
        if (mnemonic.empty()) {
            result["error"] = "No mnemonic available — wallet may have been created without BIP39";
            return result;
        }

        result["mnemonic"] = mnemonic;
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to export seed: ") + e.what();
    }

    return result;
}

/**
 * wallet.dumpwallet - Export all wallet keys to a file
 *
 * Params:
 *   [0] filename: output file path
 *
 * Returns: {filename, keys_exported}
 */
din::Json rpc_context_wallet_dumpwallet(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.dumpwallet \"filename\"";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        std::string filename = params[0].as<std::string>();
        auto& wallet = wallet_service->get();

        // Security check - don't overwrite existing files by default
        std::ifstream test(filename);
        if (test.good()) {
            test.close();
            result["error"] = "File already exists: " + filename;
            return result;
        }

        std::ofstream file(filename);
        if (!file.is_open()) {
            result["error"] = "Cannot open file for writing: " + filename;
            return result;
        }

        // Write header
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        file << "# Wallet dump created by Dinero\n";
        file << "# * Created on " << std::ctime(&time);
        file << "# * Best block at time of backup was unknown\n";
        file << "# * mnemonic available via wallet.exportmnemonic\n\n";

        // Export all addresses with their private keys
        auto addresses = wallet.getWalletAddresses();
        int exported = 0;

        for (const auto& addr : addresses) {
            // Get scriptPubKey for this address (bridge function during migration)
            auto script_pubkey_opt = wallet.getScriptPubKeyForAddress(addr);
            if (!script_pubkey_opt) continue;

            // Derive private key using scriptPubKey (Bitcoin Core semantics)
            auto privkey = wallet.deriveKeyForScriptPubKey(*script_pubkey_opt);
            if (privkey.has_value()) {
                std::string wif = wallet.encodeWIF(privkey.value());
                auto label_opt = wallet.getAddressLabel(addr);
                file << wif << " " << addr;
                if (label_opt.has_value() && !label_opt->empty()) {
                    file << " label=" << *label_opt;
                }
                file << "\n";
                exported++;
            }
        }

        file << "\n# End of dump\n";
        file.close();

        result["filename"] = filename;
        result["keys_exported"] = exported;
        result["success"] = true;

        if (ctx.logger) {
            ctx.logger->info("[wallet.dumpwallet] Exported " + std::to_string(exported) + " keys to " + filename);
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to dump wallet: ") + e.what();
    }

    return result;
}

/**
 * wallet.importwallet - Import keys from a wallet dump file
 *
 * Params:
 *   [0] filename: input file path
 *
 * Returns: {keys_imported}
 */
din::Json rpc_context_wallet_importwallet(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.importwallet \"filename\"";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        std::string filename = params[0].as<std::string>();
        auto& wallet = wallet_service->get();

        std::ifstream file(filename);
        if (!file.is_open()) {
            result["error"] = "Cannot open file: " + filename;
            return result;
        }

        int imported = 0;
        int failed = 0;
        std::string line;

        while (std::getline(file, line)) {
            // Skip comments and empty lines
            if (line.empty() || line[0] == '#') continue;

            // Parse line: WIF address [label=...]
            std::istringstream iss(line);
            std::string wif, address;
            iss >> wif >> address;

            // Extract label if present
            std::string label;
            std::string remaining;
            if (std::getline(iss, remaining)) {
                size_t label_pos = remaining.find("label=");
                if (label_pos != std::string::npos) {
                    label = remaining.substr(label_pos + 6);
                    // Trim whitespace
                    label.erase(0, label.find_first_not_of(" \t"));
                    label.erase(label.find_last_not_of(" \t") + 1);
                }
            }

            // Decode and import
            auto privkey_bytes = wallet.decodeWIF(wif);
            if (privkey_bytes.size() == 32) {
                std::string new_addr = wallet.importPrivateKey(privkey_bytes, label);
                if (!new_addr.empty()) {
                    imported++;
                } else {
                    failed++;
                }
            } else {
                failed++;
            }
        }

        file.close();

        result["keys_imported"] = imported;
        result["keys_failed"] = failed;
        result["success"] = (imported > 0);

        if (ctx.logger) {
            ctx.logger->info("[wallet.importwallet] Imported " + std::to_string(imported) +
                " keys from " + filename + " (" + std::to_string(failed) + " failed)");
        }

        if (imported > 0) {
            result["note"] = "Rescan recommended - use wallet.rescanblockchain";
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to import wallet: ") + e.what();
    }

    return result;
}

/**
 * wallet.exportcsv - Export transaction history to CSV
 *
 * Params:
 *   [0] filename: output file path
 *   [1] start_date: ISO date string (optional)
 *   [2] end_date: ISO date string (optional)
 *
 * Returns: {filename, transactions_exported}
 */
din::Json rpc_context_wallet_exportcsv(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.exportcsv \"filename\" [start_date] [end_date]";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        std::string filename = params[0].as<std::string>();
        auto& wallet = wallet_service->get();

        std::ofstream file(filename);
        if (!file.is_open()) {
            result["error"] = "Cannot open file for writing: " + filename;
            return result;
        }

        // Write CSV header
        file << "Date,Type,Address,Amount,TxID,Confirmations,Label\n";

        // Get transaction history
        auto txs = wallet.getTransactionHistory(10000, 0);  // Get up to 10k txs
        int exported = 0;

        for (const auto& tx : txs) {
            // Format timestamp
            auto time_t_val = static_cast<time_t>(tx.time);
            std::tm* tm = std::gmtime(&time_t_val);
            char date_buf[64];
            std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S UTC", tm);

            // Escape fields that might contain commas
            std::string address = tx.address;
            std::string label = tx.label;

            // Write CSV row
            file << date_buf << ","
                 << tx.category << ","
                 << address << ","
                 << std::fixed << std::setprecision(9) << tx.amount << ","
                 << tx.txid << ","
                 << tx.confirmations << ","
                 << label << "\n";
            exported++;
        }

        file.close();

        result["filename"] = filename;
        result["transactions_exported"] = exported;
        result["success"] = true;

        if (ctx.logger) {
            ctx.logger->info("[wallet.exportcsv] Exported " + std::to_string(exported) +
                " transactions to " + filename);
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to export CSV: ") + e.what();
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// Wallet Recovery & Rescan Methods (Phase 33.5)
// ═══════════════════════════════════════════════════════════════

/**
 * wallet.rescanblockchain - Rescan blockchain for wallet transactions
 *
 * This uses Utreexo proofs for fast verification when available.
 *
 * Params:
 *   [0] start_height: int - Starting block height (default: 0)
 *   [1] stop_height: int - Ending block height (default: current tip)
 *
 * Returns: {start_height, stop_height, progress}
 */
din::Json rpc_context_wallet_rescanblockchain(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate_service) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    const auto readiness = CheckRescanReadiness(chainstate_service);
    if (!readiness.ok) {
        result["error"] = readiness.reason;
        result["rescan_blocked"] = true;
        return result;
    }

    try {
        uint32_t current_height = chainstate_service->getBlockHeight();
        int start_height = 0;
        int stop_height = static_cast<int>(current_height);

        // Phase 35.1: UX improvement - support start_height parameter
        if (params.size() >= 1 && params[0].isInt()) {
            start_height = params[0].asInt();
        }

        // Phase 35.1: UX improvement - support stop_height parameter
        if (params.size() >= 2 && params[1].isInt()) {
            stop_height = params[1].asInt();
            // Clamp to current chain height
            if (stop_height > static_cast<int>(current_height)) {
                stop_height = static_cast<int>(current_height);
            }
        }

        // Validate range
        if (start_height < 0) start_height = 0;
        if (stop_height < start_height) {
            result["error"] = "stop_height cannot be less than start_height";
            return result;
        }

        if (ctx.logger) {
            ctx.logger->info("[wallet.rescanblockchain] Starting block-scanning rescan from height " +
                std::to_string(start_height) + " to " + std::to_string(stop_height));
        }

        auto& wallet = wallet_service->get();

        // Phase 39: Access ChainDB via ChainstateService
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"] = "Chainstate service not available";
            return result;
        }
        auto* chain_db = chainstate->GetChainDB();
        if (!chain_db) {
            result["error"] = "Chain database not available";
            return result;
        }

        // ═══════════════════════════════════════════════════════════════
        // USE BLOCK-SCANNING RESCAN (not forEachUTXO).
        // Dinero is a Utreexo node — the full UTXO set is NOT stored in
        // RocksDB. forEachUTXO only returns a tiny subset. The correct
        // approach scans each block's transactions for wallet outputs.
        // WalletManager::rescanBlockchain() handles:
        //   - Gap-limit address derivation (20 external + 20 change)
        //   - watch_scripts matching against block outputs
        //   - Spend tracking (inputs consuming wallet UTXOs)
        //   - Reorg safety (clears UTXOs >= start_height)
        // ═══════════════════════════════════════════════════════════════
        constexpr int kRescanGapLimit = 20;
        bool rescan_ok = wallet.rescanBlockchain(
            start_height,
            kRescanGapLimit,
            chain_db,
            ctx.daemon ? ctx.daemon->block_storage.get() : nullptr);

        if (!rescan_ok) {
            result["error"] = "Block-scanning rescan failed";
            return result;
        }

        // Phase 35.1: Enhanced UX response format
        result["start_height"] = static_cast<Json::Int>(start_height);
        result["stop_height"] = static_cast<Json::Int>(stop_height);
        result["scanned_blocks"] = static_cast<Json::Int>(stop_height - start_height + 1);

        // Phase 35.1: UX improvements - progress and completion status
        result["complete"] = true;
        result["progress"] = 1.0;
        result["success"] = true;

    } catch (const std::exception& e) {
        result["error"] = std::string("Rescan failed: ") + e.what();
    }

    return result;
}

/**
 * wallet.signpsbt - Alias for wallet.processpsbt (sign a PSBT)
 *
 * Bitcoin Core compatible alias.
 */
din::Json rpc_context_wallet_signpsbt(const ExecutionContext& ctx, const din::Json& params) {
    return rpc_context_wallet_processpsbt(ctx, params);
}

/**
 * wallet.abortrescan - Abort an ongoing blockchain rescan
 */
din::Json rpc_context_wallet_abortrescan(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    uint32_t chain_height = 0;
    if (ctx.daemon->chainstate) {
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (chainstate) {
            chain_height = chainstate->getBlockHeight();
        }
    }

    const auto scan_status = wallet_service->get().GetScanStatus(chain_height);
    result["abort_supported"] = false;
    result["is_scanning"] = scan_status.is_scanning;
    result["scan_height"] = static_cast<Json::UInt>(scan_status.scan_height);
    result["chain_height"] = static_cast<Json::UInt>(scan_status.chain_height);
    result["success"] = false;

    if (scan_status.is_scanning) {
        result["error"] = "Active scan detected but abort is not supported by current WalletManager API";
    } else {
        result["error"] = "No active wallet rescan to abort";
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// Label and Address Book Methods
// ═══════════════════════════════════════════════════════════════

din::Json rpc_context_wallet_setlabel(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.size() < 2 || !params[0].is<std::string>() || !params[1].is<std::string>()) {
        result["error"] = "Usage: wallet.setlabel <address> <label>";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        std::string address = params[0].as<std::string>();
        std::string label = params[1].as<std::string>();
        wallet_service->get().setAddressLabel(address, label);
        result["success"] = true;
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to set label: ") + e.what();
    }

    return result;
}

din::Json rpc_context_wallet_getlabel(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.getlabel <address>";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        std::string address = params[0].as<std::string>();
        auto label = wallet_service->get().getAddressLabel(address);
        if (label) {
            result["label"] = *label;
        } else {
            result["label"] = din::Json();  // null
        }
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get label: ") + e.what();
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// Wallet Management Methods
// ═══════════════════════════════════════════════════════════════

/**
 * wallet.listwallets - List all available wallets
 *
 * Returns array of wallet objects with metadata:
 * - name: Wallet name (user-defined label)
 * - encrypted: Whether wallet is encrypted
 * - network: Network (mainnet/testnet/regtest)
 *
 * Example:
 * > wallet.listwallets
 * [
 *   {"name": "Mining", "encrypted": true, "network": "mainnet"},
 *   {"name": "Daily Spend", "encrypted": false, "network": "mainnet"}
 * ]
 */
din::Json rpc_context_wallet_listwallets(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result = din::arr();

    if (!ctx.daemon || !ctx.daemon->wallet) {
        return result;  // Empty array if no wallet service
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        return result;
    }

    try {
        auto wallets = wallet_service->listWallets();
        for (const auto& name : wallets) {
            din::Json wallet_obj;
            wallet_obj["name"] = name;
            // Note: Additional metadata (encrypted, network) could be added
            // by opening each wallet, but that's expensive for listing
            result.append(wallet_obj);
        }
    } catch (const std::exception& e) {
        // Return empty array on error
        dinero::g_logger.error("[RPC] wallet.listwallets error: " + std::string(e.what()));
    }

    return result;
}

/**
 * wallet.open - Open/switch active wallet
 *
 * Parameters:
 * - wallet_name (required): Wallet name to open
 *
 * Accepted parameter formats:
 * - Array: ["wallet_name"]
 * - Object: {"name":"wallet_name"}
 */
din::Json rpc_context_wallet_open(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    std::string wallet_name;
    if (params.isArray()) {
        if (!params.empty() && params[0].isString()) {
            wallet_name = params[0].asString();
        }
    } else if (params.isObject()) {
        if (params.isMember("name") && params["name"].isString()) {
            wallet_name = params["name"].asString();
        } else if (params.isMember("wallet_name") && params["wallet_name"].isString()) {
            wallet_name = params["wallet_name"].asString();
        }
    }

    if (wallet_name.empty()) {
        result["error"] = "Usage: wallet.open <wallet_name>";
        return result;
    }

    try {
        auto& mgr = wallet_service->get();

        if (!mgr.exists(wallet_name)) {
            result["error"] = "Wallet '" + wallet_name + "' not found";
            return result;
        }

        mgr.open(wallet_name);
        wallet_service->EnsureRuntimeWalletBindings();

        result["success"] = true;
        result["wallet_name"] = mgr.getCurrentWalletName();
        result["encrypted"] = mgr.isWalletEncrypted();
        result["locked"] = mgr.isWalletLocked();
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to open wallet: ") + e.what();
    }

    return result;
}

/**
 * wallet.unload - Unload the active wallet
 *
 * Parameters:
 * - wallet_name (optional): If provided, must match the currently active wallet
 *
 * Accepted parameter formats:
 * - Array: ["wallet_name"]
 * - Object: {"name":"wallet_name"}
 */
din::Json rpc_context_wallet_unload(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    std::string requested_wallet_name;
    if (params.isArray()) {
        if (!params.empty() && params[0].isString()) {
            requested_wallet_name = params[0].asString();
        }
    } else if (params.isObject()) {
        if (params.isMember("name") && params["name"].isString()) {
            requested_wallet_name = params["name"].asString();
        } else if (params.isMember("wallet_name") && params["wallet_name"].isString()) {
            requested_wallet_name = params["wallet_name"].asString();
        }
    }

    try {
        auto& mgr = wallet_service->get();
        if (!mgr.hasActiveWallet()) {
            result["success"] = true;
            result["unloaded"] = false;
            result["message"] = "No active wallet loaded";
            return result;
        }

        const std::string active_wallet_name = mgr.getCurrentWalletName();
        if (!requested_wallet_name.empty() && requested_wallet_name != active_wallet_name) {
            result["error"] = "Active wallet is '" + active_wallet_name + "', not '" + requested_wallet_name + "'";
            return result;
        }

        mgr.unload();
        result["success"] = true;
        result["unloaded"] = true;
        result["wallet_name"] = active_wallet_name;
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to unload wallet: ") + e.what();
    }

    return result;
}

/**
 * wallet.rename - Rename an existing wallet
 *
 * Changes the user-visible wallet name (label).
 * Does NOT change the underlying wallet file path.
 *
 * Parameters:
 * - old_name (required): Current wallet name
 * - new_name (required): New wallet name
 *
 * Example:
 * > wallet.rename "default" "Mining Wallet"
 * {"success": true, "name": "Mining Wallet"}
 */
din::Json rpc_context_wallet_rename(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!params.isArray() || params.size() < 2) {
        result["error"] = "Usage: wallet.rename <old_name> <new_name>";
        return result;
    }

    std::string old_name = params[0].asString();
    std::string new_name = params[1].asString();

    if (old_name.empty() || new_name.empty()) {
        result["error"] = "Wallet names cannot be empty";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Wallet service not initialized";
        return result;
    }

    try {
        auto& mgr = wallet_service->get();

        if (!mgr.exists(old_name)) {
            result["error"] = "Wallet '" + old_name + "' not found";
            return result;
        }

        if (mgr.exists(new_name)) {
            result["error"] = "Wallet '" + new_name + "' already exists";
            return result;
        }

        // Rename wallet in registry
        mgr.rename(old_name, new_name);

        result["success"] = true;
        result["name"] = new_name;
        result["old_name"] = old_name;

    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to rename wallet: ") + e.what();
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// Utility Methods
// ═══════════════════════════════════════════════════════════════

din::Json rpc_context_wallet_rescan(const ExecutionContext& ctx, const din::Json& params) {
    return rpc_context_wallet_rescanblockchain(ctx, params);
}

din::Json rpc_context_wallet_settxfee(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (params.empty() || !params[0].is<double>()) {
        result["error"] = "Usage: wallet.settxfee <fee_rate>";
        return result;
    }

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    const double fee_rate = params[0].as<double>();
    if (fee_rate <= 0.0 || !std::isfinite(fee_rate)) {
        result["error"] = "Fee rate must be a positive finite number";
        return result;
    }

    std::ostringstream fee_rate_stream;
    fee_rate_stream << std::fixed << std::setprecision(8) << fee_rate;
    wallet_service->get().setSetting("wallet.txfee_din_kb", fee_rate_stream.str());

    result["success"] = true;
    result["fee_rate_din_kb"] = fee_rate;
    result["setting_key"] = "wallet.txfee_din_kb";
    return result;
}

din::Json rpc_context_wallet_listaddresseswithbalances(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) {
        result["error"] = "No active wallet";
        return result;
    }

    try {
        auto addresses = wallet_service->get().listAddresses(true);
        din::Json addr_array = din::arr();

        auto build_path = [](const dinero::AddressRow& addr_row) -> std::string {
            if (addr_row.account < 0) {
                return "imported";
            }
            const bool is_taproot = AddressRowIsTaproot(addr_row);
            const int purpose = is_taproot ? 86 : 84;
            return BuildStandardDerivationPath(
                static_cast<uint32_t>(purpose),
                addr_row.account,
                addr_row.change,
                addr_row.index);
        };

        for (const auto& addr_row : addresses) {
            auto balance = addr_row.script_pubkey.empty()
                               ? wallet_service->get().getAddressBalance(addr_row.address)
                               : wallet_service->get().getScriptPubKeyBalance(addr_row.script_pubkey);
            if (balance.total > 0.0) {
                din::Json addr_obj;
                addr_obj["address"] = addr_row.address;
                if (addr_row.label) {
                    addr_obj["label"] = *addr_row.label;
                }
                addr_obj["account"] = addr_row.account;
                addr_obj["change"] = addr_row.change;
                addr_obj["index"] = addr_row.index;
                addr_obj["external"] = addr_row.external;
                addr_obj["type"] = addr_row.type;
                if (!addr_row.script_pubkey.empty()) {
                    addr_obj["scriptPubKey"] = addr_row.script_pubkey;
                }
                addr_obj["path"] = build_path(addr_row);
                addr_obj["balance"] = balance.total;
                addr_obj["confirmed"] = balance.confirmed;
                addr_obj["unconfirmed"] = balance.unconfirmed;
                addr_obj["immature"] = balance.immature;
                addr_obj["spendable"] = balance.spendable;
                addr_obj["utxo_count"] = balance.utxo_count;
                addr_array.append(addr_obj);
            }
        }

        result = addr_array;
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to list addresses with balances: ") + e.what();
    }

    return result;
}

din::Json rpc_context_wallet_generateqrcode(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    din::Json result;

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"] = "Usage: wallet.generateqrcode <address> [amount] [label] [message]";
        return result;
    }

    const std::string address = params[0].as<std::string>();
    if (address.empty()) {
        result["error"] = "Address cannot be empty";
        return result;
    }

    auto url_encode = [](const std::string& value) -> std::string {
        std::ostringstream encoded;
        encoded << std::hex << std::uppercase;
        for (unsigned char c : value) {
            if ((c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') {
                encoded << static_cast<char>(c);
            } else {
                encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
            }
        }
        return encoded.str();
    };

    std::vector<std::string> query_parts;
    if (params.size() >= 2 && params[1].isNumeric()) {
        std::ostringstream amount_stream;
        amount_stream << std::fixed << std::setprecision(8) << params[1].asDouble();
        query_parts.push_back("amount=" + amount_stream.str());
    }
    if (params.size() >= 3 && params[2].is<std::string>()) {
        const std::string label = params[2].as<std::string>();
        if (!label.empty()) {
            query_parts.push_back("label=" + url_encode(label));
        }
    }
    if (params.size() >= 4 && params[3].is<std::string>()) {
        const std::string message = params[3].as<std::string>();
        if (!message.empty()) {
            query_parts.push_back("message=" + url_encode(message));
        }
    }

    std::string uri = "dinero:" + address;
    if (!query_parts.empty()) {
        uri += "?";
        for (size_t i = 0; i < query_parts.size(); ++i) {
            if (i != 0) {
                uri += "&";
            }
            uri += query_parts[i];
        }
    }

    result["success"] = true;
    result["address"] = address;
    result["uri"] = uri;
    result["format"] = "bip21";
    result["qr_content"] = uri;
    return result;
}

din::Json rpc_context_wallet_createhd(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    result = dinero::rpc::RpcCreateHDWallet(params, &wallet_service->get());
    if (!result.isMember("error")) {
        wallet_service->EnsureRuntimeWalletBindings();
    }
    return result;
}

din::Json rpc_context_wallet_restore(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    bool rescan_requested = true;
    if (params.isObject() && params.isMember("rescan") && params["rescan"].isBool()) {
        rescan_requested = params["rescan"].asBool();
    }

    result = dinero::rpc::RpcRestoreWallet(params, &wallet_service->get());
    if (!result.isMember("error")) {
        wallet_service->EnsureRuntimeWalletBindings();
    }

    if (result.isMember("success") && result["success"].asBool()) {
        if (rescan_requested) {
            din::Json rescan_params(Json::arrayValue);
            rescan_params.append(0);
            din::Json rescan_result = rpc_context_wallet_rescanblockchain(ctx, rescan_params);
            if (rescan_result.isMember("error")) {
                result["warning"] = "Wallet restored, but blockchain rescan failed";
                result["rescan_success"] = false;
                result["rescan_error"] = rescan_result["error"];
            } else {
                result["rescan_success"] =
                    !rescan_result.isMember("success") ||
                    !rescan_result["success"].isBool() ||
                    rescan_result["success"].asBool();
                result["rescan"] = rescan_result;
            }
        }
        result["birthday_height"] = wallet_service->get().getBirthdayHeight();
    }

    return result;
}

din::Json rpc_context_wallet_exportmnemonic(const ExecutionContext& ctx, const din::Json& params) {
    return rpc_context_wallet_exportseed(ctx, params);
}

/**
 * wallet.delete - Permanently remove a wallet
 *
 * Accepted parameter formats:
 * - Array: ["wallet_name"]
 * - Object: {"name":"wallet_name"}
 */
din::Json rpc_context_wallet_delete(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }

    std::string wallet_name;
    if (params.isArray()) {
        if (!params.empty() && params[0].isString()) {
            wallet_name = params[0].asString();
        }
    } else if (params.isObject()) {
        if (params.isMember("name") && params["name"].isString()) {
            wallet_name = params["name"].asString();
        } else if (params.isMember("wallet_name") && params["wallet_name"].isString()) {
            wallet_name = params["wallet_name"].asString();
        }
    }

    if (wallet_name.empty()) {
        result["error"] = "Usage: wallet.delete <wallet_name>";
        return result;
    }

    try {
        auto& mgr = wallet_service->get();
        if (!mgr.exists(wallet_name)) {
            result["error"] = "Wallet '" + wallet_name + "' not found";
            return result;
        }

        if (mgr.hasActiveWallet() && mgr.getCurrentWalletName() == wallet_name) {
            mgr.unload();
        }

        mgr.remove(wallet_name);
        result["success"] = true;
        result["wallet_name"] = wallet_name;
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to delete wallet: ") + e.what();
    }

    return result;
}

din::Json rpc_context_wallet_notarizebackup(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    (void)params;
    din::Json result;
    result["error"]["code"] = -32601;
    result["error"]["message"] = "wallet.notarizebackup is not supported in this build";
    result["error"]["detail"] = "Create a backup with wallet.backup and notarize externally";
    return result;
}

din::Json rpc_context_wallet_scanutxos(const ExecutionContext& ctx, const din::Json& params) {
    // Compatibility shim:
    // - scanutxos "start" [start_height] [stop_height] -> rescanblockchain
    // - scanutxos "status" -> wallet scan status
    // - scanutxos "abort"  -> abortrescan
    if (!params.empty() && params[0].is<std::string>()) {
        const std::string action = params[0].as<std::string>();
        if (action == "abort") {
            din::Json empty_params(Json::arrayValue);
            return rpc_context_wallet_abortrescan(ctx, empty_params);
        }
        if (action == "status") {
            din::Json result;
            if (!ctx.daemon || !ctx.daemon->wallet) {
                result["error"] = "Wallet service not available";
                return result;
            }
            auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
            if (!wallet_service || !wallet_service->hasActiveWallet()) {
                result["error"] = "No active wallet";
                return result;
            }

            uint32_t chain_height = 0;
            if (ctx.daemon->chainstate) {
                auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
                if (chainstate) {
                    chain_height = chainstate->getBlockHeight();
                }
            }

            const auto status = wallet_service->get().GetScanStatus(chain_height);
            result["action"] = "status";
            result["is_scanning"] = status.is_scanning;
            result["scan_height"] = static_cast<Json::UInt>(status.scan_height);
            result["chain_height"] = static_cast<Json::UInt>(status.chain_height);
            result["progress"] = status.progress();
            result["synced"] = status.is_synced();
            return result;
        }
        if (action != "start") {
            din::Json result;
            result["error"] = "Usage: wallet.scanutxos [\"start\"|\"status\"|\"abort\"] [start_height] [stop_height]";
            return result;
        }

        din::Json rescan_params(Json::arrayValue);
        if (params.size() >= 2 && params[1].isInt()) {
            rescan_params.append(params[1]);
        }
        if (params.size() >= 3 && params[2].isInt()) {
            rescan_params.append(params[2]);
        }
        return rpc_context_wallet_rescanblockchain(ctx, rescan_params);
    }

    return rpc_context_wallet_rescanblockchain(ctx, params);
}

// Phase W.1.1: Import mnemonic and trigger rescan
din::Json rpc_context_wallet_importmnemonic(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Get wallet service from context
    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    // Get chainstate from context (needed for rescan)
    if (!ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    bool rescan_requested = true;
    int rescan_start_height = 0;
    if (params.isObject() && params.isMember("rescan") && params["rescan"].isBool()) {
        rescan_requested = params["rescan"].asBool();
    }
    if (params.isObject() && params.isMember("birthday_height") && params["birthday_height"].isInt()) {
        rescan_start_height = std::max(0, params["birthday_height"].asInt());
    }

    if (rescan_requested) {
        auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate_service) {
            result["error"] = "Chainstate service not available";
            return result;
        }
        const auto readiness = CheckRescanReadiness(chainstate_service);
        if (!readiness.ok) {
            result["success"] = false;
            result["error"] = readiness.reason;
            result["rescan_blocked"] = true;
            result["hint"] = "Retry wallet.importmnemonic once chain activation failures stop, or import with rescan=false.";
            return result;
        }
    }

    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service) {
        result["error"] = "Failed to cast wallet service";
        return result;
    }
    dinero::WalletManager* wallet_manager = &wallet_service->get();

    // Get ChainDB from chainstate and set global for rescan
    // Phase W.1.1: Temporary until rescan API is refactored to accept ChainDB parameter
    dinero::g_chain_db_direct = ctx.daemon->chainstate->GetChainDB();

    din::Json import_params = params;
    if (import_params.isObject()) {
        import_params["rescan"] = false;
    }

    result = dinero::rpc::RpcImportMnemonic(import_params, wallet_manager);
    if (!result.isMember("error")) {
        wallet_service->EnsureRuntimeWalletBindings();
    }

    if (rescan_requested &&
        result.isMember("success") &&
        result["success"].isBool() &&
        result["success"].asBool()) {
        din::Json rescan_params(Json::arrayValue);
        rescan_params.append(rescan_start_height);
        din::Json rescan_result = rpc_context_wallet_rescanblockchain(ctx, rescan_params);
        if (rescan_result.isMember("error")) {
            result["warning"] = "Mnemonic imported, but blockchain rescan failed";
            result["rescan_success"] = false;
            result["rescan_error"] = rescan_result["error"];
        } else {
            result["rescan_success"] =
                !rescan_result.isMember("success") ||
                !rescan_result["success"].isBool() ||
                rescan_result["success"].asBool();
            result["rescan"] = rescan_result;
        }
    }

    return result;
}

din::Json rpc_context_wallet_migratelegacysidecar(const ExecutionContext& ctx, const din::Json& params) {
    dinero::WalletManager* wallet_manager = nullptr;
    if (ctx.daemon && ctx.daemon->wallet) {
        wallet_manager = &ctx.daemon->wallet->get();
    }

    return dinero::rpc::RpcMigrateLegacySidecar(params, wallet_manager);
}

// ═══════════════════════════════════════════════════════════════════════════
// TAPROOT DESCRIPTOR IMPORT (BIP341 Compliant)
// ═══════════════════════════════════════════════════════════════════════════
// wallet.importtaprootdescriptor - Import tr(<hex-privkey>) with mandatory rescan
// This is the ONLY recovery-safe way to import single Taproot keys.
// ═══════════════════════════════════════════════════════════════════════════

din::Json rpc_context_wallet_importtaprootdescriptor(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Get wallet service from context
    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    // Get chainstate from context (needed for rescan)
    if (!ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    // Get WalletManager from context
    auto& wallet_service = ctx.daemon->wallet;
    dinero::WalletManager* wallet_manager = &wallet_service->get();

    // Set global ChainDB for rescan (temporary until API refactor)
    dinero::g_chain_db_direct = ctx.daemon->chainstate->GetChainDB();

    // Call the Taproot descriptor import implementation
    return dinero::rpc::RpcImportTaprootDescriptor(params, wallet_manager);
}

// ═══════════════════════════════════════════════════════════════
// PHASE 1: DESCRIPTOR WALLET RPCs (Bitcoin Core Compatibility)
// ═══════════════════════════════════════════════════════════════

/**
 * wallet.listdescriptors - List active wallet descriptors
 */
din::Json rpc_context_wallet_listdescriptors(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Get wallet service from context
    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    // Get WalletManager from context
    auto& wallet_service = ctx.daemon->wallet;
    dinero::WalletManager* wallet_manager = &wallet_service->get();

    // Call the descriptor RPC implementation (din::Json is an alias for Json::Value)
    return dinero::rpc_wallet_listdescriptors(params, wallet_manager);
}

/**
 * wallet.getdescriptorinfo - Parse and analyze a descriptor string
 */
din::Json rpc_context_wallet_getdescriptorinfo(const ExecutionContext& ctx, const din::Json& params) {
    // Note: This RPC doesn't require a loaded wallet (descriptor-only operation)
    // Get wallet manager if available (optional)
    dinero::WalletManager* wallet_manager = nullptr;
    if (ctx.daemon && ctx.daemon->wallet) {
        wallet_manager = &ctx.daemon->wallet->get();
    }

    // Call the descriptor RPC implementation (din::Json is an alias for Json::Value)
    return dinero::rpc_wallet_getdescriptorinfo(params, wallet_manager);
}

/**
 * wallet.deriveaddresses - Derive addresses from a descriptor
 */
din::Json rpc_context_wallet_deriveaddresses(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Get wallet service from context
    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    // Get WalletManager from context
    auto& wallet_service = ctx.daemon->wallet;
    dinero::WalletManager* wallet_manager = &wallet_service->get();

    // Call the descriptor RPC implementation (din::Json is an alias for Json::Value)
    return dinero::rpc_wallet_deriveaddresses(params, wallet_manager);
}

/**
 * wallet.exportdescriptors - Export wallet descriptors for backup
 */
din::Json rpc_context_wallet_exportdescriptors(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Get wallet service from context
    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    // Get WalletManager from context
    auto& wallet_service = ctx.daemon->wallet;
    dinero::WalletManager* wallet_manager = &wallet_service->get();

    // Call the descriptor RPC implementation (din::Json is an alias for Json::Value)
    return dinero::rpc_wallet_exportdescriptors(params, wallet_manager);
}

/**
 * wallet.importdescriptors - Import descriptors for watch-only wallets
 */
din::Json rpc_context_wallet_importdescriptors(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Get wallet service from context
    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    // Get WalletManager from context
    auto& wallet_service = ctx.daemon->wallet;
    dinero::WalletManager* wallet_manager = &wallet_service->get();

    // Call the descriptor RPC implementation (din::Json is an alias for Json::Value)
    return dinero::rpc_wallet_importdescriptors(params, wallet_manager);
}

// ============================================================================
// wallet.consolidate — combine many small UTXOs of ONE script family (P2TR or
// P2MR) into a single fresh self output. Transparent only; no family mixing.
// Preview-by-default (dry_run); broadcast only when explicitly requested.
// Reuses the wallet.sendtoaddress filter/fee/build/sign/broadcast machinery.
// ============================================================================
din::Json rpc_context_wallet_consolidate(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    if (RefuseIfSafeMode(ctx, result)) return result;  // spec Fatal §3
    const auto log_debug = [&ctx](const std::string& msg) {
        if (ctx.logger) ctx.logger->debug(msg); else dinero::g_logger.debug(msg);
    };

    if (!ctx.daemon || !ctx.daemon->wallet) { result["ok"] = false; result["error"] = "Wallet service not available"; return result; }
    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) { result["ok"] = false; result["error"] = "No active wallet"; return result; }
    if (!ctx.daemon->chainstate) { result["ok"] = false; result["error"] = "Chainstate service not available"; return result; }
    auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate_service || !chainstate_service->utxoIndex()) { result["ok"] = false; result["error"] = "UTXO index not available"; return result; }

    try {
        // RPC may pass the options object directly OR wrapped in a single-element
        // array (params == [ {...} ]). Normalize to the effective options object.
        const din::Json& args = (params.isArray() && !params.empty() && params[0].isObject())
                                    ? params[0] : params;
        const auto getStr = [&](const char* k, const std::string& d) {
            return (args.isObject() && args.isMember(k) && args[k].isString()) ? args[k].asString() : d; };
        const auto getInt = [&](const char* k, int d) {
            return (args.isObject() && args.isMember(k) && args[k].isNumeric()) ? args[k].asInt() : d; };
        const auto getBool = [&](const char* k, bool d) {
            return (args.isObject() && args.isMember(k) && args[k].isBool()) ? args[k].asBool() : d; };
        const auto getDbl = [&](const char* k, double d) {
            return (args.isObject() && args.isMember(k) && args[k].isNumeric()) ? args[k].asDouble() : d; };

        std::string address_type = getStr("address_type", "auto");
        std::transform(address_type.begin(), address_type.end(), address_type.begin(), ::tolower);
        int    max_inputs        = getInt("max_inputs", 100);
        int    min_conf          = getInt("min_confirmations", 6);
        bool   include_unconf    = getBool("include_unconfirmed", false);
        double max_fee_percent   = getDbl("max_fee_percent", 1.0);
        double max_fee_din       = getDbl("max_fee_din", 1.0);
        bool   dry_run           = getBool("dry_run", true);
        bool   broadcast         = getBool("broadcast", false);
        if (max_inputs < 1)   max_inputs = 1;
        if (max_inputs > 500) max_inputs = 500;   // policy-safe cap

        if (address_type == "shielded") {
            result["ok"] = false; result["error"] = "shielded consolidation not supported in v7"; return result;
        }

        // Resolve fee_rate (una/vB): explicit numeric, else mempool estimate, else min-relay floor.
        double fee_rate = 0.0;
        if (args.isObject() && args.isMember("fee_rate") && args["fee_rate"].isNumeric())
            fee_rate = args["fee_rate"].asDouble();
        if (fee_rate <= 0.0 && ctx.daemon->mempool) {
            auto mps = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
            if (mps) {
                auto fe = mps->getFeeEstimator();
                if (fe) {
                    auto est = fe->estimateFee(dinero::policy::FeeTarget::NORMAL);
                    if (est.is_sufficient_data && est.fee_rate > 0)
                        fee_rate = static_cast<double>(est.fee_rate) / 1000.0;
                }
            }
        }
        if (fee_rate <= 0.0) fee_rate = 1.0;

        // Let the wallet worker catch up to the tip so the UTXO table isn't stale.
        {
            const uint32_t tip = chainstate_service->getBlockHeight();
            wallet_service->get().WaitForHeight(tip, std::chrono::milliseconds(5000));
        }

        // Gather + filter eligible UTXOs (same chain as wallet.sendtoaddress).
        const int floor_conf = include_unconf ? 0 : min_conf;
        auto raw = wallet_service->get().listUnspentUTXOs(floor_conf, 9999999);
        auto mps = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);

        std::vector<dinero::WalletManager::WalletUTXO> eligible;
        for (const auto& u : raw) {
            if (!u.spendable || !u.is_mature || u.is_confidential) continue;
            if (u.derivation_path.empty() || u.script_pubkey.empty()) continue;
            if (wallet_service->get().isUTXOLocked(u.txid, u.vout)) continue;
            if (mps) {
                OutPoint op(dinero::TxId(dinero::uint256::FromHexUnsafe(u.txid)), u.vout);
                if (mps->mempool().isOutputSpentInMempool(op)) continue;
            }
            std::string reason;
            if (!WalletUtxoIsPresentInLiveUtreexoForest(u, chainstate_service, &reason)) continue;
            eligible.push_back(u);
        }

        // Partition by script family.
        const auto isP2TR = [](const std::string& s){ return s.size()==68 && s.rfind("5120",0)==0; };
        const auto isP2MR = [](const std::string& s){ return s.size()==68 && s.rfind("5320",0)==0; };
        std::vector<dinero::WalletManager::WalletUTXO> p2tr, p2mr;
        for (const auto& u : eligible) {
            if (isP2TR(u.script_pubkey)) p2tr.push_back(u);
            else if (isP2MR(u.script_pubkey)) p2mr.push_back(u);
        }

        // Choose target family (auto → larger pool; tie → p2tr).
        std::string family;
        std::vector<dinero::WalletManager::WalletUTXO>* pool = nullptr;
        if (address_type == "p2mr") { family = "p2mr"; pool = &p2mr; }
        else if (address_type == "p2tr" || address_type == "taproot") { family = "p2tr"; pool = &p2tr; }
        else { if (p2mr.size() > p2tr.size()) { family = "p2mr"; pool = &p2mr; } else { family = "p2tr"; pool = &p2tr; } }
        result["address_family"] = family;

        // No-op cases.
        if (pool->size() == 0) {
            result["ok"] = true; result["dry_run"] = dry_run; result["selected_inputs"] = 0;
            result["reason"] = "no eligible UTXOs in family"; result["txid"] = din::Json(Json::nullValue); return result;
        }
        if (pool->size() == 1) {
            result["ok"] = true; result["dry_run"] = dry_run; result["selected_inputs"] = 0;
            result["reason"] = "nothing to consolidate"; result["txid"] = din::Json(Json::nullValue); return result;
        }

        // Select up to max_inputs, smallest-value-first (sweep the most dust per tx).
        std::sort(pool->begin(), pool->end(),
                  [](const dinero::WalletManager::WalletUTXO& a, const dinero::WalletManager::WalletUTXO& b){
                      return a.amount_una < b.amount_una; });
        std::vector<dinero::WalletManager::WalletUTXO> selected(
            pool->begin(), pool->begin() + std::min<size_t>(pool->size(), static_cast<size_t>(max_inputs)));

        int64_t total_una = 0;
        for (const auto& u : selected) total_una += static_cast<int64_t>(u.amount_una);

        // Single output, no change: fee from the builder's own size/fee math.
        const size_t n_in   = selected.size();
        const size_t n_p2mr = (family == "p2mr") ? n_in : 0;
        const size_t vsize  = dinero::UnsignedTxBuilder::EstimateTransactionSize(n_in, 1, n_p2mr);
        // Safety margin: the builder's no-change branch reports final_fee = (inputs - output -
        // est_fee). With output = total - fee exactly, that residual is 0, tripping the signer's
        // "Fee is zero" guard. Add a small FIXED constant (must NOT scale with fee_rate — a scaling
        // margin reaches the 546-una dust threshold at high fee_rate and arms a 2nd, dust change
        // output) so the residual — hence the reported fee — is strictly positive, while the actual
        // on-chain fee stays at/above min-relay.
        const int64_t fee_una = static_cast<int64_t>(
            dinero::UnsignedTxBuilder::CalculateFee(vsize, static_cast<uint64_t>(fee_rate)))
            // +10 una keeps the builder's no-change residual strictly positive (signer rejects
            // fee==0) while staying far below the 546-una dust threshold so we never get a change output.
            + 10;
        const int64_t output_una = total_una - fee_una;
        const double  fee_din = static_cast<double>(fee_una) / 1e8;

        if (output_una <= static_cast<int64_t>(dinero::UnsignedTxBuilder::DUST_THRESHOLD)) {
            result["ok"] = false; result["dry_run"] = dry_run; result["fee_ok"] = false;
            result["reason"] = "consolidation output below dust after fee";
            result["selected_inputs"] = static_cast<int>(n_in);
            result["input_value"] = static_cast<double>(total_una) / 1e8;
            result["estimated_fee"] = fee_din; return result;
        }

        // Fee sanity gate: reject if fee > percent-of-value OR > absolute DIN cap (both modes).
        const double pct = (total_una > 0) ? (100.0 * static_cast<double>(fee_una) / static_cast<double>(total_una)) : 0.0;
        const bool gate_abs = fee_din > max_fee_din;
        const bool gate_pct = pct > max_fee_percent;
        if (gate_abs || gate_pct) {
            std::ostringstream r;
            if (gate_abs) r << "fee " << fee_din << " DIN exceeds max_fee_din " << max_fee_din;
            else          r << "fee " << pct << "% exceeds max_fee_percent " << max_fee_percent;
            result["ok"] = false; result["dry_run"] = dry_run; result["fee_ok"] = false; result["reason"] = r.str();
            result["selected_inputs"] = static_cast<int>(n_in);
            result["input_value"] = static_cast<double>(total_una) / 1e8;
            result["estimated_fee"] = fee_din; return result;
        }

        // Fresh self destination of the target family.
        std::string dest, derive_err;
        {
            din::Json ap(Json::arrayValue);
            ap.append((family == "p2mr") ? std::string("p2mr") : std::string("taproot"));
            ap.append(std::string("consolidate"));
            auto* gh = g_rpcRegistry.lookup("wallet.getnewaddress");
            if (!gh) { result["ok"] = false; result["error"] = "wallet.getnewaddress handler not registered"; return result; }
            din::Json ar = (*gh)(ctx, ap);
            if (ar.isMember("address") && ar["address"].isString()) dest = ar["address"].asString();
            if (dest.empty() && ar.isMember("error") && ar["error"].isString()) derive_err = ar["error"].asString();
        }
        if (dest.empty()) {
            result["ok"] = false;
            result["error"] = derive_err.empty() ? "Failed to derive consolidation address" : derive_err;
            return result;
        }

        // Dry-run: return the plan, no signing.
        if (dry_run) {
            result["ok"] = true; result["dry_run"] = true;
            result["selected_inputs"] = static_cast<int>(n_in);
            result["input_value"]   = static_cast<double>(total_una) / 1e8;
            result["estimated_fee"] = fee_din;
            result["output_value"]  = static_cast<double>(output_una) / 1e8;
            result["destination"]   = dest;
            result["fee_ok"]        = true;
            result["txid"]  = din::Json(Json::nullValue);
            result["rawtx"] = din::Json(Json::nullValue);
            return result;
        }

        // Execution requires an unlocked wallet (dry-run above never needs keys).
        if (wallet_service->get().isWalletLocked()) {
            result["ok"] = false; result["error"] = "Wallet is locked. Use wallet.unlock first."; return result;
        }

        // Convert selected UTXOs to canonical builder inputs.
        std::vector<dinero::CanonicalWalletUTXO> cins;
        for (const auto& u : selected) {
            dinero::CanonicalWalletUTXO c;
            c.txid = dinero::uint256::FromHexUnsafe(u.txid);
            c.vout = u.vout;
            c.value = dinero::AmountUna::Una(u.amount_una);
            c.path = u.derivation_path;
            c.height = u.height;
            c.is_coinbase = u.is_coinbase;
            c.spk.reserve(u.script_pubkey.size() / 2);
            for (size_t i = 0; i + 1 < u.script_pubkey.size(); i += 2)
                c.spk.push_back(static_cast<uint8_t>(std::stoi(u.script_pubkey.substr(i, 2), nullptr, 16)));
            cins.push_back(std::move(c));
        }

        // Single output = total - fee; change_address = dest catches any residual (≈0).
        dinero::BuildOptions bo;
        bo.fee_rate = static_cast<uint64_t>(fee_rate);
        bo.enable_rbf = true;
        bo.change_address = dest;
        std::vector<dinero::TxOutputRequest> outs;
        outs.push_back(dinero::TxOutputRequest(dest, static_cast<uint64_t>(output_una)));

        auto br = dinero::UnsignedTxBuilder::Build(cins, outs, bo);
        if (!br.success) { result["ok"] = false; result["error"] = "Failed to build transaction: " + br.error; return result; }

        // Keys: ECDSA for P2TR via scriptPubKey/path; P2MR signs via WalletKeyProvider (PQ seed).
        std::map<std::string, std::string> path_to_key;
        for (const auto& u : selected) {
            std::vector<uint8_t> spk; spk.reserve(u.script_pubkey.size() / 2);
            for (size_t i = 0; i + 1 < u.script_pubkey.size(); i += 2)
                spk.push_back(static_cast<uint8_t>(std::stoi(u.script_pubkey.substr(i, 2), nullptr, 16)));
            if (dinero::consensus::pq::IsP2MRScript(spk)) continue;
            auto pk = wallet_service->get().deriveKeyForScriptPubKey(u.script_pubkey);
            if (pk.has_value() && !pk->empty()) {
                std::ostringstream h;
                for (uint8_t b : *pk) h << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
                path_to_key[u.derivation_path] = h.str();
            } else if (!u.derivation_path.empty()) {
                std::string hk = wallet_service->get().getPrivateKeyForPath(u.derivation_path);
                if (!hk.empty()) path_to_key[u.derivation_path] = hk;
            }
        }

        std::unique_ptr<dinero::KeyProvider> provider;
        std::unique_ptr<dinero::wallet::V7P2MRStore> store_holder;
        if (family == "p2mr") {
            auto master = wallet_service->get().GetV7PqMasterKey();
            if (!master) { result["ok"] = false; result["error"] = "Cannot spend P2MR coin: wallet locked or v7 master key unavailable"; return result; }
            const std::string sp = wallet_service->get().GetV7P2MRStorePath();
            if (sp.empty()) { result["ok"] = false; result["error"] = "v7 P2MR store path not configured"; return result; }
            store_holder = std::make_unique<dinero::wallet::V7P2MRStore>();
            if (store_holder->Open(sp) != dinero::wallet::V7P2MRStore::OpenResult::Ok) { result["ok"] = false; result["error"] = "failed to open v7 P2MR store"; return result; }
            dinero::wallet::WalletKeyProvider::Config cfg;
            cfg.legacy_keys_by_path = path_to_key;
            cfg.p2mr_store = store_holder.get();
            cfg.wallet_id = 1;
            std::memcpy(cfg.master_key.data(), master->data(), cfg.master_key.size());
            OPENSSL_cleanse(const_cast<uint8_t*>(master->data()), master->size());
            provider = std::make_unique<dinero::wallet::WalletKeyProvider>(std::move(cfg));
        } else {
            if (path_to_key.empty()) { result["ok"] = false; result["error"] = "Could not retrieve private keys for signing"; return result; }
            provider = std::make_unique<dinero::MapKeyProvider>(path_to_key);
        }

        auto sr = dinero::TransactionSigner::Sign(br.unsigned_tx, *provider);
        if (!sr.success) { result["ok"] = false; result["error"] = "Failed to sign transaction: " + sr.error; return result; }

        const dinero::Transaction& stx = sr.signed_tx.tx;
        const std::string out_txid = stx.GetTxid().AsUint256().GetHex();

        result["ok"] = true;
        result["dry_run"] = false;
        result["address_family"] = family;
        result["selected_inputs"] = static_cast<int>(n_in);
        result["input_value"]  = static_cast<double>(total_una) / 1e8;
        // Real on-chain fee = inputs - output = fee_una; matches dry-run's estimated_fee.
        // (sr.signed_tx.fee carries the builder's no-change residual, not the true fee.)
        result["fee"]          = fee_din;
        result["output_value"] = static_cast<double>(output_una) / 1e8;

        if (broadcast) {
            if (!mps) { result["ok"] = false; result["error"] = "Mempool service unavailable"; return result; }
            auto submit = mps->mempool().submitTransaction(stx, "rpc:wallet.consolidate", true);
            result["broadcast"] = true;
            result["accepted"] = submit.accepted();
            if (submit.rejected()) {
                result["ok"] = false;
                result["reject_code"] = TxRejectCodeToString(submit.code);
                result["reject_reason"] = submit.message;
                result["txid"] = din::Json(Json::nullValue);
                return result;
            }
            result["txid"]  = out_txid;
            result["rawtx"] = din::Json(Json::nullValue);
        } else {
            result["broadcast"] = false;
            result["txid"]  = din::Json(Json::nullValue);
            result["rawtx"] = stx.SerializeHex(true);
        }
        return result;

    } catch (const std::exception& e) {
        result["ok"] = false; result["error"] = std::string("consolidate failed: ") + e.what(); return result;
    }
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

extern RpcRegistry g_rpcRegistry;

void registerWalletMethodsContext() {
    // Core wallet methods (fully implemented)
    g_rpcRegistry.registerHandler("wallet.getbalance",
                                 rpc_context_wallet_getbalance,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getbalance", "wallet.getbalance");
    g_rpcRegistry.registerAlias("wallet.gettotalbalance", "wallet.getbalance");
    g_rpcRegistry.registerAlias("gettotalbalance", "wallet.getbalance");

    // Phase 35.1: Wallet introspection (read-only)
    g_rpcRegistry.registerHandler("wallet.getwalletinfo",
                                 rpc_context_wallet_getwalletinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getwalletinfo", "wallet.getwalletinfo");

    g_rpcRegistry.registerHandler("wallet.snapshot",
                                 rpc_context_wallet_snapshot,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("wallet.getsnapshot", "wallet.snapshot");

    // Phase 35.1: Fee introspection (read-only wrapper)
    g_rpcRegistry.registerHandler("wallet.estimatefee",
                                 rpc_context_wallet_estimatefee,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("estimatefee", "wallet.estimatefee");

    g_rpcRegistry.registerHandler("wallet.getnewaddress",
                                 rpc_context_wallet_getnewaddress,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("getnewaddress", "wallet.getnewaddress");
    g_rpcRegistry.registerHandler("wallet.setreceivepolicy",
                                 rpc_context_wallet_setreceivepolicy,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerHandler("wallet.getreceivepolicy",
                                 rpc_context_wallet_getreceivepolicy,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.listaddresses",
                                 rpc_context_wallet_listaddresses,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.listunspent",
                                 rpc_context_wallet_listunspent,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("listunspent", "wallet.listunspent");

    g_rpcRegistry.registerHandler("wallet.lockunspent",
                                 rpc_context_wallet_lockunspent,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.abandontransaction",
                                 rpc_context_wallet_abandontransaction,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.getinfo",
                                 rpc_context_wallet_getinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.validateaddress",
                                 rpc_context_wallet_validateaddress,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Security methods
    g_rpcRegistry.registerHandler("wallet.lock",
                                 rpc_context_wallet_lock,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.unlock",
                                 rpc_context_wallet_unlock,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.encrypt",
                                 rpc_context_wallet_encrypt,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.passphrasechange",
                                 rpc_context_wallet_passphrasechange,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Transaction methods (Phase 33.4: Full wallet activation)
    g_rpcRegistry.registerHandler("wallet.sendtoaddress",
                                 rpc_context_wallet_sendtoaddress,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("sendtoaddress", "wallet.sendtoaddress");

    g_rpcRegistry.registerHandler("wallet.consolidate",
                                 rpc_context_wallet_consolidate,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("consolidate", "wallet.consolidate");

    g_rpcRegistry.registerHandler("wallet.sendmany",
                                 rpc_context_wallet_sendmany,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("sendmany", "wallet.sendmany");

    // Utreexo extension (Dinero-specific)
    g_rpcRegistry.registerHandler("wallet.utxoproof",
                                 rpc_context_wallet_utxoproof,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerHandler("wallet.verifyutxoproof",
                                 rpc_context_wallet_verifyutxoproof,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerHandler("wallet.validatestatelessbalance",
                                 rpc_context_wallet_validatestatelessbalance,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerHandler("wallet.getproofbundle",
                                 rpc_context_wallet_getproofbundle,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerHandler("wallet.proofstatus",
                                 rpc_context_wallet_proofstatus,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.listtransactions",
                                 rpc_context_wallet_listtransactions,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("listtransactions", "wallet.listtransactions");

    g_rpcRegistry.registerHandler("wallet.gettransaction",
                                 rpc_context_wallet_gettransaction,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("gettransaction", "wallet.gettransaction");

    // Backup and maintenance
    g_rpcRegistry.registerHandler("wallet.backup",
                                 rpc_context_wallet_backup,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.deriveaddress",
                                 rpc_context_wallet_deriveaddress,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.dumpprivkey",
                                 rpc_context_wallet_dumpprivkey,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // PSBT methods
    g_rpcRegistry.registerHandler("wallet.createfundedpsbt",
                                 rpc_context_wallet_createfundedpsbt,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.processpsbt",
                                 rpc_context_wallet_processpsbt,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.finalizepsbt",
                                 rpc_context_wallet_finalizepsbt,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.combinepsbt",
                                 rpc_context_wallet_combinepsbt,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Raw transaction methods
    g_rpcRegistry.registerHandler("wallet.createrawtransaction",
                                 rpc_context_wallet_createrawtransaction,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.signrawtransaction",
                                 rpc_context_wallet_signrawtransaction,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.decoderawtransaction",
                                 rpc_context_wallet_decoderawtransaction,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.getrawtransaction",
                                 rpc_context_wallet_getrawtransaction,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.sendrawtransaction",
                                 rpc_context_wallet_sendrawtransaction,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.recordsend",
                                 rpc_context_wallet_recordsend,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Import/Export methods
    g_rpcRegistry.registerHandler("wallet.importprivkey",
                                 rpc_context_wallet_importprivkey,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.dumpwallet",
                                 rpc_context_wallet_dumpwallet,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.exportseed",
                                 rpc_context_wallet_exportseed,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.importwallet",
                                 rpc_context_wallet_importwallet,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.exportcsv",
                                 rpc_context_wallet_exportcsv,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Label methods
    g_rpcRegistry.registerHandler("wallet.setlabel",
                                 rpc_context_wallet_setlabel,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.getlabel",
                                 rpc_context_wallet_getlabel,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Wallet management methods
    g_rpcRegistry.registerHandler("wallet.listwallets",
                                 rpc_context_wallet_listwallets,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.open",
                                 rpc_context_wallet_open,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.unload",
                                 rpc_context_wallet_unload,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.rename",
                                 rpc_context_wallet_rename,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Utility methods
    g_rpcRegistry.registerHandler("wallet.rescan",
                                 rpc_context_wallet_rescan,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.rescanblockchain",
                                 rpc_context_wallet_rescanblockchain,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.abortrescan",
                                 rpc_context_wallet_abortrescan,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.signpsbt",
                                 rpc_context_wallet_signpsbt,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.settxfee",
                                 rpc_context_wallet_settxfee,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.listaddresseswithbalances",
                                 rpc_context_wallet_listaddresseswithbalances,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.generateqrcode",
                                 rpc_context_wallet_generateqrcode,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.createhd",
                                 rpc_context_wallet_createhd,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.restore",
                                 rpc_context_wallet_restore,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.exportmnemonic",
                                 rpc_context_wallet_exportmnemonic,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.delete",
                                 rpc_context_wallet_delete,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("wallet.remove", "wallet.delete");

    // Phase W.1.1: Import mnemonic and trigger rescan
    g_rpcRegistry.registerHandler("wallet.importmnemonic",
                                 rpc_context_wallet_importmnemonic,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.migratelegacysidecar",
                                 rpc_context_wallet_migratelegacysidecar,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("wallet.migrate_sidecar_to_db", "wallet.migratelegacysidecar");

    // Taproot descriptor import with mandatory rescan (BIP341)
    g_rpcRegistry.registerHandler("wallet.importtaprootdescriptor",
                                 rpc_context_wallet_importtaprootdescriptor,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.notarizebackup",
                                 rpc_context_wallet_notarizebackup,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.scanutxos",
                                 rpc_context_wallet_scanutxos,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    // Phase 1: Descriptor wallet RPCs (Bitcoin Core compatibility)
    g_rpcRegistry.registerHandler("wallet.listdescriptors",
                                 rpc_context_wallet_listdescriptors,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.getdescriptorinfo",
                                 rpc_context_wallet_getdescriptorinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.deriveaddresses",
                                 rpc_context_wallet_deriveaddresses,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.exportdescriptors",
                                 rpc_context_wallet_exportdescriptors,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("wallet.importdescriptors",
                                 rpc_context_wallet_importdescriptors,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] ✅ 48 wallet context-aware handlers registered (including 5 descriptor RPCs)");
}
