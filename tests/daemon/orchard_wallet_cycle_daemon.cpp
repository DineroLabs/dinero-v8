#include "daemon/daemon_app.h"
#include "daemon/datadir_guard.h"
#include "daemon/regtest_pow_profile.h"
#include "consensus/chainparams.h"
#include "consensus/orchard_profile.h"
#include "consensus/shielded/pedersen_generators.h"
#include <iostream>
#include <stdexcept>
#include <string>

// The normal executable initializes this before DaemonApp::Init.
namespace p2p { uint32_t g_magic = 0; }

int main(int argc, char** argv) try {
    // Process-local REGTEST schedule, as in the existing provider fixture.
    // There is no production command-line activation option or database edit.
    dinero::SelectParams(dinero::Chain::REGTEST);
#ifndef DINERO_HAS_ORCHARD_RUNTIME_READER
    throw std::runtime_error("Orchard wallet cycle requires the real backend");
#else
#ifdef DINERO_TEST_COUPLED_ORCHARD_RELEASE
    // Isolated empty historical pool prerequisites; never public parameters.
    auto& isolated = dinero::MutableParams();
    isolated.shielded_cv_binding_activation_height = 1;
    isolated.shielded_epoch_reset_height = 1;
    isolated.shielded_spend_auth_activation_height = 2;
    isolated.shielded_spend_auth_epoch_reset_height = 2;
    dinero::consensus::ConfigureOrchardRelease(isolated, 102, 1);
#else
    dinero::MutableParams().orchard_activation_height = 102;
    dinero::MutableParams().orchard_branch_id = 1;
#endif
    if (!dinero::consensus::OrchardProfileConfigurationValid(dinero::Params()))
        throw std::runtime_error("Invalid isolated Orchard schedule");
#endif
    // Use the existing enforced-PoW profile for the entire fresh history,
    // including the historical prefix independently validated during reindex.
    auto& params = dinero::MutableParams();
    params.regtest_enforce_pow = true;
    // The default lifecycle starts with 60-second timing; the coupled target
    // switches the joint release schedule at the actual Orchard boundary.
#ifndef DINERO_TEST_COUPLED_ORCHARD_RELEASE
    params.sixty_second_activation_height = 1;
#endif
    const auto profile = dinero::ConsensusChecksum(params);
    params.magic = static_cast<uint32_t>(std::stoul(profile.substr(0, 8), nullptr, 16));
    if (params.magic == 0 || params.magic == 0xFABFB5DAu ||
        params.magic == 0xD1A0C0DEu || params.magic == 0xDAB5BFFAu)
        throw std::runtime_error("Isolated PoW profile network magic collision");
    p2p::g_magic = params.magic;
    std::string error;
    if (!dinero::consensus::shielded::CheckPedersenGeneratorsStartupPrecondition(&error))
        throw std::runtime_error(error);
    // Match the normal executable's datadir ownership before DaemonApp::Init.
    // This fixture requires the explicit path supplied by its Python parent.
    std::string datadir;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument.rfind("--datadir=", 0) == 0) {
            if (!datadir.empty() || argument.size() == 10)
                throw std::runtime_error("Expected one nonempty explicit fixture datadir");
            datadir = argument.substr(10);
        }
    }
    if (datadir.empty())
        throw std::runtime_error("Expected one nonempty explicit fixture datadir");
    dinero::daemon::DatadirGuard datadir_guard;
    if (!datadir_guard.Acquire(datadir, error))
        throw std::runtime_error(error);
    dinero::daemon::CheckRegtestPowDatadir(datadir, true);
    dinero::daemon::BindRegtestPowProfile(datadir, profile);
    dinero::DaemonApp app;
    if (!app.Init(argc, argv) || !app.Start())
        throw std::runtime_error("Orchard wallet cycle daemon startup failed");
    std::cout << "READY Orchard wallet cycle JSON-RPC daemon" << std::endl;
    // Ordinary lifecycle ownership: the parent asks for a graceful stop over
    // stdin after its HTTP requests finish. No selected/SQLite lock is held.
    std::string command;
    const bool requested = static_cast<bool>(std::getline(std::cin, command));
    app.Stop();
    if (!requested || command != "stop")
        throw std::runtime_error("Expected explicit fixture stop command");
    std::cout << "PASS Orchard wallet cycle daemon stopped normally" << std::endl;
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Orchard wallet cycle fixture: " << error.what() << '\n';
    return 1;
}
