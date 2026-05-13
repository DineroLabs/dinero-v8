#pragma once
#ifdef __APPLE__
#include <json/json.h>
#else
#include <jsoncpp/json/json.h>
#endif
#include <string>
#include <memory>
#include "din_json.h"

namespace dinero::auth { class AuthStore; }

namespace dinero::rpc {

// Legacy Json::Value versions (for compatibility)
Json::Value RpcCreateAuth(Json::Value params, std::shared_ptr<dinero::auth::AuthStore> store);
Json::Value RpcListAuth(Json::Value params, std::shared_ptr<dinero::auth::AuthStore> store);
Json::Value RpcRevokeAuth(Json::Value params, std::shared_ptr<dinero::auth::AuthStore> store);

// New din::Json versions (for direct RPC integration)
din::Json RpcCreateAuthDnr(const din::Json& params, std::shared_ptr<dinero::auth::AuthStore> store);
din::Json RpcListAuthDnr(const din::Json& params, std::shared_ptr<dinero::auth::AuthStore> store);
din::Json RpcRevokeAuthDnr(const din::Json& params, std::shared_ptr<dinero::auth::AuthStore> store);

} // namespace dinero::rpc
