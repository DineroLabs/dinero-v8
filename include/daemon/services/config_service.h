#pragma once
#include "daemon/iservice.h"
#include <string>
#include <map>
#include <memory>

namespace dinero {

/**
 * ConfigService - Configuration management
 *
 * Parses command-line args and config files.
 * Provides typed accessors for config values.
 *
 * Dependencies: Logger (for logging config errors)
 *
 * Example:
 *   auto config = std::make_shared<ConfigService>();
 *   config->Init(ctx);
 *   config->Start();
 *   std::string datadir = config->GetString("datadir", "~/.dinero");
 */
class ConfigService : public IService {
public:
    ConfigService() = default;

    std::string Name() const override { return "Config"; }

    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Config accessors
    std::string GetString(const std::string& key, const std::string& default_value = "") const;
    int GetInt(const std::string& key, int default_value = 0) const;
    bool GetBool(const std::string& key, bool default_value = false) const;

    void Set(const std::string& key, const std::string& value);

    // INI-style config-file loader (Phase B of Dinero Core 1.0).
    //
    // Reads `path` and calls Set() for each key=value line.
    // Syntax (Bitcoin-Core-compatible subset):
    //   - `key=value` or `key = value` (whitespace around `=` trimmed)
    //   - `#` starts a full-line comment
    //   - Blank lines are ignored
    //   - Value containing `=` splits on FIRST `=` only
    //   - Section headers `[xxx]` are NOT supported — line is skipped with a
    //     warning, but subsequent keys are still parsed
    //   - Malformed lines (no `=`) are skipped with a warning
    //
    // Path is tilde-expanded (~/foo.conf → $HOME/foo.conf).
    //
    // Returns true on success, INCLUDING when the file does not exist
    // (Bitcoin Core convention: missing file is non-error). Returns false
    // only on read-after-open failure (corrupt I/O, permission flip mid-read).
    //
    // Precedence is the caller's responsibility: load file first, then apply
    // CLI flags via Set() to override single-value keys.
    bool LoadConfigFile(const std::string& path);

    // Default path used when --conf is not specified: <datadir>/dinero.conf.
    // Tilde-expanded.
    std::string DefaultConfigPath() const;

    // Common config paths (using dotted keys internally)
    std::string DataDir() const;
    std::string getDataDir() const { return DataDir(); }  // Alias for global shim
    std::string LogPath() const { return GetString("logpath", DataDir() + "/debug.log"); }
    int RPCPort() const { return GetInt("rpc.port", 20998); }
    int P2PPort() const { return GetInt("p2p.port", 20999); }
    bool IsTestnet() const { return GetBool("network.testnet", false); }
    bool IsRegtest() const { return GetBool("network.regtest", false); }

private:
    std::map<std::string, std::string> config_map_;
    std::shared_ptr<class LoggerService> logger_;
};

} // namespace dinero
