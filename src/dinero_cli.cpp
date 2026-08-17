/**
 * dinero-cli - Dinero command-line RPC client
 *
 * Production-quality CLI for interacting with dinerod daemon.
 *
 * Architecture:
 * - Uses unified rpc_client library (src/rpc_client.cpp)
 * - Cookie authentication from ~/.dinero/.cookie
 * - Generic RPC method forwarding
 * - Pretty JSON output
 *
 * Usage:
 *   dinero-cli [options] <method> [params...]
 *
 * Examples:
 *   dinero-cli getblockcount
 *   dinero-cli getbalance
 *   dinero-cli sendtoaddress din1q... 10.5
 *   dinero-cli -datadir=/custom/path getinfo
 */

#include "rpc_client.h"
#include "consensus/chain_identity.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <optional>

// Default configuration
static const char* DEFAULT_RPC_HOST = "127.0.0.1";
static const uint16_t DEFAULT_RPC_PORT = 20998;
static const uint16_t REGTEST_RPC_PORT = 20996;

// Get default data directory
std::string get_default_datadir() {
    const char* home = std::getenv("HOME");
    if (!home) {
        return ".dinero";
    }
    return std::string(home) + "/.dinero";
}

std::string join_path(const std::string& base, const std::string& child) {
    if (base.empty()) return child;
    if (child.empty()) return base;
    if (base.back() == '/') return base + child;
    return base + "/" + child;
}

// Parse command line arguments
struct CliConfig {
    std::string host = DEFAULT_RPC_HOST;
    uint16_t port = DEFAULT_RPC_PORT;
    std::string datadir = get_default_datadir();
    std::string username;
    std::string password;
    std::string command;
    std::vector<std::string> params;
    bool use_cookie_auth = true;
    bool json_output = false;  // --json flag (currently scoped to `health`)
    bool datadir_explicit = false;
};

void print_usage() {
    std::cout << "dinero-cli - Dinero RPC client\n\n";
    std::cout << dinero::consensus::kGenesisMotto << "\n\n";
    std::cout << "Usage: dinero-cli [options] <method> [params...]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -datadir=<dir>        Data directory (default: ~/.dinero)\n";
    std::cout << "  -testnet              Use ~/.dinero/testnet unless -datadir is set\n";
    std::cout << "  -regtest              Use ~/.dinero/regtest and RPC port 20996 unless overridden\n";
    std::cout << "  -rpcport=<port>       RPC server port (default: 20998)\n";
    std::cout << "  -rpchost=<host>       RPC server host (default: 127.0.0.1)\n";
    std::cout << "  -rpcuser=<user>       RPC username (overrides cookie auth)\n";
    std::cout << "  -rpcpassword=<pass>   RPC password (overrides cookie auth)\n\n";

    std::cout << "Common RPC methods:\n";
    std::cout << "  Blockchain:\n";
    std::cout << "    getblockcount                     - Current block height\n";
    std::cout << "    getbestblockhash                  - Latest block hash\n";
    std::cout << "    getblock <hash>                   - Block information\n\n";

    std::cout << "  Wallet:\n";
    std::cout << "    getbalance                        - Wallet balance\n";
    std::cout << "    getnewaddress [type] [label]      - Generate new address\n";
    std::cout << "    listaddresses                     - List wallet addresses\n";
    std::cout << "    sendtoaddress <address> <amount>  - Send coins\n\n";

    std::cout << "  Network:\n";
    std::cout << "    getpeerinfo                       - Connected peers\n";
    std::cout << "    getmempoolinfo                    - Mempool status\n\n";

    std::cout << "  System:\n";
    std::cout << "    getinfo                           - Daemon information\n";
    std::cout << "    rpc.version                       - RPC version info\n";
    std::cout << "    rpc.listmethods                   - List all methods\n";
    std::cout << "    consensus.checkdb                 - Database integrity\n";
    std::cout << "    help                              - Show this help\n\n";

    std::cout << "Examples:\n";
    std::cout << "  dinero-cli getblockcount\n";
    std::cout << "  dinero-cli getbalance\n";
    std::cout << "  dinero-cli getnewaddress p2mr\n";
    std::cout << "  dinero-cli getnewaddress --type p2mr --label savings\n";
    std::cout << "  dinero-cli sendtoaddress din1q... 10.5\n";
    std::cout << "  dinero-cli dpi.createinvoice '{\"amount\":0.01,\"memo\":\"coffee\",\"expiry_seconds\":300}'\n";
    std::cout << "  dinero-cli -datadir=/custom/path getinfo\n\n";
}

static std::string to_lower_copy(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

static bool is_getnewaddress_command(const std::string& command) {
    const std::string normalized = to_lower_copy(command);
    return normalized == "getnewaddress" || normalized == "wallet.getnewaddress";
}

static bool parse_json_arg(const std::string& arg, Json::Value& value) {
    if (arg.empty() || (arg[0] != '{' && arg[0] != '[')) {
        return false;
    }

    Json::CharReaderBuilder reader_builder;
    std::string errs;
    std::istringstream arg_stream(arg);
    return Json::parseFromStream(reader_builder, arg_stream, &value, &errs);
}

static bool append_getnewaddress_params(const std::vector<std::string>& raw_params,
                                        Json::Value& params,
                                        std::string& error) {
    std::optional<std::string> address_type;
    std::optional<std::string> label;

    for (size_t i = 0; i < raw_params.size(); ++i) {
        const std::string& arg = raw_params[i];
        if (arg == "--type") {
            if (++i >= raw_params.size()) {
                error = "Missing value for --type";
                return false;
            }
            address_type = raw_params[i];
            continue;
        }
        if (arg.rfind("--type=", 0) == 0) {
            address_type = arg.substr(std::strlen("--type="));
            continue;
        }
        if (arg == "--label") {
            if (++i >= raw_params.size()) {
                error = "Missing value for --label";
                return false;
            }
            label = raw_params[i];
            continue;
        }
        if (arg.rfind("--label=", 0) == 0) {
            label = arg.substr(std::strlen("--label="));
            continue;
        }
        if (arg == "--p2mr") {
            address_type = "p2mr";
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            error = "Unknown getnewaddress option: " + arg;
            return false;
        }
        if (!address_type.has_value()) {
            address_type = arg;
            continue;
        }
        if (!label.has_value()) {
            label = arg;
            continue;
        }
        error = "Too many getnewaddress arguments";
        return false;
    }

    if (address_type.has_value()) {
        params.append(*address_type);
    }
    if (label.has_value()) {
        if (!address_type.has_value()) {
            params.append("taproot");
        }
        params.append(*label);
    }

    return true;
}

bool parse_args(int argc, char* argv[], CliConfig& config) {
    if (argc < 2) {
        return false;
    }

    std::optional<std::string> network;
    bool rpcport_explicit = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (!config.command.empty()) {
            // --json applies to the command's output rendering. Currently
            // only `health` honors it; other commands ignore it (they
            // already print JSON). Consume the flag so it doesn't get
            // forwarded as an RPC param.
            if (arg == "--json") {
                config.json_output = true;
                continue;
            }
            config.params.push_back(arg);
            continue;
        }

        if (arg.rfind("-rpcport=", 0) == 0) {
            config.port = std::stoi(arg.substr(9));
            rpcport_explicit = true;
        } else if (arg.rfind("-rpchost=", 0) == 0) {
            config.host = arg.substr(9);
        } else if (arg.rfind("-datadir=", 0) == 0) {
            config.datadir = arg.substr(9);
            config.datadir_explicit = true;
        } else if (arg.rfind("-rpcuser=", 0) == 0) {
            config.username = arg.substr(9);
            config.use_cookie_auth = false;
        } else if (arg.rfind("-rpcpassword=", 0) == 0) {
            config.password = arg.substr(13);
            config.use_cookie_auth = false;
        } else if (arg == "-testnet" || arg == "--testnet") {
            network = "testnet";
        } else if (arg == "-regtest" || arg == "--regtest") {
            network = "regtest";
        } else if (arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << std::endl;
            return false;
        } else {
            // First non-option argument is the command
            config.command = arg;
        }
    }

    if (network.has_value() && !config.datadir_explicit) {
        config.datadir = join_path(get_default_datadir(), *network);
    }
    if (network == "regtest" && !rpcport_explicit) {
        config.port = REGTEST_RPC_PORT;
    }

    return !config.command.empty();
}

// Pretty-print JSON result
void print_json(const Json::Value& json) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    writer->write(json, &std::cout);
    std::cout << std::endl;
}

int main(int argc, char* argv[]) {
    CliConfig config;

    if (!parse_args(argc, argv, config)) {
        print_usage();
        return 1;
    }

    if (config.command == "help" || config.command == "--help" || config.command == "-h") {
        print_usage();
        return 0;
    }

    // Phase D.4 / E.3 — `version` command. Reports package + build
    // metadata + bundled-static dependency identities. Pure compile-
    // time data; does NOT require a running daemon. Spec §1.4
    // contract; PackageGate validates this schema in --static AND
    // --installed modes.
    if (config.command == "version") {
        Json::Value root;
        root["dinerod"]["version"]    = DINERO_PACKAGE_VERSION;
        root["dinerod"]["commit"]     = DINERO_BUILD_GIT_SHA;
        root["dinerod"]["build_date"] = DINERO_BUILD_DATE_ISO;
        // bundled-static deps. Per spec §1.4 nuance: when there is
        // no separate .so artifact, do NOT fabricate a sha256 — emit
        // the version + linkage only. Operators / CVE responders
        // identify the bundled artifact by version + git commit of
        // the parent package.
        auto add_static = [&](const char* name, const char* ver) {
            Json::Value e;
            e["version"] = ver;
            e["linkage"] = "static";
            root["bundled_libs"][name] = e;
        };
        add_static("openssl",       DINERO_BUNDLED_OPENSSL_VERSION);
        add_static("rocksdb",       DINERO_BUNDLED_ROCKSDB_VERSION);
        add_static("secp256k1_zkp", DINERO_BUNDLED_SECP256K1_ZKP_VERSION);
        add_static("jsoncpp",       DINERO_BUNDLED_JSONCPP_VERSION);
        add_static("zstd",          DINERO_BUNDLED_ZSTD_VERSION);

        if (config.json_output) {
            print_json(root);
        } else {
            std::cout << "dinerod " << root["dinerod"]["version"].asString()
                      << " (" << root["dinerod"]["commit"].asString() << ")"
                      << " built " << root["dinerod"]["build_date"].asString()
                      << "\nbundled (static):\n";
            for (const auto& name : root["bundled_libs"].getMemberNames()) {
                std::cout << "  " << name << " "
                          << root["bundled_libs"][name]["version"].asString() << "\n";
            }
        }
        return 0;
    }

    // Create RPC client
    dinero::rpc::RpcClient* client;
    if (config.use_cookie_auth) {
        client = new dinero::rpc::RpcClient(config.host, config.port, config.datadir);
    } else {
        client = new dinero::rpc::RpcClient(config.host, config.port, config.username, config.password);
    }

    // Connect to daemon
    if (!client->connect()) {
        // Spec §Standard 10: `dinero-cli health` MUST return exit 2 +
        // "FAILING <reason>" when the daemon is unreachable. Operators
        // and monitoring scripts depend on the exit-code matrix being
        // honored regardless of whether the failure is "daemon down" or
        // "daemon reports failing." Emit the spec'd token here, before
        // falling through to the generic-error path the other commands
        // use.
        if (config.command == "health") {
            const std::string reason =
                "daemon unreachable on " + config.host + ":" +
                std::to_string(config.port) + " — " + client->get_last_error();
            if (config.json_output) {
                Json::Value r;
                r["status"] = "FAILING";
                r["exit_code"] = 2;
                r["reason"] = reason;
                print_json(r);
            } else {
                std::cout << "FAILING " << reason << std::endl;
            }
            delete client;
            return 2;
        }
        std::cerr << "Error: Failed to connect to daemon\n";
        std::cerr << "  " << client->get_last_error() << std::endl;
        std::cerr << "\nMake sure dinerod is running on " << config.host << ":" << config.port << std::endl;
        delete client;
        return 1;
    }

    // Build JSON-RPC parameters. Positional CLI arguments remain an array, but a
    // single JSON object/array argument is passed through as raw JSON-RPC params
    // so named-parameter methods receive exactly what the user typed.
    Json::Value params(Json::arrayValue);
    if (is_getnewaddress_command(config.command)) {
        std::string error;
        if (!append_getnewaddress_params(config.params, params, error)) {
            std::cerr << "Error: " << error << std::endl;
            std::cerr << "Usage: dinero-cli " << config.command
                      << " [taproot|p2mr] [label]" << std::endl;
            std::cerr << "   or: dinero-cli " << config.command
                      << " --type <taproot|p2mr> [--label <label>]" << std::endl;
            return 1;
        }
    } else {
        Json::Value raw_json_param;
        if (config.params.size() == 1 && parse_json_arg(config.params.front(), raw_json_param)) {
            params = raw_json_param;
        } else {
            for (const auto& param : config.params) {
                // Smart parameter parsing: detect JSON objects/arrays, numbers, or strings

                // Check if parameter is JSON object or array
                Json::Value json_param;
                if (parse_json_arg(param, json_param)) {
                    params.append(json_param);  // Valid JSON object/array
                    continue;
                }
                // If JSON parsing fails, fall through to treat as string

                // Try parsing as number
                try {
                    size_t pos;
                    double d = std::stod(param, &pos);
                    if (pos == param.length()) {
                        // Full string is a number
                        if (param.find('.') != std::string::npos) {
                            params.append(d);  // Float
                        } else {
                            params.append(static_cast<int>(d));  // Integer
                        }
                    } else {
                        params.append(param);  // String with non-numeric chars
                    }
                } catch (...) {
                    params.append(param);  // Not a number - use as string
                }
            }
        }
    }

    // Execute RPC call (generic - forwards any method)
    std::optional<Json::Value> result = client->call(config.command, params);

    // Print result
    if (result) {
        // Defensive: a well-formed JSON-RPC reply is an object. If the server or
        // a proxy returns anything else (e.g. a bare-string error body), do NOT
        // call isMember()/operator[] on it — jsoncpp's find() throws
        // Json::LogicError on a non-object/non-null value, which is uncaught and
        // aborts the process. Print it and fail cleanly instead.
        if (!result->isObject()) {
            std::cerr << "RPC Error: malformed (non-object) response: ";
            print_json(*result);
            delete client;
            return 1;
        }
        // Check if there's an error in the response
        if (result->isMember("error") && !(*result)["error"].isNull()) {
            std::cerr << "RPC Error: ";
            print_json((*result)["error"]);
            delete client;
            return 1;
        }

        // Phase D.4 (Dinero Core 1.0): special-case `health`. The contract
        // requires:
        //   - Default text mode: print "OK" / "DEGRADED <reason>" /
        //     "FAILING <reason>" to stdout, exit 0/1/2 from result.exit_code.
        //   - --json mode: pretty-print the full result JSON, same exit code.
        // Other commands keep the legacy "always pretty-print, exit 0"
        // behavior.
        if (config.command == "health" && result->isMember("result")) {
            const Json::Value& r = (*result)["result"];
            const std::string status = r.get("status", "UNKNOWN").asString();
            const int exit_code = r.get("exit_code", 1).asInt();
            const std::string reason = r.get("reason", "").asString();

            if (config.json_output) {
                print_json(r);
            } else {
                std::cout << status;
                if (!reason.empty()) {
                    std::cout << " " << reason;
                }
                std::cout << std::endl;
            }
            delete client;
            return exit_code;
        }

        // Print the result field
        if (result->isMember("result")) {
            print_json((*result)["result"]);
        } else {
            // Fallback: print entire response
            print_json(*result);
        }
    } else {
        std::cerr << "Error: " << client->get_last_error() << std::endl;
        if (client->get_last_error_code() != 0) {
            std::cerr << "Error code: " << client->get_last_error_code() << std::endl;
        }
        delete client;
        return 1;
    }

    delete client;
    return 0;
}
