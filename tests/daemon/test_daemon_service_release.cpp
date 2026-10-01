#include "daemon/daemon_app.h"
#include "daemon/runtime_delivery_worker.h"
#include "pool/pool_manager.h"
#include <chrono>
#include <thread>
#include "daemon/services/chainstate_service.h"
#include "daemon/config.h"
#include "consensus/chainparams.h"
#include "consensus/shielded/pedersen_generators.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dinero {
// Narrow inspection only; lifecycle is exercised exclusively through DaemonApp.
struct RuntimeDeliveryStartupTestAccess {
    static RuntimeDeliveryWorker* Worker(DaemonApp& app) {return app.runtime_delivery_worker_.get();}
    static std::shared_ptr<pool::PoolManager> Pool(DaemonApp& app) {return app.pool_manager_runtime_;}
};
}

// The production entry point initializes this before DaemonApp::Init.
namespace p2p { uint32_t g_magic = 0; }

int main(int argc, char** argv) try {
    dinero::SelectParams(dinero::Chain::REGTEST);
    p2p::g_magic = dinero::Params().magic;
    std::string crypto_error;
    if (!dinero::consensus::shielded::CheckPedersenGeneratorsStartupPrecondition(&crypto_error))
        throw std::runtime_error(crypto_error);
    std::vector<std::pair<std::string, std::weak_ptr<void>>> observed;
    std::shared_ptr<dinero::pool::PoolManager> retained_pool;
    {
        dinero::DaemonApp app;
        if (!app.Init(argc, argv)) throw std::runtime_error("daemon Init failed");
        auto& ctx = app.GetContext();
        retained_pool=dinero::RuntimeDeliveryStartupTestAccess::Pool(app);
        if (static_cast<bool>(retained_pool)!=::GetConfig().allow_pool_mining)
            throw std::runtime_error("pool accounting did not follow explicit test configuration/profile");
        if (retained_pool && retained_pool->sendPendingPayouts()!=0)
            throw std::runtime_error("fresh pool unexpectedly dispatched payments");
        // Match main(): the process selects the socket server port after Init.
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg.starts_with("--wallet-socket-port="))
                ctx.wallet_socket_port = static_cast<uint16_t>(std::stoul(arg.substr(21)));
        }
        const auto watch = [&](const char* name, const auto& value) {
            if (value) observed.emplace_back(name, std::weak_ptr<void>(value));
        };
        watch("logger", ctx.logger);
        watch("config", ctx.config);
        watch("chainstate", ctx.chainstate);
        watch("mempool", ctx.mempool);
        watch("wallet", ctx.wallet);
        watch("p2p", ctx.p2p);
        watch("rpc", ctx.rpc);
        watch("mining", ctx.mining);
        watch("metrics", ctx.metrics);
        watch("peer_scoring", ctx.peer_scoring);
        watch("headers_sync", ctx.headers_sync);
        watch("compact_blocks", ctx.compact_blocks);
        watch("address_manager", ctx.address_manager);
        watch("rbf_policy", ctx.rbf_policy);
        watch("chainstate_guard", ctx.chainstate_guard);
        watch("prune", ctx.prune);
        watch("consensus", ctx.consensus);
        watch("header_chain", ctx.header_chain);
        watch("header_store", ctx.header_store);
        watch("header_sync", ctx.header_sync);
        watch("block_download", ctx.block_download);
        watch("parallel_block_download", ctx.parallel_block_download);
        watch("block_storage", ctx.block_storage);
        watch("block_relay", ctx.block_relay);
        watch("tx_relay", ctx.tx_relay);
        if (observed.size() < 20) throw std::runtime_error("daemon graph was not initialized");
        if (dinero::RuntimeDeliveryStartupTestAccess::Worker(app))
            throw std::runtime_error("delivery worker started before daemon Start");
        if(retained_pool && retained_pool->CaptureMaintenanceWakeHandle().Running())
            throw std::runtime_error("pool maintenance started before daemon Start");
        if (!app.Start()) throw std::runtime_error("daemon Start failed");
        if(retained_pool) {
            auto pool_wake=retained_pool->CaptureMaintenanceWakeHandle();
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            auto pool_report=retained_pool->MaintenanceSnapshot();
            while(!pool_report.passes && std::chrono::steady_clock::now()<deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));pool_report=retained_pool->MaintenanceSnapshot();
            }
            if(!pool_wake.Running() || !pool_report.running || !pool_report.passes || !pool_report.accounting_pass_returned || !pool_report.maintenance_pass_returned)
                throw std::runtime_error("actual daemon pool maintenance did not complete its initial checked pass");
        }
        std::cout << "PASS actual daemon pool maintenance initial pass or explicit absence\n";
        auto* delivery=dinero::RuntimeDeliveryStartupTestAccess::Worker(app);
        if (!delivery)throw std::runtime_error("daemon omitted delivery worker");
        auto wake=delivery->CaptureWakeHandle();
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        auto report=delivery->Snapshot();
        while (report.slices==0 && std::chrono::steady_clock::now()<end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));report=delivery->Snapshot();
        }
        if (!wake.Running() || !report.running || !report.slices)
            throw std::runtime_error("daemon worker did not perform its initial durable scan");
        if (report.wallet_head || report.reorg_head || report.intents || report.admission_attempts)
            throw std::runtime_error("fresh daemon invented runtime recovery progress");
        std::cout << "PASS actual daemon initial durable scan without fabricated progress\n";
        {
            auto index=dinero::ChainstateService::AcquireWalletIndexUse(ctx.chainstate);
            if (&index->Index()!=ctx.chainstate->utxoIndex())
                throw std::runtime_error("wallet index owner changed instance");
            bool refused=false;
            try { ctx.chainstate->Stop(); } catch (const std::logic_error&) { refused=true; }
            if (!refused) throw std::runtime_error("owned wallet index did not prevent same-thread shutdown");
        }
        app.Stop();
        if (retained_pool) {
            auto pool_wake=retained_pool->CaptureMaintenanceWakeHandle();
            if(pool_wake.Running() || retained_pool->MaintenanceSnapshot().running)
                throw std::runtime_error("daemon retained running pool maintenance after Stop");
            pool_wake.RequestReplay();
            if(pool_wake.Running())throw std::runtime_error("stopped pool mailbox restarted maintenance");
            bool refused=false;
            try {(void)retained_pool->sendPendingPayouts();}catch(const std::runtime_error&){refused=true;}
            if (!refused) throw std::runtime_error("stopped daemon left pool callbacks available");
            refused=false;
            try {retained_pool->setPaymentCallback([](const std::string&,uint64_t,std::string&){return false;});}
            catch(const std::logic_error&){refused=true;}
            if (!refused) throw std::runtime_error("stopped pool callback owner reopened");
        }
        if (dinero::RuntimeDeliveryStartupTestAccess::Pool(app))
            throw std::runtime_error("daemon retained its pool owner after Stop");
        std::cout << "PASS actual daemon pool maintenance stopped before dependency release\n";
        std::cout << "PASS actual daemon pool callback lifetime closed\n";
        if (dinero::RuntimeDeliveryStartupTestAccess::Worker(app) || wake.Running())
            throw std::runtime_error("daemon retained running delivery worker after Stop");
        wake.RequestReplay(); // A retained mailbox cannot restart a stopped worker.
        if (wake.Running())throw std::runtime_error("stopped delivery mailbox restarted worker");
        std::cout << "PASS actual daemon delivery worker drained before dependency release\n";
        bool stopped=false;
        try { auto index=dinero::ChainstateService::AcquireWalletIndexUse(ctx.chainstate); }
        catch (const std::runtime_error&) { stopped=true; }
        if (!stopped) throw std::runtime_error("stopped chainstate still grants wallet index ownership");
        std::cout << "PASS actual started wallet index ownership and shutdown refusal\n";
        app.Stop(); // Lifecycle remains idempotent.
    }
    if (retained_pool) {
        bool refused=false;
        try {(void)retained_pool->retryFailedPayouts(3);}catch(const std::runtime_error&){refused=true;}
        if (!refused) throw std::runtime_error("retained pool manager reopened after daemon destruction");
    }
    bool retained = false;
    for (const auto& [name, owner] : observed) {
        if (!owner.expired()) {
            std::cerr << "FAIL retained service after daemon destruction: " << name
                      << " owners=" << owner.use_count() << '\n';
            retained = true;
        }
    }
    if (retained) return 1;
    std::cout << "PASS all " << observed.size() << " real daemon services released\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
}
