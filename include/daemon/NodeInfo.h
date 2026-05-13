#pragma once
#include <string>
#include <optional>
#include "compat/jsoncpp_compat.h"
#include <fstream>

struct NodeInfo {
    std::string rpcUrl;
    std::string cookiePath;
    std::string wsUrl;
    int pid;
};

inline std::optional<NodeInfo> loadNodeInfo(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;
    
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(file, root)) return std::nullopt;
    
    NodeInfo ni;
    if (root.isMember("rpc") && root["rpc"].isMember("url")) {
        ni.rpcUrl = root["rpc"]["url"].asString();
    }
    if (root.isMember("cookie")) {
        ni.cookiePath = root["cookie"].asString();
    }
    if (root.isMember("ws") && root["ws"].isMember("url")) {
        ni.wsUrl = root["ws"]["url"].asString();
    }
    if (root.isMember("pid")) {
        ni.pid = root["pid"].asInt();
    }
    
    if (ni.rpcUrl.empty() || ni.cookiePath.empty()) return std::nullopt;
    return ni;
}
