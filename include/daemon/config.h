#pragma once

#include <string>
#include <vector>
#include <cstdint>

/**
 * Dinero Node Configuration
 * Manages daemon configuration parameters
 */
struct NodeConfig {
    // Network settings
    std::string datadir = "";
    std::string network = "main";
    int port = 8333;
    int p2p_port = 8333;
    int rpcport = 8332;
    int rpc_port = 8332;
    std::string rpcbind = "127.0.0.1";
    std::string rpc_bind = "127.0.0.1";
    std::string p2p_bind = "0.0.0.0";
    std::vector<std::string> rpcallowip = {"127.0.0.1"};
    
    // Directory paths
    std::string chain_dir = "";
    std::string founder_dir = "";
    std::string cookie_path = "";
    
    // Node behavior
    bool server = true;
    bool listen = true;
    bool daemon = false;  // Set by command line
    bool txindex = false;

    // Phase 7.4.1: Utreexo bridge mode
    bool utreexo_bridge = false;  // Advertise NODE_UTREEXO_BRIDGE service bit

    // Phase 7.4.3: Utreexo stateless mode
    bool utreexo_stateless = false;  // Sync as stateless node (no UTXO database)

    // Phase 3a of the shielded reorg invertibility plan
    // (docs/specs/atomic_consensus_persistence_phase3.md). Hidden,
    // default off — when on, ConnectTip / DisconnectTip route every
    // reorg-bound mutation through ConsensusWriteBatch instead of
    // the legacy per-write paths. Stays off on the live fleet until
    // the property tests + crash injections + leak test pass in all
    // three D3 configurations and the operator greenlights flipping
    // the default. Activated via -consensus.atomic_persist=1.
    bool consensus_atomic_persist = false;

    // Phase G: Parallel block download (10-20× IBD speedup)
    bool parallel_block_download = true;  // Enable parallel block download scheduler
    std::string sync_profile = "";
    bool allow_local_mining = true;
    bool allow_pool_mining = true;

    // Mining settings
    bool gen = false;
    int genproclimit = 0;  // 0 = auto-detect
    std::string generatetoaddress = "";
    
    // Wallet settings
    std::string wallet = "main";
    
    // RPC authentication
    std::string rpcuser = "";
    std::string rpcpassword = "";
    std::string rpcauth = "";
    std::string rpccookiefile = "";
    
    // Logging
    std::vector<std::string> debug;
    bool logips = false;
    bool shrinkdebugfile = true;
    
    // Connection limits
    int maxconnections = 125;
    uint64_t maxuploadtarget = 0;  // 0 = unlimited
    
    // Performance
    int dbcache = 450;  // MB
    int maxmempool = 300;  // MB

    // Mempool fee bumping policy
    // CPFP: Always enabled (non-controversial, user-friendly)
    // RBF: Opt-in only (preserves payment finality for merchants)
    bool mempool_enable_rbf = false;   // RBF off by default (opt-in via config)
    bool mempool_enable_cpfp = true;   // CPFP on by default (safe fee bumping)

    // Security
    bool disablewallet = false;
    bool printtoconsole = false;
    
    // WebSocket RPC settings
    bool websocket_enabled = false;
    std::string websocket_path = "/rpc.ws";
    int websocket_max_connections = 100;
    
            // Rate limiting settings
        double rpc_rate_limit = 25.0;        // tokens per second
        double rpc_burst_limit = 50.0;       // max tokens
        bool rpc_local_bypass = true;        // bypass 127.0.0.1
        bool rpc_charge_per_item = false;    // charge per batch item (default: per request)
        int rpc_batch_limit = 100;           // max batch items
    
    // Constructor with defaults
    NodeConfig() = default;
};

/**
 * Get global node configuration instance
 */
NodeConfig& GetConfig();

/**
 * Load configuration from file and command line arguments
 */
bool LoadConfig(int argc, char* argv[]);

/**
 * Get configuration file path
 */
std::string GetConfigFile();

/**
 * Get data directory path
 */
std::string GetDataDir();

/**
 * Parse command line arguments
 */
void ParseCommandLine(int argc, char* argv[]);

/**
 * Load configuration file
 */
bool LoadConfigFile(const std::string& filepath);

/**
 * Validate configuration
 */
bool ValidateConfig();

/**
 * Print configuration help
 */
void PrintConfigHelp();
