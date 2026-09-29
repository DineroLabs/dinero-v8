#include "daemon/daemon_app.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/config.h"
#include "consensus/chainparams.h"
#include "consensus/shielded/pedersen_generators.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// The production entry point initializes this before DaemonApp::Init.
namespace p2p { uint32_t g_magic = 0; }

int main(int argc, char** argv) try {
    dinero::SelectParams(dinero::Chain::REGTEST);
    p2p::g_magic = dinero::Params().magic;
    std::string crypto_error;
    if (!dinero::consensus::shielded::CheckPedersenGeneratorsStartupPrecondition(&crypto_error))
        throw std::runtime_error(crypto_error);
    std::vector<std::pair<std::string, std::weak_ptr<void>>> observed;
    {
        dinero::DaemonApp app;
        if (!app.Init(argc, argv)) throw std::runtime_error("daemon Init failed");
        auto& ctx = app.GetContext();
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
        if (!app.Start()) throw std::runtime_error("daemon Start failed");
        {
            auto index=dinero::ChainstateService::AcquireWalletIndexUse(ctx.chainstate);
            if (&index->Index()!=ctx.chainstate->utxoIndex())
                throw std::runtime_error("wallet index owner changed instance");
            bool refused=false;
            try { ctx.chainstate->Stop(); } catch (const std::logic_error&) { refused=true; }
            if (!refused) throw std::runtime_error("owned wallet index did not prevent same-thread shutdown");
        }
        app.Stop();
        bool stopped=false;
        try { auto index=dinero::ChainstateService::AcquireWalletIndexUse(ctx.chainstate); }
        catch (const std::runtime_error&) { stopped=true; }
        if (!stopped) throw std::runtime_error("stopped chainstate still grants wallet index ownership");
        std::cout << "PASS actual started wallet index ownership and shutdown refusal\n";
        app.Stop(); // Lifecycle remains idempotent.
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
