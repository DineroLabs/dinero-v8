/**
 * v0.14.0.4: Mining RPC Interface (getblocktemplate, submitblock)
 *
 * Purpose: Dinero RPC plumbing for block assembly and submission.
 *          Delegates to BlockAssembler::CreateNewBlock() for CPFP-aware selection.
 *          RPC is plumbing, not economics.
 *
 * v0.14.0.4 Changes:
 * ✅ Configure Utreexo forest and UTXO provider for BlockAssembler
 * ✅ Return proper target computed from bits (not hardcoded zeros)
 * ✅ Return utreexocommitment field for external miners
 * ✅ Add rules array with utreexo support flag
 *
 * Exit Criteria:
 * ✅ getblocktemplate calls BlockAssembler::CreateNewBlock()
 * ✅ Returns Dinero-native JSON with Utreexo commitment
 * ✅ submitblock validates and accepts blocks
 * ✅ External miners receive complete data for valid block construction
 * ✅ No CPFP recalculation (delegated to BlockAssembler)
 * ✅ No mempool policy changes
 * ✅ No fee logic alterations
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "rpc/gbt_template_time.h"
#include "rpc/longpoll_notifier.h"  // Server-side long-poll for getblocktemplate
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/config_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/p2p_service.h"
#include "mining/block_assembler.h"
#include "mining/mining_readiness.h"
#include "mining/miner.h"  // v0.14.0.4: For DineroPoW::BitsToTargetHex
#include "consensus/adapters/wallet_utxo_adapter.h"  // v2.2.0: UTXO adapter
#include "consensus/utreexo_activation.h"  // Activation height source of truth
#include "consensus/chainparams.h"  // For network-aware template safety defaults
#include "daemon/block_acceptor.h"
#include "common/logger.h"
#include "storage/chain_db.h"
#include "primitives/block.h"
#include "primitives/uint256.h"  // Phase M.0: uint256 type
#include "wallet/transaction.h"
#include <memory>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <deque>
#include <chrono>
#include <algorithm>

using dinero::uint256;  // Phase M.0: Make uint256 available without namespace prefix
#include <ctime>

// ============================================================================
// Template tracking for submitblock coinbase-modification guard
// ============================================================================
// Daemon owns the coinbase (stratum-like). Miners must use coinbasetxn exactly.
// If a miner modifies the coinbase (e.g. extranonce), the utreexo commitment
// changes and the block is rejected. This guard catches that early with a
// clear error instead of the confusing "bad-utreexo-root".
// ============================================================================
struct MiningTemplate {
    std::string utreexo_root_hex;   // GetHex() display format
    std::string coinbase_txid_hex;  // GetHex() display format
    uint32_t height;
};

static std::mutex g_template_mutex;
static std::deque<MiningTemplate> g_recent_templates;
static constexpr size_t MAX_RECENT_TEMPLATES = 10;

namespace {

void InvalidateMiningJobsForSafety();

void PopulateTemplateSafetyError(din::Json& result, const dinero::mining::MiningReadiness& decision) {
    result["code"] = "mining-safety-gate";
    result["safe_to_mine"] = decision.ready;
    result["reason"] = decision.reason_code;
    result["error"] = decision.message;

    din::Json safety;
    safety["safe_to_mine"] = decision.ready;
    safety["reason"] = decision.reason_code;
    safety["p2p_running"] = decision.p2p_running;
    safety["pause_if_ahead_of_network_view"] = decision.pause_if_ahead_of_network_view;
    if (decision.peer_count >= 0) {
        safety["peer_count"] = static_cast<int64_t>(decision.peer_count);
    }
    if (decision.min_peers >= 0) {
        safety["min_peers"] = static_cast<int64_t>(decision.min_peers);
    }
    if (decision.local_height >= 0) {
        safety["local_height"] = static_cast<int64_t>(decision.local_height);
    }
    if (decision.network_height_estimate >= 0) {
        safety["network_height_estimate"] = static_cast<int64_t>(decision.network_height_estimate);
    }
    if (decision.peer_best_height >= 0) {
        safety["peer_best_height"] = static_cast<int64_t>(decision.peer_best_height);
    }
    if (decision.peer_median_height >= 0) {
        safety["peer_median_height"] = static_cast<int64_t>(decision.peer_median_height);
    }
    if (decision.max_tip_lag >= 0) {
        safety["max_tip_lag"] = static_cast<int64_t>(decision.max_tip_lag);
    }
    if (decision.max_tip_ahead >= 0) {
        safety["max_tip_ahead"] = static_cast<int64_t>(decision.max_tip_ahead);
    }
    if (decision.peer_freshest_age_seconds >= 0) {
        safety["peer_freshest_age_seconds"] = static_cast<int64_t>(decision.peer_freshest_age_seconds);
    }
    if (decision.max_peer_staleness_seconds >= 0) {
        safety["max_peer_staleness_seconds"] = static_cast<int64_t>(decision.max_peer_staleness_seconds);
    }
    safety["is_initial_block_download"] = decision.is_ibd;
    result["mining_safety"] = safety;
}

}  // namespace

// ============================================================================
// v0.14.0.3: getblocktemplate RPC
// ============================================================================

/**
 * getblocktemplate - Get block template for mining
 *
 * Dinero block template with native Utreexo support.
 * Delegates to BlockAssembler::CreateNewBlock() for CPFP-aware transaction selection.
 *
 * Usage:
 *   getblocktemplate '{"address":"din1q..."}'
 *
 * Returns:
 *   {
 *     "version": 1,
 *     "previousblockhash": "...",
 *     "transactions": [...],
 *     "coinbasevalue": 10000000000,
 *     "target": "...",
 *     "bits": "1d00ffff",
 *     "mintime": 1234567890,
 *     "curtime": 1234567890,
 *     "height": 12345
 *   }
 */
din::Json rpc_getblocktemplate_v14(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Get services
    if (!ctx.daemon) {
        result["error"] = "DaemonContext not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<::dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto mempool_service = std::dynamic_pointer_cast<::dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Mempool service not available";
        return result;
    }

    // Phase 39: Get chain database via ChainstateService (ChainManager deleted)
    auto* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
    if (!chain_db) {
        result["error"] = "Chain database not available";
        return result;
    }

    // Get mempool (returns a reference, not a pointer)
    ::dinero::Mempool& mempool_ref = mempool_service->mempool();
    ::dinero::Mempool* mempool = &mempool_ref;

    // Get mining address (REQUIRED)
    // Handle both object params {"address":"..."} and array params [{"address":"..."}]
    std::string mining_address;

    din::Json params_obj;
    if (params.isArray() && params.size() > 0) {
        params_obj = params[0];
    } else if (params.isObject()) {
        params_obj = params;
    } else {
        result["error"] = "Missing required parameter: address (e.g., {\"address\": \"din1q...\"})";
        return result;
    }

    if (!params_obj.isMember("address") || !params_obj["address"].isString()) {
        result["error"] = "Missing or invalid address parameter (e.g., {\"address\": \"din1q...\"})";
        return result;
    }

    mining_address = params_obj["address"].asString();

    if (mining_address.empty()) {
        result["error"] = "Mining address cannot be empty";
        return result;
    }

    // Basic address validation (din1... or D...)
    if (mining_address.length() < 10) {
        result["error"] = "Invalid Dinero address (too short)";
        return result;
    }

    // ═══════════════════════════════════════════════════════════
    // Server-side long-polling (BIP22/BIP23 longpollid semantics)
    // ═══════════════════════════════════════════════════════════
    // If the caller passed a longpollid that matches the current tip,
    // block the RPC response until the tip advances (or the longpoll
    // timeout fires). Replaces client-side poll-every-N-seconds with
    // event-driven template refresh; latency drops from seconds to
    // the notifyBlockConnected wake interval (milliseconds).
    //
    // Timeout: 8 seconds. HTTP RPC server's client socket timeout is
    // 10s (src/daemon/http_rpc_server.h kClientSocketTimeout); we must
    // return with slack to serialize + ship before the socket closes.
    //
    // Backward-compat: absent longpollid, behavior is unchanged (return
    // current template immediately). The response always carries
    // `longpollid` set to the tip hash the template was built on, so
    // clients can opt into longpolling by echoing it back.
    if (params_obj.isMember("longpollid") && params_obj["longpollid"].isString()) {
        const std::string client_longpollid = params_obj["longpollid"].asString();
        if (!client_longpollid.empty()) {
            auto cur_tip = chain_db->getTip();
            const std::string current_best = cur_tip.ok() ? cur_tip.value().hash.GetHex() : "";
            if (!current_best.empty() && client_longpollid == current_best) {
                // Snapshot generation BEFORE waiting so a concurrent
                // block-connect doesn't race past our observation.
                auto& notifier = dinero::rpc::LongPollNotifier::instance();
                const uint64_t seen_generation = notifier.currentGeneration();
                notifier.waitForChange(seen_generation, std::chrono::milliseconds(8000));
                // Fall through to template build; state reads below will
                // pick up whatever the tip is now.
            }
            // client_longpollid != current_best: client is behind, give
            // them a fresh template immediately (no wait).
        }
    }

    // Safety gate: refuse templates when disconnected/behind, unless explicitly
    // configured for isolated mining (regtest default).
    auto config_service = std::dynamic_pointer_cast<::dinero::ConfigService>(ctx.daemon->config);
    const auto readiness = ::dinero::mining::EvaluateMiningReadiness(
        chainstate.get(),
        ctx.daemon->p2p.get(),
        config_service.get());
    if (!readiness.ready) {
        InvalidateMiningJobsForSafety();
        PopulateTemplateSafetyError(result, readiness);
        ::dinero::g_logger.warning("[v14 GBT] Template rejected by safety gate: reason=" +
                                   readiness.reason_code + ", detail=" + readiness.message);
        return result;
    }

    // ========================================================================
    // v0.14.0.4: Delegate to BlockAssembler::CreateNewBlock()
    // This is the ONLY place transaction selection happens (no duplication)
    // ========================================================================

    ::dinero::BlockAssembler assembler(chain_db);
    assembler.setMempool(mempool);

    // ========================================================================
    // v0.14.0.4: Configure Utreexo forest and UTXO provider for proof generation
    // Without these, CreateNewBlock() skips utreexo computation and blocks
    // submitted by external miners will be rejected.
    // ========================================================================
    auto* utreexo_forest = chainstate->utreexoForest();

    // Phase 7: chain-scoped IUTXOProvider.
    //
    // Previously this wired WalletUTXOAdapter(utxoIndex) — a wallet-scoped
    // view that only reports UTXOs the local wallet considers "mine".
    // BlockAssembler's FilterChainBackedTemplateTransactions then deferred
    // any tx whose input prevout wasn't in that view as "non-chain-backed",
    // silently excluding legitimate spends of UTXOs other wallets own
    // (and spends of P2MR outputs until Phase 6 Commit 6 taught the wallet
    // to recognize them). The miner should see the entire chain UTXO set,
    // not just this wallet's holdings.
    //
    // ChainstateService's ConsensusUTXOSet is the single source of truth
    // for chain UTXO state (Phase 2 pure-consensus architecture) and
    // IConsensusUTXOSet extends IUTXOProvider. We alias it into a
    // non-owning shared_ptr — ChainstateService retains ownership via
    // its unique_ptr and outlives every template it hands out.
    auto* consensus_utxo_set = chainstate->GetConsensusUTXOSet();
    if (utreexo_forest && consensus_utxo_set) {
        std::shared_ptr<dinero::consensus::IUTXOProvider> utxo_adapter(
            static_cast<dinero::consensus::IUTXOProvider*>(consensus_utxo_set),
            [](dinero::consensus::IUTXOProvider*) { /* non-owning: chainstate owns the backing set */ });
        assembler.SetUtreexoForest(utreexo_forest);
        assembler.SetConsensusUTXOSet(consensus_utxo_set);  // snapshot forest under shared lock (UAF guard)
        assembler.SetUTXOProvider(utxo_adapter);
        ::dinero::g_logger.debug("[v14 GBT] Utreexo forest + chain-scoped ConsensusUTXOSet configured");
    } else {
        ::dinero::g_logger.warning("[v14 GBT] Utreexo forest or ConsensusUTXOSet unavailable - "
                                "blocks may be rejected at height 2+");
    }

    // Wire BlockValidator for Utreexo root computation (single source of truth)
    auto* block_validator = chainstate->GetBlockValidator();
    if (!block_validator) {
        result["error"] = "Block validator unavailable (Utreexo oracle not wired)";
        ::dinero::g_logger.error("[v14 GBT] BlockValidator missing; refusing template generation");
        return result;
    }
    assembler.SetBlockValidator(block_validator);

    // Create block template (CPFP-aware, deterministic, with Utreexo proof)
    auto block = assembler.CreateNewBlock(mining_address);

    if (!block) {
        const std::string& detail = assembler.getLastTemplateError();
        result["error"] = detail.empty() ? "Failed to create block template"
                                         : "Failed to create block template: " + detail;
        return result;
    }

    // Get block template statistics
    auto stats = assembler.getBlockTemplateStats();

    // ========================================================================
    // Convert to Dinero JSON format (with Utreexo extensions)
    // ========================================================================

    result["version"] = static_cast<int>(block->header.version);
    result["previousblockhash"] = block->header.prev_block_hash.GetHex();  // Consensus→RPC
    // longpollid: tip token for client-side server long-polling. Client
    // echoes it back in subsequent getblocktemplate calls; when it matches
    // the current tip, the server holds the response until the tip advances.
    // See the longpoll block above and include/rpc/longpoll_notifier.h.
    result["longpollid"] = block->header.prev_block_hash.GetHex();
    result["height"] = static_cast<int>(stats.height);

    // Timestamp: publish the exact header time used to compute ASERT bits.
    uint64_t current_time = ::dinero::rpc::SelectGbtTemplateTime(
        block->header.timestamp,
        static_cast<uint64_t>(std::time(nullptr)));
    result["curtime"] = static_cast<int64_t>(current_time);
    uint64_t min_time = current_time;
    auto tip_result = chain_db->getTip();
    if (tip_result.ok()) {
        auto tip_header_result = chain_db->getHeader(tip_result.value().hash);
        if (tip_header_result.ok()) {
            min_time = std::max(current_time, tip_header_result.value().timestamp + 1);
        }
    }
    result["mintime"] = static_cast<int64_t>(min_time);

    // Difficulty bits (hex format)
    std::ostringstream bits_hex;
    bits_hex << std::hex << std::setfill('0') << std::setw(8) << block->header.difficulty;
    result["bits"] = bits_hex.str();

    // ========================================================================
    // v0.14.0.4: Compute proper target from bits
    // External miners need this to validate their PoW solutions
    // ========================================================================
    std::string target = ::dinero::DineroPoW::BitsToTargetHex(block->header.difficulty);
    result["target"] = target;

    // ========================================================================
    // v0.14.0.4: Return Utreexo commitment (AFTER-state) from block header
    // This is CRITICAL for external miners - blocks without proper utreexo_root
    // will be rejected once full rules are active.
    // ========================================================================
    std::string utreexo_commitment = block->header.utreexo_root.GetHex();

    // Structured utreexo object per MINER_PROTOCOL_V1.md spec
    din::Json utreexo_obj;
    const uint32_t utreexo_activation_height = dinero::consensus::GetUtreexoActivationHeight();

    utreexo_obj["enabled"] = true;
    utreexo_obj["required"] = (stats.height >= utreexo_activation_height);
    utreexo_obj["activation_height"] = static_cast<int>(utreexo_activation_height);
    utreexo_obj["commitment"] = utreexo_commitment;
    utreexo_obj["placement"] = "block_header";
    utreexo_obj["commitment_offset"] = 68;  // 0x44 in header
    utreexo_obj["commitment_version"] = 1;
    result["utreexo"] = utreexo_obj;

    // Keep legacy field for backwards compatibility with existing miners
    result["utreexocommitment"] = utreexo_commitment;

    ::dinero::g_logger.info("[v14 GBT] Utreexo commitment: " + utreexo_commitment.substr(0, 16) + "...");
    ::dinero::g_logger.info("[v14 GBT] Target: " + target.substr(0, 16) + "...");

    // Coinbase value (subsidy + fees)
    uint64_t coinbase_value = 0;
    if (!block->vtx.empty() && block->vtx[0].IsCoinbase() && !block->vtx[0].vout.empty()) {
        coinbase_value = block->vtx[0].vout[0].value.GetUna();
    }
    result["coinbasevalue"] = static_cast<int64_t>(coinbase_value);

    // ========================================================================
    // v0.14.0.5: Return canonical coinbasetxn for Utreexo compatibility
    // The daemon owns the coinbase. External miners MUST use this exact
    // coinbase (with extranonce in witness only) to match the Utreexo
    // commitment. This is the only sane architecture for Utreexo + mining.
    // ========================================================================
    if (!block->vtx.empty() && block->vtx[0].IsCoinbase()) {
        const auto& coinbase_tx = block->vtx[0];

        // Serialize coinbase transaction (with witness for extranonce placement)
        auto coinbase_serialized = coinbase_tx.Serialize(::dinero::TxSerializationMode::WithWitness);
        std::ostringstream coinbase_hex;
        for (uint8_t byte : coinbase_serialized) {
            coinbase_hex << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
        }

        din::Json coinbasetxn;
        coinbasetxn["data"] = coinbase_hex.str();
        coinbasetxn["txid"] = coinbase_tx.GetTxid().AsUint256().GetHex();
        coinbasetxn["hash"] = coinbase_tx.GetWtxid().AsUint256().GetHex();  // wtxid for witness

        // ====================================================================
        // WITNESS NONCE METADATA (Utreexo-compatible extranonce)
        // ====================================================================
        // Tell miners exactly where to inject their extranonce in the serialized
        // coinbase. This is in the witness section, so modifying it does NOT
        // change the coinbase txid - preserving the Utreexo commitment.
        // ====================================================================
        if (stats.witness_nonce_offset > 0 && stats.witness_nonce_size == 8) {
            din::Json witness_nonce;
            witness_nonce["offset"] = static_cast<int64_t>(stats.witness_nonce_offset);
            witness_nonce["size"] = static_cast<int>(stats.witness_nonce_size);
            coinbasetxn["witness_nonce"] = witness_nonce;

            ::dinero::g_logger.debug("[v14 GBT] Witness nonce: offset=" +
                std::to_string(stats.witness_nonce_offset) + ", size=8");
        }

        result["coinbasetxn"] = coinbasetxn;

        ::dinero::g_logger.info("[v14 GBT] Canonical coinbasetxn included (txid: " +
            coinbase_tx.GetTxid().AsUint256().GetHex().substr(0, 16) + "...)");

        // Store template for submitblock coinbase-modification guard
        {
            std::lock_guard<std::mutex> lock(g_template_mutex);
            MiningTemplate tmpl;
            tmpl.utreexo_root_hex = utreexo_commitment;
            tmpl.coinbase_txid_hex = coinbase_tx.GetTxid().AsUint256().GetHex();
            tmpl.height = stats.height;
            g_recent_templates.push_back(tmpl);
            while (g_recent_templates.size() > MAX_RECENT_TEMPLATES) {
                g_recent_templates.pop_front();
            }
        }
    }

    // Transactions (exclude coinbase, include full data)
    din::Json transactions(Json::arrayValue);
    for (size_t i = 1; i < block->vtx.size(); i++) {
        const auto& tx = block->vtx[i];

        din::Json tx_obj;
        std::string txid_hex = tx.GetTxid().AsUint256().GetHex();  // Consensus→RPC: TxId to hex
        tx_obj["txid"] = txid_hex;

        // Serialize transaction to hex
        auto tx_serialized = tx.Serialize();
        std::ostringstream hex_stream;
        for (uint8_t byte : tx_serialized) {
            hex_stream << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
        }
        tx_obj["data"] = hex_stream.str();

        // Fee (from mempool, already calculated)
        // Phase M.0: RPC boundary - convert hex to uint256 for mempool lookup
        auto fee_opt = mempool->getTransactionFee(uint256::FromHexUnsafe(txid_hex));
        if (fee_opt.has_value()) {
            tx_obj["fee"] = static_cast<int64_t>(fee_opt.value());
        } else {
            tx_obj["fee"] = 0;
        }

        transactions.append(tx_obj);
    }
    result["transactions"] = transactions;

    // Block weight and size
    result["weight"] = static_cast<int64_t>(stats.block_weight);
    result["size"] = static_cast<int64_t>(stats.block_size);

    // Total fees
    result["totalfees"] = static_cast<int64_t>(stats.total_fees);

    // Capabilities
    din::Json capabilities(Json::arrayValue);
    capabilities.append("proposal");
    result["capabilities"] = capabilities;

    // Mutable fields
    din::Json mutable_fields(Json::arrayValue);
    mutable_fields.append("time");
    mutable_fields.append("transactions");
    mutable_fields.append("prevblock");
    result["mutable"] = mutable_fields;

    // Rules (active consensus rules)
    din::Json rules(Json::arrayValue);
    rules.append("csv");
    rules.append("segwit");
    rules.append("utreexo");  // Dinero native Utreexo support
    result["rules"] = rules;

    ::dinero::g_logger.info("[v14 GBT] Block template created: height=" +
        std::to_string(stats.height) +
        ", txs=" + std::to_string(stats.total_txs) +
        ", fees=" + std::to_string(stats.total_fees) + " una" +
        ", utreexo=" + (utreexo_commitment.substr(0, 8) == "00000000" ? "ZERO" : "SET"));

    return result;
}

// ============================================================================
// v0.14.0.3: submitblock RPC
// ============================================================================

/**
 * submitblock - Submit a mined block
 *
 * Validates PoW, header fields, and transaction ordering, then hands off
 * to consensus pipeline for full validation.
 *
 * Usage:
 *   submitblock "blockhex"
 *
 * Returns:
 *   null on success, error string on failure
 */
din::Json rpc_submitblock_v14(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Validate parameters
    if (!params.isArray() || params.size() < 1) {
        result["error"] = "Missing block data parameter";
        return result;
    }

    if (!params[0].isString()) {
        result["error"] = "Block data must be a hex string";
        return result;
    }

    std::string block_hex = params[0].asString();

    if (block_hex.empty()) {
        result["error"] = "Block data cannot be empty";
        return result;
    }

    // Validate hex format
    if (block_hex.length() % 2 != 0) {
        result["error"] = "Invalid hex string (odd length)";
        return result;
    }

    for (char c : block_hex) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            result["error"] = "Invalid hex string (non-hex character)";
            return result;
        }
    }

    // ========================================================================
    // Delegate to BlockAcceptor for full validation and acceptance.
    //
    // A previous version of this handler also performed an early
    // "coinbase-modified-after-template" guard that rejected any block
    // whose utreexo_root was not among the recently-issued GBT templates
    // (g_recent_templates). That guard assumed the daemon owns block
    // assembly and external miners may only vary nonce/timestamp.
    //
    // Stratum V2 Job Declaration (SV2-JD) flips that assumption: the
    // miner picks its own coinbase outputs and derives a correct
    // post-block utreexo_root locally. With the guard in place, such
    // blocks were rejected even when the submitted root was consistent
    // with the coinbase and the pre-block accumulator state.
    //
    // The guard was redundant in any case: BlockAcceptor::ConnectBlock
    // calls ComputeUtreexoRootPure against the submitted block's
    // contents and rejects with `bad-utreexo-root` if the header's root
    // doesn't match what the block actually implies. That check is the
    // canonical one — it catches both "miner mutated the coinbase"
    // AND "miner computed the root wrong" cases, and it does not
    // require the daemon to have first issued a matching template.
    // ========================================================================

    auto accept_result = ::dinero::BlockAcceptor::AcceptBlockFromRPC(block_hex, "submitblock_v14");

    if (accept_result.rejected()) {
        result["error"] = accept_result.reason;
        ::dinero::g_logger.error("[v14 SBT] Block rejected: " + accept_result.reason);
        return result;
    }

    // Success - return null (BIP 22 spec)
    ::dinero::g_logger.info("[v14 SBT] Block accepted successfully");
    return din::Json(Json::nullValue);
}

// ============================================================================
// Stratum v2-style mining: immutable server-side block assembly
// ============================================================================
// The daemon owns full block construction. Miners only grind nonce/ntime.
//
// mining.getjob  → daemon builds block via CreateNewBlock(), stores the
//                  immutable Block object, returns 128-byte header + target
// mining.submit  → miner sends job_id + nonce (+ntime), daemon validates PoW
//                  via struct field access (no byte offsets), submits if valid
//
// This eliminates coinbase-modification issues entirely: the miner never
// touches the coinbase, transaction set, or utreexo proof.
// ============================================================================

// Helper: encode raw bytes as lowercase hex
static std::string to_hex(const uint8_t* data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}

static std::string to_hex(const std::string& binary) {
    return to_hex(reinterpret_cast<const uint8_t*>(binary.data()), binary.size());
}

// ---------------------------------------------------------------------------
// MiningJob: immutable server-side block template with structured fields
// ---------------------------------------------------------------------------
struct MiningJob {
    std::string job_id;
    dinero::Block block;                       // Complete immutable block (Block object, not bytes)
    uint256 tip_hash_at_creation;              // block.header.prev_block_hash
    uint32_t height = 0;
    uint32_t bits = 0;
    uint64_t template_time = 0;                // Original timestamp from CreateNewBlock
    uint64_t max_time = 0;                     // template_time + 7200
    std::string target_hex;                    // Display-format target (from BitsToTargetHex)
    std::string coinbase_txid_hex;             // For guard verification
    std::string utreexo_root_hex;              // For guard verification
    std::chrono::steady_clock::time_point created_at;
};

// ---------------------------------------------------------------------------
// MiningJobStore: thread-safe, bounded, TTL-expiring job cache
// ---------------------------------------------------------------------------
class MiningJobStore {
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const MiningJob>> jobs_;
    uint64_t counter_ = 0;
    static constexpr size_t MAX_JOBS = 32;
    static constexpr int TTL_SECONDS = 600;  // 10 minutes

    // Must be called with mutex_ held
    void evict_expired_locked() {
        auto now = std::chrono::steady_clock::now();
        for (auto it = jobs_.begin(); it != jobs_.end(); ) {
            auto age_s = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second->created_at).count();
            if (age_s > TTL_SECONDS) {
                it = jobs_.erase(it);
            } else {
                ++it;
            }
        }
    }

public:
    // Store a job, return its ID. Evicts expired/oldest as needed.
    std::string store(MiningJob job) {
        std::lock_guard<std::mutex> lock(mutex_);
        evict_expired_locked();
        ++counter_;
        std::ostringstream oss;
        oss << std::hex << std::setfill('0')
            << std::setw(4) << (counter_ & 0xFFFF)
            << std::setw(8) << job.height;
        std::string id = oss.str();
        job.job_id = id;

        while (jobs_.size() >= MAX_JOBS) {
            jobs_.erase(jobs_.begin());
        }

        jobs_[id] = std::make_shared<const MiningJob>(std::move(job));
        return id;
    }

    // Look up job by ID. Returns nullptr if not found or expired.
    std::shared_ptr<const MiningJob> lookup(const std::string& job_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        evict_expired_locked();
        auto it = jobs_.find(job_id);
        if (it == jobs_.end()) return nullptr;
        return it->second;
    }

    // Remove a single job (e.g. after successful block submission).
    void remove(const std::string& job_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.erase(job_id);
    }

    // Remove every cached job (used when mining readiness becomes unsafe).
    void invalidate_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.clear();
    }

    // Evict all jobs whose tip doesn't match current_tip.
    void invalidate_stale(const uint256& current_tip) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end(); ) {
            if (!(it->second->tip_hash_at_creation == current_tip)) {
                it = jobs_.erase(it);
            } else {
                ++it;
            }
        }
    }
};

static MiningJobStore g_job_store;

namespace {
void InvalidateMiningJobsForSafety() {
    g_job_store.invalidate_all();
}
}  // namespace

// ---------------------------------------------------------------------------
// mining.getjob — build immutable block, store server-side, return header
// ---------------------------------------------------------------------------
//
// Params: [{"address": "din1..."}]
//
// Returns:
//   job_id, header_hex (256 hex chars), target, height, bits, prev_hash,
//   nonce_offset/size, ntime_offset/size (from offsetof), min_time, max_time
//
din::Json rpc_mining_getjob(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // --- Service access (same pattern as getblocktemplate) ---
    if (!ctx.daemon) {
        result["error"] = "DaemonContext not available";
        return result;
    }
    auto chainstate = std::dynamic_pointer_cast<::dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }
    auto mempool_service = std::dynamic_pointer_cast<::dinero::MempoolService>(ctx.daemon->mempool);
    if (!mempool_service) {
        result["error"] = "Mempool service not available";
        return result;
    }
    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        result["error"] = "Chain database not available";
        return result;
    }

    // --- Parse mining address ---
    din::Json params_obj;
    if (params.isArray() && params.size() > 0)      params_obj = params[0];
    else if (params.isObject())                       params_obj = params;
    else { result["error"] = "Missing required parameter: address"; return result; }

    if (!params_obj.isMember("address") || !params_obj["address"].isString()) {
        result["error"] = "Missing or invalid address parameter";
        return result;
    }
    std::string mining_address = params_obj["address"].asString();
    if (mining_address.empty() || mining_address.length() < 10) {
        result["error"] = "Invalid mining address";
        return result;
    }

    // Apply the same safety gate as getblocktemplate to prevent local-only
    // template generation while disconnected/behind.
    auto config_service = std::dynamic_pointer_cast<::dinero::ConfigService>(ctx.daemon->config);
    const auto readiness = ::dinero::mining::EvaluateMiningReadiness(
        chainstate.get(),
        ctx.daemon->p2p.get(),
        config_service.get());
    if (!readiness.ready) {
        InvalidateMiningJobsForSafety();
        PopulateTemplateSafetyError(result, readiness);
        ::dinero::g_logger.warning("[mining.getjob] Job rejected by safety gate: reason=" +
                                   readiness.reason_code + ", detail=" + readiness.message);
        return result;
    }

    // --- Build block via CreateNewBlock (single authoritative source) ---
    ::dinero::Mempool& mempool_ref = mempool_service->mempool();
    ::dinero::BlockAssembler assembler(chain_db);
    assembler.setMempool(&mempool_ref);

    auto* utreexo_forest = chainstate->utreexoForest();
    auto* utxo_index = chainstate->utxoIndex();
    std::shared_ptr<dinero::consensus::WalletUTXOAdapter> utxo_adapter;
    if (utreexo_forest && utxo_index) {
        utxo_adapter = std::make_shared<dinero::consensus::WalletUTXOAdapter>(utxo_index);
        assembler.SetUtreexoForest(utreexo_forest);
        assembler.SetConsensusUTXOSet(chainstate->GetConsensusUTXOSet());  // snapshot forest under shared lock (UAF guard)
        assembler.SetUTXOProvider(utxo_adapter);
    }

    // Wire BlockValidator for Utreexo root computation (single source of truth)
    auto* block_validator = chainstate->GetBlockValidator();
    if (!block_validator) {
        result["error"] = "Block validator unavailable (Utreexo oracle not wired)";
        ::dinero::g_logger.error("[mining.getjob] BlockValidator missing; refusing job generation");
        return result;
    }
    assembler.SetBlockValidator(block_validator);

    auto block_ptr = assembler.CreateNewBlock(mining_address);
    if (!block_ptr) {
        const std::string& detail = assembler.getLastTemplateError();
        result["error"] = detail.empty() ? "Failed to create block template"
                                         : "Failed to create block template: " + detail;
        return result;
    }
    auto stats = assembler.getBlockTemplateStats();

    // --- Populate MiningJob from the Block object (structured fields only) ---
    MiningJob job;
    job.block = *block_ptr;
    job.tip_hash_at_creation = block_ptr->header.prev_block_hash;
    job.height = stats.height;
    job.bits = block_ptr->header.difficulty;
    job.template_time = block_ptr->header.timestamp;
    job.max_time = job.template_time + 7200;
    job.target_hex = ::dinero::DineroPoW::BitsToTargetHex(job.bits);
    job.utreexo_root_hex = block_ptr->header.utreexo_root.GetHex();
    if (!block_ptr->vtx.empty()) {
        job.coinbase_txid_hex = block_ptr->vtx[0].GetTxid().AsUint256().GetHex();
    }
    job.created_at = std::chrono::steady_clock::now();

    // Save for logging (job will be moved)
    std::string log_utreexo = job.utreexo_root_hex.substr(0, 16);
    std::string log_coinbase = job.coinbase_txid_hex.substr(0, 16);

    // Evict jobs for stale tips, then store
    g_job_store.invalidate_stale(job.tip_hash_at_creation);
    std::string job_id = g_job_store.store(std::move(job));

    // --- Build response: header + metadata ---
    auto header_bytes = block_ptr->header.SerializeForHash();

    result["job_id"] = job_id;
    result["header_hex"] = to_hex(header_bytes.data(), header_bytes.size());
    result["target"] = ::dinero::DineroPoW::BitsToTargetHex(block_ptr->header.difficulty);
    result["height"] = static_cast<int>(stats.height);

    std::ostringstream bits_hex;
    bits_hex << std::hex << std::setfill('0') << std::setw(8) << block_ptr->header.difficulty;
    result["bits"] = bits_hex.str();
    result["prev_hash"] = block_ptr->header.prev_block_hash.GetHex();

    // Header field offsets from struct (no magic numbers)
    result["nonce_offset"] = static_cast<int>(offsetof(dinero::BlockHeader, nonce));
    result["nonce_size"]   = static_cast<int>(sizeof(dinero::BlockHeader::nonce));
    result["ntime_offset"] = static_cast<int>(offsetof(dinero::BlockHeader, timestamp));
    result["ntime_size"]   = static_cast<int>(sizeof(dinero::BlockHeader::timestamp));
    result["min_time"]     = static_cast<int64_t>(block_ptr->header.timestamp);
    result["max_time"]     = static_cast<int64_t>(block_ptr->header.timestamp + 7200);

    ::dinero::g_logger.info("[mining.getjob] job=" + job_id +
        " height=" + std::to_string(stats.height) +
        " utreexo=" + log_utreexo + "..." +
        " coinbase_txid=" + log_coinbase + "...");

    return result;
}

// ---------------------------------------------------------------------------
// mining.submit — validate nonce, check PoW, submit stored block
// ---------------------------------------------------------------------------
//
// Params: [{"job_id": "...", "nonce": 12345, "ntime": 1234567890}]
//
// Reject codes:
//   stale-job                        — job expired, TTL exceeded, or tip changed
//   invalid-ntime                    — ntime outside [template_time, template_time+7200]
//   high-hash                        — hash does not meet target
//   coinbase-modified-after-template — utreexo root integrity check failed
//   block-rejected                   — consensus validation failed
//
din::Json rpc_mining_submit(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // --- Parse parameters ---
    din::Json p;
    if (params.isArray() && params.size() > 0)      p = params[0];
    else if (params.isObject())                       p = params;
    else { result["error"] = "Missing parameters"; return result; }

    if (!p.isMember("job_id") || !p["job_id"].isString()) {
        result["error"] = "Missing required parameter: job_id";
        return result;
    }
    if (!p.isMember("nonce") || !p["nonce"].isUInt()) {
        result["error"] = "Missing required parameter: nonce (uint32)";
        return result;
    }

    const std::string job_id = p["job_id"].asString();
    const uint32_t nonce = p["nonce"].asUInt();
    const bool has_ntime = p.isMember("ntime");
    const uint64_t ntime = has_ntime ? static_cast<uint64_t>(p["ntime"].asInt64()) : 0;

    // --- 1. Reject jobs if mining readiness is no longer safe ---
    if (ctx.daemon) {
        auto chainstate = std::dynamic_pointer_cast<::dinero::ChainstateService>(ctx.daemon->chainstate);
        auto config_service = std::dynamic_pointer_cast<::dinero::ConfigService>(ctx.daemon->config);
        const auto readiness = ::dinero::mining::EvaluateMiningReadiness(
            chainstate.get(),
            ctx.daemon->p2p.get(),
            config_service.get());
        if (!readiness.ready) {
            InvalidateMiningJobsForSafety();
            PopulateTemplateSafetyError(result, readiness);
            result["code"] = "stale-job";
            result["error"] = readiness.message;
            ::dinero::g_logger.warning("[mining.submit] stale-job (readiness): " + readiness.reason_code);
            return result;
        }
    }

    // --- 2. Look up job (stale-job if missing/expired/TTL) ---
    auto job = g_job_store.lookup(job_id);
    if (!job) {
        result["code"] = "stale-job";
        result["error"] = "Job " + job_id + " not found (expired or invalidated)";
        ::dinero::g_logger.warning("[mining.submit] stale-job: " + job_id);
        return result;
    }

    // --- 3. Stale-job: tip changed since job creation ---
    if (ctx.daemon) {
        auto cs = std::dynamic_pointer_cast<::dinero::ChainstateService>(ctx.daemon->chainstate);
        if (cs) {
            auto* cdb = cs->GetChainDB();
            if (cdb) {
                auto tip = cdb->getTip();
                if (tip.ok()) {
                    if (!(tip.value().hash == job->tip_hash_at_creation)) {
                        result["code"] = "stale-job";
                        result["error"] = "Chain tip changed since job " + job_id + " was created";
                        ::dinero::g_logger.warning("[mining.submit] stale-job (tip changed): " + job_id);
                        return result;
                    }
                }
            }
        }
    }

    // --- 4. Validate ntime range (invalid-ntime) ---
    if (has_ntime) {
        if (ntime < job->template_time || ntime > job->max_time) {
            result["code"] = "invalid-ntime";
            result["error"] = "ntime " + std::to_string(ntime) +
                " outside allowed range [" + std::to_string(job->template_time) +
                ", " + std::to_string(job->max_time) + "]";
            ::dinero::g_logger.warning("[mining.submit] invalid-ntime: " + std::to_string(ntime));
            return result;
        }
    }

    // --- 5. Build candidate header via struct field access (no byte offsets) ---
    dinero::BlockHeader header = job->block.header;  // trivial copy (128 bytes)
    header.nonce = nonce;
    if (has_ntime) {
        header.timestamp = ntime;
    }

    // --- 6. Compute hash and check PoW (high-hash) ---
    uint256 hash = header.GetHash();
    uint256 target = uint256::FromHexUnsafe(job->target_hex);
    if (target < hash) {
        result["code"] = "high-hash";
        result["error"] = "Hash " + hash.GetHex().substr(0, 16) +
            "... does not meet target " + job->target_hex.substr(0, 16) + "...";
        ::dinero::g_logger.debug("[mining.submit] high-hash for job " + job_id);
        return result;
    }

    // --- 7. Guard: utreexo root integrity (coinbase-modified-after-template) ---
    if (header.utreexo_root.GetHex() != job->utreexo_root_hex) {
        result["code"] = "coinbase-modified-after-template";
        result["error"] = "Utreexo root mismatch — block was modified after template creation";
        ::dinero::g_logger.error("[mining.submit] GUARD: coinbase-modified-after-template for job " + job_id);
        return result;
    }

    // --- 8. Build full block with solved header, serialize, submit ---
    dinero::Block candidate = job->block;   // deep copy (one-time cost for valid solution)
    candidate.header = header;              // apply solved nonce/ntime

    std::string block_binary = candidate.Serialize();
    std::string block_hex = to_hex(block_binary);

    ::dinero::g_logger.info("[mining.submit] Submitting job=" + job_id +
        " nonce=" + std::to_string(nonce) +
        (has_ntime ? " ntime=" + std::to_string(ntime) : "") +
        " height=" + std::to_string(job->height) +
        " hash=" + hash.GetHex().substr(0, 16) + "...");

    auto accept_result = ::dinero::BlockAcceptor::AcceptBlockFromRPC(block_hex, "mining_submit");

    if (accept_result.rejected()) {
        result["code"] = "block-rejected";
        result["error"] = accept_result.reason;
        ::dinero::g_logger.error("[mining.submit] REJECTED: " + accept_result.reason);
        return result;
    }

    // Success — remove used job
    g_job_store.remove(job_id);

    ::dinero::g_logger.info("[mining.submit] ACCEPTED height=" +
        std::to_string(job->height) + " job=" + job_id +
        " hash=" + hash.GetHex());
    return din::Json(Json::nullValue);
}

// ============================================================================
// Registration
// ============================================================================

void registerMiningRPCv14() {
    extern RpcRegistry g_rpcRegistry;

    // getblocktemplate - overwrite Phase 26 implementation
    RpcMethodMeta gbt_meta;
    gbt_meta.name = "getblocktemplate";
    gbt_meta.description = "Returns a block template for mining (v0.14.0.4 - with Utreexo)";
    gbt_meta.result.type = "object";
    gbt_meta.result.desc = "Dinero block template with Utreexo commitment";

    g_rpcRegistry.registerHandler("getblocktemplate", rpc_getblocktemplate_v14, gbt_meta,
                                  RegisterMode::Overwrite, "mining_v14");

    // Also register with mining. prefix for compatibility with external miners
    g_rpcRegistry.registerHandler("mining.getblocktemplate", rpc_getblocktemplate_v14, gbt_meta,
                                  RegisterMode::Overwrite, "mining_v14");

    // submitblock - overwrite Phase 26 implementation
    RpcMethodMeta sbt_meta;
    sbt_meta.name = "submitblock";
    sbt_meta.description = "Submit a mined block (v0.14.0.4)";
    sbt_meta.result.type = "null";
    sbt_meta.result.desc = "null on success, error string on failure";

    g_rpcRegistry.registerHandler("submitblock", rpc_submitblock_v14, sbt_meta,
                                  RegisterMode::Overwrite, "mining_v14");

    // mining.getjob - Stratum-style server-side block assembly
    RpcMethodMeta job_meta;
    job_meta.name = "getjob";
    job_meta.description = "Get immutable mining job — server owns block assembly (Stratum-style)";
    job_meta.result.type = "object";
    job_meta.result.desc = "Job with header_hex, target, job_id (miners only vary nonce/ntime)";

    g_rpcRegistry.registerHandler("mining.getjob", rpc_mining_getjob, job_meta,
                                  RegisterMode::Overwrite, "mining_v14");

    // mining.submit - Submit nonce solution for a stored job
    RpcMethodMeta submit_meta;
    submit_meta.name = "submit";
    submit_meta.description = "Submit mining solution — job_id + nonce (+ntime)";
    submit_meta.result.type = "null";
    submit_meta.result.desc = "null on success, error object on failure";

    g_rpcRegistry.registerHandler("mining.submit", rpc_mining_submit, submit_meta,
                                  RegisterMode::Overwrite, "mining_v14");

    ::dinero::g_logger.info("Registered mining RPC: getblocktemplate, submitblock, mining.getjob, mining.submit");
}
