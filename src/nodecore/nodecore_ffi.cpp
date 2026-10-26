// nodecore_ffi.cpp
// C ABI implementation wrapping DaemonApp for iOS embedding.
//
// Architecture:
//   Swift → C ABI (this file) → DaemonApp → services
//
// Thread model:
//   - nodecore_start() spawns the node on a dedicated thread
//   - nodecore_stop() signals shutdown and joins
//   - Event callbacks fire on the node's internal threads
//   - All public functions use mutex for thread safety

#include "nodecore/nodecore_ffi.h"
#include "daemon/daemon_app.h"
#include "daemon/datadir_guard.h"
#include "daemon/config.h"
#include "daemon/services/config_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/p2p_service.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/orchard_snapshot_store.h"
#include "consensus/header_chain.h"
#include "consensus/orchard_profile.h"
#include "consensus/chainparams.h"
#include "consensus/header_sync_manager.h"
#include "wallet/wallet_manager.h"
#include "wallet/utxo_index.h"
#include "storage/chain_db.h"
#include "common/logger.h"
#include <iostream>
#include "consensus/utreexo_accumulator.h"
#include "consensus/tx_parser.h"
#include "consensus/pow_difficulty_helpers.h"
#include "consensus/target_helpers.h"
#include "consensus/pq/ml_dsa_65.h"
#include "consensus/pq/p2mr_consensus.h"
#include "consensus/pq/scheme_registry.h"
#include "daemon/tx_relay_manager.h"
#include "daemon/bech32_decode.h"
#include "consensus/global_utxo_set.h"
#include "nodecore/sync_profile_policy.h"
#include "nodecore/runtime_guards.h"
#include "nodecore/maintenance_journal.h"
#include "rpc/rpc_registry.h"
#include "version.h"
#include "interfaces/wallet_notifier.h"
#include "wallet/p2mr_address.h"
#include "wallet/pq_derivation.h"
#include "wallet/secure_keypair.h"

#include <json/json.h>
#include <openssl/crypto.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <array>
#include <string>
#include <vector>
#include <unordered_set>
#include <optional>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <filesystem>
#include <limits>

// Context-aware wallet RPC handlers from src/rpc/methods_wallet_context.cpp
din::Json rpc_context_wallet_getbalance(const ExecutionContext& ctx, const din::Json& params);
din::Json rpc_context_wallet_getnewaddress(const ExecutionContext& ctx, const din::Json& params);
din::Json rpc_context_wallet_sendtoaddress(const ExecutionContext& ctx, const din::Json& params);

// ============================================================================
// Internal State
// ============================================================================

namespace {

thread_local bool in_nodecore_callback = false;

struct NodeCoreState {
    std::mutex mtx;
    // Declared before app so even static teardown closes every store first.
    // The lock spans Init, the worker and completed Stop, not just migration.
    std::unique_ptr<dinero::daemon::DatadirGuard> datadir_guard;
    std::unique_ptr<dinero::DaemonApp> app;
    std::thread node_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> shutdown_requested{false};
    // Stop declares cancellation BEFORE waiting for the publication mutex.
    // A generation also covers Stop during synchronous startup preparation.
    std::atomic<uint64_t> public_stop_epoch{0};
    const std::shared_ptr<dinero::ReopenRpcGate> reopen_rpc_gate=std::make_shared<dinero::ReopenRpcGate>();
    std::chrono::steady_clock::time_point start_time;

    // Event callback
    dinero::nodecore::EventCallbackSlot event_callback;

    // Watched scripts (hex-encoded for fast lookup)
    dinero::nodecore::WatchedScriptRegistry watched_scripts;

    // Configuration
    std::string datadir;
    std::string network = "mainnet";
    bool utreexo_stateless = false;
    std::string sync_profile = "unknown";
    uint64_t capabilities = 0;

    dinero::nodecore::MaintenanceJournal maintenance;
    std::string maintenance_token;
    std::string maintenance_process;
    uint64_t maintenance_generation = 0;
    bool maintenance_recovery = false;

    ~NodeCoreState() noexcept {
        shutdown_requested.store(true);
        if (node_thread.joinable()) {
            if (node_thread.get_id() == std::this_thread::get_id()) {
                node_thread.detach();
            } else {
                node_thread.join();
            }
        }
        try {
            app.reset();
        } catch (...) {
            // Never throw from destructor — absorb any DaemonApp cleanup failure
            fprintf(stderr, "[nodecore_ffi] Exception during ~NodeCoreState app cleanup\n");
        }
    }
};

NodeCoreState& state() {
    static NodeCoreState s;
    return s;
}

int CloseNodeLocked(NodeCoreState& s) {
    s.shutdown_requested = true;
    s.running = false;
    try {
        if (s.node_thread.joinable()) {
            if (s.node_thread.get_id() == std::this_thread::get_id()) return NODECORE_ERROR_CALLBACK_REENTRY;
            s.node_thread.join();
        }
        s.app.reset();
        s.datadir_guard.reset();
        return NODECORE_OK;
    } catch (...) { return NODECORE_ERROR_INTERNAL; }
}

std::string NewMaintenanceToken(NodeCoreState& s) {
    if (s.maintenance_process.empty()) s.maintenance_process = dinero::nodecore::MaintenanceJournal::RandomId();
    if (s.maintenance_generation == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("generation exhausted");
    return s.maintenance_process + "." + std::to_string(++s.maintenance_generation) + "." + s.maintenance.Operation();
}

bool is_queryable_locked(const NodeCoreState& s) {
    dinero::nodecore::RuntimeQueryState query_state;
    query_state.running = s.running.load();
    query_state.shutdown_requested = s.shutdown_requested.load();
    query_state.has_app = static_cast<bool>(s.app);
    return dinero::nodecore::IsQueryable(query_state);
}

// Helper: duplicate a C string (caller must free with nodecore_free_string)
char* strdup_c(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) {
        std::memcpy(p, s.c_str(), s.size() + 1);
    }
    return p;
}

// Helper: bytes to hex
std::string bytes_to_hex(const uint8_t* data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result.push_back(hex[data[i] >> 4]);
        result.push_back(hex[data[i] & 0x0f]);
    }
    return result;
}

template <std::size_t N>
std::string array_to_hex(const std::array<uint8_t, N>& bytes) {
    return bytes_to_hex(bytes.data(), bytes.size());
}

char* json_string(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return strdup_c(Json::writeString(builder, value));
}

char* json_error(const std::string& message) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["error"] = message;
    return json_string(err);
}

Json::Value p2mr_metadata_json(const char* hrp,
                               const dinero::consensus::pq::ml_dsa_65::Seed& seed) {
    namespace mldsa = dinero::consensus::pq::ml_dsa_65;
    namespace pq = dinero::consensus::pq;

    dinero::wallet::SecureKeypair kp(mldsa::KeygenFromSeed(seed));
    const auto merkle_root = pq::ComputeP2MRLeafHash(
        pq::SCHEME_ID_ML_DSA_65, kp.pubkey().data(), kp.pubkey().size());
    const std::string address = dinero::wallet::EncodeP2MRAddress(hrp ? hrp : "din", merkle_root);
    const auto script = dinero::wallet::BuildP2MRScriptPubKey(merkle_root);

    Json::Value out(Json::objectValue);
    if (address.empty() || script.empty()) {
        out["ok"] = false;
        out["error"] = "failed to encode P2MR address";
        return out;
    }

    out["ok"] = true;
    out["scheme_id"] = static_cast<Json::UInt>(pq::SCHEME_ID_ML_DSA_65);
    out["address"] = address;
    out["script_pubkey"] = bytes_to_hex(script.data(), script.size());
    out["merkle_root"] = array_to_hex(merkle_root);
    out["pubkey"] = array_to_hex(kp.pubkey());
    out["leaf_index"] = static_cast<Json::UInt>(0);
    out["merkle_depth"] = static_cast<Json::UInt>(0);
    return out;
}

// Helper: emit event
void emit_event(NodeCoreEventType type, const std::string& json_data) {
    auto& s = state();
    const auto snapshot = s.event_callback.Snapshot();
    if (snapshot.callback) {
        struct CallbackScope {
            bool previous = in_nodecore_callback;
            CallbackScope() { in_nodecore_callback = true; }
            ~CallbackScope() { in_nodecore_callback = previous; }
        } scope;
        snapshot.callback(static_cast<int32_t>(type), json_data.c_str(), snapshot.user_data);
    }
}

// Helper: emit event with Json::Value
void emit_event_json(NodeCoreEventType type, const Json::Value& val) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    emit_event(type, Json::writeString(builder, val));
}

std::string normalize_rpc_method(const std::string& method_name) {
    std::string lowered = method_name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Backward-compatible aliases for legacy wallet status callers.
    if (lowered == "getwalletstatus" ||
        lowered == "wallet.status" ||
        lowered == "wallet.getwalletstatus") {
        return "wallet.getstatus";
    }

    return method_name;
}

} // anonymous namespace

// ============================================================================
// WalletNotifier bridge: forwards blockchain events to Swift callback
// ============================================================================

namespace {

class NodeCoreWalletNotifier : public dinero::WalletNotifier {
public:
    void onBlockConnected(const dinero::Block& block, uint32_t height) override {
        // The WalletManager's own onBlockConnected handles UTXO scanning.
        // We emit SYNC_BLOCK_CONNECTED for the Swift side with enriched data.
        Json::Value ev;
        ev["height"] = height;
        ev["hash"] = block.header.GetHash().GetHex();
        ev["tx_count"] = static_cast<Json::UInt>(block.vtx.size());
        ev["timestamp"] = static_cast<Json::UInt64>(block.header.timestamp);
        emit_event_json(NODECORE_EVENT_SYNC_BLOCK_CONNECTED, ev);
    }

    void onBlockDisconnected(const dinero::Block& block, uint32_t height) override {
        Json::Value ev;
        ev["height"] = height;
        emit_event_json(NODECORE_EVENT_REORG, ev);
    }

    void onMempoolTransaction(const dinero::Transaction& tx) override {
        auto& s = state();
        const auto watched_scripts = s.watched_scripts.Snapshot();

        // Check outputs against watched scripts
        Json::Value outputs_json(Json::arrayValue);
        bool has_relevant_output = false;
        for (size_t i = 0; i < tx.vout.size(); ++i) {
            std::string script_hex = bytes_to_hex(
                tx.vout[i].scriptPubKey.data(),
                tx.vout[i].scriptPubKey.size());

            if (watched_scripts.count(script_hex)) {
                has_relevant_output = true;
                Json::Value out;
                out["index"] = static_cast<Json::UInt>(i);
                out["script"] = script_hex;
                out["amount"] = static_cast<Json::UInt64>(tx.vout[i].value.GetUna());
                outputs_json.append(out);
            }
        }

        std::string txid_hex = tx.GetTxid().AsUint256().GetHex();

        if (has_relevant_output) {
            Json::Value ev;
            ev["txid"] = txid_hex;
            ev["height"] = 0;  // mempool = unconfirmed
            ev["outputs"] = outputs_json;
            emit_event_json(NODECORE_EVENT_RELEVANT_TX_ADDED, ev);
        }

        // Input-side (UTXO spent) detection for mempool txs requires
        // querying the full coin set — deferred to balance refresh triggered
        // by the RELEVANT_TX_ADDED event above. Block-confirmed spends are
        // already handled by the WalletManager's onBlockConnected path.
    }
};

// Persistent notifier instance
std::unique_ptr<NodeCoreWalletNotifier> g_notifier;

bool Cancelled(const NodeCoreState& s,uint64_t epoch){
    return s.shutdown_requested.load()||s.public_stop_epoch.load()!=epoch;
}
void ConfigureEmbeddedRuntime(dinero::DaemonApp& app){
    if(app.GetContext().chainstate)if(auto* db=app.GetContext().chainstate->GetChainDB()){
        db->setFatalWriteFailureHook([](const std::string& reason){
            auto& s=state();s.running=false;s.shutdown_requested=true;
            fprintf(stderr,"[nodecore_ffi] ChainDB fatal: %s — requesting clean shutdown\n",reason.c_str());
            Json::Value ev;ev["reason"]="chaindb_fatal";ev["error"]=reason;
            emit_event_json(NODECORE_EVENT_SHUTDOWN,ev);
        });
    }
}
void ConfigureEmbeddedNotifier(dinero::DaemonApp& app){
    if(!g_notifier)g_notifier=std::make_unique<NodeCoreWalletNotifier>();
    if(auto pool=std::dynamic_pointer_cast<dinero::MempoolService>(app.GetContext().mempool))
        pool->mempool().setTxAcceptedCallback([](const dinero::Transaction& tx){g_notifier->onMempoolTransaction(tx);});
}
struct ActivationReopenData {
    std::shared_ptr<const dinero::consensus::HeaderChainSelector> headers;
    std::shared_ptr<const dinero::consensus::OrchardSnapshotSection> proof;
};
std::optional<ActivationReopenData> CaptureActivationReopen(dinero::DaemonApp& app,
    const std::function<bool()>& cancelled){
    if(!GetConfig().utreexo_stateless)return {};
    const auto& ctx=app.GetContext();
    const auto chain=std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.chainstate);
    const auto peers=std::dynamic_pointer_cast<dinero::P2PService>(ctx.p2p);
    if(!chain||!peers||!ctx.header_chain)return {};
    const auto sync=chain->GetSyncSnapshot();const auto activation=dinero::Params().orchard_activation_height;
    if(activation==UINT32_MAX||uint64_t(sync.active_tip_height)+1!=activation)return {};
    auto proof=peers->ReadyActivationCatalog();
    if(!proof)return {};
    if(proof->state.height!=activation||proof->parent_catalog.height!=sync.active_tip_height||
        proof->parent_catalog.block!=sync.active_tip_hash)throw std::runtime_error("SDK activation proof/source changed");
    // Copy values by HASH from one coherent ancestry snapshot, not a quadratic
    // series of best-height parent walks. Revalidate into an in-memory selector
    // without an old HeaderStore/DB/forest lifetime. This is proof DATA only.
    const uint64_t through=uint64_t(activation)+dinero::Params().state_commitment_burial_depth;
    if(through>1000000)throw std::runtime_error("SDK activation header capture exceeds operational bound");
    const auto best=ctx.header_chain->GetBestHeaderValue();dinero::uint256 anchor;uint32_t height=0;
    if(!best||best->height<through||!ctx.header_chain->GetAncestorHashByHash(best->hash,uint32_t(through),anchor,height))return {};
    std::vector<std::pair<dinero::uint256,uint32_t>> ancestry;
    if(!ctx.header_chain->CollectAncestorsByHash(anchor,0,height,ancestry)||height!=through||ancestry.size()!=through+1)
        throw std::runtime_error("SDK activation header ancestry unavailable");
    auto detached=std::make_shared<dinero::consensus::HeaderChainSelector>();
    for(const auto& [hash,position]:ancestry){
        if(cancelled())return {};
        const auto row=ctx.header_chain->GetHeaderValue(hash);
        if(!row||row->height!=position||row->hash!=row->header.GetHash()||!detached->AddHeader(row->header))
            throw std::runtime_error("SDK activation header capture is inconsistent");
        const auto actual=detached->GetHeaderValue(hash);
        if(!actual||actual->height!=position||actual->chainwork!=row->chainwork)
            throw std::runtime_error("SDK activation header work changed");
    }
    const auto current=ctx.header_chain->GetHeaderAtHeightValue(uint32_t(through));
    if(!current||current->hash!=anchor||cancelled())return {};
    dinero::OrchardSnapshotStore::VerifyCatalogTransferProof(*proof,*detached);
    return ActivationReopenData{std::move(detached),std::move(proof)};
}
bool AutomaticActivationReopen(NodeCoreState& s,const std::vector<std::string>& owned_args,uint64_t stop_epoch){
    const auto cancelled=[&]{return Cancelled(s,stop_epoch);};
    if(cancelled()||!s.app)return false;
    auto data=CaptureActivationReopen(*s.app,cancelled);if(!data||cancelled())return false;
    std::shared_ptr<dinero::ReopenRpcGate::Pause> pause;
    {std::unique_lock lock(s.mtx,std::try_to_lock);
     if(!lock.owns_lock()||cancelled())return false;
     pause=std::shared_ptr<dinero::ReopenRpcGate::Pause>(dinero::ReopenRpcGate::Begin(s.reopen_rpc_gate));
     s.running=false;}
    std::unique_ptr<dinero::CoreReopenReceipt> receipt;
    try{
        emit_event(NODECORE_EVENT_SYNC_STARTED,"{\"reason\":\"authenticated_activation_restarting\"}");
        receipt=s.app->CloseForInternalReopen(*s.datadir_guard,pause);
    }
    catch(...){dinero::DaemonApp::FailClosedReopenPause(pause);throw;}
    // No blocking publication lock: publicStop may hold it while JOINING us.
    std::unique_ptr<dinero::DaemonApp> old;
    while(!cancelled()){
        std::unique_lock lock(s.mtx,std::try_to_lock);
        if(lock.owns_lock()){old=std::move(s.app);break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if(cancelled())return false; // Original is stopped; durable intent survives.
    old.reset(); // ALL old services/raw readers end under SAME root guard.
    dinero::DaemonApp::SelectClosedActivationForReopen(*s.datadir_guard,*receipt,
        std::move(data->headers),std::move(data->proof),cancelled);
    if(cancelled())return false;
    auto next=std::make_unique<dinero::DaemonApp>();next->SetLockedDatadir(*s.datadir_guard);
    next->SetAutomaticStorageUpgrade(true);next->BindClosedReopenBeforeInit(*s.datadir_guard,*receipt);
    auto args=owned_args;std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());
    if(!next->Init(int(argv.size()),argv.data()))throw std::runtime_error("SDK activation replacement Init refused");
    ConfigureEmbeddedRuntime(*next);
    if(cancelled())return false;
    if(!next->Start())throw std::runtime_error("SDK activation replacement Start refused");
    ConfigureEmbeddedNotifier(*next);
    next->PrepareReadmissionAfterInternalReopen(*s.datadir_guard,*receipt);
    while(!cancelled()){
        std::unique_lock lock(s.mtx,std::try_to_lock);
        if(!lock.owns_lock()){std::this_thread::sleep_for(std::chrono::milliseconds(10));continue;}
        if(cancelled())break;
        next->PublishPreparedReadmission(*receipt,[&]{
            if(cancelled())throw std::runtime_error("SDK activation publication cancelled by public Stop");
            s.app=std::move(next);s.running=true;
            if(cancelled()){s.running=false;throw std::runtime_error("SDK activation publication lost to public Stop");}
        });
        lock.unlock();
        // Full archive/digest checks stay outside host publication. Interrupted
        // publication/Stop keeps the selected durable corpus for explicit start.
        if(!cancelled())s.app->ArchivePublishedReadmission(*s.datadir_guard,*receipt);
        emit_event(NODECORE_EVENT_SYNC_STARTED,"{\"reason\":\"authenticated_activation_reopen\"}");
        return true;
    }
    return false; // Local replacement destructs/Stops before guard can release.
}

} // anonymous namespace

// ============================================================================
// Lifecycle
// ============================================================================

extern "C" {

int32_t nodecore_start(const char* datadir, const char* config_json) {
    if (in_nodecore_callback) return NODECORE_ERROR_CALLBACK_REENTRY;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    const auto stop_epoch=s.public_stop_epoch.load();
    if(stop_epoch==std::numeric_limits<uint64_t>::max())return NODECORE_ERROR_INTERNAL;

    if (!s.maintenance_token.empty()) return NODECORE_ERROR_MAINTENANCE_BUSY;
    if (s.maintenance_recovery) return NODECORE_ERROR_RECOVERY_REQUIRED;

    if (s.running.load()) {
        return NODECORE_ERROR_ALREADY_RUNNING;
    }

    if (!datadir || !*datadir || strnlen(datadir, 4097) > 4096 || !std::filesystem::path(datadir).is_absolute()) {
        return NODECORE_ERROR_INVALID_ARGS;
    }

    // Read the persistent barrier before Init can create, repair or open files.
    const int gate = dinero::nodecore::MaintenanceJournal::StartupGate(datadir);
    if (gate != NODECORE_OK) return gate;
    // An internally stopped/failed worker may leave a joinable thread and App
    // behind even with running=false. Reap that owner before replacing it.
    if (s.app || s.node_thread.joinable()) {
        const int closed = CloseNodeLocked(s);
        if (closed != NODECORE_OK) return closed;
    }

    s.datadir = datadir;
    s.shutdown_requested = false;

    // Parse config JSON
    std::string network = "mainnet";
    int p2p_port = 21001;
    int max_peers = 4;
    std::string addnode;
    bool utreexo_stateless = dinero::nodecore::DefaultUtreexoStateless();
    std::string sync_profile = dinero::nodecore::DefaultSyncProfile();
    std::string wallet_schema_path;
    std::string assumeutxo_snapshot_path;
    std::vector<std::string> assumeutxo_snapshot_fallback_paths;
    std::string assumeutxo_manifest_path;
    bool assumeutxo_require_manifest = false;
    std::optional<std::string> explicit_profile;
    std::optional<bool> legacy_utreexo_override;
    bool verbose_logging = false;  // embedded default: quiet (see below)

    if (config_json) {
        Json::Value config;
        Json::CharReaderBuilder reader;
        std::string errs;
        std::istringstream stream(config_json);
        if (Json::parseFromStream(reader, stream, &config, &errs)) {
            auto parse_bool = [](const Json::Value& v, bool fallback) -> bool {
                if (v.isBool()) return v.asBool();
                if (v.isInt() || v.isUInt()) return v.asInt() != 0;
                if (v.isString()) {
                    std::string s = v.asString();
                    std::transform(s.begin(), s.end(), s.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
                    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
                }
                return fallback;
            };

            if (config.isMember("network")) network = config["network"].asString();
            if (config.isMember("verbose_logging")) {
                verbose_logging = parse_bool(config["verbose_logging"], false);
            }
            if (config.isMember("p2p_port")) p2p_port = config["p2p_port"].asInt();
            if (config.isMember("max_peers")) max_peers = config["max_peers"].asInt();
            if (config.isMember("addnode")) {
                // Host static peers, comma-separated host:port (the daemon's addnode).
                if (!config["addnode"].isString()) {
                    return NODECORE_ERROR_INVALID_ARGS;
                }
                addnode = config["addnode"].asString();
            }
            if (config.isMember("wallet_schema_path")) {
                if (!config["wallet_schema_path"].isString()) {
                    return NODECORE_ERROR_INVALID_ARGS;
                }
                wallet_schema_path = config["wallet_schema_path"].asString();
            }
            if (config.isMember("assumeutxo_snapshot")) {
                if (!config["assumeutxo_snapshot"].isString()) {
                    return NODECORE_ERROR_INVALID_ARGS;
                }
                assumeutxo_snapshot_path = config["assumeutxo_snapshot"].asString();
            }
            if (config.isMember("assumeutxo_snapshot_fallbacks")) {
                const auto& fallbacks = config["assumeutxo_snapshot_fallbacks"];
                if (!fallbacks.isArray()) {
                    return NODECORE_ERROR_INVALID_ARGS;
                }
                for (const auto& fallback : fallbacks) {
                    if (!fallback.isString()) {
                        return NODECORE_ERROR_INVALID_ARGS;
                    }
                    const std::string path = fallback.asString();
                    // ConfigService transports this internal candidate list as
                    // a semicolon-delimited value. App-managed snapshot paths
                    // are controlled filenames, so reject an ambiguous path
                    // instead of silently splitting it into extra candidates.
                    if (path.find(';') != std::string::npos) {
                        return NODECORE_ERROR_INVALID_ARGS;
                    }
                    if (!path.empty() &&
                        std::find(assumeutxo_snapshot_fallback_paths.begin(),
                                  assumeutxo_snapshot_fallback_paths.end(), path) ==
                            assumeutxo_snapshot_fallback_paths.end()) {
                        assumeutxo_snapshot_fallback_paths.push_back(path);
                    }
                }
            }
            if (config.isMember("assumeutxo_manifest")) {
                if (!config["assumeutxo_manifest"].isString()) {
                    return NODECORE_ERROR_INVALID_ARGS;
                }
                assumeutxo_manifest_path = config["assumeutxo_manifest"].asString();
            }
            if (config.isMember("assumeutxo_require_manifest")) {
                assumeutxo_require_manifest = parse_bool(config["assumeutxo_require_manifest"], false);
            }
            if (config.isMember("sync_profile")) {
                if (!config["sync_profile"].isString()) {
                    return NODECORE_ERROR_INVALID_ARGS;
                }
                explicit_profile = config["sync_profile"].asString();
            }
            if (config.isMember("utreexo_stateless")) {
                legacy_utreexo_override = parse_bool(config["utreexo_stateless"], utreexo_stateless);
            }
        }
    }

    const auto resolved_profile = dinero::nodecore::ResolveSyncProfile(explicit_profile, legacy_utreexo_override);
    if (!resolved_profile.ok) {
        return NODECORE_ERROR_INVALID_ARGS;
    }
    sync_profile = resolved_profile.profile;
    utreexo_stateless = resolved_profile.utreexo_stateless;

    s.network = network;
    s.utreexo_stateless = utreexo_stateless;
    s.sync_profile = sync_profile;
    s.capabilities = dinero::nodecore::CapabilitiesForProfile(sync_profile);

    // Select chain params
    if (network == "regtest") {
        dinero::SelectParams(dinero::Chain::REGTEST);
    } else if (network == "testnet") {
        dinero::SelectParams(dinero::Chain::TESTNET);
    } else {
        dinero::SelectParams(dinero::Chain::MAINNET);
    }

    // Use the same operator-root lock as dinerod. Resolve an alias exactly
    // once before Init; inner database symlinks remain subject to the existing
    // storage guards. A concurrent daemon/SDK must not migrate or open it.
    auto datadir_guard = std::make_unique<dinero::daemon::DatadirGuard>();
    std::string lock_error;
    if (!datadir_guard->Acquire(s.datadir, lock_error)) {
        fprintf(stderr, "[nodecore_ffi] datadir lock refused: %s\n", lock_error.c_str());
        return NODECORE_ERROR_INIT_FAILED;
    }
    s.datadir = datadir_guard->DataDir().string();
    // Recheck the persistent fence on the exact locked root before any Init
    // writes; an alias spelling must not select a different maintenance owner.
    const int locked_gate = dinero::nodecore::MaintenanceJournal::StartupGate(s.datadir);
    if (locked_gate != NODECORE_OK) return locked_gate;

    // Build argc/argv for DaemonApp::Init
    std::vector<std::string> args_storage;
    args_storage.push_back("dinerod-ios");
    args_storage.push_back("--datadir=" + s.datadir);
    args_storage.push_back("--p2pport=" + std::to_string(p2p_port));
    args_storage.push_back("--sync-profile=" + s.sync_profile);
    args_storage.push_back("--utreexo-stateless=" + std::string(utreexo_stateless ? "1" : "0"));
    if (!wallet_schema_path.empty()) {
        args_storage.push_back("--wallet-schema-path=" + wallet_schema_path);
    }
    // NOTE: the daemon's arg parser stores keys VERBATIM (config->Set(key, value) with
    // key = text between "--" and "="; no hyphen->underscore normalization), and every
    // consumer reads these with UNDERSCORES (config_->GetString("assumeutxo_snapshot"),
    // etc. in chainstate_service.cpp + methods_blockchain_context.cpp). Passing the
    // hyphenated form here silently stored an unread "assumeutxo-snapshot" key, so the
    // snapshot was ignored and the node fell back to genesis IBD. Match the read keys.
    if (!assumeutxo_snapshot_path.empty()) {
        args_storage.push_back("--assumeutxo_snapshot=" + assumeutxo_snapshot_path);
    }
    if (!assumeutxo_snapshot_fallback_paths.empty()) {
        std::ostringstream encoded;
        for (size_t i = 0; i < assumeutxo_snapshot_fallback_paths.size(); ++i) {
            if (i != 0) encoded << ';';
            encoded << assumeutxo_snapshot_fallback_paths[i];
        }
        args_storage.push_back("--assumeutxo_snapshot_fallbacks=" + encoded.str());
    }
    if (!assumeutxo_manifest_path.empty()) {
        args_storage.push_back("--assumeutxo_manifest=" + assumeutxo_manifest_path);
    }
    if (assumeutxo_require_manifest) {
        args_storage.push_back("--assumeutxo_require_manifest=1");
    }
    if (max_peers > 0) {
        args_storage.push_back("--maxconnections=" + std::to_string(max_peers));
    }
    // Embedded hosts never run the legacy Lightning oracle IPC clients. With a
    // scheduled Orchard profile (mainnet H150000) the daemon refuses to start
    // while they are enabled (daemon_app.cpp), so disable them explicitly.
    args_storage.push_back("--lightning.oracles.enable=0");
    if (!addnode.empty()) args_storage.push_back("--addnode=" + addnode);

    if (network == "regtest") args_storage.push_back("--regtest");
    else if (network == "testnet") args_storage.push_back("--testnet");

    // All Init/recovery receipts and journals are thread-affine. The actual
    // worker owns argv AND every receipt through closed-store selection and
    // replacement teardown. The root guard stays in State until joined.
    struct Startup {
        std::atomic<bool> initialized{false},started{false};
        std::atomic<int32_t> result{std::numeric_limits<int32_t>::max()};
    };
    auto startup=std::make_shared<Startup>();
    if (!verbose_logging) {
        dinero::g_logger.setLogLevel(dinero::LogLevel::WARNING);
        std::cout.rdbuf(nullptr);
        fprintf(stderr,"[nodecore_ffi] quiet logging active (pass verbose_logging=true to restore)\n");
    }
    s.app=std::make_unique<dinero::DaemonApp>();
    s.app->SetLockedDatadir(*datadir_guard);s.app->SetAutomaticStorageUpgrade(true);
    s.datadir_guard=std::move(datadir_guard);
    s.start_time=std::chrono::steady_clock::now();
    s.node_thread=std::thread([&s,startup,stop_epoch,args=std::move(args_storage)]() mutable {
        const auto cancelled=[&]{return Cancelled(s,stop_epoch);};
        int32_t failure=NODECORE_ERROR_INIT_FAILED;
        try {
            std::unique_ptr<dinero::CoreReopenReceipt> pending;
            {
                // This unused shell has no store/service readers. Destroy it
                // before consuming its genuine closed intent for any partial
                // filesystem selection. A live initialized App never qualifies.
                auto intent=std::make_unique<dinero::DaemonApp>();intent->SetLockedDatadir(*s.datadir_guard);
                pending=intent->PreparePendingReopen(*s.datadir_guard,s.reopen_rpc_gate);
            }
            if(cancelled())throw std::runtime_error("SDK start cancelled by public Stop");
            if(pending){
                dinero::DaemonApp::RecoverInterruptedActivationForReopen(*s.datadir_guard,*pending,cancelled);
                s.app->BindClosedReopenBeforeInit(*s.datadir_guard,*pending);
            } else s.app->GetContext().reopen_rpc_gate=s.reopen_rpc_gate;
            {
                // Same offline fresh-genesis cohort/migration prerequisites as
                // native startup, BEFORE the real Init and under SAME guard.
                auto bootstrap_args=args;
                bootstrap_args.insert(bootstrap_args.end(),{"--p2p.offline=1","--listen=0",
                    "--rpc.port=0","--wallet.socket.enable=0"});
                std::vector<char*> argv;for(auto& a:bootstrap_args)argv.push_back(a.data());
                dinero::DaemonApp bootstrap;bootstrap.SetLockedDatadir(*s.datadir_guard);
                bootstrap.SetAutomaticStorageUpgrade(true);bootstrap.SetFreshStoreBootstrapOnly(true);
                if(!bootstrap.Init(int(argv.size()),argv.data()))throw std::runtime_error("SDK storage bootstrap refused");
                if(bootstrap.FreshStoreCreated()){
                    if(!bootstrap.Start()||!bootstrap.PersistFreshStoreGenesisRecords())
                        throw std::runtime_error("SDK fresh genesis preparation refused");
                    bootstrap.Stop();
                }
            }
            if(cancelled())throw std::runtime_error("SDK initialization cancelled by public Stop");
            std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());
            if(!s.app->Init(int(argv.size()),argv.data()))throw std::runtime_error("SDK App Init refused");
            ConfigureEmbeddedRuntime(*s.app);startup->initialized=true;failure=NODECORE_ERROR_START_FAILED;
            if(cancelled())throw std::runtime_error("SDK start cancelled by public Stop");
            if(!s.app->Start())throw std::runtime_error("SDK App Start refused");
            startup->started=true;ConfigureEmbeddedNotifier(*s.app);
            if(pending)s.app->PrepareReadmissionAfterInternalReopen(*s.datadir_guard,*pending);
            if(cancelled())throw std::runtime_error("SDK readmission cancelled by public Stop");
            // Initial App is already in State and the startup caller holds the
            // publication mutex until this result is ready. No query can see
            // running/open with an unpublished App.
            if(pending)s.app->PublishPreparedReadmission(*pending,[&]{
                if(cancelled())throw std::runtime_error("SDK publication cancelled by public Stop");
                s.running=true;
                if(cancelled()){s.running=false;throw std::runtime_error("SDK publication lost to public Stop");}
            });
            else s.running=true;
            startup->result=NODECORE_OK;
            if(pending&&!cancelled())s.app->ArchivePublishedReadmission(*s.datadir_guard,*pending);
            pending.reset();
            emit_event(NODECORE_EVENT_SYNC_STARTED,"{}");
            while(!cancelled()){
                AutomaticActivationReopen(s,args,stop_epoch);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            s.running=false;
            if(s.app)s.app->Stop();
            emit_event(NODECORE_EVENT_SHUTDOWN,"{\"reason\":\"stopped\"}");
        } catch(const std::exception& error) {
            s.running=false;s.shutdown_requested=true;
            if(s.app)s.app->Stop();
            int32_t expected=std::numeric_limits<int32_t>::max();
            startup->result.compare_exchange_strong(expected,failure);
            startup->initialized=true;
            Json::Value event;event["reason"]="node_thread_exception";event["error"]=error.what();
            fprintf(stderr,"[nodecore_ffi] worker stopped: %s\n",error.what());
            emit_event_json(NODECORE_EVENT_SHUTDOWN,event);
        } catch(...) {
            s.running=false;s.shutdown_requested=true;
            if(s.app)s.app->Stop();
            int32_t expected=std::numeric_limits<int32_t>::max();
            startup->result.compare_exchange_strong(expected,failure);startup->initialized=true;
            emit_event(NODECORE_EVENT_SHUTDOWN,"{\"reason\":\"node_thread_exception\",\"error\":\"unknown\"}");
        }
    });

    // Init was previously synchronous/unbounded; it now runs on its receipt's
    // owning worker. Preserve the existing 30s App Start confirmation window.
    // New journal/readmission work is separate from that window; explicit Stop
    // cancels it, and existing per-record/corpus bounds remain unchanged.
    const auto begin=std::chrono::steady_clock::now();
    while(!startup->initialized.load()&&startup->result.load()==std::numeric_limits<int32_t>::max()&&
          !Cancelled(s,stop_epoch))std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto start_begin=std::chrono::steady_clock::now();
    while(!startup->started.load()&&startup->result.load()==std::numeric_limits<int32_t>::max()&&
          !Cancelled(s,stop_epoch)){
        if(std::chrono::steady_clock::now()-start_begin>std::chrono::seconds(30)){s.shutdown_requested=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    while(startup->result.load()==std::numeric_limits<int32_t>::max()&&!Cancelled(s,stop_epoch))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto result=startup->result.load();
    if(result!=NODECORE_OK||Cancelled(s,stop_epoch)){
        s.shutdown_requested=true;s.running=false;
        if(s.node_thread.joinable())s.node_thread.join();
        s.app.reset();s.datadir_guard.reset();
        return result==NODECORE_ERROR_INIT_FAILED?result:NODECORE_ERROR_START_FAILED;
    }
    fprintf(stderr,"[nodecore_ffi] node started in %lldms\n",
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count());
    return NODECORE_OK;
}

int32_t nodecore_stop(void) {
    if (in_nodecore_callback) return NODECORE_ERROR_CALLBACK_REENTRY;
    auto& s = state();
    auto epoch=s.public_stop_epoch.load();
    while(epoch!=std::numeric_limits<uint64_t>::max()&&
        !s.public_stop_epoch.compare_exchange_weak(epoch,epoch+1)){}
    s.shutdown_requested=true;s.running=false;
    std::lock_guard<std::mutex> lock(s.mtx);
    // Plain stop never releases another operation's token or durable intent.
    return CloseNodeLocked(s);
}

bool nodecore_is_running(void) {
    return state().running.load();
}

int32_t nodecore_maintenance_begin(const char* datadir, const char* plan, char** token) {
    if (token) *token = nullptr;
#if defined(DINERO_NODECORE_MAINTENANCE_QUALIFICATION)
    if (!token || !datadir || !plan) return NODECORE_ERROR_INVALID_ARGS;
    if (std::strcmp(plan, dinero::nodecore::MaintenanceJournal::kInspection) != 0) return NODECORE_ERROR_UNSUPPORTED_PLAN;
    if (in_nodecore_callback) return NODECORE_ERROR_CALLBACK_REENTRY;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!s.maintenance_token.empty()) return NODECORE_ERROR_MAINTENANCE_BUSY;
    if (s.maintenance_recovery) return NODECORE_ERROR_RECOVERY_REQUIRED;
    const int gate = dinero::nodecore::MaintenanceJournal::StartupGate(datadir);
    if (gate != NODECORE_OK) return gate;
    if (s.app && !dinero::nodecore::MaintenanceJournal::SameTarget(s.datadir, datadir)) return NODECORE_ERROR_INVALID_ARGS;
    const int closed = CloseNodeLocked(s);
    if (closed != NODECORE_OK) return closed;
    const int prepared = s.maintenance.Prepare(datadir);
    if (prepared != NODECORE_OK) {
        s.maintenance_recovery = dinero::nodecore::MaintenanceJournal::StartupGate(datadir) != NODECORE_OK;
        return prepared;
    }
    try {
        auto value = NewMaintenanceToken(s);
        *token = strdup_c(value);
        if (!*token) throw std::bad_alloc();
        s.maintenance_token = std::move(value);
        return NODECORE_OK;
    } catch (...) {
        s.maintenance_recovery = true;
        return NODECORE_ERROR_INTERNAL;
    }
#else
    (void)datadir; (void)plan;
    return NODECORE_ERROR_UNSUPPORTED_PLAN;
#endif
}
int32_t nodecore_maintenance_finish(const char* token, int32_t outcome) {
#if defined(DINERO_NODECORE_MAINTENANCE_QUALIFICATION)
    if (in_nodecore_callback) return NODECORE_ERROR_CALLBACK_REENTRY;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!token || s.maintenance_token.empty() || s.maintenance_token != token) return NODECORE_ERROR_INVALID_TOKEN;
    if (outcome != 0 && outcome != 1) return NODECORE_ERROR_INVALID_ARGS;
    const int result = outcome == 0 ? s.maintenance.CompleteUnchanged() : s.maintenance.RetainUncertainty();
    if (result != NODECORE_OK) return result; // Keep the owner on verification/IO failure.
    s.maintenance_token.clear();
    s.maintenance_recovery = outcome == 1;
    return outcome == 0 ? NODECORE_OK : NODECORE_ERROR_RECOVERY_REQUIRED;
#else
    (void)token; (void)outcome;
    return NODECORE_ERROR_UNSUPPORTED_PLAN;
#endif
}
int32_t nodecore_maintenance_resume(const char* datadir, const char* operation, char** token) {
    if (token) *token = nullptr;
#if defined(DINERO_NODECORE_MAINTENANCE_QUALIFICATION)
    if (!token || !datadir || !operation) return NODECORE_ERROR_INVALID_ARGS;
    if (in_nodecore_callback) return NODECORE_ERROR_CALLBACK_REENTRY;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!s.maintenance_token.empty()) return NODECORE_ERROR_MAINTENANCE_BUSY;
    if (s.app && !dinero::nodecore::MaintenanceJournal::SameTarget(s.datadir, datadir)) return NODECORE_ERROR_INVALID_ARGS;
    const int checked = s.maintenance.Resume(datadir, operation);
    if (checked != NODECORE_OK) return checked;
    const int closed = CloseNodeLocked(s);
    if (closed != NODECORE_OK) return closed;
    const int loaded = s.maintenance.Resume(datadir, operation);
    if (loaded != NODECORE_OK) return loaded;
    try {
        auto value = NewMaintenanceToken(s);
        *token = strdup_c(value);
        if (!*token) throw std::bad_alloc();
        s.maintenance_token = std::move(value);
        s.maintenance_recovery = false;
        return NODECORE_OK;
    } catch (...) {
        s.maintenance_recovery = true;
        return NODECORE_ERROR_INTERNAL;
    }
#else
    (void)datadir; (void)operation;
    return NODECORE_ERROR_UNSUPPORTED_PLAN;
#endif
}
char* nodecore_maintenance_status(const char* datadir) {
    if (in_nodecore_callback) return nullptr;
    if (!datadir) return nullptr;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    Json::Value result;
    result["gate"] = dinero::nodecore::MaintenanceJournal::StartupGate(datadir, &result);
    result["lease_held"] = !s.maintenance_token.empty();
#if defined(DINERO_NODECORE_MAINTENANCE_QUALIFICATION)
    result["inspection_qualification"] = true;
#else
    result["inspection_qualification"] = false;
#endif
    return json_string(result);
}

// ============================================================================
// Status
// ============================================================================

char* nodecore_get_status_json(void) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!is_queryable_locked(s)) {
        return nullptr;
    }

    auto& ctx = s.app->GetContext();
    Json::Value status;

    status["running"] = true;
    status["network"] = s.network;
    status["utreexo_stateless"] = s.utreexo_stateless;
    status["sync_profile"] = s.sync_profile;
    status["capabilities"] = static_cast<Json::UInt64>(s.capabilities);

    // Chain info
    if (ctx.chainstate) {
        auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.chainstate);
        if (cs) {
            // ChainstateService owns the authoritative active view. During an
            // AssumeUTXO bootstrap its active tip is the verified snapshot base
            // while ChainDB can legitimately remain at genesis until historical
            // validation promotes the replayed chain. Reporting ChainDB here made
            // a healthy embedded node appear to rewind by tens of thousands of
            // blocks after restart.
            const auto active_tip = dinero::nodecore::CaptureAuthoritativeTip(
                cs->GetSyncSnapshot());
            status["height"] = static_cast<Json::UInt64>(active_tip.height);
            status["best_hash"] = active_tip.hash;

            // Mining info: difficulty/nbits/target from the active-tip header.
            if (cs->GetChainDB()) {
                auto hdr_result = cs->GetChainDB()->getHeader(
                    dinero::uint256::FromHexUnsafe(active_tip.hash));
                if (hdr_result.ok()) {
                    uint32_t bits = hdr_result.value().difficulty;
                    status["nbits"] = bits;
                    status["difficulty"] = DifficultyFromCompact(bits);
                    // Target as 64-char hex (256-bit)
                    auto target_arr = dinero::TargetFromBitsBE(bits);
                    status["target"] = bytes_to_hex(target_arr.data(), target_arr.size());
                }
            }
        }
    }

    uint32_t max_peer_height = 0;

    // P2P info
    if (ctx.p2p) {
        auto p2p = std::dynamic_pointer_cast<dinero::P2PService>(ctx.p2p);
        if (p2p) {
            Json::Value peers;
            peers["total"] = static_cast<Json::UInt>(p2p->GetPeerCount());
            for (const auto& peer : p2p->GetConnectedPeers()) {
                max_peer_height = std::max(max_peer_height, std::max(peer.best_known_height, peer.synced_headers));
            }
            peers["best_height"] = static_cast<Json::UInt>(max_peer_height);
            status["peers"] = peers;
        }
    }

    // Mempool
    if (ctx.mempool) {
        auto mp = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.mempool);
        if (mp) {
            status["mempool_size"] = static_cast<Json::UInt>(mp->size());
        }
    }

    // Utreexo forest state
    if (ctx.chainstate) {
        auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.chainstate);
        if (cs) {
            auto forest = cs->GetUtreexoForest();
            if (forest) {
                // One shared lock over ALL forest reads in this block: keeps the
                // reported status self-consistent and prevents a concurrent
                // block-connect replace/mutate from freeing the forest (incl. the
                // getIndexedRoots() reference held across the loop below).
                // Leaf-safe: no other lock is acquired while it is held.
                auto forest_lock = cs->GetConsensusUTXOSet()->LockForestShared();
                Json::Value utreexo;
                utreexo["leaves"] = static_cast<Json::UInt64>(forest->getNumLeaves());
                utreexo["roots"] = static_cast<Json::UInt>(forest->getNumRoots());

                // Forest height (matches chain tip the forest was built to)
                if (status.isMember("height")) {
                    utreexo["height"] = status["height"];
                }

                // Commitment (32-byte hex)
                auto commitment = forest->getCommitment();
                if (commitment.size() == 32) {
                    utreexo["commitment"] = bytes_to_hex(commitment.data(), commitment.size());
                }

                // Root hashes with height indices (for stump construction)
                const auto& indexed = forest->getIndexedRoots();
                utreexo["indexed_root_count"] = static_cast<Json::UInt>(indexed.size());
                Json::Value rootsArr(Json::arrayValue);
                for (size_t h = 0; h < indexed.size(); h++) {
                    if (indexed[h].has_value()) {
                        Json::Value r;
                        r["height"] = static_cast<Json::UInt>(h);
                        r["hash"] = bytes_to_hex(indexed[h]->data(), indexed[h]->size());
                        rootsArr.append(r);
                    }
                }
                utreexo["root_hashes"] = rootsArr;

                status["utreexo"] = utreexo;
            }
        }
    }

    // Sync status.
    //
    // Previously keyed on dinero::g_header_sync_manager, which is never assigned
    // in production — so this branch never ran and the else below always did,
    // permanently reporting headers=0 (#439). Now sourced from the canonical
    // snapshot; the else remains as the fail-closed path when no best header is
    // known (cold start, restart before headers are re-read).
    //
    // NOTE the state separation: header facts come from the snapshot, but the
    // initial-download verdict still comes from IsInIBD(), which is
    // network-height and snapshot aware. Convergence is NOT substituted for it.
    const auto& cs = s.app->GetContext().chainstate;
    dinero::ChainstateService::SyncSnapshot sync;
    if (cs) {
        sync = cs->GetSyncSnapshot();
    }
    if (cs && sync.has_best_header) {
        bool is_ibd = cs->IsInIBD();
        uint32_t header_tip = sync.best_header_height;
        uint64_t validated = status.isMember("height") ? status["height"].asUInt64() : 0;
        uint64_t network_target = std::max<uint64_t>(header_tip, max_peer_height);
        bool peer_ahead = network_target > validated;
        bool syncing = is_ibd || peer_ahead;

        status["headers"] = header_tip;
        status["network_height"] = static_cast<Json::UInt64>(network_target);
        status["syncing"] = syncing;
        if (syncing) {
            if (network_target > 0) {
                double progress = static_cast<double>(validated) / static_cast<double>(network_target);
                status["sync_progress"] = std::min(1.0, std::max(0.0, progress));
            } else {
                status["sync_progress"] = 0.0;
            }
        } else {
            status["sync_progress"] = 1.0;
        }
    } else {
        status["headers"] = 0;
        uint64_t validated = status.isMember("height") ? status["height"].asUInt64() : 0;
        uint64_t network_target = std::max<uint64_t>(validated, max_peer_height);
        bool syncing = network_target > validated;
        status["network_height"] = static_cast<Json::UInt64>(network_target);
        status["syncing"] = syncing;
        if (syncing && network_target > 0) {
            double progress = static_cast<double>(validated) / static_cast<double>(network_target);
            status["sync_progress"] = std::min(1.0, std::max(0.0, progress));
        } else {
            status["sync_progress"] = syncing ? 0.0 : 1.0;
        }
    }

    // Uptime
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - s.start_time
    ).count();
    status["uptime_seconds"] = static_cast<Json::Int64>(uptime);

    // Version
    status["version"] = DINERO_VERSION_STRING;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return strdup_c(Json::writeString(builder, status));
}

uint64_t nodecore_get_height(void) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!is_queryable_locked(s)) return 0;

    auto& ctx = s.app->GetContext();
    if (ctx.chainstate) {
        auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.chainstate);
        if (cs) {
            // Do not expose ChainDB's storage frontier as the node's chain tip.
            // In AssumeUTXO mode the authoritative active tip is the verified
            // snapshot base and ChainDB intentionally lags during history replay.
            return dinero::nodecore::CaptureAuthoritativeTip(
                cs->GetSyncSnapshot()).height;
        }
    }
    return 0;
}

char* nodecore_get_best_hash(void) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!is_queryable_locked(s)) return nullptr;

    auto& ctx = s.app->GetContext();
    if (ctx.chainstate) {
        auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.chainstate);
        if (cs) {
            return strdup_c(dinero::nodecore::CaptureAuthoritativeTip(
                cs->GetSyncSnapshot()).hash);
        }
    }
    return nullptr;
}

bool nodecore_is_synced(void) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!is_queryable_locked(s)) return false;

    // Header-chain convergence via the canonical snapshot (#439).
    //
    // This previously consulted dinero::g_header_sync_manager, which is never
    // assigned in production, so the guard never fired and this function could
    // only ever return false — a fully synced node reported NOT synced,
    // permanently. See issue #439.
    //
    // IsConverged() is true only for an explicit Converged verdict, so a
    // missing selector / best header / active tip still yields false. That
    // preserves the conservative default while fixing the permanent-false bug.
    const auto& chainstate = s.app->GetContext().chainstate;
    if (chainstate) {
        return chainstate->GetSyncSnapshot().IsConverged();
    }
    return false;
}

uint64_t nodecore_capabilities(void) {
    auto& s = state();
    if (s.capabilities != 0) {
        return s.capabilities;
    }
    return dinero::nodecore::CapabilitiesForProfile(dinero::nodecore::DefaultSyncProfile());
}

// ============================================================================
// Event Stream
// ============================================================================

void nodecore_set_event_callback(nodecore_event_callback_t callback, void* user_data) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    s.event_callback.Set(callback, user_data);
}

// ============================================================================
// Wallet Watch
// ============================================================================

int32_t nodecore_watch_script(
    const uint8_t* script_pubkey,
    size_t script_len,
    const char* label
) {
    auto& s = state();
    if (!script_pubkey || script_len == 0) return NODECORE_ERROR_INVALID_ARGS;

    std::lock_guard<std::mutex> lock(s.mtx);

    std::string hex = bytes_to_hex(script_pubkey, script_len);
    s.watched_scripts.Add(hex);

    // If node is running, register with WalletManager's UTXOIndex.
    // Guard against early startup race where WalletService exists but no wallet DB is open yet.
    if (s.app && s.running.load()) {
        auto& ctx = s.app->GetContext();
        if (ctx.wallet) {
            auto ws = std::dynamic_pointer_cast<dinero::WalletService>(ctx.wallet);
            if (ws && ws->get().getCurrentDatabase() != nullptr) {
                std::vector<uint8_t> script(script_pubkey, script_pubkey + script_len);
                std::string path = label ? label : "ios-watch";
                ws->get().addWatchScript(script, path, false);
            }
        }
    }

    return NODECORE_OK;
}

int32_t nodecore_unwatch_script(
    const uint8_t* script_pubkey,
    size_t script_len
) {
    auto& s = state();
    if (!script_pubkey || script_len == 0) return NODECORE_ERROR_INVALID_ARGS;

    std::lock_guard<std::mutex> lock(s.mtx);
    std::string hex = bytes_to_hex(script_pubkey, script_len);
    s.watched_scripts.Remove(hex);

    return NODECORE_OK;
}

int32_t nodecore_watch_scripts_batch(
    const uint8_t* const* scripts,
    const size_t* lengths,
    size_t count
) {
    auto& s = state();
    if (!scripts || !lengths || count == 0) return NODECORE_ERROR_INVALID_ARGS;

    std::lock_guard<std::mutex> lock(s.mtx);

    for (size_t i = 0; i < count; ++i) {
        if (!scripts[i] || lengths[i] == 0) continue;
        std::string hex = bytes_to_hex(scripts[i], lengths[i]);
        s.watched_scripts.Add(hex);

        // Register with WalletManager if running and wallet DB is open.
        if (s.app && s.running.load()) {
            auto& ctx = s.app->GetContext();
            if (ctx.wallet) {
                auto ws = std::dynamic_pointer_cast<dinero::WalletService>(ctx.wallet);
                if (ws && ws->get().getCurrentDatabase() != nullptr) {
                    std::vector<uint8_t> script(scripts[i], scripts[i] + lengths[i]);
                    ws->get().addWatchScript(script, "ios-batch", false);
                }
            }
        }
    }

    return NODECORE_OK;
}

int32_t nodecore_clear_watches(void) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    s.watched_scripts.Clear();
    return NODECORE_OK;
}

int32_t nodecore_rescan(uint64_t from_height) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!is_queryable_locked(s)) return NODECORE_ERROR_NOT_RUNNING;

    auto& ctx = s.app->GetContext();
    if (ctx.wallet) {
        auto ws = std::dynamic_pointer_cast<dinero::WalletService>(ctx.wallet);
        if (ws) {
            dinero::ChainDB* chain_db = nullptr;
            if (ctx.chainstate) {
                auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.chainstate);
                if (cs) {
                    chain_db = cs->GetChainDB();
                }
            }
            ws->get().rescanBlockchain(
                static_cast<int>(from_height),
                20,
                chain_db,
                ctx.block_storage.get()
            );
        }
    }

    return NODECORE_OK;
}

// ============================================================================
// Query
// ============================================================================

char* nodecore_list_unspent_json(int32_t min_confirmations) {
    try {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mtx);
        if (!is_queryable_locked(s)) return nullptr;

        auto& ctx = s.app->GetContext();
        if (!ctx.wallet) return nullptr;

        auto ws = std::dynamic_pointer_cast<dinero::WalletService>(ctx.wallet);
        if (!ws) return nullptr;

        auto wallet_use = dinero::WalletService::AcquireWalletUse(ws);
        auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.chainstate);
        std::unique_ptr<dinero::ChainstateService::WalletIndexUse> index_use;
        if (cs) index_use = dinero::ChainstateService::AcquireWalletIndexUse(cs);
        auto& wallet = wallet_use->Wallet();
        // Pin service lifetimes before the database lease. The runtime mutex
        // also serializes this embedding entry point against close/shutdown.
        auto lease = wallet.AcquireDatabaseLease();
        if (wallet.getUTXOIndex() &&
            (!index_use || wallet.getUTXOIndex() != &index_use->Index()))
            return nullptr;
        auto utxos = wallet.listUnspentUTXOs(min_confirmations);

        Json::Value arr(Json::arrayValue);
        for (const auto& u : utxos) {
            Json::Value item;
            item["txid"] = u.txid;
            item["vout"] = u.vout;
            item["amount"] = static_cast<Json::UInt64>(u.amount_una);
            item["script"] = u.script_pubkey;
            item["address"] = u.address;
            item["height"] = u.height;
            item["confirmations"] = u.confirmations;
            item["coinbase"] = u.is_coinbase;
            item["spendable"] = u.spendable;
            arr.append(item);
        }

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return strdup_c(Json::writeString(builder, arr));
    } catch (...) {
        return nullptr;
    }
}

// ============================================================================
// Transaction
// ============================================================================

char* nodecore_broadcast_tx(const char* tx_hex) {
    auto& s = state();
    if (!tx_hex) return nullptr;

    std::lock_guard<std::mutex> lock(s.mtx);
    if (!is_queryable_locked(s)) return nullptr;

    Json::Value result;

    try {
        auto& ctx = s.app->GetContext();

        // 1. Parse transaction from hex
        dinero::Transaction tx;
        std::string parse_error;
        if (!dinero::consensus::TransactionParser::ParseTransaction(
                std::string(tx_hex), tx, parse_error)) {
            result["error"] = "Failed to parse transaction: " + parse_error;
            Json::StreamWriterBuilder b;
            b["indentation"] = "";
            return strdup_c(Json::writeString(b, result));
        }

        // 2. Get txid
        dinero::TxId txid = tx.GetTxid();
        std::string txid_hex = txid.AsUint256().GetHex();

        // 3. Submit to mempool
        if (!ctx.mempool) {
            result["error"] = "Mempool not available";
            result["txid"] = txid_hex;
            Json::StreamWriterBuilder b;
            b["indentation"] = "";
            return strdup_c(Json::writeString(b, result));
        }

        auto mp = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.mempool);
        if (!mp) {
            result["error"] = "Mempool service unavailable";
            result["txid"] = txid_hex;
            Json::StreamWriterBuilder b;
            b["indentation"] = "";
            return strdup_c(Json::writeString(b, result));
        }

        auto accept_result = mp->mempool().submitTransaction(tx, "ios-wallet", true);
        if (!accept_result.accepted()) {
            result["error"] = accept_result.message;
            result["txid"] = txid_hex;
            Json::StreamWriterBuilder b;
            b["indentation"] = "";
            return strdup_c(Json::writeString(b, result));
        }

        // 4. Relay to peers via TxRelayManager
        int peers_relayed = 0;
        if (ctx.tx_relay) {
            ctx.tx_relay->AnnounceTx(txid.AsUint256());
            if (ctx.p2p) {
                auto p2p = std::dynamic_pointer_cast<dinero::P2PService>(ctx.p2p);
                if (p2p) peers_relayed = static_cast<int>(p2p->GetPeerCount());
            }
        }

        result["txid"] = txid_hex;
        result["accepted"] = true;
        result["peers"] = peers_relayed;

    } catch (const std::exception& e) {
        result["error"] = std::string("Broadcast failed: ") + e.what();
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return strdup_c(Json::writeString(builder, result));
}

char* nodecore_get_transaction(const char* txid_hex) {
    // TODO: Look up transaction in block storage
    return nullptr;
}

// ============================================================================
// Address UTXO Scanner
// ============================================================================

char* nodecore_scan_address_json(const char* address) {
    auto& s = state();
    if (!address) return nullptr;

    std::lock_guard<std::mutex> lock(s.mtx);
    if (!is_queryable_locked(s)) return nullptr;

    Json::Value result;

    try {
        std::string addr(address);

        // Decode bech32m address
        int witver = -1;
        std::vector<uint8_t> witprog;
        std::string hrp = dinero::mining::GetBech32HRP();

        if (!dinero::mining::Bech32DecodeSegwit(addr, hrp, witver, witprog)) {
            result["error"] = "Invalid address: failed to decode bech32";
            Json::StreamWriterBuilder wb;
            wb["indentation"] = "";
            return strdup_c(Json::writeString(wb, result));
        }

        // Build target scriptPubKey hex string for comparison with Coin.script_pubkey
        char hex_buf[3];
        std::string target_script;
        uint8_t op = witver == 0 ? 0x00 : static_cast<uint8_t>(0x50 + witver);
        snprintf(hex_buf, sizeof(hex_buf), "%02x", op);
        target_script += hex_buf;
        snprintf(hex_buf, sizeof(hex_buf), "%02x", static_cast<uint8_t>(witprog.size()));
        target_script += hex_buf;
        for (uint8_t b : witprog) {
            snprintf(hex_buf, sizeof(hex_buf), "%02x", b);
            target_script += hex_buf;
        }

        // Get ChainDB via DaemonContext → ChainstateService
        auto& ctx = s.app->GetContext();
        if (!ctx.chainstate) {
            result["error"] = "Chainstate not available";
            Json::StreamWriterBuilder wb;
            wb["indentation"] = "";
            return strdup_c(Json::writeString(wb, result));
        }
        auto cs = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.chainstate);
        if (!cs || !cs->GetChainDB()) {
            result["error"] = "ChainDB not available";
            Json::StreamWriterBuilder wb;
            wb["indentation"] = "";
            return strdup_c(Json::writeString(wb, result));
        }
        dinero::ChainDB* chain_db = cs->GetChainDB();

        // Get tip height
        int tip_height = 0;
        auto tip_result = chain_db->getTip();
        if (tip_result.ok()) {
            tip_height = tip_result.value().height;
        }

        // Scan UTXO set
        Json::Value utxos_arr(Json::arrayValue);
        uint64_t total_amount = 0;
        uint64_t total_count = 0;

        chain_db->forEachUTXO(
            [&](const dinero::uint256& txid, uint32_t vout, const dinero::Coin& coin) -> bool {
                if (coin.script_pubkey == target_script) {
                    Json::Value entry;
                    entry["txid"] = txid.GetHex();
                    entry["vout"] = vout;
                    entry["amount"] = static_cast<Json::Int64>(coin.amount);
                    entry["height"] = coin.height;
                    entry["coinbase"] = coin.coinbase;
                    utxos_arr.append(entry);
                    total_amount += coin.amount;
                    total_count++;
                }
                return true; // continue iteration
            });

        // Format DIN
        double din = static_cast<double>(total_amount) / 100000000.0;
        char din_buf[64];
        snprintf(din_buf, sizeof(din_buf), "%.8f", din);

        result["address"] = addr;
        result["script"] = target_script;
        result["height"] = tip_height;
        result["utxos"] = utxos_arr;
        result["total_count"] = static_cast<Json::Int64>(total_count);
        result["total_amount"] = static_cast<Json::Int64>(total_amount);
        result["total_din"] = std::string(din_buf);

    } catch (const std::exception& e) {
        result["error"] = std::string("Exception: ") + e.what();
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return strdup_c(Json::writeString(builder, result));
}

// ============================================================================
// Local P2MR / Post-Quantum Wallet Primitives
// ============================================================================

char* nodecore_p2mr_derive_address_json(
    const char* hrp,
    const uint8_t* bip32_priv,
    size_t bip32_priv_len,
    const uint8_t* bip32_chain,
    size_t bip32_chain_len,
    uint32_t leaf_index
) {
    namespace wpq = dinero::wallet::pq;
    namespace mldsa = dinero::consensus::pq::ml_dsa_65;

    if (!hrp || !bip32_priv || !bip32_chain ||
        bip32_priv_len != wpq::BIP32_PRIVKEY_BYTES ||
        bip32_chain_len != wpq::BIP32_CHAINCODE_BYTES) {
        return json_error("invalid P2MR derivation arguments");
    }

    wpq::Bip32PrivKey priv{};
    wpq::Bip32ChainCode chain{};
    std::memcpy(priv.data(), bip32_priv, priv.size());
    std::memcpy(chain.data(), bip32_chain, chain.size());

    try {
        auto pq_seed = wpq::DerivePQSeed(priv, chain, leaf_index);
        mldsa::Seed seed{};
        std::memcpy(seed.data(), pq_seed.data(), seed.size());

        Json::Value out = p2mr_metadata_json(hrp, seed);
        if (out.isMember("ok") && out["ok"].asBool()) {
            out["pq_seed"] = array_to_hex(seed);
            out["leaf_index"] = static_cast<Json::UInt>(leaf_index);
        }

        OPENSSL_cleanse(seed.data(), seed.size());
        OPENSSL_cleanse(pq_seed.data(), pq_seed.size());
        OPENSSL_cleanse(priv.data(), priv.size());
        OPENSSL_cleanse(chain.data(), chain.size());
        return json_string(out);
    } catch (const std::exception& e) {
        OPENSSL_cleanse(priv.data(), priv.size());
        OPENSSL_cleanse(chain.data(), chain.size());
        return json_error(std::string("P2MR derivation failed: ") + e.what());
    } catch (...) {
        OPENSSL_cleanse(priv.data(), priv.size());
        OPENSSL_cleanse(chain.data(), chain.size());
        return json_error("P2MR derivation failed");
    }
}

char* nodecore_p2mr_address_from_seed_json(
    const char* hrp,
    const uint8_t* pq_seed,
    size_t pq_seed_len
) {
    namespace mldsa = dinero::consensus::pq::ml_dsa_65;

    if (!hrp || !pq_seed || pq_seed_len != mldsa::SEED_BYTES) {
        return json_error("invalid P2MR seed arguments");
    }

    mldsa::Seed seed{};
    std::memcpy(seed.data(), pq_seed, seed.size());

    try {
        Json::Value out = p2mr_metadata_json(hrp, seed);
        OPENSSL_cleanse(seed.data(), seed.size());
        return json_string(out);
    } catch (const std::exception& e) {
        OPENSSL_cleanse(seed.data(), seed.size());
        return json_error(std::string("P2MR address reconstruction failed: ") + e.what());
    } catch (...) {
        OPENSSL_cleanse(seed.data(), seed.size());
        return json_error("P2MR address reconstruction failed");
    }
}

char* nodecore_p2mr_sign_witness_json(
    const uint8_t* pq_seed,
    size_t pq_seed_len,
    const uint8_t* sighash,
    size_t sighash_len
) {
    namespace mldsa = dinero::consensus::pq::ml_dsa_65;
    namespace pq = dinero::consensus::pq;

    if (!pq_seed || !sighash ||
        pq_seed_len != mldsa::SEED_BYTES ||
        sighash_len != 32) {
        return json_error("invalid P2MR signing arguments");
    }

    mldsa::Seed seed{};
    std::array<uint8_t, 32> msg{};
    std::memcpy(seed.data(), pq_seed, seed.size());
    std::memcpy(msg.data(), sighash, msg.size());

    try {
        dinero::wallet::SecureKeypair kp(mldsa::KeygenFromSeed(seed));
        auto signature = mldsa::Sign(msg.data(), msg.size(), kp.secret());

        pq::P2MRWitness witness;
        witness.scheme_id = pq::SCHEME_ID_ML_DSA_65;
        witness.pubkey_bytes.assign(kp.pubkey().begin(), kp.pubkey().end());
        witness.signature_bytes.assign(signature.begin(), signature.end());
        witness.merkle_depth = 0;
        witness.leaf_index = 0;

        const auto witness_blob = pq::SerializeP2MRWitness(witness);
        if (witness_blob.empty()) {
            OPENSSL_cleanse(seed.data(), seed.size());
            OPENSSL_cleanse(signature.data(), signature.size());
            return json_error("failed to serialize P2MR witness");
        }

        Json::Value out(Json::objectValue);
        out["ok"] = true;
        out["scheme_id"] = static_cast<Json::UInt>(pq::SCHEME_ID_ML_DSA_65);
        out["pubkey"] = array_to_hex(kp.pubkey());
        out["signature"] = array_to_hex(signature);
        out["witness"] = bytes_to_hex(witness_blob.data(), witness_blob.size());

        OPENSSL_cleanse(seed.data(), seed.size());
        OPENSSL_cleanse(signature.data(), signature.size());
        return json_string(out);
    } catch (const std::exception& e) {
        OPENSSL_cleanse(seed.data(), seed.size());
        return json_error(std::string("P2MR signing failed: ") + e.what());
    } catch (...) {
        OPENSSL_cleanse(seed.data(), seed.size());
        return json_error("P2MR signing failed");
    }
}

// ============================================================================
// RPC Bridge
// ============================================================================

char* nodecore_rpc_call(const char* method, const char* params_json) {
    auto& s = state();
    const auto gate=s.reopen_rpc_gate;
    const auto epoch=gate->Snapshot();
    if(!gate->Allows(epoch))return nullptr; // Retry/no reply, not a queue refusal.
    auto write_json = [&](const Json::Value& value) -> char* {
        if(!gate->Allows(epoch))return nullptr;
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        char* response=strdup_c(Json::writeString(builder, value));
        if(!gate->Allows(epoch)){std::free(response);return nullptr;}
        return response;
    };

    if (!method) {
        Json::Value err(Json::objectValue);
        err["error"] = "Invalid RPC request: method is null";
        err["code"] = -32600;
        return write_json(err);
    }

    std::lock_guard<std::mutex> lock(s.mtx);
    if(!gate->Allows(epoch))return nullptr;
    if (!is_queryable_locked(s)) {
        Json::Value err(Json::objectValue);
        err["error"] = "NodeCore is not running";
        err["code"] = -32000;
        err["running"] = s.running.load();
        err["shutting_down"] = s.shutdown_requested.load();
        return write_json(err);
    }

    Json::Value result(Json::objectValue);
    const std::string method_name = normalize_rpc_method(method);

    try {
        auto& daemon_ctx = s.app->GetContext();

        Json::Value params(Json::arrayValue);
        if (params_json && params_json[0] != '\0') {
            Json::CharReaderBuilder reader;
            std::string errs;
            std::istringstream stream(params_json);
            if (!Json::parseFromStream(reader, stream, &params, &errs)) {
                result["error"] = "Invalid params_json: " + errs;
                return write_json(result);
            }
        }

        ExecutionContext exec_ctx;
        exec_ctx.daemon = &daemon_ctx;
        exec_ctx.logger = daemon_ctx.wallet_logger ? daemon_ctx.wallet_logger : daemon_ctx.logger_interface;

        if (daemon_ctx.wallet) {
            auto ws = std::dynamic_pointer_cast<dinero::WalletService>(daemon_ctx.wallet);
            if (ws) {
                exec_ctx.wallet_manager = &ws->get();
            }
        }

        // Route through the exact same wallet RPC handlers used by dinerod/qt.
        if (method_name == "wallet.getbalance" || method_name == "getbalance") {
            result = rpc_context_wallet_getbalance(exec_ctx, params);
        } else if (method_name == "wallet.getnewaddress" || method_name == "getnewaddress") {
            result = rpc_context_wallet_getnewaddress(exec_ctx, params);
        } else if (method_name == "wallet.sendtoaddress" || method_name == "sendtoaddress") {
            result = rpc_context_wallet_sendtoaddress(exec_ctx, params);
        } else {
            // Generic fallback: execute any method registered in g_rpcRegistry.
            // This keeps iOS behavior aligned with dinerod without per-method stubs.
            RpcHandler* handler = g_rpcRegistry.lookup(method_name);
            if (!handler && method_name.rfind("wallet.", 0) != 0) {
                // Try wallet namespace alias for callers that pass flat names.
                handler = g_rpcRegistry.lookup("wallet." + method_name);
            }

            if (handler) {
                result = (*handler)(exec_ctx, params);
            } else {
                result["error"] = std::string("RPC method not registered: ") + method_name;
            }
        }
    } catch (const std::exception& e) {
        result["error"] = std::string("Exception: ") + e.what();
    }

    return write_json(result);
}

// ============================================================================
// Memory Management
// ============================================================================

void nodecore_free_string(char* str) {
    std::free(str);
}

// ============================================================================
// Version
// ============================================================================

const char* nodecore_version(void) {
    return DINERO_VERSION_STRING;
}

} // extern "C"
