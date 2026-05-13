/**
 * Mining Extras RPC Methods - vNext Architecture
 *
 * Context-aware wrappers for mining.getblocktemplate and mining.generatetoaddress.
 * These methods access blockchain and mempool via DaemonContext instead of globals.
 */

#include "rpc/rpc_method_builder.h"
#include "rpc/methods_mining_extras.h"
#include "common/logger.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/mining_service.h"
#include "daemon/services/wallet_service.h"  // Phase W.1.1: For WalletService access
#include "storage/chain_db.h"
#include "storage/chain_direct.h"  // For GetChainHeight
#include "transaction_pool.h"
#include "primitives/block.h"  // For Block structure
#include "consensus/chainparams.h"  // Phase W.1.1: For Params() to check network
#include <ctime>  // For std::time

// Forward declarations of legacy implementations from methods_mining_extras.cpp
namespace dinero {
namespace rpc {
    extern din::Json handle_getblocktemplate(
        mempool::TransactionPool* tx_pool,
        dinero::ChainDB* chain_db,
        const din::Json& params
    );

    extern din::Json handle_generatetoaddress(
        std::shared_ptr<dinero::rpc::MiningState> mining_state,
        dinero::ChainDB* chain_db,
        const dinero::rpc::MiningExtrasConfig& config,
        const din::Json& params
    );
}
}

namespace din {
namespace rpc {

namespace {

std::string BytesToHex(const std::vector<uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kHex[(b >> 4) & 0x0f]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

}  // namespace

/**
 * Unified Template Architecture - Convert Block to getblocktemplate JSON
 *
 * Converts a Block (from MiningService::createBlockTemplate) to the JSON format
 * expected by external miners (getblocktemplate RPC spec).
 */
din::Json blockToTemplateJson(const dinero::Block& block, dinero::ChainDB* chain_db) {
    din::Json result;

    // Get chain state for height calculation
    uint32_t height = dinero::storage::GetChainHeight(chain_db);
    uint32_t next_height = height + 1;

    // Extract header fields
    const auto& header = block.header;

    // Format bits as 8-char hex string (BIP22 standard - no 0x prefix)
    char bits_hex[9];
    uint32_t bits = (header.difficulty != 0) ? header.difficulty : header.difficulty;
    std::snprintf(bits_hex, sizeof(bits_hex), "%08x", bits);

    // Build result matching BIP22/BIP23 getblocktemplate spec
    result["version"] = static_cast<int>(header.version);
    result["height"] = static_cast<int>(next_height);
    result["previousblockhash"] = header.prev_block_hash.GetHex();  // Consensus→RPC: always hex encode
    result["bits"] = std::string(bits_hex);
    result["curtime"] = static_cast<int64_t>(header.timestamp != 0 ? header.timestamp : header.timestamp);
    result["mintime"] = static_cast<int64_t>(header.timestamp != 0 ? header.timestamp : header.timestamp);
    result["maxtime"] = static_cast<int64_t>(std::time(nullptr) + 7200); // 2 hours from now

    // Mutable fields (can be changed by miners)
    result["mutable"] = din::arr();
    result["mutable"].append("time");
    result["mutable"].append("transactions");
    result["mutable"].append("prevblock");

    // Add non-coinbase transactions using BIP22-compatible fields.
    din::Json transactions = din::arr();
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const auto& tx = block.vtx[i];
        din::Json tx_obj = din::obj();
        tx_obj["data"] = BytesToHex(tx.Serialize(dinero::TxSerializationMode::WithWitness));
        tx_obj["txid"] = tx.GetTxid().AsUint256().GetHex();
        tx_obj["hash"] = tx.GetTxid().AsUint256().GetHex();
        tx_obj["depends"] = din::arr();
        tx_obj["fee"] = 0;  // Fee accounting is not surfaced by createBlockTemplate() here.
        tx_obj["sigops"] = 0;
        tx_obj["weight"] = static_cast<Json::UInt64>(tx.GetWeight());
        transactions.append(tx_obj);
    }
    result["transactions"] = transactions;

    // Coinbase value (subsidy + fees) - 100 DIN per block
    result["coinbasevalue"] = static_cast<Json::Value::Int64>(10000000000);

    result["longpollid"] = header.prev_block_hash.GetHex();  // Consensus→RPC: always hex encode

    return result;
}

/**
 * Context-aware wrapper for getblocktemplate
 * Uses MiningService::createBlockTemplate() for UNIFIED template generation
 *
 * ✅ This ensures both CPU and GPU miners use the same canonical template source.
 * ✅ Single source of truth: MiningService -> Block -> JSON
 */
din::Json getblocktemplate_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json error_result;

    if (!ctx.daemon) {
        error_result["error"]["code"] = -32000;
        error_result["error"]["message"] = "DaemonContext not available";
        return error_result;
    }

    // Get mining service (unified template source)
    auto mining_svc = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
    if (!mining_svc) {
        error_result["error"]["code"] = -32000;
        error_result["error"]["message"] = "Mining service not available";
        return error_result;
    }

    // Get chainstate for metadata
    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        error_result["error"]["code"] = -32000;
        error_result["error"]["message"] = "Chainstate service not available";
        return error_result;
    }

    // Phase 39: Get chain database via ChainstateService (ChainManager deleted)
    dinero::ChainDB* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
    if (!chain_db) {
        error_result["error"]["code"] = -32000;
        error_result["error"]["message"] = "Chain database not available";
        return error_result;
    }

    // ═══════════════════════════════════════════════════════════════
    // UNIFIED TEMPLATE ARCHITECTURE
    // ═══════════════════════════════════════════════════════════════
    // Create block template using the SAME source used by CPU and GPU miners
    // This guarantees consistency across all mining backends
    dinero::Block block_template = mining_svc->createBlockTemplate(*ctx.daemon);

    // Convert Block to getblocktemplate JSON format
    return blockToTemplateJson(block_template, chain_db);
}

/**
 * Context-aware wrapper for generatetoaddress
 * Accesses chainstate and mining services via DaemonContext
 */
din::Json generatetoaddress_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json error_result;

    if (!ctx.daemon) {
        error_result["error"]["code"] = -32000;
        error_result["error"]["message"] = "DaemonContext not available";
        return error_result;
    }

    // Get chainstate service
    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        error_result["error"]["code"] = -32000;
        error_result["error"]["message"] = "Chainstate service not available";
        return error_result;
    }

    // Phase 39: Get chain database via ChainstateService (ChainManager deleted)
    dinero::ChainDB* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
    if (!chain_db) {
        error_result["error"]["code"] = -32000;
        error_result["error"]["message"] = "Chain database not available";
        return error_result;
    }

    // Get mining service
    auto mining = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
    if (!mining) {
        error_result["error"]["code"] = -32000;
        error_result["error"]["message"] = "Mining service not available";
        return error_result;
    }

    // Seed mining state from the active MiningManager snapshot.
    auto mining_state = std::make_shared<dinero::rpc::MiningState>();
    auto& mining_manager = mining->getMiningManager();
    const auto& stats = mining_manager.getStats();
    int thread_count = mining_manager.getThreadCount();
    if (thread_count < 0) thread_count = 0;
    mining_state->mining_enabled.store(mining_manager.isMining());
    mining_state->num_threads.store(static_cast<uint32_t>(thread_count));
    mining_state->total_hashes.store(stats.total_hashes.load());
    mining_state->blocks_found.store(static_cast<uint32_t>(stats.blocks_found.load()));
    mining_state->current_hashrate.store(stats.current_hashrate.load());
    mining_state->mining_address = mining_manager.getMiningAddress();

    // Build config from context
    dinero::rpc::MiningExtrasConfig config;
    // Phase W.1.1: Check actual network from consensus params
    const auto& chain_params = dinero::Params();
    config.regtest = (chain_params.name == "regtest");
    config.testnet = (chain_params.name == "testnet");
    config.datadir = "";
    config.rpc_port = 0;

    // Call legacy implementation
    return dinero::rpc::handle_generatetoaddress(mining_state, chain_db, config, params);
}

void registerMiningExtrasMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // ADVANCED MINING
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("mining.generatetoaddress", "mining")
        .description("Mine blocks immediately to a specified address (regtest only)")
        .param("nblocks", "number", "Number of blocks to mine", true)
        .param("address", "string", "Address to receive block rewards", true)
        .result("array", "Array of generated block hashes")
        .handler(generatetoaddress_impl)
        .mode(RegisterMode::Overwrite)  // Replace legacy handler from methods_mining_context.cpp
        .examples({
            "generatetoaddress 10 \"din1q...\"",
            "generatetoaddress 1 \"din1q...\""
        });

    // Phase W.1.1: Add deterministic "generate" RPC (Bitcoin Core compatible)
    // Automatically uses wallet address if available, otherwise fallback
    RPC_METHOD("generate", "mining")
        .description("Mine blocks immediately (regtest only, wallet address auto-selected)")
        .param("nblocks", "number", "Number of blocks to mine", true)
        .result("array", "Array of generated block hashes")
        .handler([](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            // Get wallet address or use fallback
            std::string address = "";

            // Try to get address from wallet
            if (ctx.daemon && ctx.daemon->wallet) {
                try {
                    auto wallet_svc = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
                    if (wallet_svc) {
                        auto& mgr = wallet_svc->get();
                        address = mgr.getNewAddress("mining", "taproot");
                    }
                } catch (...) {
                    // Wallet not available - use fallback
                }
            }

            // Fallback to default address
            if (address.empty()) {
                address = "din1pegrzhlug8ak32yd89fu2p8e6zl9kwd8ee6z5874xdalrsr2c6xmss6h8k0";
            }

            // Build params for generatetoaddress
            din::Json delegateParams = din::arr();
            if (params.isArray() && params.size() > 0) {
                delegateParams.append(params[0]);  // nblocks
            } else {
                delegateParams.append(1);  // default 1 block
            }
            delegateParams.append(address);

            // Delegate to generatetoaddress
            return generatetoaddress_impl(ctx, delegateParams);
        })
        .mode(RegisterMode::Overwrite)
        .examples({
            "generate 10",
            "generate 1"
        });

    // =========================================================================
    // Mining Template RPC: Fail-Closed on vNext Stub, Alias to Production GBT
    // =========================================================================
    // The vNext "unified template" JSON conversion in this TU is intentionally
    // incomplete (e.g., tx list omitted). Exposing it in production would cause
    // miners/pools to receive templates that may not validate.
    //
    // Instead, provide compatibility aliases that resolve to the production
    // getblocktemplate handler (registered later by mining_v14).
    ::g_rpcRegistry.registerAlias("mining.gettemplate", "getblocktemplate");
    ::g_rpcRegistry.registerAlias("gettemplate", "getblocktemplate");

    dinero::g_logger.info("✅ Registered mining extras (vNext DSL): mining.generatetoaddress, generate; aliases: mining.gettemplate/gettemplate → getblocktemplate");
}

// NOTE: Auto-registration removed - registration is now called explicitly from
// rpc_context_wiring.cpp in WireRpcContext() to ensure proper daemon context setup

} // namespace rpc
} // namespace din
