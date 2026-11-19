// Zcash swaps for dinerod (docs/design/din-zec-atomic-swaps.md). Compiled into
// dinerod only with DINERO_SWAP_ZEC: it registers SwapService's Zcash
// installer at start-up, so the default dinerod contains no Zcash code.
//
// Config: swap.zec_rpc=HOST:PORT (Zebra), optional swap.zec_rpc_user /
// swap.zec_rpc_pass (Zebra cookie auth: user "__cookie__", the cookie as pass).
#include "daemon/services/config_service.h"
#include "daemon/services/swap_service.h"
#include "rpc_client.h"
#include "wallet/swap/swap_manager_zec.h"

#include <iostream>
#include <memory>

namespace dinero {
namespace {

bool InstallZec(swap::SwapManager& manager, const ConfigService& config, swap::SwapNetwork network, swap::DinRpc din) {
    const std::string zec = config.GetString("swap.zec_rpc", "");
    const auto colon = zec.rfind(':');
    if (colon == std::string::npos) {
        std::cerr << "[Swap] swap.zec_rpc must be HOST:PORT (Zebra RPC)" << std::endl;
        return false;
    }
    auto client = std::make_shared<rpc::RpcClient>(zec.substr(0, colon), uint16_t(std::stoi(zec.substr(colon + 1))),
                                                   config.GetString("swap.zec_rpc_user", ""),
                                                   config.GetString("swap.zec_rpc_pass", ""));
    swap::ZecDaemonConfig c;
    c.din = std::move(din);
    c.network = network;
    c.expected_chain = network == swap::SwapNetwork::Mainnet ? "main" : "test";
    c.zec = [client](const std::string& m, const Json::Value& p) -> std::optional<Json::Value> {
        const auto r = client->call(m, p);
        if (!r || !r->isMember("result") || (r->isMember("error") && !(*r)["error"].isNull())) return std::nullopt;
        return (*r)["result"];
    };
    // Keeps Zebra's message: the claim's may-have-left check reads it.
    c.broadcast = [client](const std::string& hex) {
        Json::Value p(Json::arrayValue);
        p.append(hex);
        const auto r = client->call("sendrawtransaction", p);
        if (r && (*r)["result"].isString()) return (*r)["result"].asString();
        throw std::runtime_error(r && r->isMember("error") && (*r)["error"].isObject()
                                     ? (*r)["error"]["message"].asString()
                                     : "RPC failed: " + client->get_last_error());
    };
    manager.SetZecSupport(swap::MakeZecSupport(std::move(c)));
    if (network == swap::SwapNetwork::Mainnet) {
        std::cout << "[Swap] Zcash swaps are not enabled on mainnet yet (testnet beta first)" << std::endl;
    }
    return true;
}

const bool g_registered = [] {
    SwapService::SetZecInstaller(InstallZec);
    return true;
}();

}  // namespace
}  // namespace dinero
