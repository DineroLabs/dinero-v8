/**
 * LibFuzzer target for JSON-RPC request parsing/dispatch shape checks.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <json/json.h>

namespace {

std::string DispatchMethod(const Json::Value& request) {
    if (!request.isObject()) {
        return R"({"error":{"code":-32600,"message":"Invalid Request"}})";
    }

    const Json::Value& method = request["method"];
    if (!method.isString()) {
        return R"({"error":{"code":-32600,"message":"Missing method"}})";
    }

    const std::string method_name = method.asString();

    if (method_name == "getblockchaininfo") {
        return R"({"result":{"chain":"main","blocks":12345}})";
    }
    if (method_name == "getnewaddress") {
        return R"({"result":{"address":"din1qexample123"}})";
    }
    if (method_name == "sendtoaddress") {
        const Json::Value& params = request["params"];
        if (!params.isArray() || params.size() < 2) {
            return R"({"error":{"code":-32602,"message":"Invalid params"}})";
        }
        return R"({"result":"txid123"})";
    }
    if (method_name == "startmining" || method_name == "stopmining") {
        return R"({"result":true})";
    }

    return R"({"error":{"code":-32601,"message":"Method not found"}})";
}

bool ParseJson(const std::string& text, Json::Value& out) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["allowTrailingCommas"] = true;

    std::string errs;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(text.data(), text.data() + text.size(), &out, &errs);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 65536) {
        return 0;
    }

    const std::string input(reinterpret_cast<const char*>(data), size);

    Json::Value request;
    if (!ParseJson(input, request)) {
        return 0;
    }

    const std::string response = DispatchMethod(request);

    // Parse the generated response to exercise response-shape invariants.
    Json::Value parsed_response;
    if (ParseJson(response, parsed_response) && parsed_response.isObject()) {
        const bool has_result = parsed_response.isMember("result");
        const bool has_error = parsed_response.isMember("error");
        if (has_result && has_error) {
            __builtin_trap();
        }
    }

    return 0;
}

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size,
                                          size_t max_size, unsigned int seed) {
    if (max_size < 64) {
        return size;
    }

    static const char* kMethods[] = {
        "getblockchaininfo",
        "getnewaddress",
        "sendtoaddress",
        "startmining",
        "stopmining",
        "unknown_method"
    };
    static const char* kTemplates[] = {
        R"({"jsonrpc":"2.0","id":1,"method":"%s"})",
        R"({"jsonrpc":"2.0","id":1,"method":"%s","params":[]})",
        R"({"jsonrpc":"2.0","id":1,"method":"%s","params":["addr",1.0]})"
    };

    if ((seed % 4) != 0) {
        return size;
    }

    const char* method = kMethods[seed % (sizeof(kMethods) / sizeof(kMethods[0]))];
    const char* format = kTemplates[seed % (sizeof(kTemplates) / sizeof(kTemplates[0]))];

    char buffer[512];
    const int n = std::snprintf(buffer, sizeof(buffer), format, method);
    if (n <= 0) {
        return size;
    }

    const size_t len = static_cast<size_t>(n);
    if (len > max_size) {
        return size;
    }

    std::memcpy(data, buffer, len);
    return len;
}
