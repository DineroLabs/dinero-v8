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
//   swap.din_fee_una / swap.din_fee_urgent_una / swap.btc_fee_sat   (optional)
// Swap files live in <datadir>/swaps (mode 0700), encrypted with a key derived
// from the wallet seed; while the wallet is locked swaps are paused.

#include "daemon/iservice.h"
#include "wallet/swap/swap_manager.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

struct DaemonContext;

namespace dinero {

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

private:
    void Loop();

    DaemonContext* ctx_{nullptr};
    std::unique_ptr<swap::SwapManager> manager_;
    uint32_t tick_seconds_{30};
    std::thread thread_;
    std::mutex wake_mu_;
    std::condition_variable wake_;
    bool stopping_{false};
};

}  // namespace dinero
