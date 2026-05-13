#include "cli/retry.h"
#include "compat/jsoncpp_compat.h"
#include <chrono>
#include <thread>

namespace dinero::cli {

std::pair<bool, std::string> waitForReady(
    std::function<std::pair<bool, Json::Value>(const std::string&, const Json::Value&)> rpcCall,
    int timeoutSec, 
    int retriesPerProbe
) {
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + std::chrono::seconds(timeoutSec > 0 ? timeoutSec : 10);

    std::string lastErr;
    while (Clock::now() < deadline) {
        // Optional: quick ping via 'getnetworkinfo' to warm the pipe
        {
            Json::Value r;
            rpcCall("getnetworkinfo", Json::Value(Json::arrayValue)); // ignore result; best effort
        }

        // Probe: getblockchaininfo
        Json::Value info(Json::nullValue);
        bool ok = false;
        
        for (int a = 0; a <= std::max(0, retriesPerProbe); ++a) {
            auto result = rpcCall("getblockchaininfo", Json::Value(Json::arrayValue));
            if (result.first) { 
                info = result.second;
                ok = true; 
                break; 
            }
            lastErr = "RPC call failed";
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        
        if (!ok) { 
            std::this_thread::sleep_for(std::chrono::milliseconds(250)); 
            continue; 
        }

        // Readiness heuristic:
        // - RPC responsive (we're here)
        // - Either not in initial block download, or (regtest) headers==blocks
        const bool ibd = info.isMember("initialblockdownload") && info["initialblockdownload"].asBool();
        const auto blocks  = info.isMember("blocks")  ? info["blocks"].asInt64()  : 0;
        const auto headers = info.isMember("headers") ? info["headers"].asInt64() : 0;

        bool syncedEnough = !ibd || (headers > 0 && blocks >= headers);
        if (syncedEnough) {
            return {true, "node ready"};
        }

        lastErr = "initial block download in progress";
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return {false, lastErr.empty() ? "timeout waiting for node readiness" : lastErr};
}

} // namespace dinero::cli
