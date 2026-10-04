// Helper for the Bitcoin Core regtest check of BtcWatcher
// (tests/wallet/swap_btc_watch_regtest.sh). Same fixed terms as
// swap_btc_regtest_tool; prints one observation as a single line:
//   ok=<0|1> seen=<0|1> confs=<n> value=<sat> spent=<0|1> claim=<0|1> spendconfs=<n> secret=<0|1> mtp=<unix>
//   usage: swap_btc_watch_tool <rpcport> <user> <pass> <scan_from_height> [interactive]
#include "wallet/swap/btc_watcher.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "rpc_client.h"

#include <secp256k1.h>

#include <cstdlib>
#include <iostream>
#include <string>

using namespace dinero;
using namespace dinero::swap;

namespace {
std::array<uint8_t, 33> Pubkey(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    std::array<uint8_t, 32> s{};
    s.back() = scalar;
    secp256k1_pubkey pk;
    secp256k1_ec_pubkey_create(secp, &pk, s.data());
    std::array<uint8_t, 33> out{};
    size_t len = out.size();
    secp256k1_ec_pubkey_serialize(secp, out.data(), &len, &pk, SECP256K1_EC_COMPRESSED);
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: <rpcport> <user> <pass> <scan_from_height> [observations]\n";
        return 2;
    }
    dinero::rpc::RpcClient client("127.0.0.1", static_cast<uint16_t>(std::stoi(argv[1])), argv[2], argv[3]);
    if (std::getenv("SWAP_RPC_DEBUG")) client.set_debug(true);
    BtcWatchTarget target;
    const std::vector<uint8_t> preimage(32, 0x5a);
    crypto::CSHA256().Write(preimage.data(), preimage.size()).Finalize(target.terms.payment_hash.data());
    target.terms.claim_pubkey = Pubkey(4);
    target.terms.refund_pubkey = Pubkey(6);
    target.terms.refund_locktime_unix = 1'800'000'000;
    target.scan_from_height = static_cast<uint32_t>(std::stoul(argv[4]));
    BtcWatcher watcher(
        [&](const std::string& m, const Json::Value& p) {
            // RpcClient returns the whole JSON-RPC envelope; BtcRpc wants the result.
            auto r = client.call(m, p);
            if (!r || !r->isMember("result") || (r->isMember("error") && !(*r)["error"].isNull())) {
                if (std::getenv("SWAP_RPC_DEBUG")) std::cerr << m << ": " << client.get_last_error() << "\n";
                return std::optional<Json::Value>{};
            }
            return std::optional<Json::Value>{(*r)["result"]};
        },
        target);
    auto print = [](const BtcWatchReport& r) {
        std::cout << "ok=" << r.ok << " seen=" << r.htlc.output_seen << " confs=" << r.htlc.output_confirmations
                  << " value=" << r.htlc.output_value << " spent=" << r.htlc.spent
                  << " claim=" << r.htlc.spent_by_claim << " spendconfs=" << r.htlc.spend_confirmations
                  << " secret=" << r.htlc.revealed_preimage.has_value() << " mtp=" << r.mtp_unix << std::endl;
    };
    // "interactive": one observation per stdin line, from the SAME watcher, so
    // the caller can change the chain (mine, reorg) between observations.
    if (argc > 5 && std::string(argv[5]) == "interactive") {
        for (std::string line; std::getline(std::cin, line);) print(watcher.Observe());
        return 0;
    }
    print(watcher.Observe());
    return 0;
}
