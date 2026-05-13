#include <CLI/CLI.hpp>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "crypto/crypto_init.hpp"
// #include <json/json.h>  // Removed for simplicity

// ===== GLOBAL FLAGS (PODs only, no work) =====
struct GlobalFlags {
    std::string rpc_url = "http://127.0.0.1:8332";
    std::string rpc_user = "";
    std::string rpc_pass = "";
    std::string datadir = "./mining_stats";
    bool json = false;
    bool verbose = false;
};

static GlobalFlags g_flags;

// ===== LAZY RPC CLIENT =====
struct RpcClient {
    std::string url, userpass;
    bool verbose;
    
    std::string call(const std::string& method, const std::vector<std::string>& params = {}) {
        if (verbose) {
            std::cerr << "[RPC] " << method << " params=";
            for (const auto& p : params) std::cerr << p << " ";
            std::cerr << std::endl;
        }
        
        // TODO: Replace with actual RPC implementation
        std::cout << "🔗 RPC Call: " << method;
        if (!params.empty()) {
            std::cout << " with params: ";
            for (const auto& p : params) std::cout << p << " ";
        }
        std::cout << std::endl;
        return "mock_response_for_" + method;
    }
};

static RpcClient& rpc() {
    static std::unique_ptr<RpcClient> inst;
    if (!inst) {
        auto r = std::make_unique<RpcClient>();
        r->url = g_flags.rpc_url;
        r->userpass = g_flags.rpc_user + ":" + g_flags.rpc_pass;
        r->verbose = g_flags.verbose;
        inst = std::move(r);
    }
    return *inst;
}

// ===== LAZY DATABASE =====
struct Database {
    std::string path;
    bool initialized = false;
    
    void open(const std::string& db_path) {
        if (!initialized) {
            path = db_path;
            initialized = true;
            if (g_flags.verbose) {
                std::cout << "📊 Database opened: " << path << std::endl;
            }
        }
    }
};

static Database& db() {
    static std::unique_ptr<Database> inst;
    if (!inst) {
        auto d = std::make_unique<Database>();
        d->open(g_flags.datadir);
        inst = std::move(d);
    }
    return *inst;
}

// ===== COMMAND REGISTRATION FUNCTIONS =====

void register_blockchain(CLI::App& app) {
    auto* blockchain = app.add_subcommand("blockchain", "Blockchain operations");
    
    // height -> getblockcount
    auto* height = blockchain->add_subcommand("height", "Show current block height");
    height->callback([](){
        std::cout << "📏 Getting blockchain height..." << std::endl;
        auto result = rpc().call("getblockcount");
        std::cout << "✅ Current height: " << result << std::endl;
    });
    
    // getblockhash <n>
    auto* getblockhash = blockchain->add_subcommand("getblockhash", "Get block hash by height");
    static int block_height = 0;
    getblockhash->add_option("height", block_height, "Block height")->required();
    getblockhash->callback([](){
        std::cout << "🔗 Getting block hash for height " << block_height << std::endl;
        auto result = rpc().call("getblockhash", {std::to_string(block_height)});
        std::cout << "✅ Block hash: " << result << std::endl;
    });
    
    // listunspent
    auto* listunspent = blockchain->add_subcommand("listunspent", "List unspent outputs");
    listunspent->callback([](){
        std::cout << "💰 Listing unspent outputs..." << std::endl;
        auto result = rpc().call("listunspent");
        std::cout << "✅ UTXO data: " << result << std::endl;
    });
    
    // stop
    auto* stop = blockchain->add_subcommand("stop", "Stop the daemon");
    stop->callback([](){
        std::cout << "🛑 Stopping Dinero daemon..." << std::endl;
        auto result = rpc().call("stop");
        std::cout << "✅ Daemon shutdown initiated" << std::endl;
    });
}

void register_miner(CLI::App& app) {
    auto* miner = app.add_subcommand("miner", "Mining operations");
    
    // start
    auto* start = miner->add_subcommand("start", "Start mining");
    static int threads = 4;
    start->add_option("--threads", threads, "Number of threads (default: 4)");
    start->callback([](){
        std::cout << "🚀 Starting miner with " << threads << " threads..." << std::endl;
        auto result = rpc().call("setgenerate", {"true", std::to_string(threads)});
        std::cout << "✅ Mining started successfully!" << std::endl;
    });
    
    // stop
    auto* stop = miner->add_subcommand("stop", "Stop mining");
    stop->callback([](){
        std::cout << "🛑 Stopping miner..." << std::endl;
        auto result = rpc().call("setgenerate", {"false"});
        std::cout << "✅ Mining stopped successfully!" << std::endl;
    });
    
    // status
    auto* status = miner->add_subcommand("status", "Show mining status");
    status->callback([](){
        std::cout << "⛏️ Getting mining status..." << std::endl;
        auto result = rpc().call("getmininginfo");
        std::cout << "✅ Mining info: " << result << std::endl;
    });
}

void register_wallet(CLI::App& app) {
    auto* wallet = app.add_subcommand("wallet", "Wallet operations");
    
    // addr new
    auto* addr = wallet->add_subcommand("addr", "Address operations");
    auto* addr_new = addr->add_subcommand("new", "Generate new address");
    static std::string addr_type = "bech32";
    addr_new->add_option("--type", addr_type, "Address type (bech32, p2pkh)");
    addr_new->callback([](){
        std::cout << "🏠 Generating new " << addr_type << " address..." << std::endl;
        auto result = rpc().call("getnewaddress", {"", addr_type});
        std::cout << "✅ New address: " << result << std::endl;
    });
    
    // balance
    auto* balance = wallet->add_subcommand("balance", "Show wallet balance");
    balance->callback([](){
        std::cout << "💰 Getting wallet balance..." << std::endl;
        auto result = rpc().call("getbalance");
        std::cout << "✅ Balance: " << result << " DIN" << std::endl;
    });
}

void register_control(CLI::App& app) {
    // Top-level essential commands
    auto* height = app.add_subcommand("height", "Quick blockchain height");
    height->callback([](){
        std::cout << "📏 Current Block Height: ";
        auto result = rpc().call("getblockcount");
        std::cout << result << std::endl;
    });
    
    auto* stop = app.add_subcommand("stop", "Stop daemon");
    stop->callback([](){
        std::cout << "🛑 Stopping daemon..." << std::endl;
        rpc().call("stop");
        std::cout << "✅ Shutdown initiated" << std::endl;
    });
}

void wire_global_flags(CLI::App& app, GlobalFlags& gf) {
    app.add_option("--rpc-url", gf.rpc_url, "RPC URL (default: http://127.0.0.1:8332)");
    app.add_option("--rpc-user", gf.rpc_user, "RPC username");
    app.add_option("--rpc-pass", gf.rpc_pass, "RPC password");
    app.add_option("--datadir", gf.datadir, "Data directory (default: ./mining_stats)");
    app.add_flag("-j,--json", gf.json, "JSON output");
    app.add_flag("-v,--verbose", gf.verbose, "Verbose output");
}

// ===== CLEAN MAIN =====
int main(int argc, char** argv) {
    // Initialize crypto system first
    try {
        CryptoInit::init();
    } catch (const std::exception& e) {
        std::cerr << "Crypto initialization failed: " << e.what() << std::endl;
        return 1;
    }
    
    CLI::App app{"Dinero CLI - Enterprise Edition"};
    
    // Check for self-test flag first
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--selftest-crypto") {
            bool ok = CryptoInit::self_test();
            std::cout << (ok ? "crypto self-test: OK" : "crypto self-test: FAIL") << "\n";
            return ok ? 0 : 1;
        }
    }

    // 1) Light, parse-only options (no heavy work)
    wire_global_flags(app, g_flags);

    // 2) Register subcommands (NO heavy code)
    register_blockchain(app);
    register_miner(app);
    register_wallet(app);
    register_control(app);

    // Debug aid (toggle on if needed)
    if (argc == 1) {  // Show help if no args
        std::cout << app.help() << std::endl;
        return 0;
    }

    try {
        // 3) Only parsing here - NO heavy initialization before this!
        CLI11_PARSE(app, argc, argv);
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    }
    
    return 0; // All work happens in callbacks
}
