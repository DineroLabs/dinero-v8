// Dinero Daemon - Clean Service Architecture
// Week 1 Migration: Using DaemonApp with dependency injection

#include "daemon/daemon_app.h"
#include "daemon/db_repair.h"  // Database repair utility
#include "daemon/doctor/doctor_command.h"  // dinerod doctor subcommand
#include "daemon/datadir_guard.h"
#include "daemon/genesis_guard.h"  // Stale chain data detection
// NOTE: Stratum removed from dinerod - use separate dinero-stratum binary
#include "build/build_identity.h"
#include "consensus/chain_identity.h"
#include "consensus/chainparams.h"
#include "version_config.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <cerrno>
#include <cstring>

#ifndef _WIN32
#include <unistd.h>  // For fork(), setsid(), getppid()
#include <sys/types.h>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/event.h>
#include <sys/time.h>
#endif
#include <signal.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

// Network magic bytes for P2P protocol (must match iOS Protocol.swift)
namespace p2p {
namespace NetworkMagic {
    constexpr uint32_t MAINNET = 0xD1A0C0DEu;
    constexpr uint32_t TESTNET = 0xDAB5BFFAu;
    constexpr uint32_t REGTEST = 0xFABFB5DAu;
}
uint32_t g_magic = NetworkMagic::MAINNET;  // Runtime configurable
}

// Global flag for shutdown signal
static std::atomic<bool> g_shutdown_requested{false};

namespace {

using ShutdownClock = std::chrono::steady_clock;

void LogShutdownPhase(const char* phase,
                      const ShutdownClock::time_point& start,
                      const std::string& detail = {}) {
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(ShutdownClock::now() - start).count();
    std::cout << "[ShutdownPhase] phase=" << phase
              << " elapsed_ms=" << elapsed_ms;
    if (!detail.empty()) {
        std::cout << " detail=\"" << detail << "\"";
    }
    std::cout << std::endl;
}

std::string ExpandTildePath(const std::string& path) {
    if (path.empty() || path[0] != '~') {
        return path;
    }

    const char* home = std::getenv("HOME");
    if (!home) {
        return path;
    }

    if (path.size() == 1) {
        return std::string(home);
    }

    if (path[1] == '/') {
        return std::string(home) + path.substr(1);
    }

    return path;
}

std::string ResolveStartupDataDir(int argc, char* argv[], bool use_testnet, bool use_regtest) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--datadir=", 0) == 0) {
            return ExpandTildePath(arg.substr(10));
        }
        if (arg == "--datadir" && i + 1 < argc) {
            return ExpandTildePath(argv[i + 1]);
        }
    }

    const char* home = std::getenv("HOME");
    if (!home) {
        return {};
    }

    if (use_regtest) {
        return std::string(home) + "/.dinero/regtest";
    }
    if (use_testnet) {
        return std::string(home) + "/.dinero/testnet";
    }
    return std::string(home) + "/.dinero";
}

long ParseEmbeddedParentPid(const std::string& value) {
    try {
        size_t consumed = 0;
        long pid = std::stol(value, &consumed);
        if (consumed == value.size() && pid > 1) {
            return pid;
        }
    } catch (...) {
    }
    return 0;
}

#ifndef _WIN32
void RequestShutdownFromParentMonitor(const char* reason) {
    std::cout << "[EmbeddedParent] " << reason << "; initiating shutdown\n";
    g_shutdown_requested.store(true);
}

void StartEmbeddedParentMonitor(pid_t parent_pid) {
    if (parent_pid <= 1) {
        return;
    }

#if defined(__linux__)
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
        std::cerr << "[EmbeddedParent] Warning: PR_SET_PDEATHSIG failed: "
                  << std::strerror(errno) << "\n";
    }
    if (getppid() != parent_pid) {
        RequestShutdownFromParentMonitor("parent exited before monitor armed");
        return;
    }
    std::cout << "[EmbeddedParent] Linux parent-death SIGTERM armed for pid "
              << parent_pid << "\n";
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    std::thread([parent_pid]() {
        int kq = kqueue();
        if (kq == -1) {
            std::cerr << "[EmbeddedParent] Warning: kqueue failed: "
                      << std::strerror(errno) << "; falling back to polling\n";
        } else {
            struct kevent event;
            EV_SET(&event, static_cast<uintptr_t>(parent_pid), EVFILT_PROC,
                   EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
            if (kevent(kq, &event, 1, nullptr, 0, nullptr) == 0) {
                struct kevent fired;
                if (kevent(kq, nullptr, 0, &fired, 1, nullptr) > 0) {
                    close(kq);
                    RequestShutdownFromParentMonitor("parent process exited");
                    return;
                }
            } else {
                std::cerr << "[EmbeddedParent] Warning: kevent registration failed: "
                          << std::strerror(errno) << "; falling back to polling\n";
            }
            close(kq);
        }

        while (!g_shutdown_requested.load()) {
            if (kill(parent_pid, 0) != 0 && errno == ESRCH) {
                RequestShutdownFromParentMonitor("parent process disappeared");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
    std::cout << "[EmbeddedParent] kqueue parent monitor armed for pid "
              << parent_pid << "\n";
#else
    std::thread([parent_pid]() {
        while (!g_shutdown_requested.load()) {
            if (kill(parent_pid, 0) != 0 && errno == ESRCH) {
                RequestShutdownFromParentMonitor("parent process disappeared");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
    std::cout << "[EmbeddedParent] polling parent monitor armed for pid "
              << parent_pid << "\n";
#endif
}
#endif

}  // namespace

// Signal handler for clean shutdown
void signal_handler(int signal) {
    std::cout << "\n[Signal] Received signal " << signal << ", initiating shutdown...\n";
    g_shutdown_requested = true;
}

// Print usage information
void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --help              Show this help message\n";
    std::cout << "  --version           Show version information\n";
    std::cout << "  --datadir=<dir>     Specify data directory\n";
    std::cout << "  --rpcport=<port>           RPC server port (default: 20998)\n";
    std::cout << "  --p2pport=<port>           P2P network port (default: 20999)\n";
    std::cout << "  --wallet-socket-port=<port> Wallet socket server port (default: 50051)\n";
    std::cout << "  --regtest                  Use regression test network\n";
    std::cout << "  --testnet           Use test network\n";
    std::cout << "  -daemon             Run in background (Unix only)\n";
    std::cout << "  --embedded-parent-pid=<pid>  Internal: shut down when GUI parent exits\n";
    std::cout << "\n";
    std::cout << "Recovery Options:\n";
    std::cout << "  --reindex                      Rebuild block index and UTXO set from blk*.dat files\n";
    std::cout << "  --reindex-chainstate           Rebuild UTXO set only (faster, keeps block index)\n";
    std::cout << "  --repair-db                    Scan databases for issues (read-only report)\n";
    std::cout << "  --repair-db --confirm          Scan AND repair detected issues\n";
    std::cout << "  --wipe-stale-chain             Wipe incompatible chain data (backs up wallet first)\n";
    std::cout << "\n";
    std::cout << "Diagnostics:\n";
    std::cout << "  doctor                         Run node health checks (read-only)\n";
    std::cout << "  doctor --deep                  Run extended health checks\n";
    std::cout << "  doctor --help                  Show doctor usage\n";
    std::cout << "\n";
    std::cout << "Storage & Pruning:\n";
    std::cout << "  -prune=<N>                     Prune old blocks, keep N recent (min 288)\n";
    std::cout << "  -prune=0                       Headers-only mode (no block data)\n";
    std::cout << "  -archival                      Full archival node (keep all blocks, default)\n";
    std::cout << "  --p2p.offline=1                Disable peer bootstrap/listeners for zero-peer local replay\n";
    std::cout << "\n";
    std::cout << "Stratum Mining:\n";
    std::cout << "  Stratum is a separate binary: dinero-stratum\n";
    std::cout << "  Example: dinero-stratum --rpcport=20998 --stratumport=3333\n";
    std::cout << "\n";
    std::cout << "Pool Accounting (disabled by default):\n";
    std::cout << "  --pool.accounting.enable=1  Enable pool.* accounting RPC + pool DB\n";
    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    // Handle "doctor" subcommand immediately (before general arg parsing)
    // This ensures doctor gets its own --help, --deep, etc. without interference
    if (argc >= 2 && std::string(argv[1]) == "doctor") {
        return dinero::doctor::RunDoctorCommand(argc, argv);
    }

    // Banner is printed AFTER daemonization (if -daemon flag used)
    // to avoid duplicate output from fork()

    // Parse command line arguments
    bool show_help = false;
    bool show_version = false;
    bool use_testnet = false;
    bool use_regtest = false;
    bool run_as_daemon = false;
    bool do_reindex = false;
    bool do_reindex_chainstate = false;
    bool do_repair_db = false;
    bool repair_confirm = false;
    bool wipe_stale_chain = false;
    long embedded_parent_pid = 0;
    uint16_t wallet_socket_port = 0;  // 0 = use default (will be set from env or default)

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            show_help = true;
        } else if (arg == "--version" || arg == "-v") {
            show_version = true;
        } else if (arg == "--testnet" || arg == "-testnet") {
            use_testnet = true;
        } else if (arg == "--regtest" || arg == "-regtest") {
            use_regtest = true;
        } else if (arg == "-daemon" || arg == "--daemon") {
            run_as_daemon = true;
        } else if (arg == "--reindex") {
            do_reindex = true;
        } else if (arg == "--reindex-chainstate") {
            do_reindex_chainstate = true;
        } else if (arg == "--repair-db") {
            do_repair_db = true;
        } else if (arg == "--confirm") {
            repair_confirm = true;
        } else if (arg == "--wipe-stale-chain" || arg == "--auto-wipe-stale-chain") {
            wipe_stale_chain = true;
        } else if (arg.find("--embedded-parent-pid=") == 0) {
            embedded_parent_pid = ParseEmbeddedParentPid(arg.substr(22));
        } else if (arg.find("--wallet-socket-port=") == 0) {
            std::string port_str = arg.substr(21);  // Skip "--wallet-socket-port="
            try {
                int port_val = std::stoi(port_str);
                if (port_val > 0 && port_val <= 65535) {
                    wallet_socket_port = static_cast<uint16_t>(port_val);
                } else {
                    std::cerr << "Error: Invalid port number: " << port_str << " (must be 1-65535)\n";
                    return 1;
                }
            } catch (const std::exception& e) {
                std::cerr << "Error: Invalid port number: " << port_str << "\n";
                return 1;
            }
        }
    }

    // Check environment variable if CLI flag wasn't provided
    if (wallet_socket_port == 0) {
        const char* env_port = std::getenv("DINERO_WALLET_SOCKET_PORT");
        if (env_port != nullptr) {
            try {
                int port_val = std::stoi(env_port);
                if (port_val > 0 && port_val <= 65535) {
                    wallet_socket_port = static_cast<uint16_t>(port_val);
                    std::cout << "Using wallet socket port from DINERO_WALLET_SOCKET_PORT: " << wallet_socket_port << "\n";
                } else {
                    std::cerr << "Error: Invalid DINERO_WALLET_SOCKET_PORT value: " << env_port << " (must be 1-65535)\n";
                    return 1;
                }
            } catch (const std::exception& e) {
                std::cerr << "Error: Invalid DINERO_WALLET_SOCKET_PORT value: " << env_port << "\n";
                return 1;
            }
        }
    }

    // Use default port if neither CLI nor env var specified
    if (wallet_socket_port == 0) {
        wallet_socket_port = 50051;  // Default
    }

    if (show_help) {
        print_usage(argv[0]);
        return 0;
    }

    if (show_version) {
        std::cout << dinero::build::FormatIdentityMultiline();
        return 0;
    }

    if (embedded_parent_pid > 0 && run_as_daemon) {
        std::cerr << "[EmbeddedParent] --embedded-parent-pid conflicts with --daemon; "
                  << "running in foreground child mode so parent-death cleanup works.\n";
        run_as_daemon = false;
    }

    // Handle --repair-db before daemon startup
    if (do_repair_db) {
        // Determine datadir from command line or default
        std::string repair_datadir;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg.find("--datadir=") == 0) {
                repair_datadir = arg.substr(10);
                break;
            }
        }

        // Use default datadir if not specified
        if (repair_datadir.empty()) {
            const char* home = std::getenv("HOME");
            if (home) {
                if (use_regtest) {
                    repair_datadir = std::string(home) + "/.dinero/regtest";
                } else if (use_testnet) {
                    repair_datadir = std::string(home) + "/.dinero/testnet";
                } else {
                    repair_datadir = std::string(home) + "/.dinero";
                }
            } else {
                std::cerr << "Error: Cannot determine data directory. Use --datadir=<path>\n";
                return 1;
            }
        }

        // Run repair utility and exit
        return dinero::repair::run_repair_db(repair_datadir, repair_confirm);
    }

    // Daemonize if requested (Unix only)
#ifndef _WIN32
    if (run_as_daemon) {
        // Flush output before forking to prevent buffer duplication
        std::cout.flush();
        std::cerr.flush();
        fflush(stdout);
        fflush(stderr);

        // Fork to background
        pid_t pid = fork();
        if (pid < 0) {
            std::cerr << "[FATAL] Failed to fork daemon process\n";
            return 1;
        }
        if (pid > 0) {
            // Parent process: exit immediately without unwinding C++ static
            // objects. Returning from main() after fork can run destructors for
            // thread-owning globals in a process that no longer has the original
            // thread state, which is exactly how we ended up with
            // std::thread::join ESRCH on daemon restart.
            _exit(0);
        }

        // Child process: become session leader
        if (setsid() < 0) {
            std::cerr << "[FATAL] Failed to create new session\n";
            return 1;
        }

        // Fork again to prevent acquiring a controlling terminal
        pid = fork();
        if (pid < 0) {
            std::cerr << "[FATAL] Failed to fork daemon process (second fork)\n";
            return 1;
        }
        if (pid > 0) {
            // First child: same rule as the original parent above. Do not run
            // normal process teardown in the intermediate daemonization
            // process.
            _exit(0);
        }

        // Second child (actual daemon): redirect stdin/stdout/stderr
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
    }
#endif

#ifndef _WIN32
    if (embedded_parent_pid > 0) {
        StartEmbeddedParentMonitor(static_cast<pid_t>(embedded_parent_pid));
    }
#endif

    const std::string startup_datadir = ResolveStartupDataDir(argc, argv, use_testnet, use_regtest);

    dinero::daemon::DatadirGuard datadir_guard;
    if (!startup_datadir.empty()) {
        std::string datadir_guard_error;
        if (!datadir_guard.Acquire(startup_datadir, datadir_guard_error)) {
            std::cerr << "[FATAL] " << datadir_guard_error << "\n";
            return 1;
        }
    }

    // ══════════════════════════════════════════════════════════════════════
    // Genesis Guard: detect stale/incompatible chain data before full init
    //
    // Run this AFTER daemonization. Opening and closing RocksDB before the
    // double-fork path caused restart-only crashes on macOS in daemon mode.
    // Non-daemon launches still preserve the original exit-code behavior.
    // ══════════════════════════════════════════════════════════════════════
    {
        if (!startup_datadir.empty()) {
            auto guard_result = dinero::checkGenesisGuard(startup_datadir);

            if (guard_result == dinero::GenesisGuardResult::MISMATCH) {
                if (wipe_stale_chain) {
                    std::cout << "[GenesisGuard] Wiping stale chain data...\n";
                    if (!dinero::wipeStaleChainData(startup_datadir)) {
                        std::cerr << "[GenesisGuard] Failed to wipe stale chain data.\n";
                        return 1;
                    }
                    std::cout << "[GenesisGuard] Chain data wiped. Proceeding with fresh sync.\n";
                } else {
                    // Exit with code 10 so dinero-qt can detect and offer wipe
                    // in foreground launches. In daemon mode the parent has
                    // already exited; the final daemon child still exits early
                    // here rather than starting on incompatible state.
                    return 10;
                }
            } else if (guard_result == dinero::GenesisGuardResult::READ_ERROR) {
                std::cerr << "[GenesisGuard] Warning: could not read chain database. Proceeding anyway.\n";
            }
            // OK and NO_DB: proceed normally
        }
    }

    // Print version banner (after daemonization, so only final process prints)
    // Note: In daemon mode, stdout is now /dev/null, but we print anyway for consistency
    if (!run_as_daemon) {
        auto build_identity = dinero::build::CurrentIdentity();
        std::cout << build_identity.component << " " << build_identity.version << "\n";
        std::cout << "Built: " << build_identity.build_time << "\n";
        std::cout << "\n";
        std::cout << dinero::consensus::kGenesisMotto << "\n";
        std::cout << "\n";
    }

    // Install signal handlers
#ifndef _WIN32
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  // Ignore broken pipe
#endif

    // Select network parameters BEFORE initializing services
    dinero::Chain chain = dinero::Chain::MAINNET;  // Default to mainnet
    if (use_regtest) {
        chain = dinero::Chain::REGTEST;
        std::cout << "[Network] Using regtest network\n";
    } else if (use_testnet) {
        chain = dinero::Chain::TESTNET;
        std::cout << "[Network] Using testnet network\n";
    } else {
        std::cout << "[Network] Using mainnet network\n";
    }

    dinero::SelectParams(chain);
    std::cout << "[Network] Chain parameters initialized: " << dinero::ChainToString(chain) << "\n";

    // Initialize P2P network magic bytes based on selected chain
    if (chain == dinero::Chain::REGTEST) {
        p2p::g_magic = p2p::NetworkMagic::REGTEST;
        std::cout << "[P2P] Network magic: 0x" << std::hex << p2p::g_magic << std::dec << " (regtest)\n";
    } else if (chain == dinero::Chain::TESTNET) {
        p2p::g_magic = p2p::NetworkMagic::TESTNET;
        std::cout << "[P2P] Network magic: 0x" << std::hex << p2p::g_magic << std::dec << " (testnet)\n";
    } else {
        p2p::g_magic = p2p::NetworkMagic::MAINNET;
        std::cout << "[P2P] Network magic: 0x" << std::hex << p2p::g_magic << std::dec << " (mainnet)\n";
    }
    std::cout << "\n";

    std::cout << "[DaemonApp] Starting Dinero daemon with service architecture...\n";
    std::cout << "\n";

    // Create and initialize the daemon application
    dinero::DaemonApp app;

    app.GetContext().request_shutdown = []() {
        g_shutdown_requested.store(true);
    };

    // Initialize with command-line arguments (DaemonApp will parse and inject into ConfigService)
    if (!app.Init(argc, argv)) {
        std::cerr << "[FATAL] Failed to initialize daemon\n";
        return 1;
    }

    // Init() resets DaemonContext, so apply command-line/env config afterward.
    app.GetContext().wallet_socket_port = wallet_socket_port;

    std::cout << "\n";

    if (!app.Start()) {
        std::cerr << "[FATAL] Failed to start daemon\n";
        app.Stop();
        return 2;
    }

    // ✅ BRIDGE PATTERN STATUS: Week 5 - Wallet bridge removed
    // - ChainstateService::Init() sets dinero::legacy::g_chain_db_direct() and dinero::legacy::g_utxo_set_direct() (still required)
    // - WalletService::Init() NO LONGER sets dinero::legacy::g_wallet_manager() (removed Week 5)
    // - P2PService::Init() NO LONGER sets dinero::legacy::g_peer_manager() (removed Week 4)
    // - Services clear globals in Stop() for clean shutdown
    // 
    // NOTE: Chainstate bridge still active for legacy code compatibility
    // All active RPC handlers now use ExecutionContext.daemon
    std::cout << "[Bridge] Chainstate bridge active (legacy compatibility)\n";

    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "Dinero daemon is running\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n";
    std::cout << "\n";

    // Main event loop - wait for shutdown signal
    while (!g_shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n";
    const auto shutdown_start = ShutdownClock::now();
    LogShutdownPhase("interrupting", shutdown_start, "main loop observed shutdown request");
    std::cout << "[Shutdown] Stopping services...\n";

    // Watchdog: if Stop() hangs for >30s, force exit
    std::thread watchdog([] {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        std::cerr << "[Shutdown] WATCHDOG: Shutdown stuck for >30s, forcing exit\n";
        _exit(1);
    });
    watchdog.detach();

    app.Stop();

    LogShutdownPhase("shutdown_complete", shutdown_start, "main returning cleanly");
    std::cout << "[Shutdown] Clean shutdown complete\n";
    return 0;
}
