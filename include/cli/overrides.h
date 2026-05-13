#pragma once
#include <string>
#include <optional>
#include <vector>

namespace dinero::cli {

// Strong sources → weak sources: Flag > Env > Config > Auto
enum class ValueSource { Flag, Env, Config, Auto };

enum class OutputFormat {
    Text,
    Json
};

// Standard exit codes (sysexits.h compatible)
namespace ExitCodes {
    constexpr int SUCCESS = 0;           // Success
    constexpr int FAILURE = 1;           // Generic failure
    constexpr int USAGE = 64;            // Bad args / unknown subcommand (EX_USAGE)
    constexpr int UNAVAILABLE = 69;      // Daemon unavailable / not ready (EX_UNAVAILABLE)
    constexpr int TEMPFAIL = 75;         // Transient RPC failure after retries (EX_TEMPFAIL)
    constexpr int NOPERM = 77;           // Auth/cookie failure (EX_NOPERM)
}

struct CliOverrides {
    std::optional<std::string> rpcUrl;      // --rpc-url
    std::optional<std::string> cookieFile;  // --cookie-file
    std::optional<std::string> datadir;     // --datadir
    std::optional<std::string> wallet;      // -w / --wallet
    bool nodeinfo = false;                  // --nodeinfo
    OutputFormat format = OutputFormat::Text; // --format (text|json|plain)
    std::string jsonSchema = "din.cli.v1";    // --json-schema (version tag)
    
    // Security / networking
    bool acceptInsecureCookie = false;        // --accept-insecure-cookie
    int connectTimeoutMs = 5000;              // --connect-timeout-ms
    int readTimeoutMs    = 30000;             // --read-timeout-ms
    
    // Retry and reliability
    int retries = 3;                          // --retries N
    int timeoutSeconds = 10;                  // --timeout SECONDS
    bool waitReady = false;                   // --wait-ready
    
    // Output control
    bool curl = false;                        // --curl (show equivalent curl command)
    bool verbose = false;                     // --verbose, -v
    bool version = false;                     // --version
    
    // Profile management
    std::optional<std::string> profile;       // --profile NAME
    
    // Paging and filtering
    int limit = 100;                          // --limit N (default 100, max 5000)
    int offset = 0;                           // --offset N
    std::optional<std::string> cursor;        // --cursor TOKEN (opaque pagination token)
    std::optional<std::string> filter;        // --filter PATTERN
    std::optional<std::string> since;         // --since <ISO8601|height>
    std::optional<std::string> until;         // --until <ISO8601|height>
    bool all = false;                         // --all (bypass limits, JSON only)
    
    // Command-specific filters
    std::optional<int> minConf;               // --min-conf N (listtransactions)
    std::optional<std::string> address;       // --address ADDR (listtransactions)
    std::optional<std::string> txType;        // --type send|receive|immature|orphan
    std::optional<std::string> label;         // --label LABEL
    std::optional<double> minAmount;          // --min-amount X (listutxos)
    std::optional<double> maxAmount;          // --max-amount X (listutxos)
    bool confirmedOnly = false;               // --confirmed-only (listutxos)
    std::optional<std::string> state;         // --state (peers)
    std::optional<int> minVersion;            // --min-version N (peers)
    std::optional<double> minFeeRate;         // --min-fee-rate X (mempool)
    std::optional<std::string> txid;          // --txid TXID (mempool)
};

// Parse argv into overrides (supports --key=value and --key value).
CliOverrides ParseOverrides(const std::vector<std::string>& argv);

} // namespace dinero::cli
