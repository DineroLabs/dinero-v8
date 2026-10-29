#include "wallet/swap/swap_manager_zec.h"

#include "wallet/swap/swap_crypto.h"
#include "wallet/swap/zec_chain_io.h"

#include <algorithm>
#include <ctime>
#include <memory>
#include <stdexcept>

namespace dinero::swap {

ZecSupport MakeZecSupport(ZecDaemonConfig c) {
    if (!c.clock) c.clock = [] { return static_cast<uint32_t>(std::time(nullptr)); };
    const ZecNetwork net = ZecNetworkFor(c.network);
    auto cfg = std::make_shared<ZecDaemonConfig>(std::move(c));

    auto chain_now = [cfg] {
        const auto chain = ObserveZecChain(cfg->zec, cfg->expected_chain);
        if (!chain) throw std::runtime_error("Zcash node unreachable or on another network (swap.zec_rpc)");
        return *chain;
    };
    auto address_of = [net](const Bytes32& key) {
        return EncodeZecTAddress(ZecP2pkhAddressOf(detail::CompressedOf(key), net));
    };

    ZecSupport z;
    z.make_io = [cfg, net](const SwapSession& s, const RunnerConfig& rc, const Bytes32& deposit_key) {
        ZecIoConfig io;
        io.expected_chain = cfg->expected_chain;
        io.network = net;
        io.deposit_secret_key = deposit_key;
        io.change_script = s.btc_payout_script;  // Bob: a deposit's excess goes back to his refund address
        io.broadcast = cfg->broadcast;
        io.clock = cfg->clock;
        return std::unique_ptr<RpcSwapChainIo>(new ZecSwapChainIo(cfg->din, cfg->zec, WatchSpecFor(s, rc), io));
    };
    z.deposit_address = address_of;
    z.deposit_status = [cfg, address_of](const Bytes32& key) {
        const auto d = ObserveZecDeposit(cfg->zec, address_of(key), cfg->expected_chain);
        if (!d.ok) throw std::runtime_error("Zcash node unreachable or on another network (swap.zec_rpc)");
        ZecDepositStatus st;
        st.seen_zat = d.confirmed_zat(1);
        st.confirmed_zat = d.confirmed_zat(kZecDepositConfirmations);
        for (const auto& coin : d.coins) {
            st.confirmations = st.confirmations == 0 ? coin.confirmations : std::min(st.confirmations, coin.confirmations);
        }
        return st;
    };
    z.sweep_deposit = [cfg, chain_now, address_of](const Bytes32& key, const std::vector<uint8_t>& to) {
        const auto d = ObserveZecDeposit(cfg->zec, address_of(key), cfg->expected_chain);
        if (!d.ok) throw std::runtime_error("Zcash node unreachable or on another network (swap.zec_rpc)");
        const auto coins = d.spendable(1);
        if (coins.empty()) throw std::runtime_error("no confirmed deposit payment to send back yet; try after one confirmation");
        const auto chain = chain_now();
        auto tx = BuildZecSweepTx(coins, to, chain.next_block_branch_id, chain.tip_height + 40);
        SignZecP2pkhInputs(tx, coins, key);
        return cfg->broadcast(detail::ToHex(SerializeZecTx(tx)));
    };
    z.payout_script = [net](const std::string& address) {
        const auto a = DecodeZecTAddress(address);  // throws on anything but t1 / t3 / tm / t2
        const bool main = net == ZecNetwork::Main;
        if ((a.network == ZecNetwork::Main) != main) {
            throw std::invalid_argument("Zcash address is for another network: " + address);
        }
        return ZecScriptPubKey(a);
    };
    z.require_no_upgrade = [chain_now](const SwapOffer& offer, uint32_t now) {
        const auto chain = chain_now();
        const uint64_t until = uint64_t(offer.t_btc_unix) + 24 * 60 * 60;
        if (const auto u = ZecUpgradeBefore(chain, now, static_cast<uint32_t>(std::min<uint64_t>(until, UINT32_MAX)))) {
            throw std::runtime_error("Zcash network upgrade " + u->name + " at height " +
                                     std::to_string(u->activation_height) + " falls inside this swap");
        }
    };
    z.tip_height = [chain_now] { return chain_now().tip_height; };
    return z;
}

}  // namespace dinero::swap
