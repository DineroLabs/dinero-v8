// Native test executable ONLY. Compile the actual SDK worker with one fixed
// isolated genuine-PoW profile selection, not a shipped configuration escape.
// No owner, proof verifier, database, service or lifecycle function is replaced.
#include "consensus/chainparams.h"
#include <stdexcept>
namespace dinero {
void SelectSdkActivationFixture(Chain chain) {
    if(chain!=Chain::REGTEST)throw std::runtime_error("SDK activation subject is regtest-only");
    SelectParams(chain);
    auto& p=MutableParams();p.magic=0x02a1c925;
    p.state_commitment_activation_height=UINT32_MAX;p.regtest_enforce_pow=true;
    p.sixty_second_activation_height=102;p.orchard_activation_height=102;p.orchard_branch_id=1;
    p.shielded_activation_height=1;p.shielded_epoch_reset_height=UINT32_MAX;
    p.shielded_spend_auth_epoch_reset_height=UINT32_MAX;
    p.orchard_catalog_commitment_activation_height=102;p.state_commitment_burial_depth=1;
}
}
#define SelectParams SelectSdkActivationFixture
#include "../../src/nodecore/nodecore_ffi.cpp"
#undef SelectParams
#include "p2p/orchard_catalog_transfer.h"
namespace dinero {
struct P2PServiceStopTestAccess {
    static bool HasCatalogClient(const P2PService& service){return bool(service.CatalogDownload());}
};
}

// Read-only native-test diagnostics. No production API or state mutation.
namespace {
std::atomic<unsigned> activation_pause_probes{0},activation_pause_refusals{0};
void ObserveActivationPause(int32_t event,const char* text,void*){
    if(event!=NODECORE_EVENT_SYNC_STARTED||!text||
        !std::strstr(text,"authenticated_activation_restarting"))return;
    // The actual worker has HELD its gate, and cannot continue to publication
    // until this callback returns. No sleep, parked lifecycle thread or private
    // gate manipulation is needed to make this real C ABI observation stable.
    try{
        for(const auto* method:{"wallet.orchard.queueshield","wallet.orchard.queuesend"})
            if(char* reply=nodecore_rpc_call(method,"{}")){
                ++activation_pause_refusals;nodecore_free_string(reply);
            }
    }catch(...){++activation_pause_refusals;}
    ++activation_pause_probes;
}
}
void InstallSdkActivationTestEvents(){nodecore_set_event_callback(ObserveActivationPause,nullptr);}
Json::Value SdkActivationTestState(){
    auto& s=state();std::lock_guard lock(s.mtx);Json::Value row;
    row["pause_probes"]=activation_pause_probes.load();row["pause_refusals"]=activation_pause_refusals.load();
    row["running"]=s.running.load();row["shutdown_requested"]=s.shutdown_requested.load();
    if(!s.running||!s.app)return row;
    const auto& context=s.app->GetContext();
    row["global_context_matches"]=&context==DaemonContext::instance();
    row["stateless"]=GetConfig().utreexo_stateless;
    row["profile_valid"]=dinero::consensus::OrchardProfileConfigurationValid(dinero::Params());
    row["work_directory"]=GetConfig().datadir;
    if(context.header_chain){
        const auto best=context.header_chain->GetBestHeaderValue();
        if(best){row["header_height"]=best->height;row["header_hash"]=best->hash.GetHex();}
        try{dinero::uint256 nonce;nonce.begin()[0]=1;
            (void)dinero::p2p::OrchardCatalogTransfer::Request::Selected(*context.header_chain,nonce).Encode();
            row["request_selected"]=true;
        }catch(const std::exception& e){row["request_error"]=e.what();}
    }
    if(const auto chain=std::dynamic_pointer_cast<dinero::ChainstateService>(context.chainstate)){
        row["started"]=chain->IsStarted();row["needs_catalog"]=chain->NeedsOrchardActivationCatalog();
        row["height"]=chain->GetSyncSnapshot().active_tip_height;
    }
    if(const auto peers=std::dynamic_pointer_cast<dinero::P2PService>(context.p2p)){
        row["network_active"]=peers->get().is_network_active();
        row["catalog_client"]=dinero::P2PServiceStopTestAccess::HasCatalogClient(*peers);
        row["proof_ready"]=bool(peers->ReadyActivationCatalog());row["peers"]=Json::Value(Json::arrayValue);
        for(const auto& peer:peers->get().connected_peer_sessions()){
            Json::Value p;p["address"]=peer.address;p["session"]=Json::UInt64(peer.nonce);
            p["services"]=Json::UInt64(peer.services);row["peers"].append(p);
        }
    }
    return row;
}
