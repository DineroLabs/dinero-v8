#pragma once
#include <json/json.h>           // JsonCpp header

namespace dinero {
Json::Value daemon_rpc_dispatch(const std::string& path,
                                const std::string& method,
                                const Json::Value& params,
                                const Json::Value& id);
}
