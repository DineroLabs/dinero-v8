#include "rpc/mining_rpc_handlers.h"
#include "wallet/wallet_manager.h"
#include "wallet/wallet_balances.h"
#include "daemon/mining_payout_resolver.h"
#include "daemon/address_helpers.h"
#include "common/logger.h"
#include <stdexcept>
#include <cstdio>

namespace dinero {

sqlite3* MiningRpcContext::currentWalletDb() {
    if (!wallet) {
        throw std::runtime_error("No wallet manager available");
    }
    
    // Get the current wallet database from WalletManager
    // This assumes WalletManager has a method to get the current DB
    // You may need to adjust this based on your WalletManager API
    return wallet->getCurrentDatabase();
}

int MiningRpcContext::tipHeight() {
    if (!chain) {
        throw std::runtime_error("No blockchain available");
    }
    
    return chain->getBlockHeight();
}

bool IsValidDineroBech32(const std::string& addr) {
    // Basic validation: starts with "din1" and reasonable length
    if (addr.length() < 8 || addr.length() > 90) {
        return false;
    }
    
    if (addr.substr(0, 4) != "din1") {
        return false;
    }
    
    // Check for valid bech32 characters (simplified)
    const std::string valid_chars = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    for (size_t i = 4; i < addr.length(); ++i) {
        if (valid_chars.find(addr[i]) == std::string::npos) {
            return false;
        }
    }
    
    return true;
}

Json::Value rpc_wallet_getbalances(MiningRpcContext& ctx, const Json::Value& params) {
    (void)params; // Unused parameter
    
    sqlite3* db = ctx.currentWalletDb();
    if (!db) {
        throw std::runtime_error("No wallet opened");
    }
    
    int tip = ctx.tipHeight();
    WalletBalances b{};
    
    if (!compute_wallet_balances(db, tip, b)) {
        throw std::runtime_error("Failed to compute wallet balances");
    }
    
    return balances_to_json(b);
}

Json::Value rpc_mining_getpayoutaddress(MiningRpcContext& ctx, const Json::Value& params) {
    (void)params; // Unused parameter
    (void)ctx; // Unused parameter
    
    Json::Value out(Json::objectValue);
    
    // Get current mining address using global function
    std::string addr = GetMiningAddress();
    if (!addr.empty()) {
        out["explicit"] = addr;
        out["resolved"] = addr;
    } else {
        out["explicit"] = Json::Value(Json::nullValue);
        out["resolved"] = Json::Value(Json::nullValue);
        out["error"] = "No mining address configured";
    }
    
    return out;
}

Json::Value rpc_mining_setpayoutaddress(MiningRpcContext& ctx, const Json::Value& params) {
    (void)ctx; // Unused parameter
    
    if (!params.isMember("address") || !params["address"].isString()) {
        throw std::runtime_error("Missing or invalid 'address' parameter");
    }
    
    const std::string addr = params["address"].asString();
    
    if (!IsValidDineroBech32(addr)) {
        throw std::runtime_error("Invalid bech32 address format");
    }
    
    // Use global function to set mining address
    SetMiningAddress(addr, GetChainParams());
    g_logger.info("Mining payout address set to: " + addr);
    
    Json::Value out;
    out = "ok";
    return out;
}

Json::Value rpc_mining_start(MiningRpcContext& ctx, const Json::Value& params) {
    (void)params; // Unused parameter
    
    if (!ctx.miner) {
        throw std::runtime_error("Mining component not available");
    }
    
    // Verify we have a valid payout address before starting
    std::string addr = GetMiningAddress();
    if (addr.empty()) {
        throw std::runtime_error("Cannot start mining: No payout address configured");
    }
    g_logger.info("Starting mining with payout address: " + addr);
    
    ctx.miner->setMiningEnabled(true);
    g_logger.info("Mining started");
    
    Json::Value out;
    out = "ok";
    return out;
}

Json::Value rpc_mining_stop(MiningRpcContext& ctx, const Json::Value& params) {
    (void)params; // Unused parameter
    
    if (!ctx.miner) {
        throw std::runtime_error("Mining component not available");
    }
    
    ctx.miner->setMiningEnabled(false);
    g_logger.info("Mining stopped");
    
    Json::Value out;
    out = "ok";
    return out;
}

Json::Value rpc_getmininginfo(MiningRpcContext& ctx, const Json::Value& params) {
    (void)params; // Unused parameter
    
    if (!ctx.miner) {
        throw std::runtime_error("Mining component not available");
    }
    
    Json::Value j(Json::objectValue);
    
    // Basic mining status
    j["mining"] = ctx.miner->isMiningEnabled();
    j["hashrate_hps"] = int64_t(static_cast<long long>(ctx.miner->getCurrentHashrateHps()));
    
    // Difficulty information
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08x", ctx.miner->getDifficulty());
    j["difficulty_bits"] = buf;
    
    // Payout address using global function
    std::string addr = GetMiningAddress();
    if (!addr.empty()) {
        j["payout_address"] = addr;
    } else {
        j["payout_address"] = Json::Value(Json::nullValue);
    }
    
    // Mining statistics (using real metrics from MinerCore)
    j["blocks_found"] = int64_t(static_cast<long long>(ctx.miner->getBlocksFound()));
    
    // Get rejected and stale block counts from Mining component
    j["rejected_blocks"] = int64_t(ctx.miner->getRejectedBlocks());
    j["stale_blocks"] = int64_t(ctx.miner->getStaleBlocks());
    
    // Last block time using metrics
    std::string lastBlockTime = ctx.miner->getLastBlockTimeISO8601();
    if (!lastBlockTime.empty()) {
        j["last_block_time"] = lastBlockTime;
    } else {
        j["last_block_time"] = Json::Value(Json::nullValue);
    }
    
    return j;
}

Json::Value rpc_wallet_getnextrecv(MiningRpcContext& ctx, const Json::Value& params) {
    (void)params; // Unused parameter
    
    if (!ctx.wallet) {
        throw std::runtime_error("Wallet manager not available");
    }
    
    // Use actual wallet derivation from WalletManager
    // Real implementation: get next receive address from HD wallet
    throw std::runtime_error("wallet.getnextrecv not yet implemented - need to wire HD wallet derivation");
}

} // namespace dinero
