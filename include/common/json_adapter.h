#pragma once

#include "compat/jsoncpp_compat.h"
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace dinero {
namespace jj {

using V = Json::Value;

// Factory functions
inline V obj() { return V(Json::objectValue); }
inline V arr() { return V(Json::arrayValue); }

// Hex encoding/decoding utilities
inline std::string toHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<unsigned>(b);
    }
    return oss.str();
}

inline std::string toHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return oss.str();
}

inline bool fromHex(std::string_view hex, std::vector<uint8_t>& out) {
    if (hex.length() % 2 != 0) return false;
    
    out.clear();
    out.reserve(hex.length() / 2);
    
    for (size_t i = 0; i < hex.length(); i += 2) {
        unsigned int byte;
        if (std::sscanf(hex.data() + i, "%2x", &byte) != 1) {
            return false;
        }
        out.push_back(static_cast<uint8_t>(byte));
    }
    return true;
}

// JSON field setters (snake_case naming)
inline void putHex(V& o, const char* k, const std::vector<uint8_t>& b) {
    o[k] = toHex(b);
}

inline void putHex(V& o, const char* k, const uint8_t* data, size_t len) {
    o[k] = toHex(data, len);
}

inline void putNum(V& o, const char* k, uint64_t n) {
    o[k] = Json::Value::UInt64(n);
}

inline void putNum(V& o, const char* k, int64_t n) {
    o[k] = Json::Value::Int64(n);
}

inline void putNum(V& o, const char* k, uint32_t n) {
    o[k] = Json::Value::UInt(n);
}

inline void putNum(V& o, const char* k, int32_t n) {
    o[k] = Json::Value::Int(n);
}

inline void putStr(V& o, const char* k, std::string_view s) {
    o[k] = std::string(s);
}

inline void putBool(V& o, const char* k, bool b) {
    o[k] = b;
}

inline void putObj(V& o, const char* k, const V& obj) {
    o[k] = obj;
}

inline void putArr(V& o, const char* k, const V& arr) {
    o[k] = arr;
}

// JSON field getters with error handling
inline bool getHex(const V& o, const char* k, std::vector<uint8_t>& out) {
    if (!o.isMember(k) || !o[k].isString()) {
        return false;
    }
    return fromHex(o[k].asString(), out);
}

inline bool getNum(const V& o, const char* k, uint64_t& out) {
    if (!o.isMember(k) || !o[k].isUInt64()) {
        return false;
    }
    out = o[k].asUInt64();
    return true;
}

inline bool getNum(const V& o, const char* k, int64_t& out) {
    if (!o.isMember(k) || !o[k].isInt64()) {
        return false;
    }
    out = o[k].asInt64();
    return true;
}

inline bool getNum(const V& o, const char* k, uint32_t& out) {
    if (!o.isMember(k) || !o[k].isUInt()) {
        return false;
    }
    out = o[k].asUInt();
    return true;
}

inline bool getNum(const V& o, const char* k, int32_t& out) {
    if (!o.isMember(k) || !o[k].isInt()) {
        return false;
    }
    out = o[k].asInt();
    return true;
}

inline bool getStr(const V& o, const char* k, std::string& out) {
    if (!o.isMember(k) || !o[k].isString()) {
        return false;
    }
    out = o[k].asString();
    return true;
}

inline bool getBool(const V& o, const char* k, bool& out) {
    if (!o.isMember(k) || !o[k].isBool()) {
        return false;
    }
    out = o[k].asBool();
    return true;
}

inline bool getObj(const V& o, const char* k, V& out) {
    if (!o.isMember(k) || !o[k].isObject()) {
        return false;
    }
    out = o[k];
    return true;
}

inline bool getArr(const V& o, const char* k, V& out) {
    if (!o.isMember(k) || !o[k].isArray()) {
        return false;
    }
    out = o[k];
    return true;
}

// JSON-RPC 2.0 envelope helpers
inline V makeRpcResponse(const V& id, const V& result) {
    V response = obj();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    response["error"] = Json::Value::null;
    response["schema"] = "din.rpc.v1";
    return response;
}

inline V makeRpcError(const V& id, int code, const std::string& message, const V& data = Json::Value::null) {
    V error = obj();
    error["code"] = code;
    error["message"] = message;
    if (!data.isNull()) {
        error["data"] = data;
    }
    
    V response = obj();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = Json::Value::null;
    response["error"] = error;
    response["schema"] = "din.rpc.v1";
    return response;
}

// Compact vs pretty printing
inline std::string toString(const V& value, bool pretty = false) {
    if (pretty) {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        return Json::writeString(builder, value);
    } else {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return Json::writeString(builder, value);
    }
}

} // namespace jj
} // namespace dinero
