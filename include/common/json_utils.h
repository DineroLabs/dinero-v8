#pragma once

#include <string>
#include "compat/jsoncpp_compat.h"

namespace dinero {

// JSON parsing and creation
Json::Value parseJson(const std::string& json_str);
bool parseJsonStrict(const std::string& json_str, Json::Value& out, std::string& error);
std::string toJsonString(const Json::Value& value, bool pretty = false);

// JSON field access with type safety
bool hasField(const Json::Value& json, const std::string& field);
std::string getStringField(const Json::Value& json, const std::string& field, const std::string& default_value = "");
int getIntField(const Json::Value& json, const std::string& field, int default_value = 0);
bool getBoolField(const Json::Value& json, const std::string& field, bool default_value = false);
Json::Value getObjectField(const Json::Value& json, const std::string& field);
Json::Value getArrayField(const Json::Value& json, const std::string& field);

// RPC-specific JSON utilities
Json::Value createRpcRequest(const std::string& method, const Json::Value& params, const std::string& id = "1");
Json::Value createRpcResponse(const Json::Value& result, const std::string& id, const Json::Value& error = Json::Value());
bool isRpcError(const Json::Value& response);
std::string getRpcErrorMessage(const Json::Value& response);
int getRpcErrorCode(const Json::Value& response);

} // namespace dinero 