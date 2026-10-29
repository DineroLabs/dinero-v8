#pragma once
#include "rpc/orchard_account_rpc.h"
namespace {
din::Json OrchardBoundParamsForTest(const ExecutionContext& ctx,din::Json params) {
    if(!ctx.daemon){params["wallet_binding"]=std::string(64,'1');return params;}
    din::Json request;request["wallet_name"]=ctx.walletName;
    const auto binding=rpc_context_wallet_orchard_getwalletbinding(ctx,request);
    if(binding.isMember("error") || !binding["wallet_binding"].isString())
        throw std::runtime_error("fixture wallet binding unavailable");
    params["wallet_binding"]=binding["wallet_binding"];return params;
}
}
