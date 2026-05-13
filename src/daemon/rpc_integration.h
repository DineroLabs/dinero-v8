#pragma once

#include "../../rpc/rpc_http.hpp"
#include "compat/jsoncpp_compat.h"

// Convert between Json::Value and jsoncpp Json::Value
Json::Value nlohmann_to_jsoncpp(const Json::Value& nj);
Json::Value jsoncpp_to_nlohmann(const Json::Value& jv);

// Main RPC dispatch function for the daemon
// This bridges our robust rpc_http.hpp (Json::Value) with the existing daemon (jsoncpp)
Json::Value daemon_rpc_dispatch(const std::string& path, const std::string& method,
                                   const Json::Value& params, const Json::Value& id);
