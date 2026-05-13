#pragma once
#if !DIN_ENABLE_LEGACY_RPC
#  error "Legacy RPC header included with DIN_ENABLE_LEGACY_RPC=OFF"
#endif
#include <string>
#include <thread>
#include "compat/jsoncpp_compat.h"
#include "compat/net_compat.h"
#ifdef __APPLE__
#include <json/json.h>
#else
#include <jsoncpp/json/json.h>
#endif
#include <string>
#include <map>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include "../rpc/rpc_http.hpp"
#include "../rpc/rpc_types.h"
#include "../rpc/rpc_adapters.h"
#include "daemon/execution_context.h"
#include "consensus/coin_type.h"

// Forward declaration
class WalletHandlers;
class MiningEventBus;

// P2P forward declarations (dinero namespace)
namespace dinero {
    class PeerManager;
}
class HeadersSync;

namespace dinero {

// Forward declarations
class Blockchain;
class Mining;
class Mempool;
class MinerCore;
class WalletBalanceService;
class WalletManager;
// WebSocket bridge forward declaration (using new ws_bus implementation)
// class WsBridge;

// WebSocket subscriptions disabled - using new ws_bus implementation
// #include "daemon/ws_subscriptions.hpp"


// PSBT structures for transaction building and signing
struct PSBTInput {
    std::string txid;
    uint32_t vout;
    std::vector<uint8_t> scriptSig;
    std::vector<uint8_t> witness;
    std::vector<uint8_t> partialSigs;
    std::vector<uint8_t> sighashType;
    std::vector<uint8_t> redeemScript;
    std::vector<uint8_t> witnessScript;
    std::vector<uint8_t> bip32Derivs;
    std::vector<uint8_t> finalScriptSig;
    std::vector<uint8_t> finalScriptWitness;
    std::vector<uint8_t> ripemd160;
    std::vector<uint8_t> sha256;
    std::vector<uint8_t> hash160;
    std::vector<uint8_t> hash256;
    std::vector<uint8_t> unknown;
};

struct PSBTOutput {
    std::vector<uint8_t> redeemScript;
    std::vector<uint8_t> witnessScript;
    std::vector<uint8_t> bip32Derivs;
    std::vector<uint8_t> unknown;
};

struct PSBT {
    std::string globalTxVersion;
    std::vector<uint8_t> globalXpub;
    std::vector<uint8_t> globalTxModifiable;
    std::vector<uint8_t> globalVersion;
    std::vector<uint8_t> globalProprietary;
    std::vector<uint8_t> globalUnknown;
    std::vector<PSBTInput> inputs;
    std::vector<PSBTOutput> outputs;
};

// Network types - clean, focused implementation
enum class Net { MAIN, TEST, REGTEST };

struct NetParams {
    const char* hrp;
    uint32_t    coin_type;   // SLIP-44
    uint32_t    p2p_magic;   // if you have it
    const char* genesis_hex; // optional
};

class RPCServer {
public:
    RPCServer();
    ~RPCServer();
    
    bool initialize(int port = 8332);
    void shutdown();
    void start(); // Public method to start the server
    std::string handleRequest(const std::string& request);
    std::string handleRequest(const std::string& request, const std::string& path);
    
    // Get the actual bound port (useful after auto-port selection)
    int getActualPort() const { return m_port; }
    
    // Set references to blockchain, mining, and mempool components
    void setMining(Mining* mining) { m_mining = mining; }
    void setMiningEvents(MiningEventBus* events) { m_mining_events = events; }
    void setMempool(std::shared_ptr<Mempool> mempool) { m_mempool = mempool; }
    void setMinerCore(MinerCore* miner_core) { m_miner_core = miner_core; }
    void setWalletBalanceService(WalletBalanceService* wallet_balance_service) { m_wallet_balance_service = wallet_balance_service; }
    void setWalletLoaded(bool loaded) { m_wallet_loaded.store(loaded); }
    void setWalletManager(WalletManager* wallet_manager) { m_wallet_manager = wallet_manager; }
    WalletManager* getWalletManager() const { return m_wallet_manager; }
    
    // ExecutionContext for clean interface access (no globals)
    void setExecutionContext(const din::ExecutionContext& ctx) { execution_context_ = ctx; }
    const din::ExecutionContext& getExecutionContext() const { return execution_context_; }
    
    // Set P2P components
    void setPeerManager(dinero::PeerManager* peer_manager) { m_peer_manager = peer_manager; }
    void setHeadersSync(HeadersSync* headers_sync) { m_headers_sync = headers_sync; }
    
    // V2 RPC initialization
    void initializeV2Router();
    
    // Method registration for v2 RPC - using string interface
    void registerMethod(const std::string& name, std::function<std::string(const std::string&)> handler);
    
    // Set data directory for cookie authentication
    void setDataDir(const std::string& datadir) { m_data_dir = datadir; }
    
    // Set cookie path directly (overrides datadir-based path)
    void setCookiePath(const std::string& cookie_path) { m_cookie_path = cookie_path; }
    
    // WebSocket support via integrated subscriptions system
    
    // Method handler access for RPC integration - using string interface
    bool hasMethod(const std::string& method) const;
    std::string callMethod(const std::string& method, const std::string& params);
    
    // WebSocket event broadcasting
    void broadcastNewBlock(int height, const std::string& hash, int64_t time, double difficulty);
    void broadcastMempoolTx(const std::string& txid, double fee, int size);
    void broadcastMiningInfo(bool generating, int threads, double hashrate);
    
    // Initialize method handlers
    void initializeMethodHandlers();
    
    // Response helpers for v2 RPC - business handlers return Json::Value
    Json::Value createErrorResponse(int code, const std::string& message);
    Json::Value createSuccessResponse(const Json::Value& result);
    
    // Public wallet management methods for auto-wallet - using string interface
    // autoCreateWallet removed - wallet.create routes to WalletHandlers::create
    std::string autoLoadWallet(const std::string& params);
    // walletCreate removed - wallet.create routes to WalletHandlers::create
    
    // Health check endpoint - using string interface
    std::string getHealth();

private:
    void run();
    void handleClient(int client_socket, struct sockaddr_in client_addr);
    std::string parseHTTPRequest(const std::string& request);
    std::string createHTTPResponse(const std::string& body, int status_code = 200);
    std::string handleJSONRPC(const std::string& json_request);
    Json::Value handleSingleRequest(const Json::Value& request);
    
    // New robust RPC handler methods - using string interface
    std::string dispatchRpcMethod(const std::string& path, const std::string& method, const std::string& params, const std::string& id);
    
    // Background mining control helpers - using string interface
    std::string setGenerate(const std::string& params);
    std::string startMining(const std::string& params);
    Json::Value startMiningWithValidation(const Json::Value& params);
    std::string stopMining(const std::string& params);
    Json::Value generateToAddress(const Json::Value& params);
    Json::Value setMiningAddress(const Json::Value& params);
    Json::Value setMiningAddressWithValidation(const Json::Value& params);
    Json::Value getMiningAddress(const Json::Value& params);
    void storeMiningAddressSetting(const std::string& address);
    std::string loadMiningAddressSetting();
    void initializeMiningAddress();
    Json::Value getMiningEvents(const Json::Value& params);
    Json::Value setMiningPayoutAddress(const Json::Value& params);
    Json::Value getMiningPayoutAddress(const Json::Value& params);
    
    // Wallet management RPCs - using Json::Value interface
    std::string walletList();
    Json::Value walletOpen(const Json::Value& params);
    Json::Value walletRename(const Json::Value& params);
    Json::Value walletDelete(const Json::Value& params);
    
    // Node info management
    void writeNodeInfo();
    Json::Value walletValidateAddress(const Json::Value& params);
    
    // Address management RPCs - using string interface
    Json::Value addressSetLabel(const Json::Value& params);
    Json::Value addressGetLabel(const Json::Value& params);
    Json::Value addressList(const Json::Value& params);
    Json::Value addressRemove(const Json::Value& params);
    void startMiningLoop();
    void startMiningInfoTimer();
    
    // New Mining Controller API - using string interface
    std::string minerStart(const std::string& params);
    std::string minerStop(const std::string& params);
    std::string minerStatus(const std::string& params);
    std::string minerSetConfig(const std::string& params);
    
    // Core RPC methods - using string interface
    Json::Value getHelp(const Json::Value& params);
    std::string getNetworkInfo();
    std::string getBlockchainInfo();
    std::string getHealthRpc();
    std::string getPeers();
    std::string getChainTips();
    std::string getBlockCount();
    std::string getBestBlockHash();
    std::string getBlockHash(const std::string& params);
    std::string getBlock(const std::string& params);
    std::string getBlockEnhanced(const std::string& params);
    std::string getBlockByHeight(const std::string& params);
    std::string getBlockHeader(const std::string& params);
    std::string getMiningInfo();
    std::string getInfo();
    std::string getRpcInfo();
    Json::Value getRpcCapabilities();
    Json::Value getRpcListMethods();
    std::string getRpcHelp(const std::string& params);
    std::string getRpcHealth();
    Json::Value getBlockTemplate(const Json::Value& params);
    std::string debugBuildTemplate(const std::string& params);
    std::string getNetworkStats();

    std::string getWalletInfo();
    std::string getNewAddress();
    std::string getWalletBalance();
    
    // Mining RPCs for secure PoW architecture - using Json::Value interface
    Json::Value validateMiningAddress(const Json::Value& params);
    
    // Watch-only support for GUI balance display - using string interface
    std::string importWatchXPub(const std::string& params);
    std::string importWatchAddr(const std::string& params);
    std::string listWatchBalances(const std::string& params);
    std::string listWatchTransactions(const std::string& params);
    std::string getUtxosBySpk(const std::string& params);
    std::string rescanWatch(const std::string& params);
    
    // PSBT and transaction methods - using string interface
    std::string listAddresses(const std::string& params);
    std::string walletCreateFundedPSBT(const std::string& params);
    std::string walletProcessPSBT(const std::string& params);
    std::string finalizePSBT(const std::string& params);
    
    // Message signing and verification - business handlers return Json::Value
    Json::Value signMessage(const Json::Value& params);
    Json::Value verifyMessage(const Json::Value& params);
    Json::Value signTransaction(const Json::Value& params);
    Json::Value verifyTransaction(const Json::Value& params);
    
    // Address validation and decoding - business handlers return Json::Value
    Json::Value validateAddress(const Json::Value& params);
    Json::Value decodeAddress(const Json::Value& params);
    Json::Value generateAddress(const Json::Value& params);
    std::string dumpPrivateKey(const std::string& params);
    
    // Missing core RPC methods - JSON interface for new schema compliance
    Json::Value getNewAddress(const Json::Value& params);
    Json::Value getBalance(const Json::Value& params);
    Json::Value listUnspent(const Json::Value& params);
    Json::Value listTransactions(const Json::Value& params);
    Json::Value miningStart(const Json::Value& params);
    Json::Value miningStop(const Json::Value& params);
    Json::Value miningStatus(const Json::Value& params);
    Json::Value estimateSmartFee(const Json::Value& params);
    
    // PSBT workflow methods
    Json::Value rpcPsbtCreate(const Json::Value& params);
    Json::Value rpcPsbtFund(const Json::Value& params);
    Json::Value rpcPsbtSign(const Json::Value& params);
    Json::Value rpcPsbtSubmit(const Json::Value& params);
    Json::Value rpcPsbtDecode(const Json::Value& params);
    Json::Value rpcPsbtFinalize(const Json::Value& params);
    Json::Value rpcPsbtExtract(const Json::Value& params);
    
    // Realtime event methods
    Json::Value rpcEventsSubscribe(const Json::Value& params);
    Json::Value rpcEventsUnsubscribe(const Json::Value& params);
    
    // Wallet management methods - using string interface
    // createWallet removed - wallet.create routes to WalletHandlers::create
    std::string loadWallet(const std::string& params);
    std::string unloadWallet(const std::string& params);
    Json::Value walletPassphrase(const Json::Value& params);
    Json::Value walletLock(const Json::Value& params);
    std::string getNewChangeAddress();
    std::string importAddress(const std::string& params);
    std::string importPrivateKey(const std::string& params);
    std::string importXPub(const std::string& params);
    std::string rescanBlockchain(const std::string& params);
    
    // Logging methods - using string interface
    std::string getLogLines(const std::string& params);
    std::string getActivity(const std::string& params);
    std::string streamLogs(const std::string& params);
    std::string getLogLevel();
    std::string setLogLevel(const std::string& params);
    
    // Helper method for adding logs to buffer
    void addLogToBuffer(const std::string& level, const std::string& message);

    // Blockchain methods - using string interface
    std::string getBalance(const std::string& params);
    std::string listUnspent(const std::string& params);
    std::string getTransaction(const std::string& params);
    std::string sendRawTransaction(const std::string& params);
    std::string createRawTransaction(const std::string& params);
    std::string sendToAddress(const std::string& params);
    std::string walletPreviewSend(const std::string& params);
    
    // Mempool endpoints - using string interface
    std::string getMempoolInfo();
    std::string getRawMempool();
    std::string getMempoolEntry(const std::string& params);
    std::string submitDummyTx(const std::string& params);
    
    // Batch RPC support
    // executeBatchRpc removed - batch RPC now handled in HTTP layer
    std::string testMempoolAccept(const std::string& params);
    
    // UTXO and transaction endpoints - using string interface
    std::string getTxOut(const std::string& params);
    std::string getTxOutSetInfo();
    std::string getRawTransaction(const std::string& params);
    
    // Utility methods
    std::string bytesToHex(const uint8_t* data, size_t len);
    std::vector<uint8_t> hexToBytes(const std::string& hex);
    // Wallet store helpers
    std::vector<std::string> loadStoredAddresses();
    void appendStoredAddress(const std::string& address);

     // --- Phase 0/1 helpers ---
     bool isWalletDisabled() const;
     bool ensureWalletDir();
     std::string walletFilePath() const;
     bool writeFileAtomic(const std::string& path, const std::string& contents, mode_t mode = 0600);

     std::vector<uint8_t> hexToBytesVec(const std::string& hex);
     std::string sha256Hex(const std::vector<uint8_t>& data);
     bool readJsonFile(const std::string& path, Json::Value& out);
     bool writeJsonFileAtomic(const std::string& path, const Json::Value& j, mode_t mode = 0600);
     bool aesGcmEncrypt(const std::vector<uint8_t>& key,
                        const std::vector<uint8_t>& nonce,
                        const std::vector<uint8_t>& plaintext,
                        std::vector<uint8_t>& ciphertext,
                        std::vector<uint8_t>& tag);
     bool aesGcmDecrypt(const std::vector<uint8_t>& key,
                        const std::vector<uint8_t>& nonce,
                        const std::vector<uint8_t>& ciphertext,
                        const std::vector<uint8_t>& tag,
                        std::vector<uint8_t>& plaintext_out);
     bool pbkdf2HmacSha256(const std::string& password,
                           const std::vector<uint8_t>& salt,
                           uint32_t iterations,
                           std::vector<uint8_t>& key);
     void secureZero(std::vector<uint8_t>& v);
     void scheduleRelock(uint32_t timeout_seconds);
    
    // Helper functions
    std::string getWalletFilePath() const;
    std::string getWalletMetadataPath() const;
    
    // Wallet management helpers
    bool isWalletLoaded() const;
    bool storeAddressInWallet(const Json::Value& addressMeta);
    bool loadWalletFromFile(const std::string& walletPath);
    void unloadCurrentWallet();
    
    // Network configuration methods - clean implementation
    std::string getNetworkHRP() const;
    uint32_t    getCoinType() const;
    void setNetwork(Net net);
    Net detectNetwork() const;

    // Descriptor methods
    std::string generateDescriptor(const std::string& fingerprint, 
                                 const std::string& xpub, 
                                 uint32_t change_index);
    std::string calculateDescriptorChecksum(const std::string& descriptor);
    bool validateHRP(const std::string& address);

    // Base64 encoding/decoding functions
    std::string base64Encode(const std::vector<uint8_t>& data);
    std::vector<uint8_t> base64Decode(const std::string& encoded);
    
    // PSBT serialization functions
    std::vector<uint8_t> serializePSBT(const PSBT& psbt);
    PSBT deserializePSBT(const std::vector<uint8_t>& data);
    
    // Random number generation
    bool generateRandomBytes(std::vector<uint8_t>& bytes);

    // Cookie authentication
    bool initializeCookieAuth();
    bool validateCookieAuth(const std::string& auth_header);
    std::string m_cookie_user;
    std::string m_cookie_pass;
    std::filesystem::path m_cookie_path;
    std::string m_data_dir;

    std::atomic<bool> m_running;
    int m_port;
    std::thread m_rpc_thread;
    std::atomic<bool> m_started{false};
    int m_server_socket{-1};
    
    // References to blockchain, mining, and mempool components
    Mining* m_mining;
    std::shared_ptr<Mempool> m_mempool;
    MinerCore* m_miner_core;
    WalletBalanceService* m_wallet_balance_service;
    WalletManager* m_wallet_manager;
    MiningEventBus* m_mining_events;
    
    // P2P components
    dinero::PeerManager* m_peer_manager;
    HeadersSync* m_headers_sync;
    
    // Clean interface execution context (no globals)
    din::ExecutionContext execution_context_;
    
    // WebSocket server
    // WebSocket components disabled - using new ws_bus implementation
    // std::unique_ptr<Subscriptions> m_subscriptions;
    // std::unique_ptr<WsBridge> m_ws_bridge;
    

    
    // Method handler map - using string-returning handlers for now
    std::map<std::string, rpc::LegacyStringHandler> m_method_handlers;
    
    // Wallet handlers removed to avoid incomplete type issues

    // Mining loop state
    std::atomic<bool> m_mining_loop_enabled{false};
    std::thread m_mining_loop_thread;
    
    // WebSocket mining info broadcast timer
    std::atomic<bool> m_mining_info_timer_enabled{false};
    std::thread m_mining_info_timer_thread;

     // Server options
     bool m_dev_mode{false};

     // Wallet state (Phase 1)
     std::atomic<bool> m_wallet_unlocked{false};
     std::mutex m_wallet_mutex;
     std::chrono::steady_clock::time_point m_unlock_until{};
     std::thread m_relock_thread;
     // seed generated at createwallet, held only in RAM until walletpassphrase encrypts it
     std::vector<uint8_t> m_pending_seed;
    
    // Wallet loading state tracking
    std::atomic<bool> m_wallet_loaded{false};
    std::string m_wallet_name;
    std::string m_wallet_path;
    
    // Wallet database for addresses
    std::string m_wallet_db_path;

     // Descriptor + address state (Phase 2)
     std::string m_hrp{"din"};
     uint32_t m_coin_type{dinero::consensus::DINERO_COIN_TYPE};
     uint32_t m_next_index_external{0};
     uint32_t m_next_index_internal{0};
     uint32_t m_next_index_change{0};
     
     // Additional wallet state variables
     std::atomic<bool> m_wallet_locked{true};
     std::time_t m_wallet_unlock_time{0};
     uint32_t m_wallet_fingerprint{0};
     std::string m_wallet_master_xpub;
     std::string m_wallet_master_xprv;
     uint32_t m_wallet_pbkdf2_iters{600000};

    // Network configuration - clean implementation
    Net        m_net{Net::MAIN};
    NetParams  m_params{};
    
    // Network detection and configuration
    Net detectNetworkFromConfig() const;  // reads flags like -testnet/-regtest
    void loadNetParams(Net n);

    // Descriptor checksum constants
    static const std::string DESCRIPTOR_CHARSET;
    static const std::vector<uint64_t> DESCRIPTOR_GENERATOR;
    
    // Helper functions for varint serialization (using shared ser namespace)
    // Note: Use dinero::ser::writeCompactSize and dinero::ser::readCompactSize instead
    
    // Helper function to generate change address
    std::string generateChangeAddress();
    
    // Helper function to derive next change address
    std::string deriveNextChangeAddress();
    
    // Log streaming state
    std::atomic<bool> m_log_streaming_enabled{false};
    std::mutex m_log_mutex;
    std::vector<std::string> m_log_buffer;
    std::string m_current_log_level{"info"};
    size_t m_max_log_buffer_size{1000};
};

} // namespace dinero 
