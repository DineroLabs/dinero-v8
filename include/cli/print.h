#pragma once
#include <string>
#include <optional>
#include "compat/jsoncpp_compat.h"
#include "cli/overrides.h"

namespace dinero::cli {

struct PrintCtx {
    OutputFormat fmt = OutputFormat::Text;
    std::string schema = "din.cli.v1";
    std::string command;     // filled by the caller
    std::string network;     // params.name
    std::string rpcUrl;      // effective RPC URL (wallet-scoped if set)
    std::optional<std::string> wallet;
};

inline void printText(const std::string& s) {
    fwrite(s.data(), 1, s.size(), stdout);
    if (s.empty() || s.back() != '\n') fputc('\n', stdout);
}

inline void printJsonEnvelope(const PrintCtx& ctx, const Json::Value& data, bool ok = true, const std::string& errMsg = "", const Json::Value& pageInfo = Json::Value(Json::nullValue)) {
    Json::Value root(Json::objectValue);
    root["schema"]  = ctx.schema;
    if (!ctx.command.empty()) root["command"] = ctx.command;
    root["network"] = ctx.network;
    root["rpc_url"] = ctx.rpcUrl;
    if (ctx.wallet.has_value()) root["wallet"] = *ctx.wallet; else root["wallet"] = Json::Value(Json::nullValue);
    root["ok"]      = ok;
    if (ok) {
        root["data"] = data;
        // Add page info if provided (backward-compatible)
        if (!pageInfo.isNull()) {
            root["page"] = pageInfo;
        }
    } else {
        Json::Value e(Json::objectValue);
        e["message"] = errMsg;
        root["error"] = e;
    }
    Json::StreamWriterBuilder b; b["indentation"] = ""; // stable, one-line
    const std::string s = Json::writeString(b, root);
    printText(s);
}

inline int printResult(const PrintCtx& ctx, const std::string& textOk,
                       const Json::Value& dataOk, bool ok, const std::string& errMsg, 
                       const Json::Value& pageInfo = Json::Value(Json::nullValue)) {
    if (ctx.fmt == OutputFormat::Json) {
        printJsonEnvelope(ctx, ok ? dataOk : Json::Value(Json::objectValue), ok, errMsg, pageInfo);
        return ok ? 0 : 1;
    }
    if (ok) {
        printText(textOk);
        // In text mode, show pagination hint if has_more
        if (!pageInfo.isNull() && pageInfo.isMember("has_more") && pageInfo["has_more"].asBool()) {
            int nextOffset = pageInfo.isMember("next_offset") ? pageInfo["next_offset"].asInt() : 0;
            std::string hint = "... (more: use --offset " + std::to_string(nextOffset);
            if (pageInfo.isMember("cursor") && !pageInfo["cursor"].asString().empty()) {
                hint += " or --cursor " + pageInfo["cursor"].asString();
            }
            hint += ")";
            printText(hint);
        }
    } else {
        printText("ERROR: " + errMsg);
    }
    return ok ? 0 : 1;
}

} // namespace dinero::cli
