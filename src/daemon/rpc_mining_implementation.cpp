#include "rpc/rpc_http.hpp"
#include "daemon/mining_defaults.h"
#include "daemon/mining_safety_gates.h"
#include "util/validation.h"
#include "consensus/params.h"
#include "chainparams.h"
#include <univalue.h>
#include <stdexcept>
#include <string>

// Global mining state (simplified for initial implementation)
static bool g_miningActive = false;
static unsigned g_miningThreads = 0;
static double g_miningThrottle = 0.35;
static std::string g_miningAddress;
static std::chrono::steady_clock::time_point g_miningStartTime;

// Mining statistics (will be replaced by proper mining engine)
struct MiningStats {
    double hashrateMA = 0.0;
    uint64_t blocksFound = 0;
    std::chrono::steady_clock::time_point lastSubmitTime;
    std::string pauseReason;
} g_miningStats;

/**
 * @brief mining.start RPC method
 * 
 * Starts mining with comprehensive safety validation
 */
UniValue mining_start(const JSONRPCRequest& request) {
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "mining.start {\"address\":\"...\", \"threads\":\"auto\", \"throttle\":0.35, \"low_power_mode\":true, \"i_understand\":false}\n"
            "\nStart CPU mining with safety validation\n"
            "\nArguments:\n"
            "1. options             (object, required)\n"
            "   {\n"
            "     \"address\"         (string, required) Mining address (must be valid and owned)\n"
            "     \"threads\"         (string|number, optional, default=\"auto\") Number of threads or \"auto\"\n"
            "     \"throttle\"        (number, optional, default=0.35) CPU throttle 0.15-0.90\n"
            "     \"low_power_mode\"  (boolean, optional, default=true) Increase throttle on battery\n"
            "     \"i_understand\"    (boolean, required for mainnet/testnet) Acknowledge mining risks\n"
            "   }\n"
            "\nResult:\n"
            "{\n"
            "  \"running\": true,           (boolean) Mining is active\n"
            "  \"effective\": {             (object) Effective settings\n"
            "    \"threads\": 8,            (number) Actual thread count\n"
            "    \"throttle\": 0.35         (number) Actual throttle setting\n"
            "  },\n"
            "  \"network\": \"regtest\"      (string) Current network\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("mining.start", "\'{\"address\":\"din1q...\", \"threads\":\"auto\"}\'")
            + HelpExampleRpc("mining.start", "{\"address\":\"din1q...\", \"threads\":8, \"throttle\":0.25}")
        );
    }

    RPCTypeCheck(request.params, {UniValue::VOBJ});
    const UniValue& options = request.params[0];

    // Extract parameters
    if (!options.exists("address")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing required parameter: address");
    }
    std::string address = options["address"].get_str();

    // Get threads setting
    unsigned threads = suggestedThreadsAuto();
    if (options.exists("threads")) {
        const UniValue& threadsVal = options["threads"];
        if (threadsVal.isStr() && threadsVal.get_str() == "auto") {
            threads = suggestedThreadsAuto();
        } else if (threadsVal.isNum()) {
            threads = clampThreads(threadsVal.get_int());
        } else {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "threads must be \"auto\" or a number");
        }
    }

    // Get throttle setting
    double throttle = MiningDefaults::DEFAULT_THROTTLE;
    if (options.exists("throttle")) {
        throttle = clampThrottle(options["throttle"].get_real());
    }

    // Low power mode adjustment
    bool lowPowerMode = true;
    if (options.exists("low_power_mode")) {
        lowPowerMode = options["low_power_mode"].get_bool();
    }
    if (lowPowerMode) {
        throttle = std::max(throttle, 0.35); // Bias toward gentler mining
    }

    // Network-specific validation
    const std::string& network = Params().NetworkIDString();
    bool enableLocalMining = gArgs.GetBoolArg("-enablelocalmining", false);
    bool userUnderstands = false;
    
    if (!Params().IsRegTest()) {
        // Public network validation
        if (!enableLocalMining) {
            throw JSONRPCError(RPC_MISC_ERROR, 
                "Local mining disabled on " + network + ". "
                "Start daemon with -enablelocalmining=1 to enable.");
        }
        
        if (!options.exists("i_understand") || !options["i_understand"].get_bool()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, 
                "Set i_understand=true to acknowledge local mining risks on " + network);
        }
        userUnderstands = true;
    }

    // Comprehensive safety validation
    auto safetyResult = MiningSafetyGates::ValidateMiningSafety(
        address, network, enableLocalMining, userUnderstands
    );

    if (!safetyResult.canStartMining) {
        throw JSONRPCError(RPC_MISC_ERROR, 
            "Mining safety check failed: " + safetyResult.blockingReason);
    }

    // Log any warnings (non-blocking)
    for (const auto& warning : safetyResult.warnings) {
        LogPrintf("Mining warning: %s\n", warning);
    }

    // This legacy RPC file is not wired to the production MiningService.
    // Do not claim success or mutate pseudo-state here.
    (void)threads;
    (void)throttle;
    (void)address;
    throw JSONRPCError(
        RPC_MISC_ERROR,
        "Legacy mining.start handler is disabled (not wired to active MiningService). "
        "Use the context-aware mining RPC path."
    );
}

/**
 * @brief mining.status RPC method
 * 
 * Returns current mining status and statistics
 */
UniValue mining_status(const JSONRPCRequest& request) {
    if (request.fHelp || request.params.size() != 0) {
        throw std::runtime_error(
            "mining.status\n"
            "\nReturns current mining status and statistics\n"
            "\nResult:\n"
            "{\n"
            "  \"running\": true,              (boolean) Mining is active\n"
            "  \"threads\": 8,                 (number) Active thread count\n"
            "  \"throttle\": 0.35,             (number) Current throttle setting\n"
            "  \"hashrate_hps_ma\": 245000.0,  (number) 5-minute moving average hashrate\n"
            "  \"blocks_mined\": 0,            (number) Blocks found this session\n"
            "  \"last_submit_ts\": 1737240350, (number) Unix timestamp of last submission\n"
            "  \"pause_reason\": null          (string|null) Reason if paused\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("mining.status", "")
            + HelpExampleRpc("mining.status", "")
        );
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("running", g_miningActive);
    result.pushKV("threads", (int)g_miningThreads);
    result.pushKV("throttle", g_miningThrottle);
    result.pushKV("hashrate_hps_ma", g_miningStats.hashrateMA);
    result.pushKV("blocks_mined", (int)g_miningStats.blocksFound);
    
    // Convert last submit time to Unix timestamp
    auto lastSubmitEpoch = g_miningStats.lastSubmitTime.time_since_epoch();
    auto lastSubmitSeconds = std::chrono::duration_cast<std::chrono::seconds>(lastSubmitEpoch).count();
    result.pushKV("last_submit_ts", lastSubmitSeconds);
    
    // Pause reason (null if not paused)
    if (g_miningStats.pauseReason.empty()) {
        result.pushKV("pause_reason", UniValue(UniValue::VNULL));
    } else {
        result.pushKV("pause_reason", g_miningStats.pauseReason);
    }

    result.pushKV("backend", "legacy-disabled");
    result.pushKV("note", "handler not wired to active MiningService");
    
    return result;
}

/**
 * @brief mining.stop RPC method
 * 
 * Stops mining and returns final statistics
 */
UniValue mining_stop(const JSONRPCRequest& request) {
    if (request.fHelp || request.params.size() != 0) {
        throw std::runtime_error(
            "mining.stop\n"
            "\nStop mining and return final statistics\n"
            "\nResult:\n"
            "{\n"
            "  \"stopped\": true,       (boolean) Mining was stopped\n"
            "  \"stop_reason\": \"user\" (string) Reason for stopping\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("mining.stop", "")
            + HelpExampleRpc("mining.stop", "")
        );
    }

    // This legacy RPC file is not wired to the production MiningService.
    throw JSONRPCError(
        RPC_MISC_ERROR,
        "Legacy mining.stop handler is disabled (not wired to active MiningService). "
        "Use the context-aware mining RPC path."
    );
}

// Helper function to register mining RPCs
void RegisterMiningRPCs() {
    // These will be registered in the main RPC registry
    // Implementation depends on existing RPC framework
}
