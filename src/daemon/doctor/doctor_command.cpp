// doctor_command.cpp - CLI entry point for dinerod doctor
#include "daemon/doctor/doctor_command.h"
#include "daemon/doctor/doctor_context.h"
#include "daemon/doctor/doctor_fixer.h"
#include "daemon/doctor/doctor_json_emitter.h"
#include "daemon/doctor/doctor_registry.h"
#include "daemon/doctor/doctor_renderer.h"
#include "daemon/doctor/doctor_runner.h"

#include "version_config.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace dinero {
namespace doctor {

static void PrintDoctorUsage() {
    std::cout << "Usage: dinerod doctor [options]\n\n";
    std::cout << "Modes:\n";
    std::cout << "  (default)          Quick mode (target < 30s)\n";
    std::cout << "  --deep             Deep mode (target < 10 minutes)\n";
    std::cout << "\n";
    std::cout << "Output:\n";
    std::cout << "  --json             Machine-readable JSON output\n";
    std::cout << "  --output <path>    Write JSON to file (implies --json)\n";
    std::cout << "\n";
    std::cout << "Inspection:\n";
    std::cout << "  --list-checks      List all registered checks\n";
    std::cout << "  --explain <id>     Show details for a specific check\n";
    std::cout << "  --checks <glob>    Filter checks by pattern (comma-separated)\n";
    std::cout << "\n";
    std::cout << "Fixes (read-only by default):\n";
    std::cout << "  --apply-safe-fixes             Enable safe auto-fixes\n";
    std::cout << "  --fix <id>                     Apply specific fix (repeatable)\n";
    std::cout << "  --yes-i-know-what-im-doing     Apply all eligible fixes\n";
    std::cout << "\n";
    std::cout << "Network:\n";
    std::cout << "  --regtest          Use regtest network data directory\n";
    std::cout << "  --testnet          Use testnet network data directory\n";
    std::cout << "  --datadir=<dir>    Specify data directory\n";
    std::cout << "\n";
    std::cout << "Exit codes:\n";
    std::cout << "  0  Healthy\n";
    std::cout << "  1  Warnings only\n";
    std::cout << "  2  Critical findings\n";
    std::cout << "  3  Doctor internal error\n";
}

static DoctorConfig ParseDoctorArgs(int argc, char* argv[],
                                     std::string& datadir,
                                     std::string& network) {
    DoctorConfig config;
    datadir.clear();
    network = "mainnet";

    // argv[0] = "dinerod", argv[1] = "doctor", doctor args start at argv[2]
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            PrintDoctorUsage();
            std::exit(0);
        } else if (arg == "--deep") {
            config.mode = RunMode::DEEP;
        } else if (arg == "--json") {
            config.json_output = true;
        } else if (arg == "--output" && i + 1 < argc) {
            config.json_output = true;
            config.json_output_path = argv[++i];
        } else if (arg == "--list-checks") {
            config.list_checks = true;
        } else if (arg == "--explain" && i + 1 < argc) {
            config.explain_check_id = argv[++i];
        } else if (arg == "--checks" && i + 1 < argc) {
            // Parse comma-separated glob patterns
            std::string patterns = argv[++i];
            std::istringstream ss(patterns);
            std::string token;
            while (std::getline(ss, token, ',')) {
                if (!token.empty()) {
                    config.check_filter.push_back(token);
                }
            }
        } else if (arg == "--apply-safe-fixes") {
            config.apply_safe_fixes = true;
        } else if (arg == "--fix" && i + 1 < argc) {
            config.fix_ids.push_back(argv[++i]);
        } else if (arg == "--yes-i-know-what-im-doing") {
            config.force_all_fixes = true;
        } else if (arg == "--regtest" || arg == "-regtest") {
            network = "regtest";
        } else if (arg == "--testnet" || arg == "-testnet") {
            network = "testnet";
        } else if (arg.find("--datadir=") == 0) {
            datadir = arg.substr(10);
        } else {
            std::cerr << "dinerod doctor: unknown option: " << arg << "\n";
            std::cerr << "Try 'dinerod doctor --help' for usage.\n";
            std::exit(3);
        }
    }

    return config;
}

static std::string ResolveDataDir(const std::string& explicit_dir,
                                   const std::string& network) {
    if (!explicit_dir.empty()) {
        return explicit_dir;
    }

    const char* home = std::getenv("HOME");
    if (!home) {
        std::cerr << "Error: Cannot determine HOME directory. Use --datadir=<path>\n";
        std::exit(3);
    }

    std::string base = std::string(home) + "/.dinero";
    if (network == "regtest") {
        return base + "/regtest";
    } else if (network == "testnet") {
        return base + "/testnet";
    }
    return base;
}

int RunDoctorCommand(int argc, char* argv[]) {
    std::string datadir;
    std::string network;
    DoctorConfig config = ParseDoctorArgs(argc, argv, datadir, network);

    // Resolve data directory
    datadir = ResolveDataDir(datadir, network);

    // Initialize registry
    DoctorRegistry& registry = GetDoctorRegistry();
    RegisterV1Checks(registry);

    // Handle --list-checks
    if (config.list_checks) {
        DoctorRenderer::RenderCheckList(std::cout, registry);
        return 0;
    }

    // Handle --explain
    if (!config.explain_check_id.empty()) {
        const auto* check = registry.Find(config.explain_check_id);
        if (!check) {
            std::cerr << "Unknown check: " << config.explain_check_id << "\n";
            return 3;
        }
        DoctorRenderer::RenderExplain(std::cout, *check);
        return 0;
    }

    // Build context
    DoctorContext ctx(datadir, network);
    ctx.SetMode(config.mode);

    // Set node version from build macros
#ifdef DINERO_CLI_VERSION
    ctx.SetNodeVersion(DINERO_CLI_VERSION);
#else
    ctx.SetNodeVersion("unknown");
#endif

    // Run checks
    DoctorRunner runner;
    DoctorRunResult run = runner.Run(ctx, registry, config.check_filter);

    // ── Fix flow ──────────────────────────────────────────────────────
    // --fix <id> alone is not enough — require --apply-safe-fixes as gate
    if (!config.fix_ids.empty() && !config.apply_safe_fixes && !config.force_all_fixes) {
        std::cerr << "Error: --fix requires --apply-safe-fixes (or --yes-i-know-what-im-doing)\n";
        std::cerr << "This prevents accidental mutation. Use:\n";
        std::cerr << "  dinerod doctor --apply-safe-fixes --fix " << config.fix_ids[0] << "\n";
        return 3;
    }

    bool fix_requested = config.apply_safe_fixes ||
                         config.force_all_fixes;

    if (fix_requested) {
        auto candidates = DoctorFixer::CollectEligibleFixes(run, config);

        if (candidates.empty()) {
            if (!config.json_output) {
                DoctorRenderer::RenderResults(std::cout, run);
                std::cout << "\nNo eligible fixes found.\n";
            }
        } else {
            // Preview
            if (!config.json_output) {
                DoctorRenderer::RenderResults(std::cout, run);
                DoctorFixer::PreviewFixes(std::cout, candidates);
            }

            // Apply
            FixSession session = DoctorFixer::ApplyFixes(candidates, ctx);

            if (!config.json_output) {
                DoctorFixer::RenderFixResults(std::cout, session);
            }

            // Re-run affected checks to verify fixes
            if (!session.affected_check_ids.empty() && session.succeeded > 0) {
                if (!config.json_output) {
                    std::cout << "\n── Re-checking affected checks ─────────────────\n\n";
                }

                DoctorRunner recheck_runner;
                DoctorRunResult recheck = recheck_runner.Run(ctx, registry, session.affected_check_ids);

                if (!config.json_output) {
                    DoctorRenderer::RenderResults(std::cout, recheck);
                }

                // Use recheck exit code if it's better than the original
                if (static_cast<int>(recheck.exit_code) < static_cast<int>(run.exit_code)) {
                    run.exit_code = recheck.exit_code;
                }
            }
        }

        // JSON output for fix mode
        if (config.json_output) {
            if (!config.json_output_path.empty()) {
                if (!DoctorJsonEmitter::EmitToFile(config.json_output_path, run)) {
                    std::cerr << "Error: Failed to write JSON to " << config.json_output_path << "\n";
                    return 3;
                }
                std::cout << "Results written to " << config.json_output_path << "\n";
            } else {
                DoctorJsonEmitter::Emit(std::cout, run);
            }
        }

        return static_cast<int>(run.exit_code);
    }

    // Output
    if (config.json_output) {
        if (!config.json_output_path.empty()) {
            if (!DoctorJsonEmitter::EmitToFile(config.json_output_path, run)) {
                std::cerr << "Error: Failed to write JSON to " << config.json_output_path << "\n";
                return 3;
            }
            std::cout << "Results written to " << config.json_output_path << "\n";
        } else {
            DoctorJsonEmitter::Emit(std::cout, run);
        }
    } else {
        DoctorRenderer::RenderResults(std::cout, run);
    }

    return static_cast<int>(run.exit_code);
}

} // namespace doctor
} // namespace dinero
