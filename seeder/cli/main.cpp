// dinero-seeder — Phase E of v8 peer-discovery.
//
// Crawls the Dinero P2P network starting from a bootstrap list, probes
// each known peer for reachability, gossips for new addresses via
// getaddr, and emits a healthy-peers list compatible with the format
// of contrib/seeds/seeds_main.txt.
//
// Operator usage (foreground daemon, managed by systemd or equivalent):
//
//   dinero-seeder \
//       --bootstrap=172.93.160.131:20999,173.249.195.59:20999,... \
//       --state=/var/lib/dinero-seeder/peers.state \
//       --output=/var/lib/dinero-seeder/seeds_observed.txt
//
// Or one-shot crawl for ad-hoc audits:
//
//   dinero-seeder --bootstrap=... --duration=300 --output=/tmp/observed.txt

#include "dinero/seeder/crawler.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = [](int /*sig*/) { g_stop.store(true, std::memory_order_release); };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void usage(const char* argv0) {
    std::cerr <<
        "dinero-seeder — Dinero v8 peer-discovery crawler.\n"
        "\n"
        "Usage:\n"
        "  " << argv0 << " --bootstrap=HOST:PORT[,HOST:PORT...] [options]\n"
        "\n"
        "Required:\n"
        "  --bootstrap=LIST   Comma-separated host:port entries to seed from.\n"
        "                     At least one is required; the crawler grows\n"
        "                     the candidate set from there via getaddr.\n"
        "\n"
        "Options:\n"
        "  --state=PATH        Persistent peer-state file. Atomic write +\n"
        "                      load-on-startup. Recommended for long-running\n"
        "                      operators so reachability history survives\n"
        "                      restarts. Default: empty (in-memory only).\n"
        "  --output=PATH       seeds_main.txt-format file of healthy peers.\n"
        "                      Rewritten periodically. Default: empty (no\n"
        "                      file output; stats printed to stdout only).\n"
        "  --duration=SECONDS  Run for N seconds then exit cleanly. 0 = run\n"
        "                      until SIGINT/SIGTERM. Default: 0.\n"
        "  --batch=N           Peers probed per cycle before pausing.\n"
        "                      Default: 8.\n"
        "  --cycle-pause=SEC   Sleep N seconds between batches. Default: 30.\n"
        "  --healthy-window=H  A peer counts as healthy if it last\n"
        "                      handshook within this many hours. Default:\n"
        "                      24.\n"
        "  --min-success-ratio=F\n"
        "                      Minimum success ratio for the output file.\n"
        "                      Default: 0.6.\n"
        "  --port=N            Default port for bootstrap entries without\n"
        "                      one. Default: 20999.\n"
        "  --help              Show this help.\n";
}

bool starts_with(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && std::memcmp(s.data(), prefix, n) == 0;
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    dinero::seeder::CrawlerConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        }
        if (starts_with(arg, "--bootstrap=")) {
            cfg.bootstrap = split_csv(arg.substr(std::strlen("--bootstrap=")));
        } else if (starts_with(arg, "--state=")) {
            cfg.state_path = arg.substr(std::strlen("--state="));
        } else if (starts_with(arg, "--output=")) {
            cfg.output_path = arg.substr(std::strlen("--output="));
        } else if (starts_with(arg, "--duration=")) {
            cfg.total_duration =
                std::chrono::seconds(std::stol(arg.substr(std::strlen("--duration="))));
        } else if (starts_with(arg, "--batch=")) {
            cfg.batch_size = static_cast<size_t>(
                std::stoul(arg.substr(std::strlen("--batch="))));
        } else if (starts_with(arg, "--cycle-pause=")) {
            cfg.cycle_pause =
                std::chrono::seconds(std::stol(arg.substr(std::strlen("--cycle-pause="))));
        } else if (starts_with(arg, "--healthy-window=")) {
            const long h = std::stol(arg.substr(std::strlen("--healthy-window=")));
            cfg.output.healthy_window = std::chrono::seconds(h * 3600);
        } else if (starts_with(arg, "--min-success-ratio=")) {
            cfg.output.min_success_ratio =
                std::stod(arg.substr(std::strlen("--min-success-ratio=")));
        } else if (starts_with(arg, "--port=")) {
            cfg.default_port = static_cast<uint16_t>(
                std::stoul(arg.substr(std::strlen("--port="))));
        } else {
            std::cerr << "unknown option: " << arg << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    if (cfg.bootstrap.empty()) {
        std::cerr << "error: --bootstrap is required (at least one host:port)\n";
        usage(argv[0]);
        return 1;
    }

    install_signal_handlers();

    std::cout << "[seeder] starting "
              << "bootstrap=" << cfg.bootstrap.size() << " entries"
              << " batch=" << cfg.batch_size
              << " cycle_pause=" << cfg.cycle_pause.count() << "s"
              << " duration="
              << (cfg.total_duration.count() == 0 ? std::string("forever")
                                                  : std::to_string(cfg.total_duration.count()) + "s")
              << "\n";

    const size_t probes = dinero::seeder::run_crawler(cfg, g_stop);
    return probes > 0 ? 0 : 2;
}
