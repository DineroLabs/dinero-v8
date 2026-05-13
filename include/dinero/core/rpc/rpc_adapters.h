#pragma once
#ifdef __APPLE__
#include <json/json.h>
#else
#include <jsoncpp/json/json.h>
#endif
#include <functional>

namespace rpc {
  using Params   = Json::Value;
  using JsonFunc = std::function<Json::Value(const Params&)>;
  using StrFunc  = std::function<std::string(const Params&)>;

  inline std::string json_to_string(const Json::Value& v) {
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";               // compact output
    return Json::writeString(wb, v);
  }

  // Adapter: JSON-returning handler -> string-returning handler
  inline StrFunc adaptJson(JsonFunc f) {
    return [f](const Params& p) {
      return json_to_string(f(p));
    };
  }

  // RPC handler types
  using RpcJsonHandler = std::function<Json::Value(const Json::Value&)>;
  using LegacyStringHandler = std::function<std::string(const std::string&)>;
  
  // Legacy string handler adapter for existing StrFunc interface
  inline StrFunc adapt(LegacyStringHandler lsh) {
    return [lsh](const Params& p) {
      // Convert Json::Value params to string for legacy handler
      std::string params_str = json_to_string(p);
      return lsh(params_str);
    };
  }

  // Helper functions for string-returning functions that produce Json::Value
  inline std::string createSuccessResponseStr(const Json::Value& v) {
    Json::Value response;
    response["result"] = v;
    return json_to_string(response);
  }

  inline std::string createErrorResponseStr(int code, const std::string& msg) {
    Json::Value response;
    response["error"]["code"] = code;
    response["error"]["message"] = msg;
    return json_to_string(response);
  }

  // JSON-RPC 2.0 envelope helpers
  inline std::string makeRpcOk(const Json::Value& id, const Json::Value& result) {
    Json::Value resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = result;
    return json_to_string(resp);
  }

  inline std::string makeRpcErr(const Json::Value& id, int code, const std::string& msg) {
    Json::Value e;
    e["code"] = code;
    e["message"] = msg;
    Json::Value resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["error"] = e;
    return json_to_string(resp);
  }

  // Single adapter from JSON handler to LegacyStringHandler
  inline LegacyStringHandler adaptJsonToLegacy(std::function<Json::Value(const Json::Value&)> fn) {
    return [fn](const std::string& raw) -> std::string {
      Json::CharReaderBuilder rb;
      std::unique_ptr<Json::CharReader> r(rb.newCharReader());
      Json::Value req;
      std::string errs;
      if (!r->parse(raw.data(), raw.data() + raw.size(), &req, &errs)) {
        return makeRpcErr(Json::Value(), -32700, "Parse error");
      }

      const Json::Value id = req.isMember("id") ? req["id"] : Json::Value();
      const Json::Value params = req.isMember("params") ? req["params"] : Json::Value(Json::arrayValue);

      Json::Value result = fn(params);
      return makeRpcOk(id, result);
    };
  }
}
