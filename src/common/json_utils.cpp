#include "common/json_utils.h"
#include "common/logger.h"
#include "compat/jsoncpp_compat.h"
#include <sstream>

namespace dinero {

Json::Value parseJson(const std::string& json_str) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["strictRoot"] = true;
    builder["failIfExtra"] = true;   // forbid trailing bytes
    builder["allowSpecialFloats"] = false;
    builder["stackLimit"] = 1024;   // recursion guard
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errors;
    
    bool success = reader->parse(json_str.c_str(), json_str.c_str() + json_str.length(), &root, &errors);
    
    if (!success) {
        g_logger.error("JSON parsing failed: " + errors);
        return Json::Value();
    }
    
    return root;
}

bool parseJsonStrict(const std::string& json_str, Json::Value& out, std::string& error) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["strictRoot"] = true;
    builder["failIfExtra"] = true;   // forbid trailing bytes
    builder["allowSpecialFloats"] = false;
    builder["stackLimit"] = 1024;   // recursion guard
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    
    const char* begin = json_str.c_str();
    const char* end = begin + json_str.length();
    return reader->parse(begin, end, &out, &error);
}

std::string toJsonString(const Json::Value& value, bool pretty) {
    Json::StreamWriterBuilder builder;
    if (pretty) {
        builder["indentation"] = "  ";
    }
    
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    std::stringstream ss;
    writer->write(value, &ss);
    
    return ss.str();
}

bool hasField(const Json::Value& json, const std::string& field) {
    return json.isMember(field);
}

std::string getStringField(const Json::Value& json, const std::string& field, const std::string& default_value) {
    if (!hasField(json, field)) {
        return default_value;
    }
    
    const Json::Value& value = json[field];
    if (value.isString()) {
        return value.asString();
    }
    
    g_logger.warning("Field '" + field + "' is not a string");
    return default_value;
}

int getIntField(const Json::Value& json, const std::string& field, int default_value) {
    if (!hasField(json, field)) {
        return default_value;
    }
    
    const Json::Value& value = json[field];
    if (value.isInt()) {
        return value.asInt();
    } else if (value.isString()) {
        try {
            return std::stoi(value.asString());
        } catch (const std::exception& e) {
            g_logger.warning("Could not convert field '" + field + "' to integer");
            return default_value;
        }
    }
    
    g_logger.warning("Field '" + field + "' is not an integer");
    return default_value;
}

bool getBoolField(const Json::Value& json, const std::string& field, bool default_value) {
    if (!hasField(json, field)) {
        return default_value;
    }
    
    const Json::Value& value = json[field];
    if (value.isBool()) {
        return value.asBool();
    }
    
    g_logger.warning("Field '" + field + "' is not a boolean");
    return default_value;
}

Json::Value getObjectField(const Json::Value& json, const std::string& field) {
    if (!hasField(json, field)) {
        return Json::Value();
    }
    
    const Json::Value& value = json[field];
    if (value.isObject()) {
        return value;
    }
    
    g_logger.warning("Field '" + field + "' is not an object");
    return Json::Value();
}

Json::Value getArrayField(const Json::Value& json, const std::string& field) {
    if (!hasField(json, field)) {
        return Json::Value();
    }
    
    const Json::Value& value = json[field];
    if (value.isArray()) {
        return value;
    }
    
    g_logger.warning("Field '" + field + "' is not an array");
    return Json::Value();
}

Json::Value createRpcRequest(const std::string& method, const Json::Value& params, const std::string& id) {
    Json::Value request;
    request["jsonrpc"] = "1.0";
    request["id"] = id;
    request["method"] = method;
    request["params"] = params;
    return request;
}

Json::Value createRpcResponse(const Json::Value& result, const std::string& id, const Json::Value& error) {
    Json::Value response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    
    if (error.isNull()) {
        response["result"] = result;
    } else {
        response["error"] = error;
    }
    
    return response;
}

bool isRpcError(const Json::Value& response) {
    return hasField(response, "error") && !response["error"].isNull();
}

std::string getRpcErrorMessage(const Json::Value& response) {
    if (!isRpcError(response)) {
        return "";
    }
    
    const Json::Value& error = response["error"];
    if (hasField(error, "message")) {
        return getStringField(error, "message", "");
    }
    
    return "Unknown RPC error";
}

int getRpcErrorCode(const Json::Value& response) {
    if (!isRpcError(response)) {
        return 0;
    }
    
    const Json::Value& error = response["error"];
    return getIntField(error, "code", -1);
}

} // namespace dinero 