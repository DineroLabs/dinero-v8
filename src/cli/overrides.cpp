#include "cli/overrides.h"
#include <algorithm>
#include <stdexcept>

namespace dinero::cli {

CliOverrides ParseOverrides(const std::vector<std::string>& argv) {
    CliOverrides overrides;
    
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        
        // Handle --key=value format
        if (arg.find('=') != std::string::npos) {
            size_t pos = arg.find('=');
            std::string key = arg.substr(0, pos);
            std::string value = arg.substr(pos + 1);
            
            if (key == "--rpc-url") overrides.rpcUrl = value;
            else if (key == "--cookie-file") overrides.cookieFile = value;
            else if (key == "--datadir") overrides.datadir = value;
            else if (key == "--wallet" || key == "-w") overrides.wallet = value;
            else if (key == "--json-schema") overrides.jsonSchema = value;
            else if (key == "--connect-timeout-ms") overrides.connectTimeoutMs = std::stoi(value);
            else if (key == "--read-timeout-ms") overrides.readTimeoutMs = std::stoi(value);
            else if (key == "--retries") overrides.retries = std::stoi(value);
            else if (key == "--timeout") overrides.timeoutSeconds = std::stoi(value);
            else if (key == "--limit") overrides.limit = std::stoi(value);
            else if (key == "--offset") overrides.offset = std::stoi(value);
            else if (key == "--cursor") overrides.cursor = value;
            else if (key == "--filter") overrides.filter = value;
            else if (key == "--since") overrides.since = value;
            else if (key == "--until") overrides.until = value;
            else if (key == "--min-conf") overrides.minConf = std::stoi(value);
            else if (key == "--address") overrides.address = value;
            else if (key == "--type") overrides.txType = value;
            else if (key == "--label") overrides.label = value;
            else if (key == "--min-amount") overrides.minAmount = std::stod(value);
            else if (key == "--max-amount") overrides.maxAmount = std::stod(value);
            else if (key == "--state") overrides.state = value;
            else if (key == "--min-version") overrides.minVersion = std::stoi(value);
            else if (key == "--min-fee-rate") overrides.minFeeRate = std::stod(value);
            else if (key == "--txid") overrides.txid = value;
            else if (key == "--profile") overrides.profile = value;
            else if (key == "--format") {
                if (value == "json") overrides.format = OutputFormat::Json;
                else if (value == "text" || value == "table" || value == "plain") overrides.format = OutputFormat::Text;
            }
        }
        // Handle --key value format
        else if (arg.size() >= 2 && arg.substr(0, 2) == "--" && i + 1 < argv.size()) {
            const std::string& nextArg = argv[i + 1];
            
            if (arg == "--rpc-url") { overrides.rpcUrl = nextArg; ++i; }
            else if (arg == "--cookie-file") { overrides.cookieFile = nextArg; ++i; }
            else if (arg == "--datadir") { overrides.datadir = nextArg; ++i; }
            else if (arg == "--wallet") { overrides.wallet = nextArg; ++i; }
            else if (arg == "--json-schema") { overrides.jsonSchema = nextArg; ++i; }
            else if (arg == "--connect-timeout-ms") { overrides.connectTimeoutMs = std::stoi(nextArg); ++i; }
            else if (arg == "--read-timeout-ms") { overrides.readTimeoutMs = std::stoi(nextArg); ++i; }
            else if (arg == "--retries") { overrides.retries = std::stoi(nextArg); ++i; }
            else if (arg == "--timeout") { overrides.timeoutSeconds = std::stoi(nextArg); ++i; }
            else if (arg == "--limit") { overrides.limit = std::stoi(nextArg); ++i; }
            else if (arg == "--offset") { overrides.offset = std::stoi(nextArg); ++i; }
            else if (arg == "--cursor") { overrides.cursor = nextArg; ++i; }
            else if (arg == "--filter") { overrides.filter = nextArg; ++i; }
            else if (arg == "--since") { overrides.since = nextArg; ++i; }
            else if (arg == "--until") { overrides.until = nextArg; ++i; }
            else if (arg == "--min-conf") { overrides.minConf = std::stoi(nextArg); ++i; }
            else if (arg == "--address") { overrides.address = nextArg; ++i; }
            else if (arg == "--type") { overrides.txType = nextArg; ++i; }
            else if (arg == "--label") { overrides.label = nextArg; ++i; }
            else if (arg == "--min-amount") { overrides.minAmount = std::stod(nextArg); ++i; }
            else if (arg == "--max-amount") { overrides.maxAmount = std::stod(nextArg); ++i; }
            else if (arg == "--state") { overrides.state = nextArg; ++i; }
            else if (arg == "--min-version") { overrides.minVersion = std::stoi(nextArg); ++i; }
            else if (arg == "--min-fee-rate") { overrides.minFeeRate = std::stod(nextArg); ++i; }
            else if (arg == "--txid") { overrides.txid = nextArg; ++i; }
            else if (arg == "--profile") { overrides.profile = nextArg; ++i; }
            else if (arg == "--format") {
                if (nextArg == "json") overrides.format = OutputFormat::Json;
                else if (nextArg == "text" || nextArg == "table" || nextArg == "plain") overrides.format = OutputFormat::Text;
                ++i;
            }
        }
        // Handle boolean flags
        else if (arg == "--nodeinfo") overrides.nodeinfo = true;
        else if (arg == "--version") overrides.version = true;
        else if (arg == "--json" || arg == "--format=json") overrides.format = OutputFormat::Json;
        else if (arg == "--format=text" || arg == "--format=table" || arg == "--format=plain") overrides.format = OutputFormat::Text;
        else if (arg == "--accept-insecure-cookie") overrides.acceptInsecureCookie = true;
        else if (arg == "--wait-ready") overrides.waitReady = true;
        else if (arg == "--curl") overrides.curl = true;
        else if (arg == "--verbose" || arg == "-v") overrides.verbose = true;
        else if (arg == "--all") overrides.all = true;
        else if (arg == "--confirmed-only") overrides.confirmedOnly = true;
        else if (arg == "-w" && i + 1 < argv.size()) { overrides.wallet = argv[i + 1]; ++i; }
    }
    
    return overrides;
}

} // namespace dinero::cli
