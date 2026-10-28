// Test-only RPC bridge to the real payer-bound retained funding owner.
// The installed daemon never links this file or these RPC methods.
#include "daemon/daemon_context.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/swap_service.h"
#include "rpc/rpc_registry.h"
#include "util/hex.h"
#include "consensus/chainparams.h"
#include <algorithm>
#include <filesystem>
#include <stdexcept>

// Reuse the production startup, datadir locking, parameter checks and shutdown.
#define main SwapFixtureProductionMain
#include "../../src/daemon/main.cpp"
#undef main

namespace {
void RequireFixtureContext(const ExecutionContext& ctx) {
    if (dinero::Params().name != "regtest" || !ctx.daemon || !ctx.daemon->wallet)
        throw std::runtime_error("Swap funding fixture requires a live regtest wallet");
}
din::Json FixtureIdentity(const ExecutionContext& ctx,const din::Json&) {
    RequireFixtureContext(ctx);
    auto use=dinero::WalletService::AcquireWalletUse(ctx.daemon->wallet);
    auto lease=use->Wallet().AcquireDatabaseLease();
    const auto id=lease->ReadDeliveryIdentity();
    din::Json result;
    result["wallet_id"]=util::hex(std::vector<uint8_t>(id.begin(),id.end()));
    return result;
}
din::Json FixtureFunding(const ExecutionContext& ctx,const din::Json& p) {
    RequireFixtureContext(ctx);
    if(!p.isObject() || !p["wallet_id"].isString() || !p["offer_id"].isString() ||
       !p["address"].isString() || !p["amount_una"].isUInt64() ||
       !p["fee_rate_hint"].isUInt64() || !p["maximum_fee_una"].isUInt64() ||
       !p["operation"].isUInt()) throw std::runtime_error("Invalid fixture funding request");
    dinero::swap::DinFundingRequest request;
    request.binding=dinero::swap::ParseDinFundingBinding(p["wallet_id"].asString(),
        std::to_string(p["fee_rate_hint"].asUInt64()),std::to_string(p["maximum_fee_una"].asUInt64()));
    std::vector<uint8_t> offer;
    if(!util::unhex(p["offer_id"].asString(),offer) || offer.size()!=request.offer_id.size())
        throw std::runtime_error("Invalid fixture offer identity");
    std::copy(offer.begin(),offer.end(),request.offer_id.begin());
    request.address=p["address"].asString();request.amount_una=p["amount_una"].asUInt64();
    const auto op=p["operation"].asUInt();
    if(op>static_cast<unsigned>(dinero::swap::DinFundingOperation::SubmitRetained))
        throw std::runtime_error("Invalid fixture funding operation");
    std::vector<uint8_t> expected;
    if(p.isMember("expected_body") && (!p["expected_body"].isString() ||
       !util::unhex(p["expected_body"].asString(),expected)))
        throw std::runtime_error("Invalid fixture retained body");
    const auto result=dinero::MakeRetainedSwapFundingOwner(*ctx.daemon)(request,
        static_cast<dinero::swap::DinFundingOperation>(op),expected);
    din::Json reply;reply["hex"]=util::hex(result.raw);reply["txid"]=result.txid;
    return reply;
}
}
int main(int argc,char** argv) try {
    bool regtest=false;std::filesystem::path directory;
    for(int i=1;i<argc;++i) {
        const std::string arg(argv[i]);
        if(arg=="-regtest" || arg=="--regtest")regtest=true;
        for(const std::string prefix:{"-datadir=","--datadir="})
            if(arg.rfind(prefix,0)==0)directory=arg.substr(prefix.size());
    }
    const auto base=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
    const auto path=std::filesystem::weakly_canonical(directory);
    const auto relative=path.lexically_relative(base);
    if(!regtest || directory.empty() || relative.empty() || relative=="." || *relative.begin()=="..")
        throw std::runtime_error("Swap fixture requires explicit regtest and a private temporary datadir");
    g_rpcRegistry.registerHandler("test.swap_funding.identity",FixtureIdentity,RegisterMode::Overwrite,"swap-e2e-fixture");
    g_rpcRegistry.registerHandler("test.swap_funding.owner",FixtureFunding,RegisterMode::Overwrite,"swap-e2e-fixture");
    return SwapFixtureProductionMain(argc,argv);
} catch(const std::exception& error) {
    std::cerr<<"Swap fixture: "<<error.what()<<'\n';return 1;
}
