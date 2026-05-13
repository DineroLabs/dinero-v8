// SPDX-License-Identifier: MIT
// Dinero CLI v0.6.0 - Production-grade control cockpit
// Build: link with Boost::system and JsonCpp (vendored)

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <set>
#include <iomanip>
#include <termios.h>
#include "cli/ConnectionResolver.h"
#include "cli/NodeinfoValidator.h"
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <cstdlib>

#include "compat/jsoncpp_compat.h"

#include "cli/overrides.h"
#include "cli/security.h"
#include "cli/print.h"
#include "cli/version.h"
#include "cli/schema_validation.h"
#include "cli/health_client.h"
#include "cli/url_utils.h"

// Boost.Beast HTTP (header-only; needs Boost::system)
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
namespace fs = std::filesystem;

// Exit codes (sysexits-style)
#ifndef EX_USAGE
#define EX_USAGE 64
#endif
#ifndef EX_NOPERM
#define EX_NOPERM 77
#endif

static const int EXIT_OK = 0;
static const int EXIT_ERROR = 1;
static const int EXIT_NOT_READY = 2;
static const int EXIT_SECURITY_ERROR = 3;
constexpr int EXIT_USAGE = EX_USAGE;   // 64
constexpr int EXIT_AUTH  = EX_NOPERM;  // 77
static constexpr int EXIT_NETWORK_MISMATCH = 10;
static constexpr int EXIT_SERVICE_UNAVAILABLE = 69;

// -------------------------------
// Small utils
// -------------------------------

// URL-encode wallet segment
static std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out; out.reserve(s.size()*3);
    for (unsigned char c : s) {
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out.push_back(char(c));
        else { out.push_back('%'); out.push_back(hex[(c>>4)&0xF]); out.push_back(hex[c&0xF]); }
    }
    return out;
}

// Scope base URL to /wallet/<name>
static std::string withWalletScope(const std::string& baseUrl, const std::string& wallet) {
    if (wallet.empty()) return baseUrl;
    std::string u = baseUrl;
    while (!u.empty() && u.back()=='/') u.pop_back();
    return u + "/wallet/" + urlEncode(wallet);
}

// Create print context for commands
static dinero::cli::PrintCtx makePrintCtx(const std::string& network,
                                          const dinero::cli::CliOverrides& flags,
                                          const std::string& effectiveUrl,
                                          const std::string& command,
                                          const std::string& walletName) {
    dinero::cli::PrintCtx ctx;
    ctx.fmt = flags.format;
    ctx.schema = flags.jsonSchema;
    ctx.command = command;
    ctx.network = network;
    ctx.rpcUrl = effectiveUrl;
    if (!walletName.empty()) ctx.wallet = walletName;
    return ctx;
}


// Small helper (near your args/env parsing helpers):
static inline std::string trim(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c){ return !std::isspace(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char c){ return !std::isspace(c); }).base(), s.end());
    return s;
}

static inline bool envTruthy(const char* k) {
    const char* v = std::getenv(k);
    return v && (std::string(v)=="1" || std::string(v)=="true");
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::in | std::ios::binary);
    if (!f) throw std::runtime_error("Failed to read file: " + p.string());
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

static bool file_exists(const fs::path& p) {
    std::error_code ec; return fs::exists(p, ec);
}

// Very small Base64 (RFC 4648) for cookie auth
static std::string base64_encode(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve(((in.size()+2)/3)*4);
    int val=0, valb=-6;
    for (unsigned char c: in) { val = (val<<8) + c; valb += 8;
        while (valb >= 0) { out.push_back(tbl[(val>>valb)&0x3F]); valb -= 6; } }
    if (valb>-6) out.push_back(tbl[((val<<8)>>(valb+8))&0x3F]);
    while (out.size()%4) out.push_back('=');
    return out;
}

// -------------------------------
// CLI options & discovery
// -------------------------------
enum class Network { MAINNET, TESTNET, REGTEST };
enum class Transport { AUTO, HTTP, WS };
enum class OutputFormat {
    TABLE,
    JSON,
    PLAIN
};

static std::string netName(Network n) {
    switch (n) {
        case Network::MAINNET: return "mainnet";
        case Network::TESTNET: return "testnet";
        case Network::REGTEST: return "regtest";
        default:               return "unknown";
    }
}

// Overload adapter for Network enum
static dinero::cli::PrintCtx makePrintCtx(Network net,
                                          const dinero::cli::CliOverrides& flags,
                                          const std::string& effectiveUrl,
                                          const std::string& command,
                                          const std::string& walletName) {
    return makePrintCtx(netName(net), flags, effectiveUrl, command, walletName);
}

struct OutputContract {
    std::string version = "v1";
    std::string timestamp;
    std::string cli_version = "0.6.0";
    Json::Value data;
    
    Json::Value to_json() const {
        Json::Value root;
        root["output_version"] = version;
        root["timestamp"] = timestamp;
        root["cli_version"] = cli_version;
        root["data"] = data;
        return root;
    }
};

struct Config {
    Network network = Network::REGTEST;
    Transport transport = Transport::AUTO;
    OutputFormat format = OutputFormat::JSON;
    std::optional<std::string> default_wallet;
    std::optional<fs::path> datadir;
    int timeout_sec = 10;
    int retries = 3;
    bool pretty_json = true;
    bool color_output = true;
    
    static Config load_from_file(const fs::path& config_path);
    void apply_env_overrides();
};

struct Options {
    Network network = Network::REGTEST;
    Transport transport = Transport::AUTO;
    OutputFormat format = OutputFormat::JSON;
    bool force_http_only = false;
    bool json_out = false;
    bool pretty_json = true;
    bool color_output = true;
    bool dry_run = false;
    bool wait_ready = false;
    bool show_curl = false;
    bool verbose = false;                        // --verbose for discovery details
    bool show_nodeinfo = false;                  // --nodeinfo flag (print and exit)
    bool version = false;                        // --version flag (print and exit)
    int timeout_sec = 10;
    int retries = 3;
    int wait_timeout = 30;
    std::optional<std::string> wallet_name;      // --wallet context
    std::optional<fs::path> datadir;             // network-aware root
    std::optional<fs::path> nodeinfo_path;       // explicit
    std::optional<fs::path> config_path;         // config file
    bool no_nodeinfo = false;                    // --no-nodeinfo (skip discovery)
    // Explicit overrides (always win over auto-discovery)
    std::optional<std::string> rpc_url_override; // --rpc-url
    std::optional<fs::path> cookie_file_override; // --cookie-file
    // Security and networking
    bool accept_insecure_cookie = false;         // --accept-insecure-cookie
    int connect_timeout_ms = 5000;               // --connect-timeout-ms
    int read_timeout_ms = 30000;                 // --read-timeout-ms
    std::string json_schema = "din.cli.v1";     // --json-schema
    std::vector<std::string> argv_tail;          // subcommand + args
    
    // CLI overrides for new features
    dinero::cli::CliOverrides overrides;
};

struct NodeInfo {
    std::string rpc_url;
    std::optional<std::string> ws_url;
    fs::path cookie_path;
    fs::path datadir;
    std::string network;
    // Discovery metadata for transparency
    std::string discovery_source;  // "explicit", "nodeinfo", "override"
    fs::path nodeinfo_path;        // where nodeinfo was loaded from
};

static const char* network_name(Network n) {
    switch (n) {
        case Network::MAINNET: return "mainnet";
        case Network::TESTNET: return "testnet";
        case Network::REGTEST: return "regtest";
    }
    return "mainnet";
}

static Network parse_network(const std::string& s) {
    if (s == "mainnet") return Network::MAINNET;
    if (s == "testnet") return Network::TESTNET;
    if (s == "regtest") return Network::REGTEST;
    throw std::runtime_error("Invalid network: " + s + " (must be mainnet|testnet|regtest)");
}

// Config file and environment handling
Config Config::load_from_file(const fs::path& config_path) {
    Config cfg;
    if (!file_exists(config_path)) return cfg;
    
    try {
        std::string content = read_file(config_path);
        Json::Value root;
        Json::CharReaderBuilder rb;
        std::string errs;
        std::istringstream iss(content);
        if (!Json::parseFromStream(rb, iss, &root, &errs)) {
            std::cerr << "[warn] Invalid config JSON: " << errs << std::endl;
            return cfg;
        }
        
        if (root["network"].isString()) cfg.network = parse_network(root["network"].asString());
        if (root["transport"].isString()) {
            std::string t = root["transport"].asString();
            if (t == "auto") cfg.transport = Transport::AUTO;
            else if (t == "http") cfg.transport = Transport::HTTP;
            else if (t == "ws") cfg.transport = Transport::WS;
        }
        if (root["default_wallet"].isString()) cfg.default_wallet = root["default_wallet"].asString();
        if (root["datadir"].isString()) cfg.datadir = root["datadir"].asString();
        if (root["timeout"].isInt()) cfg.timeout_sec = root["timeout"].asInt();
        if (root["retries"].isInt()) cfg.retries = root["retries"].asInt();
        if (root["pretty_json"].isBool()) cfg.pretty_json = root["pretty_json"].asBool();
        if (root["color_output"].isBool()) cfg.color_output = root["color_output"].asBool();
    } catch (const std::exception& e) {
        std::cerr << "[warn] Config load error: " << e.what() << std::endl;
    }
    return cfg;
}

void Config::apply_env_overrides() {
    if (const char* env = std::getenv("DINERO_NETWORK")) {
        try { network = parse_network(env); } catch (...) {}
    }
    if (const char* env = std::getenv("DINERO_DATADIR")) {
        datadir = env;
    }
    if (const char* env = std::getenv("DINERO_TIMEOUT")) {
        try { timeout_sec = std::max(1, std::stoi(env)); } catch (...) {}
    }
}

// Secure password input
static std::string read_password_stdin(const std::string& prompt) {
    std::cout << prompt << std::flush;
    
    struct termios old_term, new_term;
    tcgetattr(STDIN_FILENO, &old_term);
    new_term = old_term;
    new_term.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
    
    std::string password;
    std::getline(std::cin, password);
    
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    std::cout << std::endl;
    return password;
}

// File permissions check
static bool check_file_permissions(const fs::path& path, mode_t expected_mode) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return (st.st_mode & 0777) == expected_mode;
}

static Options parse_args(int argc, char** argv) {
    // Load config file first
    fs::path config_dir;
#ifdef __APPLE__
    const char* home = std::getenv("HOME");
    if (home) config_dir = fs::path(home) / ".dinero-cli";
#else
    const char* home = std::getenv("HOME");
    if (home) config_dir = fs::path(home) / ".dinero-cli";
#endif
    
    Config cfg;
    if (!config_dir.empty()) {
        fs::path config_path = config_dir / "config.json";
        cfg = Config::load_from_file(config_path);
        cfg.apply_env_overrides();
    }
    
    Options o;
    // Apply config defaults
    o.network = cfg.network;
    o.transport = cfg.transport;
    o.timeout_sec = cfg.timeout_sec;
    o.retries = cfg.retries;
    o.pretty_json = cfg.pretty_json;
    o.color_output = cfg.color_output;
    if (cfg.default_wallet) o.wallet_name = cfg.default_wallet;
    if (cfg.datadir) o.datadir = cfg.datadir;
    
    // Security and timeout defaults
    o.accept_insecure_cookie = envTruthy("DIN_CLI_ACCEPT_INSECURE_COOKIE");
    o.connect_timeout_ms = 5000;
    o.read_timeout_ms = 30000;
    
    for (int i=1; i<argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string flag)->std::string {
            if (i+1>=argc) throw std::runtime_error("Missing value for " + flag);
            return argv[++i];
        };
        auto match = [&](const char* longf, const char* shortf=nullptr)->bool {
            if (a==longf) return true; if (shortf && a==shortf) return true;
            if (a.rfind(std::string(longf)+"=",0)==0) {
                std::string v = a.substr(std::string(longf).size()+1);
                a = longf;
                const_cast<char*&>(argv[i]) = (char*)longf;
                const_cast<char*&>(argv[i+1]) = (char*)v.c_str();
            }
            return a==longf;
        };

        if (match("--help","-h")) {
            std::cout <<
            "Dinero CLI v1.0.0 - Production-grade control cockpit\n\n"
            "Global Flags:\n"
            "  --network=regtest|testnet|mainnet     Select network (default: regtest)\n"
            "  --regtest, --testnet, --mainnet       Network aliases (prefer --network)\n"
            "  -w, --wallet NAME                     Target wallet for operations\n"
            "  --transport auto|http|ws              Transport (default: auto)\n"
            "  --http-only                           Force HTTP transport\n"
            "  --datadir PATH                        Network datadir root\n"
            "  --nodeinfo PATH                       Override nodeinfo.json path\n"
            "  --no-nodeinfo                          Skip nodeinfo discovery (requires --rpc-url and --cookie-file)\n"
            "  --rpc-url URL                         Override RPC URL\n"
            "  --cookie-file PATH                    Override cookie file path\n"
            "  --format table|json|plain             Output format (default: table)\n"
            "  --json                                JSON output (alias for --format=json)\n"
            "  --json-schema TAG                     JSON contract tag (default: din.cli.v1)\n"
            "  --pretty, --no-pretty                 Pretty JSON formatting\n"
            "  --color, --no-color                   Color output\n"
            "  --timeout SECONDS                     RPC timeout (default: 10)\n"
            "  --retries N                           Retry attempts (default: 3)\n"
            "  --wait-ready [--timeout N]            Wait for daemon ready\n"
            "  --dry-run                             Show what would be done\n"
            "  --curl                                Show equivalent curl command\n"
            "  --verbose, -v                         Show connection discovery details\n"
            "  --nodeinfo                            Print resolved connection info and exit\n"
            "  --version                             Show version information and exit\n"
            "\nPaging & Filtering:\n"
            "  --limit N                             Page size (default: 100, max: 5000)\n"
            "  --offset N                            Start offset for pagination\n"
            "  --cursor TOKEN                        Opaque pagination cursor\n"
            "  --filter PATTERN                      Filter results (case-insensitive)\n"
            "  --since ISO8601|HEIGHT                Filter by time/block height\n"
            "  --until ISO8601|HEIGHT                Filter by time/block height\n"
            "  --all                                 Bypass limits (JSON only)\n"
            "\nCommand-Specific Filters:\n"
            "  --min-conf N                          Minimum confirmations (transactions)\n"
            "  --address ADDR                        Filter by address\n"
            "  --type TYPE                           Filter by transaction type\n"
            "  --label LABEL                         Filter by label\n"
            "  --min-amount N                        Minimum amount (UTXOs)\n"
            "  --max-amount N                        Maximum amount (UTXOs)\n"
            "  --confirmed-only                      Confirmed UTXOs only\n"
            "  --state STATE                         Peer connection state\n"
            "  --min-version N                       Minimum peer version\n"
            "  --min-fee-rate N                      Minimum fee rate (mempool)\n"
            "  --txid TXID                           Specific transaction ID\n"
            "\nProfile Management:\n"
            "  --profile NAME                        Use profile for connection settings\n"
            "\nSecurity:\n"
            "  --accept-insecure-cookie              Bypass cookie permission checks (NOT for production)\n"
            "  --connect-timeout-ms <n>              RPC connect timeout (default: 5000)\n"
            "  --read-timeout-ms <n>                 RPC read timeout (default: 30000)\n"
            "\nCore Commands:\n"
            "  status                                Node health and status\n"
            "  nodeinfo print|path                   Show nodeinfo details\n"
            "  doctor                                Comprehensive diagnostics\n"
            "\nBlockchain Commands:\n"
            "  chain tip|info|count                  Chain information\n"
            "  chain getblockhash HEIGHT             Get block hash by height\n"
            "  chain getblock HASH [--verbose]       Get block details\n"
            "\nNetwork Commands:\n"
            "  net info|peers|connections            Network status and peers\n"
            "  mempool info|raw                      Memory pool information\n"
            "\nTransaction Commands:\n"
            "  tx get TXID [--verbose]               Get transaction details\n"
            "  tx decode HEX_TX                      Decode raw transaction\n"
            "\nWallet Commands:\n"
            "  wallet create|load|info NAME          Wallet lifecycle\n"
            "  wallet balance|history|utxos          Balance and transaction info\n"
            "  wallet addresses [--labels]           List addresses\n"
            "  wallet newaddress [--label L]         Generate new address\n"
            "  wallet encrypt|lock|unlock            Security operations\n"
            "  wallet export|backup --to DIR         Backup operations\n"
            "\nSend Commands:\n"
            "  send --to ADDR --amount X [opts]      Send transaction\n"
            "    --fee-rate R --subtract-fee --dry-run\n"
            "\nMining Commands:\n"
            "  mining info|setaddress|getaddress     Mining operations\n"
            "  mining start|stop [--threads N]       Control mining\n"
            "\nChain Commands:\n"
            "  chain tip|getblockhash|getblock       Blockchain queries\n"
            "\nDiagnostics:\n"
            "  --nodeinfo                            Print resolved connection info and exit\n"
            "  rpc parity | rpc-parity               Compare daemon RPC methods vs CLI command coverage\n"
            "\nAdvanced:\n"
            "  rpc METHOD [JSON_PARAMS]              Raw RPC calls\n"
            "  doctor                                Health diagnostics\n"
            "\nEnvironment Variables:\n"
            "  DINERO_NETWORK, DINERO_DATADIR, DINERO_TIMEOUT\n"
            "\nConfig File: ~/.dinero-cli/config.json\n"
            "\nNetwork Info:\n"
            "  Block Time: 520 seconds (8.7 minutes)\n"
            "  Retarget Interval: 60 blocks (8.7 hours)\n"
            "  CPU-Friendly Phase: 3.0 years (18M DIN at 99 DIN/block)\n"
            "  Total Supply: 180.5M DIN (18M CPU + 162.5M halving)\n"
            "\nExamples:\n"
            "  dinero-cli --network=regtest status\n"
            "  dinero-cli --wallet=mywallet wallet balance\n"
            "  dinero-cli send --to rdin1... --amount 1.25 --dry-run\n"
            "  dinero-cli mining setaddress rdin1...\n"
            << std::endl;
            std::exit(EXIT_OK);
        }
        else if (match("--version","-v")) {
            std::cout << "dinero-cli v1.0.0\n";
            std::exit(EXIT_OK);
        }
        // Network flags (unified and legacy aliases)
        else if (match("--network")) {
            std::string v = next("--network");
            o.network = parse_network(v);
        }
        else if (a=="--regtest") { o.network = Network::REGTEST; }
        else if (a=="--testnet") { o.network = Network::TESTNET; }
        else if (a=="--mainnet") { o.network = Network::MAINNET; }
        // Wallet context
        else if (match("--wallet")) {
            o.wallet_name = next("--wallet");
        }
        // Transport
        else if (match("--transport")) {
            std::string v = next("--transport");
            if (v=="auto") o.transport=Transport::AUTO;
            else if (v=="http") o.transport=Transport::HTTP;
            else if (v=="ws") o.transport=Transport::WS;
            else throw std::runtime_error("Invalid transport: "+v+" (must be auto|http|ws)");
        }
        else if (a=="--http-only") { o.force_http_only = true; o.transport = Transport::HTTP; }
        // Output format
        else if (match("--format")) {
            std::string v = next("--format");
            if (v=="table") o.format=OutputFormat::TABLE;
            else if (v=="json") o.format=OutputFormat::JSON;
            else if (v=="plain") o.format=OutputFormat::PLAIN;
            else throw std::runtime_error("Invalid format: "+v+" (must be table|json|plain)");
        }
        else if (a=="--json") { o.json_out = true; o.format = OutputFormat::JSON; }
        else if (a=="--pretty") { o.pretty_json = true; }
        else if (a=="--no-pretty") { o.pretty_json = false; }
        else if (a=="--color") { o.color_output = true; }
        else if (a=="--no-color") { o.color_output = false; }
        // Timeouts and retries
        else if (match("--timeout")) {
            o.timeout_sec = std::max(1, std::stoi(next("--timeout")));
        }
        else if (match("--retries")) {
            o.retries = std::max(0, std::stoi(next("--retries")));
        }
        // Paths
        else if (match("--datadir")) {
            o.datadir = fs::path(next("--datadir"));
        }
        else if (match("--nodeinfo")) {
            if (i+1 < argc && argv[i+1][0] != '-') {
                o.nodeinfo_path = fs::path(next("--nodeinfo"));
            } else {
                // --nodeinfo without path means print and exit
                o.show_nodeinfo = true;
            }
        }
        else if (a=="--no-nodeinfo") {
            o.no_nodeinfo = true;
        }
        else if (match("--config")) {
            o.config_path = fs::path(next("--config"));
        }
        else if (match("--cookie-file")) {
            o.cookie_file_override = fs::path(next("--cookie-file"));
        }
        else if (match("--json-schema")) {
            o.json_schema = next("--json-schema");
        }
        else if (match("--accept-insecure-cookie")) {
            o.accept_insecure_cookie = true;
        }
        else if (match("--connect-timeout-ms")) {
            o.connect_timeout_ms = std::max(0, std::stoi(next("--connect-timeout-ms")));
        }
        else if (match("--read-timeout-ms")) {
            o.read_timeout_ms = std::max(0, std::stoi(next("--read-timeout-ms")));
        }
        else if (match("--rpc-url")) {
            o.rpc_url_override = next("--rpc-url");
        }
        else if (a.rfind("--cookie-file=", 0) == 0) {
            o.cookie_file_override = fs::path(a.substr(14));
        }
        else if (match("--wallet", "-w")) {
            o.wallet_name = next("--wallet");
        }
        // Behavior flags
        else if (a=="--wait-ready") {
            o.wait_ready = true;
            // Check if next arg is --timeout for wait
            if (i+1<argc && std::string(argv[i+1]).rfind("--timeout",0)==0) {
                ++i; // consume --timeout
                if (std::string(argv[i])=="--timeout") {
                    o.wait_timeout = std::stoi(next("--timeout"));
                } else {
                    // --timeout=N format
                    std::string v = std::string(argv[i]).substr(10); // skip "--timeout="
                    o.wait_timeout = std::stoi(v);
                }
            }
        }
        else if (a=="--dry-run") { o.dry_run = true; }
        else if (a=="--curl") { o.show_curl = true; }
        else if (a=="--verbose" || a=="-v") { o.verbose = true; }
        else if (a=="--version") { o.version = true; }
        // First non-option = subcommand & args
        else if (a[0] != '-') {
            // This is a positional argument (command)
            for (int j=i; j<argc; ++j) {
                o.argv_tail.emplace_back(argv[j]);
            }
            break;
        }
        else {
            // Unknown option
            std::cerr << "Unknown option: " << a << "\n";
            std::cerr << "Use --help for usage information\n";
            exit(EXIT_USAGE);
        }
    }
    
    return o;
}

// macOS default datadir root: ~/Library/Application Support/Dinero/<network>
static fs::path default_datadir(Network net) {
#ifdef __APPLE__
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("HOME not set");
    fs::path p = fs::path(home) / "Library" / "Application Support" / "Dinero" / network_name(net);
    return p;
#else
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("HOME not set");
    fs::path p = fs::path(home) / ".dinero" / network_name(net);
    return p;
#endif
}

// --- begin helpers -----------------------------------------------------------
#include <fstream>
#include <json/json.h>

static std::string expand_path(const std::string& path) {
    if (path.empty()) return path;
    
    std::string result = path;
    
    // Expand ~ to home directory
    if (result[0] == '~') {
        const char* home = getenv("HOME");
        if (!home) {
            // Fallback to getpwuid
            struct passwd* pw = getpwuid(getuid());
            if (pw && pw->pw_dir) {
                home = pw->pw_dir;
            }
        }
        if (home) {
            result = std::string(home) + result.substr(1);
        }
    }
    
    // Expand environment variables
    size_t start = 0;
    while ((start = result.find("$", start)) != std::string::npos) {
        size_t end = result.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", start + 1);
        if (end == std::string::npos) end = result.length();
        
        std::string var_name = result.substr(start + 1, end - start - 1);
        const char* var_value = getenv(var_name.c_str());
        if (var_value) {
            result.replace(start, end - start, var_value);
            start += strlen(var_value);
        } else {
            start = end;
        }
    }
    
    return result;
}

static std::string read_file_trim(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("cookie file not found: " + path);
    }
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back()=='\n' || s.back()=='\r' || s.back()==' ' || s.back()=='\t')) s.pop_back();
    return s;
}

// Try to read cookie from JSON "rpc" object using multiple shapes.
// Returns the *cookie value* (not a path), reading file when a path is provided.
static std::string extract_cookie_from_rpc_json(const Json::Value& rpc, bool accept_insecure_inline_cookie) {
    // Inline literal cookie: rpc.cookie = "user:token"
    if (rpc.isMember("cookie") && rpc["cookie"].isString()) {
        if (!accept_insecure_inline_cookie) {
            throw std::runtime_error("inline cookie provided; pass --accept-insecure-cookie to allow it");
        }
        return rpc["cookie"].asString();
    }

    // Object form: rpc.cookie = { "path": "/path/to/.cookie" }
    if (rpc.isMember("cookie") && rpc["cookie"].isObject()) {
        const auto& obj = rpc["cookie"];
        if (obj.isMember("path") && obj["path"].isString()) {
            return read_file_trim(obj["path"].asString());
        }
    }

    // Synonyms: rpc.cookie_file / rpc.cookie_path
    if (rpc.isMember("cookie_file") && rpc["cookie_file"].isString()) {
        return read_file_trim(rpc["cookie_file"].asString());
    }
    if (rpc.isMember("cookie_path") && rpc["cookie_path"].isString()) {
        return read_file_trim(rpc["cookie_path"].asString());
    }

    // Nothing usable found
    throw std::runtime_error("nodeinfo.json missing cookie path");
}
// --- end helpers -------------------------------------------------------------

static NodeInfo load_nodeinfo(const Options& o) {
    using namespace dinero::cli;
    
    // Build ConnInput from Options
    ConnInput input;
    input.rpc_url_flag = o.rpc_url_override;
    input.cookie_file_flag = o.cookie_file_override ? std::optional<std::string>(o.cookie_file_override->string()) : std::nullopt;
    input.nodeinfo_path = o.nodeinfo_path ? std::optional<std::string>(o.nodeinfo_path->string()) : std::nullopt;
    input.accept_insecure_cookie = o.accept_insecure_cookie;
    input.no_nodeinfo = o.no_nodeinfo;
    
    // Try to load nodeinfo.json if not using --no-nodeinfo AND not using full overrides
    Json::Value ni_json;
    bool ni_loaded = false;
    
    if (!o.no_nodeinfo && !(o.rpc_url_override && o.cookie_file_override)) {
        fs::path nodeinfo_path;
        
        if (o.nodeinfo_path) {
            nodeinfo_path = *o.nodeinfo_path;
        } else {
            fs::path root = o.datadir.value_or(default_datadir(o.network));
            nodeinfo_path = root / "nodeinfo.json";
        }
        
        if (file_exists(nodeinfo_path)) {
            try {
                std::string content = read_file(nodeinfo_path);
                
                // Validate schema first
                auto validation = dinero::cli::validateNodeinfoSchema(content);
                if (!validation.valid) {
                    std::string error_msg = "nodeinfo.json validation failed:";
                    for (const auto& error : validation.errors) {
                        error_msg += "\n  - " + error;
                    }
                    throw std::runtime_error(error_msg);
                }
                
                // Show deprecation warnings
                if (o.verbose && !validation.warnings.empty()) {
                    std::cerr << "[discovery] Schema warnings:\n";
                    for (const auto& warning : validation.warnings) {
                        std::cerr << "[discovery]   ⚠️  " << warning << "\n";
                    }
                }
                
                Json::CharReaderBuilder rb;
                std::string errs;
                std::istringstream iss(content);
                if (Json::parseFromStream(rb, iss, &ni_json, &errs)) {
                    ni_loaded = true;
                    
                    if (o.verbose) {
                        std::cerr << "[discovery] Loading nodeinfo from: " << nodeinfo_path.string() << "\n";
                    }
                }
            } catch (const std::runtime_error& e) {
                // Re-throw validation errors
                throw;
            } catch (...) {
                // Ignore parsing errors, will be handled by resolver
            }
        }
    }
    
    // Extract nodeinfo values if loaded
    if (ni_loaded && ni_json.isMember("rpc") && ni_json["rpc"].isObject()) {
        const Json::Value& rpc = ni_json["rpc"];
        
        if (rpc.isMember("url") && rpc["url"].isString()) {
            input.nodeinfo_rpc_url = rpc["url"].asString();
        }
        
        // Extract cookie using tolerant parsing
        if (rpc.isMember("cookie_path") && rpc["cookie_path"].isString()) {
            input.nodeinfo_cookie_path = rpc["cookie_path"].asString();
        } else if (rpc.isMember("cookie_file") && rpc["cookie_file"].isString()) {
            input.nodeinfo_cookie_path = rpc["cookie_file"].asString();
        } else if (rpc.isMember("cookie") && rpc["cookie"].isObject() && 
                  rpc["cookie"].isMember("path") && rpc["cookie"]["path"].isString()) {
            input.nodeinfo_cookie_path = rpc["cookie"]["path"].asString();
        } else if (rpc.isMember("cookie") && rpc["cookie"].isString()) {
            input.nodeinfo_cookie_literal = rpc["cookie"].asString();
        }
    }
    
    // Resolve connection using the deterministic resolver
    ConnResolved resolved = resolve(input);
    
    // Convert to NodeInfo
    NodeInfo ni;
    ni.rpc_url = resolved.rpc_url;
    ni.cookie_path = fs::path(resolved.cookie_path);
    ni.datadir = o.datadir.value_or(default_datadir(o.network));
    ni.network = network_name(o.network);
    ni.discovery_source = resolved.discovery_method;
    ni.nodeinfo_path = ""; // Not used in new system
    
    // Verbose output
    if (o.verbose) {
        std::cerr << "[discovery] " << resolved.discovery_method << ":\n";
        std::cerr << "[discovery]   RPC URL: " << ni.rpc_url << " (" << resolved.rpc_url_source << ")\n";
        std::cerr << "[discovery]   Cookie: " << ni.cookie_path << " (" << resolved.cookie_source << ")\n";
        std::cerr << "[discovery]   Datadir: " << ni.datadir.string() << " (--datadir or default)\n";
    }
    
    return ni;
}

// -------------------------------
// HTTP JSON-RPC via Boost.Beast with retries
// -------------------------------
struct RpcContext {
    std::string url_host;  // 127.0.0.1
    std::string url_port;  // "58064"
    std::string url_target; // "/"
    std::string auth_header; // "Basic ..."

    // parse http://host:port[/path]
    static RpcContext from_url_and_cookie(const std::string& http_url, const fs::path& cookie_path) {
        // very small parser for http://host:port/path
        static const std::regex rx(R"(^(http)://([^/:]+):(\d+)(/.*)?$)");
        std::smatch m;
        if (!std::regex_match(http_url, m, rx)) {
            throw std::runtime_error("Unsupported RPC URL: " + http_url);
        }
        RpcContext ctx;
        ctx.url_host = m[2].str();
        ctx.url_port = m[3].str();
        ctx.url_target = m[4].matched ? m[4].str() : "/";
        // cookie "__cookie__:secret"
        std::string cookie = trim(read_file(cookie_path));
        ctx.auth_header = "Basic " + base64_encode(cookie);
        return ctx;
    }
};

static Json::Value rpc_call_http_once(const RpcContext& ctx, const std::string& method, const Json::Value& params, int timeout_sec) {
    // Build JSON request
    Json::Value req;
    req["jsonrpc"] = "2.0";
    req["id"] = 1;
    req["method"] = method;
    if (!params.isNull()) req["params"] = params;

    Json::StreamWriterBuilder wb;
    wb["indentation"] = ""; // compact
    std::string body = Json::writeString(wb, req);

    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);
    boost::beast::tcp_stream stream(ioc);

    // Optional timeout
    stream.expires_after(std::chrono::seconds(std::max(1, timeout_sec)));

    auto const results = resolver.resolve(ctx.url_host, ctx.url_port);
    stream.connect(results);

    http::request<http::string_body> http_req{http::verb::post, ctx.url_target.empty()?"/":ctx.url_target, 11};
    http_req.set(http::field::host, ctx.url_host + ":" + ctx.url_port);
    http_req.set(http::field::user_agent, "dinero-cli/0.6");
    http_req.set(http::field::content_type, "application/json");
    http_req.set(http::field::authorization, ctx.auth_header);
    http_req.body() = body;
    http_req.prepare_payload();

    http::write(stream, http_req);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    // Graceful close
    boost::system::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    if (res.result() != http::status::ok) {
        std::ostringstream oss;
        oss << "HTTP " << res.result_int() << " " << res.reason() << " - " << res.body();
        throw std::runtime_error(oss.str());
    }

    // Parse JSON-RPC response
    Json::Value out;
    {
        Json::CharReaderBuilder rb;
        std::string errs;
        std::istringstream iss(res.body());
        if (!Json::parseFromStream(rb, iss, &out, &errs))
            throw std::runtime_error("HTTP bad JSON: " + errs);
    }
    
    // Validate RPC schema for dinerod vNext compatibility
    if (out.isMember("result") && out["result"].isObject()) {
        // Convert Json::Value to Json::Value for schema validation
        std::string jsonStr = Json::writeString(Json::StreamWriterBuilder(), out["result"]);
        try {
            Json::CharReaderBuilder reader_builder;
            std::string parse_errors;
            Json::Value json_value;
            std::istringstream json_stream(jsonStr);
            
            if (Json::parseFromStream(reader_builder, json_stream, &json_value, &parse_errors)) {
                dinero::SchemaValidator::validateRpcSchema(json_value);
            }
        } catch (const std::exception& e) {
            // Schema validation failed, but continue with response
        }
    }
    
    return out;
}

// HTTP RPC with retries and exponential backoff
static Json::Value rpc_call_http(const RpcContext& ctx, const std::string& method, const Json::Value& params, int timeout_sec, int max_retries) {
    std::exception_ptr last_exception;
    
    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        try {
            return rpc_call_http_once(ctx, method, params, timeout_sec);
        } catch (const std::exception& e) {
            last_exception = std::current_exception();
            
            if (attempt < max_retries) {
                // Exponential backoff: 100ms, 200ms, 400ms, etc.
                int delay_ms = 100 * (1 << attempt);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
    }
    
    // Re-throw the last exception
    if (last_exception) {
        std::rethrow_exception(last_exception);
    }
    
    throw std::runtime_error("RPC call failed after " + std::to_string(max_retries + 1) + " attempts");
}

// -------------------------------
// Output formatting and health checks
// -------------------------------
static std::string get_iso_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

static void print_output(const Json::Value& v, const Options& opt) {
    if (opt.format == OutputFormat::JSON || opt.json_out) {
        // Use stable output contract for JSON
        OutputContract contract;
        contract.timestamp = get_iso_timestamp();
        contract.data = v;
        
        Json::StreamWriterBuilder wb;
        wb["indentation"] = opt.pretty_json ? "  " : "";
        std::cout << Json::writeString(wb, contract.to_json()) << std::endl;
    } else if (opt.format == OutputFormat::TABLE) {
        // Table format not implemented yet - fall back to JSON
        if (opt.verbose) {
            std::cerr << "Table format not implemented; falling back to JSON\n";
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "  ";
        std::cout << Json::writeString(wb, v) << std::endl;
    } else {
        std::cout << v.asString() << std::endl;
    }
}

static void print_ok(const std::string& msg, bool use_color = true) {
    if (use_color) {
        std::cout << "\033[32m✅\033[0m " << msg << "\n";
    } else {
        std::cout << "[OK] " << msg << "\n";
    }
}

static void print_err(const std::string& msg, bool use_color = true) {
    if (use_color) {
        std::cerr << "\033[31m❌\033[0m " << msg << "\n";
    } else {
        std::cerr << "[ERROR] " << msg << "\n";
    }
}

static void print_warn(const std::string& msg, bool use_color = true) {
    if (use_color) {
        std::cerr << "\033[33m⚠️\033[0m " << msg << "\n";
    } else {
        std::cerr << "[WARN] " << msg << "\n";
    }
}

// Security utilities
static Json::Value redact_sensitive_fields(const Json::Value& input) {
    Json::Value result = input;
    
    // List of sensitive field names to redact
    std::vector<std::string> sensitive_fields = {
        "password", "passphrase", "private_key", "privkey", "seed", 
        "mnemonic", "cookie", "auth", "token", "secret"
    };
    
    std::function<void(Json::Value&)> redact_recursive = [&](Json::Value& val) {
        if (val.isObject()) {
            for (const std::string& key : val.getMemberNames()) {
                // Check if key contains sensitive terms
                std::string lower_key = key;
                std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
                
                bool is_sensitive = false;
                for (const std::string& sensitive : sensitive_fields) {
                    if (lower_key.find(sensitive) != std::string::npos) {
                        is_sensitive = true;
                        break;
                    }
                }
                
                if (is_sensitive) {
                    val[key] = "[REDACTED]";
                } else {
                    redact_recursive(val[key]);
                }
            }
        } else if (val.isArray()) {
            for (Json::ArrayIndex i = 0; i < val.size(); ++i) {
                redact_recursive(val[i]);
            }
        }
    };
    
    redact_recursive(result);
    return result;
}

// Health check utilities
static bool check_daemon_health(const NodeInfo& ni, bool verbose = false) {
    bool healthy = true;
    
    // Check nodeinfo schema version
    try {
        std::string content = read_file(ni.nodeinfo_path.empty() ? ni.datadir / "nodeinfo.json" : ni.nodeinfo_path);
        Json::Value root;
        Json::CharReaderBuilder rb;
        std::string errs;
        std::istringstream iss(content);
        if (Json::parseFromStream(rb, iss, &root, &errs)) {
            if (!root["schema_version"].isString() || root["schema_version"].asString() != "v1") {
                if (verbose) print_warn("nodeinfo.json missing or invalid schema_version");
                healthy = false;
            }
        }
    } catch (...) {
        if (verbose) print_warn("Failed to read nodeinfo.json");
        healthy = false;
    }
    
    // Check cookie file permissions (security hardening)
    if (!check_file_permissions(ni.cookie_path, 0600)) {
        if (verbose) print_warn("Cookie file permissions not 0600: " + ni.cookie_path.string());
        if (verbose) print_warn("Run: chmod 600 " + ni.cookie_path.string());
        healthy = false;
    }
    
    // Check if cookie file exists and is readable
    if (!file_exists(ni.cookie_path)) {
        if (verbose) print_err("Cookie file missing: " + ni.cookie_path.string());
        healthy = false;
    }
    
    return healthy;
}

static void print_curl_equivalent(const RpcContext& ctx, const std::string& method, const Json::Value& params) {
    Json::Value req;
    req["jsonrpc"] = "2.0";
    req["id"] = 1;
    req["method"] = method;
    if (!params.isNull()) req["params"] = params;
    
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    std::string body = Json::writeString(wb, req);
    
    std::cout << "# Equivalent curl command:\n";
    std::cout << "curl -X POST " << "http://" << ctx.url_host << ":" << ctx.url_port << ctx.url_target
              << " -H 'Content-Type: application/json' -H 'Authorization: " << ctx.auth_header << "'"
              << " -d '" << body << "'\n\n";
}

// -------------------------------
// Command handlers
// -------------------------------
struct Client {
    Options opt;
    NodeInfo ni;
    RpcContext http;

    explicit Client(const Options& o, const NodeInfo& nodeinfo) : opt(o), ni(nodeinfo),
        http(RpcContext::from_url_and_cookie(ni.rpc_url, ni.cookie_path)) {

        // Always show connection info for transparency (unless JSON output)
        if (opt.format != OutputFormat::JSON && !opt.json_out) {
            std::cerr << "Connected: " << ni.rpc_url << " (" << ni.discovery_source << ")\n";
            
            // Show cookie source for explicit connections
            if (ni.discovery_source == "explicit" && opt.verbose) {
                if (opt.cookie_file_override) {
                    std::cerr << "Cookie source: --cookie-file\n";
                } else {
                    std::cerr << "Cookie source: nodeinfo.json\n";
                }
            }
        }

        if (opt.transport == Transport::WS && !opt.force_http_only) {
            // NOTE: WS client not wired yet for plain RPC; HTTP remains the control plane.
            // We still keep WS URL detected for future 'subscribe' commands.
            if (!ni.ws_url.has_value()) std::cerr << "[warn] WS requested but nodeinfo.ws missing; using HTTP.\n";
        }
    }

    Json::Value call(const std::string& method, const Json::Value& params = Json::Value()) {
        if (opt.show_curl) {
            print_curl_equivalent(http, method, params);
            return Json::Value();
        }
        
        // Build final method name with wallet context
        std::string final_method = method;
        RpcContext final_ctx = http;
        
        // Apply wallet scoping if specified
        if (opt.wallet_name) {
            final_ctx.url_target = "/wallet/" + *opt.wallet_name;
        }
        
        Json::Value result = rpc_call_http(final_ctx, final_method, params, opt.timeout_sec, opt.retries);
        
        // Apply security hardening - redact sensitive fields in output
        if (opt.format == OutputFormat::JSON || opt.json_out) {
            result = redact_sensitive_fields(result);
        }
        
        return result;
    }
    
    bool wait_for_ready() {
        auto start = std::chrono::steady_clock::now();
        auto timeout = std::chrono::seconds(opt.wait_timeout);
        
        // Construct base URL for health endpoint
        std::string baseUrl = "http://" + http.url_host + ":" + http.url_port;
        
        while (std::chrono::steady_clock::now() - start < timeout) {
            // Try health endpoint first (faster)
            if (dinero::cli::HealthClient::isReady(baseUrl, 2)) {
                return true;
            }
            
            // Fallback to RPC call for older daemons
            try {
                call("getbestblockhash");
                return true;
            } catch (...) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        return false;
    }
};

// status - comprehensive health check
static int cmd_status(Client& c) {
    Json::Value out;
    bool healthy = true;
    
    // Basic connectivity
    try {
        Json::Value r1 = c.call("blockchain.getbestblockhash");
        out["connectivity"] = "ok";
        out["bestblockhash"] = r1["result"];
    } catch (const std::exception& e) {
        out["connectivity"] = "failed";
        out["connectivity_error"] = e.what();
        healthy = false;
    }
    
    // Transport info
    out["transport"] = "http";
    out["rpc_url"] = c.ni.rpc_url;
    if (c.ni.ws_url) out["ws_url"] = *c.ni.ws_url;
    out["network"] = c.ni.network;
    
    // Health checks
    Json::Value health;
    health["daemon_health"] = check_daemon_health(c.ni, false);
    health["cookie_permissions"] = check_file_permissions(c.ni.cookie_path, 0600);
    health["nodeinfo_exists"] = file_exists(c.ni.datadir / "nodeinfo.json");
    
    // Wallet status (if available)
    try {
        Json::Value wallet_info = c.call("wallet.info");
        out["wallet"] = wallet_info["result"];
        health["wallet_loaded"] = true;
    } catch (...) {
        health["wallet_loaded"] = false;
    }
    
    // Mining status
    try {
        Json::Value mining_info = c.call("mining.getaddress");
        out["mining"] = mining_info["result"];
    } catch (...) {
        out["mining"] = "not_configured";
    }
    
    out["health"] = health;
    out["overall_status"] = healthy ? "healthy" : "degraded";
    
    print_output(out, c.opt);
    return healthy ? EXIT_OK : EXIT_NOT_READY;
}

// wallet create NAME
static int cmd_wallet_create(Client& c, const std::vector<std::string>& args) {
    if (args.size()<1) throw std::runtime_error("wallet create NAME");
    Json::Value p(Json::objectValue);
    p["name"] = args[0];
    auto resp = c.call("wallet.create", p);
    print_output(resp, c.opt);
    return 0;
}

// wallet load NAME
static int cmd_wallet_load(Client& c, const std::vector<std::string>& args) {
    if (args.size()<1) throw std::runtime_error("wallet load NAME");
    Json::Value p(Json::objectValue);
    p["name"] = args[0];
    auto resp = c.call("wallet.load", p);
    print_output(resp, c.opt);
    return 0;
}

// wallet info
static int cmd_wallet_info(Client& c) {
    auto resp = c.call("wallet.info");
    print_output(resp, c.opt);
    return 0;
}

// wallet addresses
static int cmd_wallet_addresses(Client& c) {
    auto resp = c.call("wallet.listaddresses");
    print_output(resp, c.opt);
    return 0;
}

// wallet newaddress [--label X]
static int cmd_wallet_newaddress(Client& c, const std::vector<std::string>& args) {
    Json::Value p(Json::arrayValue);
    p.append("taproot");
    if (!args.empty() && args[0]=="--label") {
        if (args.size()<2) throw std::runtime_error("wallet newaddress --label LABEL");
        p.append(args[1]);
    }
    auto resp = c.call("wallet.getnewaddress", p);
    print_output(resp, c.opt);
    return 0;
}

// addr validate ADDRESS
static int cmd_addr_validate(Client& c, const std::vector<std::string>& args) {
    if (args.size()<1) throw std::runtime_error("addr validate ADDRESS");
    Json::Value p(Json::arrayValue);
    p.append(args[0]);
    auto resp = c.call("wallet.validateaddress", p);
    print_output(resp, c.opt);
    return 0;
}

// send --to A --amount X [--fee-rate N]
static int cmd_send(Client& c, const std::vector<std::string>& args) {
    std::string to; double amount = -1.0; std::optional<double> feerate;
    for (size_t i=0;i<args.size();++i) {
        if (args[i]=="--to") { if (++i>=args.size()) throw std::runtime_error("--to ADDR"); to=args[i]; }
        else if (args[i]=="--amount") { if (++i>=args.size()) throw std::runtime_error("--amount AMOUNT"); amount=std::stod(args[i]); }
        else if (args[i]=="--fee-rate") { if (++i>=args.size()) throw std::runtime_error("--fee-rate RATE"); feerate=std::stod(args[i]); }
        else throw std::runtime_error("Unknown send arg: "+args[i]);
    }
    if (to.empty() || amount<=0) throw std::runtime_error("send --to ADDRESS --amount AMOUNT [--fee-rate N]");

    Json::Value p(Json::objectValue);
    p["to"] = to;
    p["amount"] = amount;
    if (feerate) p["fee_rate"] = *feerate;

    // Prefer canonical wallet.send if you implemented it; fallback to Bitcoin-like
    Json::Value resp;
    try { resp = c.call("wallet.send", p); }
    catch (const std::exception&) { resp = c.call("wallet.sendtoaddress", Json::Value(Json::arrayValue).append(to).append(amount)); }

    print_output(resp, c.opt);
    return 0;
}

// mining setaddress ADDRESS
static int cmd_mining_setaddr(Client& c, const std::vector<std::string>& args) {
    if (args.size()<1) throw std::runtime_error("mining setaddress ADDRESS");
    Json::Value p(Json::arrayValue); p.append(args[0]);
    auto resp = c.call("mining.setaddress", p);
    print_output(resp, c.opt);
    return 0;
}

// mining getaddress
static int cmd_mining_getaddr(Client& c) {
    auto resp = c.call("mining.getaddress");
    print_output(resp, c.opt);
    return 0;
}

// mining start [--threads N]
static int cmd_mining_start(Client& c, const std::vector<std::string>& args) {
    int threads = 0;
    if (!args.empty()) {
        if (args[0]=="--threads") {
            if (args.size()<2) throw std::runtime_error("mining start --threads N");
            threads = std::stoi(args[1]);
        } else {
            throw std::runtime_error("mining start [--threads N]");
        }
    }
    Json::Value p(Json::objectValue);
    if (threads>0) p["threads"] = threads;
    auto resp = c.call("mining.start", p);
    print_output(resp, c.opt);
    return 0;
}

// mining stop
static int cmd_mining_stop(Client& c) {
    auto resp = c.call("mining.stop");
    print_output(resp, c.opt);
    return 0;
}

// mining generatetoaddress N ADDRESS (regtest only)
static int cmd_mining_generateto(Client& c, const std::vector<std::string>& args) {
    if (args.size()<2) throw std::runtime_error("mining generatetoaddress N ADDRESS  (regtest only)");
    int n = std::stoi(args[0]);
    const std::string& addr = args[1];
    Json::Value p(Json::arrayValue);
    p.append(n);
    p.append(addr);
    auto resp = c.call("mining.generatetoaddress", p);
    print_output(resp, c.opt);
    return 0;
}

// wallet security operations
static int cmd_wallet_encrypt(Client& c) {
    std::string passphrase = read_password_stdin("Enter new passphrase: ");
    std::string confirm = read_password_stdin("Confirm passphrase: ");
    if (passphrase != confirm) {
        throw std::runtime_error("Passphrases do not match");
    }
    Json::Value p(Json::objectValue);
    p["passphrase"] = passphrase;
    auto resp = c.call("wallet.encrypt", p);
    print_output(resp, c.opt);
    return EXIT_OK;
}

static int cmd_wallet_lock(Client& c) {
    auto resp = c.call("wallet.lock");
    print_output(resp, c.opt);
    return EXIT_OK;
}

static int cmd_wallet_unlock(Client& c) {
    std::string passphrase = read_password_stdin("Enter passphrase: ");
    Json::Value p(Json::objectValue);
    p["passphrase"] = passphrase;
    auto resp = c.call("wallet.unlock", p);
    print_output(resp, c.opt);
    return EXIT_OK;
}

static int cmd_wallet_change_passphrase(Client& c) {
    std::string old_pass = read_password_stdin("Enter current passphrase: ");
    std::string new_pass = read_password_stdin("Enter new passphrase: ");
    std::string confirm = read_password_stdin("Confirm new passphrase: ");
    if (new_pass != confirm) {
        throw std::runtime_error("New passphrases do not match");
    }
    Json::Value p(Json::objectValue);
    p["old_passphrase"] = old_pass;
    p["new_passphrase"] = new_pass;
    auto resp = c.call("wallet.changepassphrase", p);
    print_output(resp, c.opt);
    return EXIT_OK;
}

// wallet balance and history
static int cmd_wallet_balance(Client& c) {
    auto resp = c.call("wallet.getbalance");
    print_output(resp, c.opt);
    return EXIT_OK;
}

static int cmd_wallet_history(Client& c, const std::vector<std::string>& args) {
    Json::Value p(Json::objectValue);
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--limit") {
            if (++i >= args.size()) throw std::runtime_error("--limit N");
            p["count"] = std::stoi(args[i]);
        } else if (args[i] == "--offset") {
            if (++i >= args.size()) throw std::runtime_error("--offset N");
            p["offset"] = std::stoi(args[i]);
        } else if (args[i] == "--type") {
            if (++i >= args.size()) throw std::runtime_error("--type mined|sent|received|all");
            p["type"] = args[i];
        } else if (args[i] == "--min-conf") {
            if (++i >= args.size()) throw std::runtime_error("--min-conf N");
            p["minconf"] = std::stoi(args[i]);
        } else if (args[i] == "--address") {
            if (++i >= args.size()) throw std::runtime_error("--address ADDR");
            p["address"] = args[i];
        } else if (args[i] == "--since") {
            if (++i >= args.size()) throw std::runtime_error("--since HEIGHT|TIME");
            // Try to parse as height (integer) or time
            try {
                int height = std::stoi(args[i]);
                p["since_height"] = height;
            } catch (...) {
                p["since_time"] = args[i];
            }
        } else {
            throw std::runtime_error("Unknown history option: " + args[i]);
        }
    }
    auto resp = c.call("wallet.listtransactions", p);
    print_output(resp, c.opt);
    return EXIT_OK;
}

static int cmd_wallet_utxos(Client& c, const std::vector<std::string>& args) {
    Json::Value p(Json::objectValue);
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--minconf") {
            if (++i >= args.size()) throw std::runtime_error("--minconf N");
            p["minconf"] = std::stoi(args[i]);
        } else if (args[i] == "--max") {
            if (++i >= args.size()) throw std::runtime_error("--max N");
            p["max"] = std::stoi(args[i]);
        } else {
            throw std::runtime_error("Unknown utxos option: " + args[i]);
        }
    }
    auto resp = c.call("wallet.listunspent", p);
    print_output(resp, c.opt);
    return EXIT_OK;
}

// wallet backup and export
static int cmd_wallet_backup(Client& c, const std::vector<std::string>& args) {
    std::string to_path;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--to") {
            if (++i >= args.size()) throw std::runtime_error("--to PATH");
            to_path = args[i];
        } else {
            throw std::runtime_error("Unknown backup option: " + args[i]);
        }
    }
    if (to_path.empty()) throw std::runtime_error("wallet backup --to PATH");
    
    Json::Value p(Json::objectValue);
    p["path"] = to_path;
    auto resp = c.call("wallet.backup", p);
    print_output(resp, c.opt);
    return EXIT_OK;
}

static int cmd_wallet_export(Client& c, const std::vector<std::string>& args) {
    std::string to_path;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--to") {
            if (++i >= args.size()) throw std::runtime_error("--to DIR");
            to_path = args[i];
        } else {
            throw std::runtime_error("Unknown export option: " + args[i]);
        }
    }
    if (to_path.empty()) throw std::runtime_error("wallet export --to DIR");
    
    Json::Value p(Json::objectValue);
    p["directory"] = to_path;
    p["include_private"] = false; // Safe default - metadata only
    auto resp = c.call("wallet.export", p);
    print_output(resp, c.opt);
    return EXIT_OK;
}

// mining convenience commands
static int cmd_mining_info(Client& c) {
    auto resp = c.call("mining.info");
    print_output(resp, c.opt);
    return EXIT_OK;
}

static int cmd_mining_setthreads(Client& c, const std::vector<std::string>& args) {
    if (args.size() < 1) throw std::runtime_error("mining setthreads N");
    int threads = std::stoi(args[0]);
    
    // Check if mining is already running
    try {
        Json::Value mining_info = c.call("mining.info");
        if (mining_info["result"]["running"].asBool()) {
            // Stop, then restart with new thread count
            c.call("mining.stop");
            Json::Value p(Json::objectValue);
            p["threads"] = threads;
            auto resp = c.call("mining.start", p);
            print_output(resp, c.opt);
        } else {
            // Just start with specified threads
            Json::Value p(Json::objectValue);
            p["threads"] = threads;
            auto resp = c.call("mining.start", p);
            print_output(resp, c.opt);
        }
    } catch (...) {
        // Fallback to just starting
        Json::Value p(Json::objectValue);
        p["threads"] = threads;
        auto resp = c.call("mining.start", p);
        print_output(resp, c.opt);
    }
    return EXIT_OK;
}

// nodeinfo commands
static int cmd_nodeinfo_print(Client& c) {
    try {
        std::string content = read_file(c.ni.datadir / "nodeinfo.json");
        Json::Value root;
        Json::CharReaderBuilder rb;
        std::string errs;
        std::istringstream iss(content);
        if (Json::parseFromStream(rb, iss, &root, &errs)) {
            print_output(root, c.opt);
        } else {
            throw std::runtime_error("Invalid nodeinfo.json: " + errs);
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to read nodeinfo.json: " + std::string(e.what()));
    }
    return EXIT_OK;
}

static int cmd_nodeinfo_path(Client& c) {
    fs::path nodeinfo_path = c.ni.datadir / "nodeinfo.json";
    if (c.opt.format == OutputFormat::JSON) {
        Json::Value out;
        out["nodeinfo_path"] = nodeinfo_path.string();
        print_output(out, c.opt);
    } else {
        std::cout << nodeinfo_path.string() << std::endl;
    }
    return EXIT_OK;
}

// doctor command - comprehensive diagnostics
static int cmd_doctor(Client& c) {
    Json::Value report;
    bool overall_healthy = true;
    
    // Schema version check
    try {
        std::string content = read_file(c.ni.datadir / "nodeinfo.json");
        Json::Value root;
        Json::CharReaderBuilder rb;
        std::string errs;
        std::istringstream iss(content);
        if (Json::parseFromStream(rb, iss, &root, &errs)) {
            bool schema_ok = root["schema_version"].isString() && root["schema_version"].asString() == "v1";
            report["schema_version"] = schema_ok ? "ok" : "invalid";
            if (!schema_ok) overall_healthy = false;
        } else {
            report["schema_version"] = "parse_error";
            overall_healthy = false;
        }
    } catch (...) {
        report["schema_version"] = "missing";
        overall_healthy = false;
    }
    
    // Cookie permissions
    bool cookie_perms_ok = check_file_permissions(c.ni.cookie_path, 0600);
    report["cookie_permissions"] = cookie_perms_ok ? "0600" : "incorrect";
    if (!cookie_perms_ok) overall_healthy = false;
    
    // HTTP/WS reachability
    try {
        c.call("blockchain.getbestblockhash");
        report["http_reachable"] = true;
    } catch (...) {
        report["http_reachable"] = false;
        overall_healthy = false;
    }
    
    // Port information
    try {
        Json::Value netinfo = c.call("network.getinfo");
        if (netinfo["result"]["version"].isString()) {
            report["daemon_version"] = netinfo["result"]["version"].asString();
        }
        // Extract port info from RPC URL
        std::string rpc_url = c.opt.rpc_url_override.value_or("http://127.0.0.1:20998");
        if (rpc_url.find("://") != std::string::npos) {
            size_t port_start = rpc_url.find(":", rpc_url.find("://") + 3);
            if (port_start != std::string::npos) {
                std::string port = rpc_url.substr(port_start + 1);
                report["http_json_rpc_port"] = port;
            }
        }
    } catch (...) {
        report["port_info"] = "unavailable";
    }
    
    // Wallet loaded
    try {
        c.call("wallet.info");
        report["wallet_loaded"] = true;
    } catch (...) {
        report["wallet_loaded"] = false;
    }
    
    // Mining address ownership
    try {
        Json::Value mining_addr = c.call("mining.getaddress");
        if (mining_addr["result"]["address"].isString()) {
            Json::Value validate = c.call("wallet.validateaddress", 
                Json::Value(Json::arrayValue).append(mining_addr["result"]["address"].asString()));
            bool ismine = validate["result"]["ismine"].asBool();
            report["mining_address_owned"] = ismine;
            if (!ismine) overall_healthy = false;
        } else {
            report["mining_address_owned"] = "not_set";
        }
    } catch (...) {
        report["mining_address_owned"] = "error";
    }
    
    // Network HRP check
    try {
        Json::Value addr = c.call("wallet.getnewaddress");
        std::string address = addr["result"]["address"].asString();
        std::string expected_hrp;
        if (c.ni.network == "regtest") expected_hrp = "rdin1";
        else if (c.ni.network == "testnet") expected_hrp = "tdin1";
        else expected_hrp = "din1";
        
        bool hrp_match = address.rfind(expected_hrp, 0) == 0;
        report["network_hrp_match"] = hrp_match;
        if (!hrp_match) overall_healthy = false;
    } catch (...) {
        report["network_hrp_match"] = "error";
    }
    
    report["overall_status"] = overall_healthy ? "healthy" : "issues_found";
    report["timestamp"] = static_cast<int64_t>(std::time(nullptr));
    
    print_output(report, c.opt);
    return overall_healthy ? EXIT_OK : EXIT_NOT_READY;
}

// chain tip / getblockhash / getblock
static int cmd_chain_tip(Client& c) {
    auto r = c.call("blockchain.getbestblockhash");
    Json::Value out; out["bestblockhash"]=r["result"]; 
    print_output(out, c.opt); 
    return EXIT_OK;
}

static int cmd_chain_info(Client& c) {
    auto r = c.call("blockchain.getinfo");
    print_output(r, c.opt);
    return EXIT_OK;
}

static int cmd_chain_count(Client& c) {
    auto r = c.call("blockchain.getblockcount");
    print_output(r, c.opt);
    return EXIT_OK;
}

static int cmd_chain_getblockhash(Client& c, const std::vector<std::string>& args) {
    if (args.size()<1) throw std::runtime_error("chain getblockhash HEIGHT");
    Json::Value p(Json::arrayValue); p.append(std::stoi(args[0]));
    auto r = c.call("blockchain.getblockhash", p); 
    print_output(r, c.opt); 
    return EXIT_OK;
}

static int cmd_chain_getblock(Client& c, const std::vector<std::string>& args) {
    if (args.size()<1) throw std::runtime_error("chain getblock HASH [--verbose]");
    bool verbose=false; std::string hash=args[0];
    if (args.size()>1) { if (args[1]=="--verbose") verbose=true; else throw std::runtime_error("chain getblock HASH [--verbose]"); }
    Json::Value p(Json::arrayValue); p.append(hash); if (verbose) p.append(1);
    auto r = c.call("blockchain.getblock", p); 
    print_output(r, c.opt); 
    return EXIT_OK;
}

// Enhanced send command with dry-run and coin control
static int cmd_send_enhanced(Client& c, const std::vector<std::string>& args) {
    std::string to; 
    double amount = -1.0; 
    std::optional<double> feerate;
    bool subtract_fee = false;
    std::vector<std::string> utxos;
    
    for (size_t i=0; i<args.size(); ++i) {
        if (args[i]=="--to") { 
            if (++i>=args.size()) throw std::runtime_error("--to ADDR"); 
            to=args[i]; 
        }
        else if (args[i]=="--amount") { 
            if (++i>=args.size()) throw std::runtime_error("--amount AMOUNT"); 
            amount=std::stod(args[i]); 
        }
        else if (args[i]=="--fee-rate") { 
            if (++i>=args.size()) throw std::runtime_error("--fee-rate RATE"); 
            feerate=std::stod(args[i]); 
        }
        else if (args[i]=="--subtract-fee") { 
            subtract_fee = true; 
        }
        else if (args[i]=="--utxo") {
            if (++i>=args.size()) throw std::runtime_error("--utxo TXID:VOUT");
            utxos.push_back(args[i]);
        }
        else {
            throw std::runtime_error("Unknown send option: "+args[i]);
        }
    }
    
    if (to.empty() || amount<=0) {
        throw std::runtime_error("send --to ADDRESS --amount AMOUNT [--fee-rate N] [--subtract-fee] [--utxo id ...] [--dry-run]");
    }

    Json::Value p(Json::objectValue);
    p["to"] = to;
    p["amount"] = amount;
    if (feerate) p["fee_rate"] = *feerate;
    if (subtract_fee) p["subtract_fee"] = true;
    if (!utxos.empty()) {
        Json::Value utxo_array(Json::arrayValue);
        for (const auto& utxo : utxos) {
            utxo_array.append(utxo);
        }
        p["utxos"] = utxo_array;
    }
    
    if (c.opt.dry_run) {
        p["dry_run"] = true;
        print_ok("DRY RUN - Transaction would be:", c.opt.color_output);
    }

    Json::Value resp;
    try { 
        resp = c.call("wallet.send", p); 
    } catch (const std::exception&) { 
        // Fallback to simpler sendtoaddress
        Json::Value simple_params(Json::arrayValue);
        simple_params.append(to);
        simple_params.append(amount);
        resp = c.call("wallet.sendtoaddress", simple_params); 
    }

    print_output(resp, c.opt);
    return EXIT_OK;
}

// Network and peer commands
static int cmd_net_info(Client& c) {
    auto r = c.call("network.getinfo");
    print_output(r, c.opt);
    return EXIT_OK;
}

static int cmd_net_peers(Client& c) {
    auto r = c.call("network.getpeerinfo");
    print_output(r, c.opt);
    return EXIT_OK;
}

static int cmd_net_connections(Client& c) {
    auto r = c.call("network.getconnectioncount");
    print_output(r, c.opt);
    return EXIT_OK;
}

// Memory pool commands
static int cmd_mempool_info(Client& c) {
    auto r = c.call("mempool.getinfo");
    print_output(r, c.opt);
    return EXIT_OK;
}

static int cmd_mempool_raw(Client& c) {
    auto r = c.call("mempool.getraw");
    print_output(r, c.opt);
    return EXIT_OK;
}

// Transaction commands
static int cmd_tx_get(Client& c, const std::vector<std::string>& args) {
    if (args.size()<1) throw std::runtime_error("tx get TXID [--verbose]");
    bool verbose = args.size()>1 && args[1]=="--verbose";
    Json::Value p(Json::arrayValue); 
    p.append(args[0]); 
    if (verbose) p.append(1);
    auto r = c.call("blockchain.getrawtransaction", p);
    print_output(r, c.opt);
    return EXIT_OK;
}

static int cmd_tx_decode(Client& c, const std::vector<std::string>& args) {
    if (args.size()<1) throw std::runtime_error("tx decode HEX_TX");
    Json::Value p(Json::arrayValue); p.append(args[0]);
    auto r = c.call("decoderawtransaction", p);
    print_output(r, c.opt);
    return EXIT_OK;
}

// rpc passthrough: rpc METHOD [JSON_PARAMS]  (or '-' to read params from stdin)
static int cmd_rpc_passthrough(Client& c, const std::vector<std::string>& args) {
    if (args.empty()) throw std::runtime_error("rpc METHOD [JSON_PARAMS]");
    std::string method = args[0];
    Json::Value params;
    if (args.size()>=2) {
        std::string p = args[1];
        if (p=="-") {
            std::ostringstream ss; ss << std::cin.rdbuf(); p = ss.str();
        }
        Json::CharReaderBuilder rb; std::string errs; std::istringstream iss(p);
        if (!Json::parseFromStream(rb, iss, &params, &errs))
            throw std::runtime_error("Invalid JSON params: " + errs);
    } else {
        params = Json::Value(); // null
    }
    auto r = c.call(method, params);
    print_output(r, c.opt);
    return EXIT_OK;
}

// RPC parity matrix: compare daemon RPC methods vs CLI coverage
static int cmd_rpc_parity(Client& c, const std::vector<std::string>& args) {
    // Get daemon RPC methods via multiple strategies
    std::vector<std::string> daemon_methods;
    
    // Try rpcmethods first (most direct)
    try {
        auto r = c.call("rpcmethods", Json::Value());
        if (r.isArray()) {
            for (const auto& method : r) {
                if (method.isString()) {
                    daemon_methods.push_back(method.asString());
                }
            }
        }
    } catch (...) {
        // Fallback to getrpcinfo
        try {
            auto r = c.call("rpc.info", Json::Value());
            if (r.isObject() && r.isMember("active_commands")) {
                for (const auto& cmd : r["active_commands"]) {
                    if (cmd.isString()) {
                        daemon_methods.push_back(cmd.asString());
                    }
                }
            }
        } catch (...) {
            // Final fallback: parse help output
            try {
                auto r = c.call("help", Json::Value());
                if (r.isString()) {
                    std::string help_text = r.asString();
                    std::istringstream iss(help_text);
                    std::string line;
                    while (std::getline(iss, line)) {
                        // Extract method names from help lines
                        if (!line.empty() && line[0] != '=' && line.find(' ') != std::string::npos) {
                            std::string method = line.substr(0, line.find(' '));
                            if (!method.empty()) {
                                daemon_methods.push_back(method);
                            }
                        }
                    }
                }
            } catch (...) {
                throw std::runtime_error("Unable to fetch daemon RPC methods");
            }
        }
    }
    
    // CLI implemented commands (simplified mapping)
    std::set<std::string> cli_commands = {
        "getbestblockhash", "getblockcount", "getblockchaininfo",
        "getnetworkinfo", "getpeerinfo", "getconnectioncount",
        "getmempoolinfo", "getrawmempool", "getrawtransaction", "decoderawtransaction",
        "getbalance", "getnewaddress", "listaddresses", "validateaddress",
        "listtransactions", "sendtoaddress", "sendmany",
        "getmininginfo", "setminingaddress", "getminingaddress",
        "help", "stop", "uptime"
    };
    
    // Calculate coverage
    std::set<std::string> daemon_set(daemon_methods.begin(), daemon_methods.end());
    std::vector<std::string> missing, extra;
    
    for (const auto& method : daemon_set) {
        if (cli_commands.find(method) == cli_commands.end()) {
            missing.push_back(method);
        }
    }
    
    for (const auto& cmd : cli_commands) {
        if (daemon_set.find(cmd) == daemon_set.end()) {
            extra.push_back(cmd);
        }
    }
    
    double coverage = daemon_methods.empty() ? 0.0 : 
        (double)(daemon_methods.size() - missing.size()) / daemon_methods.size() * 100.0;
    
    if (c.opt.format == OutputFormat::JSON) {
        dinero::cli::CliOverrides flags;
        flags.format = dinero::cli::OutputFormat::Json;
        flags.jsonSchema = c.opt.json_schema;
        
        auto ctx = makePrintCtx(c.opt.network, flags, "", "rpc-parity", 
                               c.opt.wallet_name.value_or(""));
        
        Json::Value data(Json::objectValue);
        data["daemon_methods_count"] = (int)daemon_methods.size();
        data["cli_commands_count"] = (int)cli_commands.size();
        data["coverage_percent"] = coverage;
        
        Json::Value missing_arr(Json::arrayValue);
        for (const auto& m : missing) missing_arr.append(m);
        data["missing_in_cli"] = missing_arr;
        
        Json::Value extra_arr(Json::arrayValue);
        for (const auto& e : extra) extra_arr.append(e);
        data["extra_in_cli"] = extra_arr;
        
        dinero::cli::printJsonEnvelope(ctx, data, true);
    } else {
        std::cout << "Dinero RPC Parity Matrix\n";
        std::cout << "============================\n";
        std::cout << "Daemon RPC Methods: " << daemon_methods.size() << "\n";
        std::cout << "CLI Commands:       " << cli_commands.size() << "\n";
        std::cout << "Coverage:           " << std::fixed << std::setprecision(1) << coverage << "%\n\n";
        
        if (!missing.empty()) {
            std::cout << "Missing in CLI (" << missing.size() << "):\n";
            for (const auto& m : missing) {
                std::cout << "  - " << m << "\n";
            }
            std::cout << "\n";
        }
        
        if (!extra.empty()) {
            std::cout << "Extra in CLI (" << extra.size() << "):\n";
            for (const auto& e : extra) {
                std::cout << "  + " << e << "\n";
            }
        }
    }
    
    return EXIT_OK;
}

// -------------------------------
// Dispatch
// -------------------------------
// Print nodeinfo with wallet scoping and JSON support
static void printNodeInfo(const NodeInfo& ni, const Options& opt) {
    std::string effective_url = ni.rpc_url;
    if (opt.wallet_name) {
        effective_url = withWalletScope(ni.rpc_url, *opt.wallet_name);
    }
    
    if (opt.format == OutputFormat::JSON) {
        dinero::cli::CliOverrides flags;
        flags.format = dinero::cli::OutputFormat::Json;
        flags.jsonSchema = opt.json_schema;
        
        auto ctx = makePrintCtx(ni.network, flags, effective_url, "nodeinfo", 
                               opt.wallet_name.value_or(""));
        
        Json::Value data(Json::objectValue);
        data["network"] = ni.network;
        data["rpc_url"] = ni.rpc_url;
        data["effective_url"] = effective_url;
        data["cookie_path"] = ni.cookie_path.string();
        data["datadir"] = ni.datadir.string();
        data["discovery_source"] = ni.discovery_source;
        if (!ni.nodeinfo_path.empty()) data["nodeinfo_path"] = ni.nodeinfo_path.string();
        if (opt.wallet_name) data["wallet_context"] = *opt.wallet_name;
        
        // Connection transparency: show timeout and security flags
        data["connect_timeout_ms"] = opt.connect_timeout_ms;
        data["read_timeout_ms"] = opt.read_timeout_ms;
        data["accept_insecure_cookie"] = opt.accept_insecure_cookie;
        
        // Add health and metrics data if available (dinerod vNext integration)
        std::string baseUrl = "http://" + dinero::cli::extractHost(ni.rpc_url) + ":" + dinero::cli::extractPort(ni.rpc_url);
        
        if (auto health = dinero::cli::HealthClient::getHealthStatus(baseUrl)) {
            Json::Value healthData(Json::objectValue);
            healthData["ok"] = health->ok;
            healthData["tip_height"] = health->tipHeight;
            healthData["peers"] = health->peers;
            healthData["mempool_size"] = health->mempoolSize;
            healthData["status"] = health->status;
            if (health->error) {
                healthData["error"] = *health->error;
            }
            data["health"] = healthData;
        }
        
        if (opt.verbose) {
            if (auto metrics = dinero::cli::HealthClient::getKeyMetrics(baseUrl)) {
                Json::Value metricsData(Json::objectValue);
                metricsData["chain_tip"] = metrics->chainTip;
                metricsData["peers"] = metrics->peers;
                metricsData["mempool_tx_count"] = metrics->mempoolTxCount;
                metricsData["uptime_seconds"] = metrics->uptimeSeconds;
                metricsData["rpc_calls_total"] = metrics->rpcCallsTotal;
                data["metrics"] = metricsData;
            }
        }
        
        dinero::cli::printJsonEnvelope(ctx, data, true);
    } else {
        std::cout << "Dinero CLI NodeInfo\n";
        std::cout << "  Network:        " << ni.network << "\n";
        std::cout << "  RPC URL:        " << ni.rpc_url << " (" << ni.discovery_source << ")\n";
        std::cout << "  Cookie File:    " << ni.cookie_path.string() << "\n";
        std::cout << "  Data Dir:       " << ni.datadir.string() << "\n";
        if (opt.wallet_name) {
            std::cout << "  Wallet Context: " << *opt.wallet_name << "\n";
            std::cout << "  Effective URL:  " << effective_url << "\n";
        }
        if (!ni.nodeinfo_path.empty()) {
            std::cout << "  NodeInfo Path:  " << ni.nodeinfo_path << "\n";
        }
        // Connection transparency: show timeout and security flags
        std::cout << "  Connect Timeout: " << opt.connect_timeout_ms << " ms\n";
        std::cout << "  Read Timeout:    " << opt.read_timeout_ms << " ms\n";
        std::cout << "  Security Mode:   " << (opt.accept_insecure_cookie ? "INSECURE" : "SECURE") << "\n";
        
        // Add health and metrics data if available (dinerod vNext integration)
        std::string baseUrl = "http://" + dinero::cli::extractHost(ni.rpc_url) + ":" + dinero::cli::extractPort(ni.rpc_url);
        
        if (auto health = dinero::cli::HealthClient::getHealthStatus(baseUrl)) {
            std::cout << "\nHealth Status:\n";
            std::cout << "  Status:         " << (health->ok ? "OK" : "ERROR") << "\n";
            std::cout << "  Tip Height:     " << health->tipHeight << "\n";
            std::cout << "  Peers:          " << health->peers << "\n";
            std::cout << "  Mempool Size:   " << health->mempoolSize << " tx\n";
            if (health->error) {
                std::cout << "  Error:          " << *health->error << "\n";
            }
        }
        
        if (opt.verbose) {
            if (auto metrics = dinero::cli::HealthClient::getKeyMetrics(baseUrl)) {
                std::cout << "\nKey Metrics:\n";
                std::cout << "  Chain Tip:      " << metrics->chainTip << "\n";
                std::cout << "  Connected Peers:" << metrics->peers << "\n";
                std::cout << "  Mempool Txs:    " << metrics->mempoolTxCount << "\n";
                std::cout << "  Uptime:         " << std::fixed << std::setprecision(1) 
                         << metrics->uptimeSeconds << " seconds\n";
                std::cout << "  RPC Calls:      " << metrics->rpcCallsTotal << "\n";
            }
        }
    }
}

int main(int argc, char** argv) try {
    Options opt = parse_args(argc, argv);
    
    // Handle --version flag early (print and exit)
    if (opt.version) {
        dinero::cli::printVersion();
        return EXIT_OK;
    }
    
    NodeInfo ni = load_nodeinfo(opt);
    
    // Security hardening: Check cookie file permissions
    if (!opt.accept_insecure_cookie) {
        auto [status, msg] = dinero::cli::CheckCookiePermissions(ni.cookie_path.string());
        if (status != dinero::cli::CookiePermStatus::Ok) {
            std::cerr << "Cookie security error: " << msg << "\n";
            std::cerr << "Cookie file: " << ni.cookie_path.string() << "\n";
            std::cerr << "Use --accept-insecure-cookie to bypass (NOT for production)\n";
            std::cerr << "Or set DIN_CLI_ACCEPT_INSECURE_COOKIE=1\n";
            return EXIT_SECURITY_ERROR;
        }
    } else {
        std::cerr << "WARNING: Cookie permission checks bypassed (insecure mode)\n";
    }
    
    // Handle --nodeinfo flag (print and exit)
    if (opt.show_nodeinfo) {
        printNodeInfo(ni, opt);
        return EXIT_OK;
    }
    
    if (opt.argv_tail.empty()) {
        std::cerr << "No command given. Try: dinero-cli status (or --help)\n";
        return EXIT_USAGE;
    }

    Client cli(opt, ni);
    
    // Wait for daemon ready if requested
    if (opt.wait_ready) {
        if (!cli.wait_for_ready()) {
            print_err("Daemon not ready after " + std::to_string(opt.wait_timeout) + " seconds", opt.color_output);
            return EXIT_NOT_READY;
        }
    }
    
    const std::string cmd = opt.argv_tail[0];
    std::vector<std::string> rest(opt.argv_tail.begin()+1, opt.argv_tail.end());
    
    // Debug: Print command bytes to help diagnose parsing issues
    if (getenv("DIN_DEBUG_CMD")) {
        fprintf(stderr, "cmd='%s' bytes:", cmd.c_str());
        for (unsigned char b : cmd) fprintf(stderr, " %02X", b);
        fputc('\n', stderr);
    }
    
    // Convert command to lowercase safely
    auto to_lower_copy = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char ch){ return static_cast<char>(std::tolower(ch)); });
        return s;
    };
    
    const std::string command = to_lower_copy(cmd);

    // Top-level commands
    if (command=="status") return cmd_status(cli);

    // nodeinfo ...
    if (command=="nodeinfo") {
        if (rest.empty()) throw std::runtime_error("nodeinfo <print|path>");
        std::string sub = to_lower_copy(rest[0]);
        if (sub=="print") return cmd_nodeinfo_print(cli);
        if (sub=="path") return cmd_nodeinfo_path(cli);
        throw std::runtime_error("Unknown nodeinfo subcommand: " + sub);
    }

    // doctor
    if (command=="doctor") return cmd_doctor(cli);

    // wallet ...
    if (command=="wallet") {
        if (rest.empty()) throw std::runtime_error("wallet <create|load|info|balance|history|utxos|addresses|newaddress|encrypt|lock|unlock|change-passphrase|backup|export>");
        std::string sub = to_lower_copy(rest[0]); std::vector<std::string> a(rest.begin()+1, rest.end());
        if (sub=="create") return cmd_wallet_create(cli, a);
        if (sub=="load") return cmd_wallet_load(cli, a);
        if (sub=="info") return cmd_wallet_info(cli);
        if (sub=="balance") return cmd_wallet_balance(cli);
        if (sub=="history") return cmd_wallet_history(cli, a);
        if (sub=="utxos") return cmd_wallet_utxos(cli, a);
        if (sub=="addresses") return cmd_wallet_addresses(cli);
        if (sub=="newaddress") return cmd_wallet_newaddress(cli, a);
        if (sub=="encrypt") return cmd_wallet_encrypt(cli);
        if (sub=="lock") return cmd_wallet_lock(cli);
        if (sub=="unlock") return cmd_wallet_unlock(cli);
        if (sub=="change-passphrase") return cmd_wallet_change_passphrase(cli);
        if (sub=="backup") return cmd_wallet_backup(cli, a);
        if (sub=="export") return cmd_wallet_export(cli, a);
        throw std::runtime_error("Unknown wallet subcommand: " + sub);
    }

    // addr ...
    if (command=="addr") {
        if (rest.size()<2) throw std::runtime_error("addr validate ADDRESS");
        if (to_lower_copy(rest[0])!="validate") throw std::runtime_error("addr validate ADDRESS");
        return cmd_addr_validate(cli, {rest.begin()+1, rest.end()});
    }

    // send ...
    if (command=="send") return cmd_send_enhanced(cli, rest);

    // mining ...
    if (command=="mining") {
        if (rest.empty()) throw std::runtime_error("mining <info|setaddress|getaddress|start|stop|setthreads|generatetoaddress>");
        std::string sub = to_lower_copy(rest[0]); std::vector<std::string> a(rest.begin()+1, rest.end());
        if (sub=="info") return cmd_mining_info(cli);
        if (sub=="setaddress") return cmd_mining_setaddr(cli, a);
        if (sub=="getaddress") return cmd_mining_getaddr(cli);
        if (sub=="start") return cmd_mining_start(cli, a);
        if (sub=="stop") return cmd_mining_stop(cli);
        if (sub=="setthreads") return cmd_mining_setthreads(cli, a);
        if (sub=="generatetoaddress") return cmd_mining_generateto(cli, a);
        throw std::runtime_error("Unknown mining subcommand: " + sub);
    }

    // chain ...
    if (command=="chain") {
        if (rest.empty()) throw std::runtime_error("chain <tip|info|count|getblockhash|getblock>");
        std::string sub = to_lower_copy(rest[0]); std::vector<std::string> a(rest.begin()+1, rest.end());
        if (sub=="tip") return cmd_chain_tip(cli);
        if (sub=="info") return cmd_chain_info(cli);
        if (sub=="count") return cmd_chain_count(cli);
        if (sub=="getblockhash") return cmd_chain_getblockhash(cli, a);
        if (sub=="getblock") return cmd_chain_getblock(cli, a);
        throw std::runtime_error("Unknown chain subcommand: " + sub);
    }

    // net ...
    if (command=="net") {
        if (rest.empty()) throw std::runtime_error("net <info|peers|connections>");
        std::string sub = to_lower_copy(rest[0]);
        if (sub=="info") return cmd_net_info(cli);
        if (sub=="peers") return cmd_net_peers(cli);
        if (sub=="connections") return cmd_net_connections(cli);
        throw std::runtime_error("Unknown net subcommand: " + sub);
    }

    // mempool ...
    if (command=="mempool") {
        if (rest.empty()) throw std::runtime_error("mempool <info|raw>");
        std::string sub = to_lower_copy(rest[0]);
        if (sub=="info") return cmd_mempool_info(cli);
        if (sub=="raw") return cmd_mempool_raw(cli);
        throw std::runtime_error("Unknown mempool subcommand: " + sub);
    }

    // tx ...
    if (command=="tx") {
        if (rest.empty()) throw std::runtime_error("tx <get|decode>");
        std::string sub = to_lower_copy(rest[0]); std::vector<std::string> a(rest.begin()+1, rest.end());
        if (sub=="get") return cmd_tx_get(cli, a);
        if (sub=="decode") return cmd_tx_decode(cli, a);
        throw std::runtime_error("Unknown tx subcommand: " + sub);
    }

    // rpc passthrough and parity
    if (command=="rpc") {
        if (!rest.empty() && (to_lower_copy(rest[0]) == "parity" || to_lower_copy(rest[0]) == "rpc-parity")) {
            return cmd_rpc_parity(cli, {rest.begin()+1, rest.end()});
        }
        return cmd_rpc_passthrough(cli, rest);
    }
    
    // Direct rpc-parity command
    if (command=="rpc-parity") return cmd_rpc_parity(cli, rest);

    throw std::runtime_error("Unknown command: " + command);
}
catch (const std::exception& e) {
    std::string msg = e.what();
    
    // Map common errors to appropriate exit codes
    if (msg.find("HTTP 401") != std::string::npos || msg.find("Unauthorized") != std::string::npos) {
        print_err("Authentication failed: " + msg, true);
        return EXIT_AUTH;
    }
    if (msg.find("Connection refused") != std::string::npos || msg.find("Service unavailable") != std::string::npos) {
        print_err("Service unavailable: " + msg, true);
        return EXIT_SERVICE_UNAVAILABLE;
    }
    if (msg.find("network mismatch") != std::string::npos || msg.find("Network mismatch") != std::string::npos) {
        print_err("Network mismatch: " + msg, true);
        return EXIT_NETWORK_MISMATCH;
    }
    if (msg.find("Missing value") != std::string::npos || msg.find("Unknown command") != std::string::npos) {
        print_err("Usage error: " + msg, true);
        return EXIT_USAGE;
    }
    
    print_err(msg, true);
    return EXIT_USAGE;
}
