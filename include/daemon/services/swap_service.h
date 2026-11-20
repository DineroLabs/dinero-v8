#pragma once
// SwapService: runs DIN <-> BTC atomic swaps inside dinerod (milestone 6 of
// docs/design/din-btc-atomic-swaps-v1-plan.md). OFF unless `swap.enable=1`:
// a node without it starts no thread and touches no file.
//
// Config (dinero.conf or --key=value):
//   swap.enable=1
//   swap.btc_rpc=HOST:PORT        Bitcoin Core RPC (one loaded wallet: BTC funding)
//   swap.btc_rpc_user / swap.btc_rpc_pass   (the password is never logged)
//   swap.tick_seconds=30
//   swap.tower_inbox=DIR          optional: arm a dinero-swap-tower for Bob
//   swap.tower_url=https://HOST   optional: arm a tower over the network instead
//   swap.din_fee_una / swap.din_fee_urgent_una / swap.btc_fee_sat   (optional)
//   swap.btc_rehearsal_chain=testnet4   regtest only: the Bitcoin side is the
//                                 public testnet4 (rehearsals)
//   swap.optional=1               a swap setting this node refuses leaves swaps
//                                 off instead of stopping the node (dinero-qt's
//                                 "Enable swaps" switch); swap.* then say why
// Swap files live in <datadir>/swaps (mode 0700), encrypted with a key derived
// from the wallet seed; while the wallet is locked swaps are paused.

#include "daemon/iservice.h"
#include "wallet/swap/swap_manager.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct DaemonContext;

namespace dinero {

class ConfigService;
// Typed component shared by the service and serialized adapter qualification.
// The context must outlive the callback; each call retains its wallet service.
swap::DinFundingOwner MakeRetainedSwapFundingOwner(DaemonContext& ctx);

class SwapService : public IService {
public:
    std::string Name() const override { return "Swap"; }
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // nullptr when swaps are disabled.
    swap::SwapManager* manager() { return manager_.get(); }
    uint32_t tick_seconds() const { return tick_seconds_; }

    // The running instance (for the swap.* RPCs); nullptr when disabled.
    static SwapService* Active();
    // False when no service runs, and also when it runs for the built-in
    // Bitcoin wallet alone (swap.btc_wallet=1 without swap.enable=1): then
    // the manager exists, and every swap.* request is refused as "disabled".
    static bool SwapsOn();

    // Why swap.enable=1 did not turn swaps on (swap.optional=1 kept the node
    // running); empty when swaps are on or were never asked for.
    static std::string OffReason();
    // Prints why the swap configuration is refused and remembers it for
    // OffReason(). For Init and the installers below; always returns false.
    static bool Refuse(const std::string& why);

    // Which Bitcoin network a node on `dinero_network` swaps against: its own
    // counterpart, except in a rehearsal. swap.btc_rehearsal_chain=testnet4 on
    // a Dinero REGTEST node pairs it with the public Bitcoin testnet4, because
    // Dinero's testnet does not enforce time locks and so cannot host a swap.
    // Init refuses the option anywhere else.
    static swap::SwapNetwork BitcoinNetworkFor(const ConfigService& config, swap::SwapNetwork dinero_network);

    // Zcash swaps (dinerod built with DINERO_SWAP_ZEC): swap_zec_install.cpp,
    // linked only into that dinerod, registers this at start-up; Init calls it
    // when swap.zec_rpc is set. Returns false (after printing why) on a bad
    // config. Without it, swap.zec_rpc is refused.
    using ZecInstaller = std::function<bool(swap::SwapManager& manager, const ConfigService& config,
                                            swap::SwapNetwork network, swap::DinRpc din)>;
    static void SetZecInstaller(ZecInstaller installer);

    // Bitcoin without Bitcoin Core (dinerod built with DINERO_SWAP_LIGHT_BTC):
    // swap_light_btc_install.cpp registers this; Init calls it when
    // swap.btc_backend=light. It installs the support in the manager and
    // returns what to run before every tick (the header sync), or nothing
    // (after printing why) on a bad config.
    using LightBtcInstaller = std::function<std::function<void(uint32_t now_unix)>(
        swap::SwapManager& manager, const ConfigService& config, swap::SwapNetwork network, swap::DinRpc din)>;
    static void SetLightBtcInstaller(LightBtcInstaller installer);

    // swap.tower_url (the same dinerod: it has the HTTPS client). Installs the
    // tower's sink and ack in the manager; false (after Refuse) on a bad address.
    using TowerUrlInstaller = std::function<bool(swap::SwapManager& manager, const ConfigService& config)>;
    static void SetTowerUrlInstaller(TowerUrlInstaller installer);

    // Starts the built-in Bitcoin wallet in a node that is already running
    // (no swaps): what swap.btc_wallet=1 does at start-up, on request, so
    // that turning it on in the app needs no restart. True when it runs
    // afterwards (also if it ran already, alone or with swaps). False with
    // the reason in `why` when this node cannot run it.
    static bool StartBitcoinWalletNow(std::string* why);

private:
    bool bitcoin_wallet_only_{false};
    bool bitcoin_wallet_requested_{false};  // StartBitcoinWalletNow, as swap.btc_wallet=1
    bool Setup(DaemonContext& ctx);
    void Loop();

    DaemonContext* ctx_{nullptr};
    std::unique_ptr<swap::SwapManager> manager_;
    uint32_t tick_seconds_{30};
    std::function<void(uint32_t)> before_tick_;  // light Bitcoin: header sync
    std::thread thread_;
    std::mutex wake_mu_;
    std::condition_variable wake_;
    bool stopping_{false};
};

}  // namespace dinero
