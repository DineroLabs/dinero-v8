#include "daemon/daemon_app.h"
#include "consensus/chainparams.h"
#include "primitives/uint256.h"
#include "consensus/shielded/pedersen_generators.h"
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

// Test executable only: production dinerod gains no checkpoint override.
namespace p2p { uint32_t g_magic = 0; }
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
}
int main(int argc, char** argv) try {
    bool regtest = false;
    for (int i = 1; i < argc; ++i) regtest |= std::string(argv[i]) == "--regtest";
    if (!regtest) throw std::runtime_error("checkpoint fixture is regtest-only");
    dinero::SelectParams(dinero::Chain::REGTEST);
    auto& params = dinero::MutableParams();
    params.regtest_enforce_pow = true;
    params.sixty_second_activation_height = 4;
    if (const char* hash = std::getenv("DINERO_TEST_CHECKPOINT_HASH")) {
        dinero::uint256 parsed;
        if (std::string(hash).size() != 64 || !dinero::uint256::FromHex(hash, parsed) || parsed.IsNull())
            throw std::runtime_error("invalid fixture checkpoint hash");
        params.vCheckpoints.emplace(8, hash);
    }
    p2p::g_magic = params.magic;
    std::string error;
    if (!dinero::consensus::shielded::CheckPedersenGeneratorsStartupPrecondition(&error))
        throw std::runtime_error(error);
    dinero::DaemonApp app;
    if (!app.Init(argc, argv)) throw std::runtime_error("daemon Init failed");
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg.starts_with("--wallet-socket-port="))
            app.GetContext().wallet_socket_port = static_cast<uint16_t>(std::stoul(arg.substr(21)));
    }
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    if (!app.Start()) throw std::runtime_error("daemon Start failed");
    while (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    app.Stop();
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL checkpoint fixture: " << error.what() << '\n';
    return 1;
}
