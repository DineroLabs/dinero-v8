#include "rpc/rpc_handler.h"
#include "consensus/chainparams.h"
#include "amount.h"
#include <json/json.h>

namespace dinero {
namespace rpc {

/**
 * getgenesisinfo RPC - Returns genesis block information for operators
 * 
 * This RPC provides all critical genesis constants for verification
 * and operational monitoring.
 */
Json::Value getgenesisinfo(const Json::Value& params) {
    const ChainParams& chainparams = Params();
    
    Json::Value result;
    
    // Genesis block header
    result["genesis"]["version"] = static_cast<int>(chainparams.genesis.nVersion);
    result["genesis"]["time"] = static_cast<int64_t>(chainparams.genesis.nTime);
    result["genesis"]["bits"] = "0x" + std::to_string(chainparams.genesis.nBits);
    result["genesis"]["nonce"] = static_cast<int64_t>(chainparams.genesis.nNonce);
    result["genesis"]["hash"] = chainparams.genesis.genesisHashHex;
    result["genesis"]["merkleroot"] = chainparams.genesis.merkleRootHex;
    result["genesis"]["coinbasetext"] = chainparams.genesis.coinbaseText;
    
    // Network information
    result["network"]["id"] = chainparams.net.id;
    result["network"]["hrp"] = chainparams.net.hrp;
    result["network"]["p2p_port"] = chainparams.net.p2p_port;
    result["network"]["rpc_port"] = chainparams.net.rpc_port;
    
    // Currency information
    result["currency"]["ticker"] = "DIN";
    result["currency"]["decimals"] = 6;
    result["currency"]["smallest_unit"] = "una";
    result["currency"]["smallest_unit_plural"] = "una";
    result["currency"]["una_per_din"] = static_cast<int64_t>(dinero::UNA_PER_DIN);
    
    // Premine: removed from Dinero monetary policy
    result["premine"]["enabled"] = false;
    
    // Consensus rules
    result["consensus"]["pow_target_spacing"] = chainparams.consensus.nPowTargetSpacingSec;
    result["consensus"]["pow_target_timespan"] = chainparams.consensus.nPowTargetTimespanSec;
    result["consensus"]["coinbase_maturity"] = 100;  // Standard Bitcoin-like maturity
    
    // Mining phases
    result["mining"]["phase1_end_una"] = static_cast<int64_t>(chainparams.consensus.devFundEndSats);
    result["mining"]["phase1_end_din"] = dinero::FormatDIN(chainparams.consensus.devFundEndSats);
    result["mining"]["phase2_end_una"] = static_cast<int64_t>(chainparams.consensus.phase2StartSats);
    result["mining"]["phase2_end_din"] = dinero::FormatDIN(chainparams.consensus.phase2StartSats);
    
    // Difficulty bounds
    result["difficulty"]["pow_limit_bits"] = "0x" + std::to_string(chainparams.consensus.powLimitBits);
    result["difficulty"]["initial_bits"] = "0x" + std::to_string(chainparams.consensus.nInitialDifficultyBits);
    result["difficulty"]["easy_bits"] = "0x" + std::to_string(chainparams.consensus.easyBits);
    result["difficulty"]["normal_bits"] = "0x" + std::to_string(chainparams.consensus.normalBits);
    
    return result;
}

} // namespace rpc
} // namespace dinero
