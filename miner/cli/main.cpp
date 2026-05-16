#include "solo_miner/miner.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <iomanip>
#include <exception>

using namespace dinero::solo;

// Global miner for signal handling
static std::atomic<bool> g_shutdown{false};
static SoloMiner* g_miner = nullptr;

void signalHandler(int signal) {
    std::cout << "\n\nReceived signal " << signal << ", shutting down...\n";
    g_shutdown = true;
    if (g_miner) {
        g_miner->stop();
    }
}

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "\n"
              << "Options:\n"
              << "  --url <url>         RPC URL (default: http://127.0.0.1:20998)\n"
              << "  --cookie <path>     Path to .cookie file for authentication\n"
              << "  --user <user>       RPC username (alternative to cookie)\n"
              << "  --pass <pass>       RPC password (alternative to cookie)\n"
              << "  --address <addr>    Payout address (REQUIRED)\n"
              << "  --threads <n>       Number of mining threads (default: auto)\n"
              << "  --backend <name>    Mining backend: auto, cpu, metal, cuda, opencl (default: auto)\n"
              << "  --benchmark [secs]  Pure throughput measurement (no daemon, no submit).\n"
              << "                      Runs the selected --backend for <secs> seconds (default 30)\n"
              << "                      against a synthetic header + near-impossible target.\n"
              << "                      Useful for closing the v8 release-gate gap of measuring\n"
              << "                      real GPU MH/s without needing a high-difficulty chain.\n"
              << "  --version           Show version/build information\n"
              << "  --help              Show this help message\n"
              << "\n"
              << "Example:\n"
              << "  " << program << " --address din1q... --url http://127.0.0.1:20998\n"
              << std::endl;
}

std::string formatHashrate(double hashrate) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);

    if (hashrate >= 1e12) {
        ss << (hashrate / 1e12) << " TH/s";
    } else if (hashrate >= 1e9) {
        ss << (hashrate / 1e9) << " GH/s";
    } else if (hashrate >= 1e6) {
        ss << (hashrate / 1e6) << " MH/s";
    } else if (hashrate >= 1e3) {
        ss << (hashrate / 1e3) << " KH/s";
    } else {
        ss << hashrate << " H/s";
    }

    return ss.str();
}

int main(int argc, char* argv[]) {
    MinerConfig config;
    bool benchmark_mode = false;
    double benchmark_seconds = 30.0;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--version") {
            std::cout << FormatBuildIdentity();
            return 0;
        } else if (arg == "--url" && i + 1 < argc) {
            config.rpc_url = argv[++i];
        } else if (arg == "--cookie" && i + 1 < argc) {
            config.cookie_path = argv[++i];
        } else if (arg == "--user" && i + 1 < argc) {
            config.rpc_user = argv[++i];
        } else if (arg == "--pass" && i + 1 < argc) {
            config.rpc_password = argv[++i];
        } else if (arg == "--address" && i + 1 < argc) {
            config.payout_address = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            config.threads = std::stoi(argv[++i]);
        } else if (arg == "--backend" && i + 1 < argc) {
            try {
                config.backend = minerBackendFromString(argv[++i]);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n\n";
                printUsage(argv[0]);
                return 1;
            }
        } else if (arg == "--benchmark") {
            benchmark_mode = true;
            // Optional duration argument. If the next arg parses as a
            // positive number, consume it; otherwise default to 30s.
            if (i + 1 < argc) {
                try {
                    double parsed = std::stod(argv[i + 1]);
                    if (parsed > 0.0) {
                        benchmark_seconds = parsed;
                        ++i;
                    }
                } catch (const std::exception&) {
                    // Not a number — leave default and let next iteration
                    // re-process the arg.
                }
            }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    // Benchmark mode short-circuits the normal mining flow. No daemon
    // required, no payout address required.
    if (benchmark_mode) {
        SoloMiner miner;
        SoloMiner::BenchmarkResult result;
        std::cout << "Running benchmark for " << benchmark_seconds
                  << "s on backend "
                  << minerBackendToString(config.backend) << "...\n";
        if (!miner.benchmark(config.backend, benchmark_seconds, result)) {
            std::cerr << "Benchmark failed: " << miner.getLastError() << "\n";
            return 1;
        }
        std::cout << "\nBenchmark result:\n"
                  << "  backend:        " << result.backend_label << "\n"
                  << "  device:         " << result.device_name << "\n"
                  << "  duration:       " << std::fixed << std::setprecision(2)
                  << result.duration_seconds << " s\n"
                  << "  total hashes:   " << result.total_hashes << "\n"
                  << "  hashrate:       " << result.hashrate_mhs << " MH/s\n";
        return 0;
    }

    // Validate required parameters
    if (config.payout_address.empty()) {
        std::cerr << "Error: --address is required\n\n";
        printUsage(argv[0]);
        return 1;
    }

    // Print banner
    std::cout << "\n"
              << "═══════════════════════════════════════════════════════════\n"
              << "  Dinero Solo Miner " << GetBuildIdentity().version << "\n"
              << "  Direct RPC Mining (no stratum)\n"
              << "═══════════════════════════════════════════════════════════\n"
              << "\n"
              << "  RPC URL:    " << config.rpc_url << "\n"
              << "  Address:    " << config.payout_address << "\n"
              << "  Threads:    " << (config.threads > 0 ? std::to_string(config.threads) : "auto") << "\n"
              << "  Backend:    " << minerBackendToString(config.backend) << "\n"
              << "\n"
              << "═══════════════════════════════════════════════════════════\n"
              << std::endl;

    // Set up signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Create miner
    SoloMiner miner;
    g_miner = &miner;

    // Set up callbacks
    miner.setHashrateCallback([](double hashrate) {
        std::cout << "\r⚡ Hashrate: " << formatHashrate(hashrate) << "    " << std::flush;
    });

    miner.setBlockFoundCallback([](const dinero::solo::BlockFoundInfo& info) {
        std::cout << "\n\n"
                  << "╔═══════════════════════════════════════════════════════════╗\n"
                  << "║  🎉 BLOCK FOUND!                                          ║\n"
                  << "╠═══════════════════════════════════════════════════════════╣\n"
                  << "║  Height: " << std::setw(10) << info.height << "                                     ║\n"
                  << "║  Difficulty: 0x" << std::hex << std::setw(8) << std::setfill('0')
                  << info.nbits << std::setfill(' ') << std::dec << "                              ║\n"
                  << "║  Hash:   " << info.block_hash.substr(0, 16) << "...                       ║\n"
                  << "╚═══════════════════════════════════════════════════════════╝\n"
                  << std::endl;
    });

    miner.setErrorCallback([](const std::string& error) {
        std::cerr << "\n❌ Error: " << error << std::endl;
    });

    miner.setTemplateCallback([](uint32_t height, uint32_t difficulty) {
        std::cout << "\n📦 New template: height=" << height
                  << " difficulty=0x" << std::hex << difficulty << std::dec << std::endl;
    });

    // Start mining
    std::cout << "🚀 Starting miner...\n" << std::endl;

    if (!miner.start(config)) {
        std::cerr << "❌ Failed to start miner: " << miner.getLastError() << std::endl;
        return 1;
    }

    std::cout << "✅ Miner started successfully\n" << std::endl;
    std::cout << "Backend active: " << minerBackendToString(miner.getStats().active_backend) << "\n" << std::endl;

    // Wait for shutdown
    while (!g_shutdown && miner.isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Stop miner
    miner.stop();

    // Print final stats
    auto stats = miner.getStats();
    std::cout << "\n"
              << "═══════════════════════════════════════════════════════════\n"
              << "  Final Statistics\n"
              << "═══════════════════════════════════════════════════════════\n"
              << "  Total hashes:     " << stats.hashes_total << "\n"
              << "  Blocks found:     " << stats.blocks_found << "\n"
              << "  Blocks accepted:  " << stats.blocks_accepted << "\n"
              << "  Blocks rejected:  " << stats.blocks_rejected << "\n"
              << "═══════════════════════════════════════════════════════════\n"
              << std::endl;

    g_miner = nullptr;
    return 0;
}
