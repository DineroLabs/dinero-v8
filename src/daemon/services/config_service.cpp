#include "daemon/services/config_service.h"
#include "daemon/services/logger_service.h"
#include "daemon/daemon_context.h"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <set>
#include <unordered_map>

namespace dinero {

// ConfigService v2.0 — Flat/Dotted Hybrid Key Support
// Normalizes flat keys (--datadir) to dotted keys (wallet.datadir)
// Maintains backward compatibility while moving toward structured config
static std::string NormalizeKey(const std::string& key) {
    static const std::unordered_map<std::string, std::string> aliases = {
        // Wallet configuration
        {"datadir",        "wallet.datadir"},
        {"walletdir",      "wallet.datadir"},
        {"walletpath",     "wallet.datadir"},

        // RPC configuration
        {"rpcport",        "rpc.port"},
        {"rpcbind",        "rpc.bind"},
        {"rpchost",        "rpc.host"},
        {"rpcuser",        "rpc.user"},
        {"rpcpassword",    "rpc.password"},
        {"rpcallowip",     "rpc.allowip"},
        {"rpc-readonly",   "rpc.readonly"},
        {"rpcreadonly",    "rpc.readonly"},

        // P2P configuration
        {"port",           "p2p.port"},
        {"p2pport",        "p2p.port"},
        {"addnode",        "p2p.addnode"},
        {"bind",           "p2p.bind"},
        {"whitelist",      "p2p.whitelist"},
        {"connect",        "p2p.connect"},
        {"listen",         "p2p.listen"},
        {"maxconnections", "p2p.maxconnections"},
        {"portmap",        "p2p.portmap"},
        {"upnp",           "p2p.upnp"},
        {"natpmp",         "p2p.natpmp"},
        {"nat-pmp",        "p2p.natpmp"},
        {"externalport",   "p2p.external_port"},
        {"portmaplifetime","p2p.portmap_lifetime"},
        {"onion",          "p2p.onion"},
        {"onionproxy",     "p2p.onion"},
        {"tor",            "p2p.onion"},
        {"listenonion",    "p2p.listen_onion"},
        {"torcontrol",     "p2p.tor_control"},
        {"torcontrolpassword", "p2p.tor_control_password"},

        // Network selection
        {"testnet",        "network.testnet"},
        {"regtest",        "network.regtest"},
        {"mainnet",        "network.mainnet"},

        // Mining configuration
        {"gen",            "mining.enabled"},
        {"genproclimit",   "mining.threads"},
        {"miningaddress",  "mining.address"},

        // Storage & Pruning configuration
        {"prune",          "storage.prune_target"},
        {"archival",       "storage.archival"}
    };

    auto it = aliases.find(key);
    if (it != aliases.end()) {
        return it->second;  // Map flat → dotted
    }
    return key;  // Already dotted or unknown
}

// Helper: Expand tilde in path
static std::string expand_tilde(const std::string& path) {
    if (path.empty() || path[0] != '~') {
        return path;
    }
    
    const char* home = std::getenv("HOME");
    if (!home) {
        return path;  // Can't expand, return as-is
    }
    
    if (path.length() == 1) {
        return std::string(home);
    }
    
    if (path[1] == '/') {
        return std::string(home) + path.substr(1);
    }
    
    return path;  // Invalid tilde usage, return as-is
}

bool ConfigService::Init(DaemonContext& ctx) {
    if (ctx.logger) {
        logger_ = std::dynamic_pointer_cast<LoggerService>(ctx.logger);
    }

    // Set defaults ONLY if not already set (preserves command-line args)
    // Using dotted keys internally for structured configuration
    if (config_map_.find("wallet.datadir") == config_map_.end()) {
        config_map_["wallet.datadir"] = "~/.dinero";
    }
    if (config_map_.find("rpc.port") == config_map_.end()) {
        config_map_["rpc.port"] = "20998";
    }
    if (config_map_.find("p2p.port") == config_map_.end()) {
        config_map_["p2p.port"] = "20999";
    }
    if (config_map_.find("network.testnet") == config_map_.end()) {
        config_map_["network.testnet"] = "false";
    }
    if (config_map_.find("network.regtest") == config_map_.end()) {
        config_map_["network.regtest"] = "false";
    }
    if (config_map_.find("storage.archival") == config_map_.end()) {
        config_map_["storage.archival"] = "true";
    }

    return true;
}

bool ConfigService::Start() {
    if (logger_) {
        logger_->info("[ConfigService] Configuration loaded");
        logger_->info("[ConfigService]   datadir: " + DataDir());
        logger_->info("[ConfigService]   rpcport: " + std::to_string(RPCPort()));
        logger_->info("[ConfigService]   p2pport: " + std::to_string(P2PPort()));
    }
    return true;
}

void ConfigService::Stop() {
    if (logger_) {
        logger_->info("[ConfigService] Configuration saved");
    }
}

std::string ConfigService::GetString(const std::string& key, const std::string& default_value) const {
    auto normalized = NormalizeKey(key);
    auto it = config_map_.find(normalized);
    return (it != config_map_.end()) ? it->second : default_value;
}

int ConfigService::GetInt(const std::string& key, int default_value) const {
    auto normalized = NormalizeKey(key);
    auto it = config_map_.find(normalized);
    if (it == config_map_.end()) return default_value;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return default_value;
    }
}

bool ConfigService::GetBool(const std::string& key, bool default_value) const {
    auto normalized = NormalizeKey(key);
    auto it = config_map_.find(normalized);
    if (it == config_map_.end()) return default_value;
    const std::string& val = it->second;
    return (val == "true" || val == "1" || val == "yes");
}

void ConfigService::Set(const std::string& key, const std::string& value) {
    // Normalize flat keys to dotted format (e.g., datadir → wallet.datadir)
    auto normalized = NormalizeKey(key);

    // Multi-value keys that should be appended (e.g., addnode can have multiple peers)
    static const std::set<std::string> multi_value_keys = {
        "p2p.addnode", "p2p.connect", "p2p.whitelist", "p2p.bind"
    };

    // Check if this is a multi-value key
    bool is_multi_value = multi_value_keys.count(normalized) > 0;

    if (is_multi_value) {
        // For multi-value keys, append with comma separator
        auto it = config_map_.find(normalized);
        if (it != config_map_.end() && !it->second.empty()) {
            config_map_[normalized] = it->second + "," + value;
        } else {
            config_map_[normalized] = value;
        }
    } else {
        // For single-value keys, replace the value
        config_map_[normalized] = value;
    }
}

std::string ConfigService::DataDir() const {
    std::string datadir = GetString("wallet.datadir", "~/.dinero");
    return expand_tilde(datadir);  // Expand ~ to home directory
}

// ─────────────────────────────────────────────────────────────────────────
// INI-style config-file loader (Phase B of Dinero Core 1.0).
// See header for syntax + semantics. The parser is intentionally small.
// ─────────────────────────────────────────────────────────────────────────

namespace {

// Trim leading/trailing whitespace (space and tab) in-place.
void trim_inplace(std::string& s) {
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    size_t lead = 0;
    while (lead < s.size() && is_ws(s[lead])) ++lead;
    size_t trail = s.size();
    while (trail > lead && is_ws(s[trail - 1])) --trail;
    s = s.substr(lead, trail - lead);
}

// Log a warning to logger_ if available, else stderr. Used during file
// parsing where logger_ may or may not be wired up (LoadConfigFile is
// called before Init() in the daemon startup sequence).
void warn(const std::shared_ptr<LoggerService>& logger,
          const std::string& path, size_t line_num, const std::string& msg) {
    std::string full = "[ConfigService] " + path + ":" +
                       std::to_string(line_num) + ": " + msg;
    if (logger) {
        logger->warning(full);
    } else {
        std::cerr << full << std::endl;
    }
}

}  // namespace

bool ConfigService::LoadConfigFile(const std::string& path) {
    std::string expanded = expand_tilde(path);

    std::ifstream in(expanded);
    if (!in.is_open()) {
        // Missing file is non-error per Bitcoin Core convention. The daemon
        // proceeds with CLI flags + defaults.
        return true;
    }

    std::string line;
    size_t line_num = 0;
    while (std::getline(in, line)) {
        ++line_num;

        // Trim whole line first to handle leading/trailing whitespace
        // before classification.
        trim_inplace(line);

        // Blank line: skip silently.
        if (line.empty()) continue;

        // Full-line comment: skip silently.
        if (line[0] == '#') continue;

        // Section header (`[main]` etc.): not supported in 1.0; warn + skip.
        // Subsequent lines are still parsed (the section is ignored, not a
        // hard boundary).
        if (line.front() == '[' && line.back() == ']') {
            warn(logger_, expanded, line_num,
                 "section headers not supported in dinero.conf 1.0; ignoring " + line);
            continue;
        }

        // Find first `=`. Everything before is the key; everything after is
        // the value (which may itself contain `=`).
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            warn(logger_, expanded, line_num,
                 "malformed line (no `=`); skipping: " + line);
            continue;
        }

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim_inplace(key);
        trim_inplace(value);

        if (key.empty()) {
            warn(logger_, expanded, line_num, "empty key; skipping");
            continue;
        }

        // Set() handles flat→dotted normalization and multi-value-key
        // append semantics. Calling Set() per occurrence gives:
        //   - single-value keys: last-wins on duplicate (Bitcoin Core behavior)
        //   - multi-value keys: comma-append across occurrences
        Set(key, value);
    }

    if (in.bad()) {
        // Distinguish "EOF reached normally" (good state) from "I/O error
        // mid-read" (bad state). Only the latter is a hard failure.
        if (logger_) {
            logger_->error("[ConfigService] I/O error reading " + expanded);
        } else {
            std::cerr << "[ConfigService] I/O error reading " << expanded << std::endl;
        }
        return false;
    }

    if (logger_) {
        logger_->info("[ConfigService] Loaded config file: " + expanded);
    }
    return true;
}

std::string ConfigService::DefaultConfigPath() const {
    return DataDir() + "/dinero.conf";
}

} // namespace dinero
