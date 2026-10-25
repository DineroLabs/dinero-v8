// Test transport for the linked production C ABI, plus a read-only observation
// of the retired ChainDB pointer (never dereferenced).
// Replies use a separate file because DaemonApp owns stdout/stderr logging.
#include "nodecore/nodecore_ffi.h"
#include "storage/chain_direct.h"
#include "daemon/daemon_app.h"
#include "daemon/datadir_guard.h"
#include "daemon/reopen_candidate_journal.h"
#include "daemon/daemon_context.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/rpc_service.h"
#include "daemon/http_rpc_server.h"
#include "consensus/chainparams.h"
#include "rpc/rpc_registry.h"

#include <json/json.h>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct HttpRpcRequestTestAccess {
    static Json::Value Dispatch(HttpRpcServer& server,const std::string& method,const Json::Value& params){
        Json::Value request;request["jsonrpc"]="2.0";request["id"]=84;request["method"]=method;request["params"]=params;
        return server.process_rpc_call(request);
    }
};

namespace {
using namespace std::chrono_literals;

Json::Value Parse(const std::string& text) {
    Json::Value value;
    Json::CharReaderBuilder reader;
    std::string error;
    std::istringstream input(text);
    if (!Json::parseFromStream(reader, input, &value, &error)) {
        throw std::runtime_error(error);
    }
    return value;
}

std::string Encode(const Json::Value& value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

Json::Value Owned(char* text) {
    if (!text) return Json::Value();
    const std::string copy(text);
    nodecore_free_string(text);
    return Parse(copy);
}

struct ShutdownBarrier {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    bool timed_out = false;
    bool probe_reentry = false;
    std::string datadir;
    int32_t reentrant_stop = 0;
    int32_t reentrant_start = 0;
    int32_t reentrant_begin = 0;

    static void Callback(int32_t event, const char*, void* context) {
        if (event != NODECORE_EVENT_SHUTDOWN) return;
        auto& gate = *static_cast<ShutdownBarrier*>(context);
        if (gate.probe_reentry) {
            // These new entry-point guards must return before taking the
            // mutex held by the operation joining this callback's thread.
            gate.reentrant_stop = nodecore_stop();
            gate.reentrant_start = nodecore_start(gate.datadir.c_str(), "{}");
            char* token = nullptr;
            gate.reentrant_begin = nodecore_maintenance_begin(gate.datadir.c_str(), "inspect-unchanged-v1", &token);
            nodecore_free_string(token);
        }
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        // Always release, including a broken test driver.
        if (!gate.changed.wait_for(lock, 10s, [&] { return gate.released; })) {
            gate.timed_out = true;
        }
    }
};

Json::Value StopAtCallback(const Json::Value& request) {
    ShutdownBarrier gate;
    gate.probe_reentry = request["op"] == "maintenance_at_callback";
    gate.datadir = request["datadir"].asString();
    char* token = nullptr;
    nodecore_set_event_callback(ShutdownBarrier::Callback, &gate);
    auto stopped = std::async(std::launch::async, [&] {
        return gate.probe_reentry
            ? nodecore_maintenance_begin(gate.datadir.c_str(), "inspect-unchanged-v1", &token)
            : nodecore_stop();
    });
    Json::Value result;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        result["callback_entered"] = gate.changed.wait_for(lock, 10s, [&] { return gate.entered; });
    }
    result["running_during_callback"] = nodecore_is_running();
    result["stop_waits_for_callback"] = stopped.wait_for(100ms) == std::future_status::timeout;
    std::future<int32_t> restarted;
    if (request.isMember("restart_datadir")) {
        const auto path = request["restart_datadir"].asString();
        const auto config = Encode(request["config"]);
        std::promise<void> attempted;
        auto attempt = attempted.get_future();
        restarted = std::async(std::launch::async, [ready = std::move(attempted), path, config]() mutable {
            ready.set_value();
            return nodecore_start(path.c_str(), config.c_str());
        });
        result["start_attempted"] = attempt.wait_for(10s) == std::future_status::ready;
        result["start_waits_for_stop"] = restarted.wait_for(100ms) == std::future_status::timeout;
    }
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.released = true;
        gate.changed.notify_all();
    }
    result["stop_result"] = stopped.get();
    if (gate.probe_reentry) {
        result["token"] = token ? Json::Value(token) : Json::Value();
        nodecore_free_string(token);
        result["reentrant_stop"] = gate.reentrant_stop;
        result["reentrant_start"] = gate.reentrant_start;
        result["reentrant_begin"] = gate.reentrant_begin;
    }
    if (restarted.valid()) result["restart_result"] = restarted.get();
    nodecore_set_event_callback(nullptr, nullptr);
    result["callback_timed_out"] = gate.timed_out;
    return result;
}

// Real offline DaemonApp bootstrap, stopped BEFORE its second (migrating)
// Init. Supplies a nine-family genesis cohort without test writes to ChainDB.
Json::Value LegacyStorageFixture(const std::string& path) {
    if (nodecore_is_running() || !std::filesystem::path(path).is_absolute() ||
        std::filesystem::exists(std::filesystem::path(path)/"blockchain"))
        throw std::runtime_error("legacy fixture requires a fresh isolated root");
    dinero::daemon::DatadirGuard guard;std::string error;
    if (!guard.Acquire(path,error)) throw std::runtime_error(error);
    dinero::SelectParams(dinero::Chain::MAINNET);
    std::vector<std::string> args{"legacy-storage-fixture", "--datadir="+guard.DataDir().string(),
        "--p2p.offline=1","--listen=0","--rpc.port=0","--wallet.socket.enable=0",
        "--lightning.oracles.enable=0","--sync-profile=mac_fullblock","--utreexo-stateless=0"};
    std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());
    dinero::DaemonApp app;app.SetLockedDatadir(guard);app.SetAutomaticStorageUpgrade(true);
    app.SetFreshStoreBootstrapOnly(true);
    if(!app.Init(int(argv.size()),argv.data())||!app.FreshStoreCreated()||!app.Start()||
        !app.PersistFreshStoreGenesisRecords())throw std::runtime_error("legacy fixture bootstrap failed");
    app.Stop();return 0;
}
Json::Value ActualAppReopen(const std::string& path){
    if(nodecore_is_running()||!std::filesystem::path(path).is_absolute()||std::filesystem::exists(std::filesystem::path(path)/"blockchain"))
        throw std::runtime_error("reopen fixture needs a fresh isolated root");
    dinero::daemon::DatadirGuard guard;std::string error;
    if(!guard.Acquire(path,error))throw std::runtime_error(error);
    dinero::SelectParams(dinero::Chain::REGTEST);
    std::vector<std::string> args{"real-app-reopen-fixture","--regtest","--datadir="+guard.DataDir().string(),
        "--p2p.offline=1","--listen=0","--rpc.port=0","--wallet.socket.enable=0",
        "--lightning.oracles.enable=0","--sync-profile=mac_fullblock","--utreexo-stateless=0"};
    std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());
    auto initialize=[&](dinero::DaemonApp& app){
        app.SetLockedDatadir(guard);app.SetAutomaticStorageUpgrade(true);
        if(!app.Init(int(argv.size()),argv.data())||!app.Start())throw std::runtime_error("real reopen App startup failed");
    };
    auto call=[](dinero::DaemonApp& app,const std::string& method,const Json::Value& params){
        const auto rpc=std::dynamic_pointer_cast<dinero::RPCService>(app.GetContext().rpc);
        if(!rpc||!rpc->GetHttpServer())throw std::runtime_error("real App HTTP dispatcher missing");
        return HttpRpcRequestTestAccess::Dispatch(*rpc->GetHttpServer(),method,params);
    };
    auto old=std::make_unique<dinero::DaemonApp>();initialize(*old);
    Json::Value restore;restore["name"]="reopen-owner";
    restore["mnemonic"]="abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    restore["rescan"]=false;
    if(!call(*old,"wallet.restore",restore)["result"]["success"].asBool())throw std::runtime_error("fixture named restore failed");
    const auto gate=old->GetContext().reopen_rpc_gate;const auto epoch=gate->Snapshot();
    auto pause=std::shared_ptr<dinero::ReopenRpcGate::Pause>(dinero::ReopenRpcGate::Begin(gate));
    auto receipt=old->CloseForInternalReopen(guard,pause);
    if(receipt->WalletName()!="reopen-owner"||gate->Allows(epoch))throw std::runtime_error("close did not fence/retain named wallet");
    std::string selected_wallet;
    auto pending=dinero::daemon::ReopenCandidateJournal::OpenPending(guard,selected_wallet);
    if(!pending||pending->Name()!=receipt->CandidateJournal()||selected_wallet!="reopen-owner")
        throw std::runtime_error("close did not durably select the exact named-wallet intent");
    old.reset(); // ALL raw forest/bridge/validation readers end before new Init.
    receipt->CheckBeforeInit(guard);
    auto reopened=std::make_unique<dinero::DaemonApp>();reopened->GetContext().reopen_rpc_gate=receipt->Gate();initialize(*reopened);
    Json::Value result;result["paused_queues_retryable"]=true;
    for(const auto* method:{"wallet.orchard.queueshield","wallet.orchard.queuespend","wallet.orchard.queuesend"}){
        const auto paused=call(*reopened,method,Json::Value(Json::objectValue));
        result["paused_queues_retryable"]=result["paused_queues_retryable"].asBool()&&
            paused["error"]["code"].asInt()==-28&&paused["error"]["data"]["retryable"].asBool()&&!paused.isMember("result");
    }
    const auto admitted=reopened->ResumeAfterInternalReopen(guard,*receipt);
    result["completed_intent_archived"]=!dinero::daemon::ReopenCandidateJournal::OpenPending(guard,selected_wallet);
    result["candidates"]=Json::UInt64(admitted.candidates);result["resolved"]=Json::UInt64(admitted.resolved);result["retained"]=Json::UInt64(admitted.retained);
    Json::Value open(Json::arrayValue);open.append("reopen-owner");const auto opened=call(*reopened,"wallet.open",open);
    result["wallet_open_without_phrase"]=opened["result"]["success"].asBool();
    result["wallet_name"]=opened["result"]["wallet_name"];result["stale_epoch_refused"]=!gate->Allows(epoch);
    try{(void)reopened->ResumeAfterInternalReopen(guard,*receipt);result["repeat_resume_refused"]=false;}
    catch(const std::logic_error&){result["repeat_resume_refused"]=true;}
    result["resumed_epoch_allowed"]=gate->Allows(gate->Snapshot());
    reopened->Stop();reopened.reset();return result;
}
Json::Value ReopenFences(){
    auto* context=DaemonContext::instance();
    if(!nodecore_is_running()||!context||!context->wallet||!context->mempool)
        throw std::runtime_error("real running Core required for fence probe");
    const auto wallet=std::dynamic_pointer_cast<dinero::WalletService>(context->wallet);
    const auto pool=std::dynamic_pointer_cast<dinero::MempoolService>(context->mempool);
    Json::Value result;
    {auto use=dinero::WalletService::AcquireWalletUse(wallet);
     try{auto bad=dinero::WalletService::FenceForReopen(wallet,0ms);result["wallet_reentrant_refused"]=false;}
     catch(const std::exception&){result["wallet_reentrant_refused"]=true;}}
    {auto use=dinero::MempoolService::AcquirePoolUse(pool);
     try{auto bad=dinero::MempoolService::FenceForReopen(pool,0ms);result["pool_reentrant_refused"]=false;}
     catch(const std::exception&){result["pool_reentrant_refused"]=true;}}
    const auto wf=dinero::WalletService::FenceForReopen(wallet,100ms);
    const auto pf=dinero::MempoolService::FenceForReopen(pool,100ms);
    result["wallet_name"]=wallet->ReopenWalletName(*wf);unsigned count=0;
    try{(void)wallet->ReopenWalletName(*pf);result["foreign_fence_refused"]=false;}
    catch(const std::exception&){result["foreign_fence_refused"]=true;}
    pool->ForEachReopenCandidate(*pf,[&](const auto&){++count;});result["candidate_count"]=count;
    try{auto use=dinero::WalletService::AcquireWalletUse(wallet);result["wallet_gate_closed"]=false;}
    catch(const std::exception&){result["wallet_gate_closed"]=true;}
    try{auto use=dinero::MempoolService::AcquirePoolUse(pool);result["pool_gate_closed"]=false;}
    catch(const std::exception&){result["pool_gate_closed"]=true;}
    try{wallet->Stop();result["wallet_stop_requires_decision"]=false;}
    catch(const std::exception&){result["wallet_stop_requires_decision"]=true;}
    try{pool->Stop();result["pool_stop_requires_decision"]=false;}
    catch(const std::exception&){result["pool_stop_requires_decision"]=true;}
    // Abandoning both leases restores the exact prior owners, not new stores.
    return result;
}
Json::Value ReopenRpcPause(){
    auto* context=DaemonContext::instance();
    if(!nodecore_is_running()||!context||!context->wallet||!context->mempool)
        throw std::runtime_error("real running Core required for RPC pause probe");
    const auto gate=context->reopen_rpc_gate;
    const auto before=gate->Snapshot();Json::Value result;
    {
        const auto pause=dinero::ReopenRpcGate::Begin(gate);
        try{auto bad=dinero::ReopenRpcGate::Begin(gate);result["nested_refused"]=false;}
        catch(const std::logic_error&){result["nested_refused"]=true;}
        const auto wallet=std::dynamic_pointer_cast<dinero::WalletService>(context->wallet);
        const auto pool=std::dynamic_pointer_cast<dinero::MempoolService>(context->mempool);
        const auto wf=dinero::WalletService::FenceForReopen(wallet,100ms);
        const auto pf=dinero::MempoolService::FenceForReopen(pool,100ms);
        for(const auto* method:{"wallet.orchard.queueshield","wallet.orchard.queuespend","wallet.orchard.queuesend","getblockcount"}){
            char* reply=nodecore_rpc_call(method,"{}");result[method]=reply==nullptr;nodecore_free_string(reply);
        }
        // Both fences abort back to the SAME owners before the RPC pause ends.
    }
    result["stale_epoch_refused"]=!gate->Allows(before);
    result["resumed_epoch_allowed"]=gate->Allows(gate->Snapshot());
    // A deterministic complete pause/abort INSIDE dispatch must suppress an
    // otherwise semantic refusal even though admission has already reopened.
    if(!g_rpcRegistry.registerHandler("test.reopenepoch",[gate](const ExecutionContext&,const Json::Value&){
        {auto pause=dinero::ReopenRpcGate::Begin(gate);}
        Json::Value refused;refused["error"]="must not escape";refused["accepted"]=false;return refused;
    },"test"))throw std::runtime_error("pause probe registration refused");
    char* reply=nodecore_rpc_call("test.reopenepoch","{}");
    result["overlap_refusal_suppressed"]=reply==nullptr;nodecore_free_string(reply);
    return result;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ofstream replies(argv[1]);
    if (!replies) return 2;
    std::string line;
    while (std::getline(std::cin, line)) {
        Json::Value result;
        try {
            const auto request = Parse(line);
            const auto operation = request["op"].asString();
            if (operation == "reopen_fences") {
                result = ReopenFences();
            } else if (operation == "actual_app_reopen") {
                result = ActualAppReopen(request["datadir"].asString());
            } else if (operation == "reopen_rpc_pause") {
                result = ReopenRpcPause();
            } else if (operation == "legacy_storage_fixture") {
                result = LegacyStorageFixture(request["datadir"].asString());
            } else if (operation == "start") {
                const auto path = request["datadir"].asString();
                const auto config = Encode(request["config"]);
                result = nodecore_start(path.c_str(), config.c_str());
            } else if (operation == "stop") {
                result = nodecore_stop();
            } else if (operation == "stop_at_callback" || operation == "maintenance_at_callback") {
                result = StopAtCallback(request);
            } else if (operation == "maintenance_begin" || operation == "maintenance_resume") {
                char* token = nullptr;
                const auto path = request["datadir"].asString();
                const auto plan = request["plan"].asString();
                const auto id = request["operation_id"].asString();
                result["code"] = operation == "maintenance_begin"
                    ? nodecore_maintenance_begin(path.c_str(), plan.c_str(), &token)
                    : nodecore_maintenance_resume(path.c_str(), id.c_str(), &token);
                if (token) { result["token"] = token; nodecore_free_string(token); }
                else result["token"] = Json::Value();
            } else if (operation == "maintenance_finish") {
                const auto token = request["token"].asString();
                result = nodecore_maintenance_finish(token.c_str(), request["outcome"].asInt());
            } else if (operation == "maintenance_status") {
                const auto path = request["datadir"].asString();
                result = Owned(nodecore_maintenance_status(path.c_str()));
            } else if (operation == "status") {
                result = Owned(nodecore_get_status_json());
            } else if (operation == "legacy_chain_db_bound") {
                // Observe the legacy pointer without dereferencing possibly
                // freed storage. No production hook or fabricated DB involved.
                result = dinero::g_chain_db_direct != nullptr;
            } else if (operation == "rpc") {
                const auto method = request["method"].asString();
                const auto params = Encode(request["params"]);
                result = Owned(nodecore_rpc_call(method.c_str(), params.c_str()));
            } else {
                throw std::runtime_error("unknown test operation");
            }
        } catch (const std::exception& error) {
            result["driver_error"] = error.what();
        }
        replies << Encode(result) << '\n' << std::flush;
    }
    return nodecore_stop() == NODECORE_OK ? 0 : 1;
}
