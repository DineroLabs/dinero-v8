/**
 * Config File Loader Test
 *
 * Phase B of the Dinero Core 1.0 plan. Verifies the INI-style config-file
 * loader behavior locked in docs/specs/dinero-core-1.0.md:
 *   - Default path <datadir>/dinero.conf, --conf=<path> override
 *   - File missing is non-error (Bitcoin Core convention)
 *   - INI syntax: key=value, # comments, blank lines, first-= split,
 *     trim whitespace, section [headers] not supported (warn+skip)
 *   - Precedence: CLI > file > defaults (asserted via test #12)
 *   - Multi-value keys (p2p.addnode) append across lines
 *   - Single-value keys, last-wins on duplicate
 *
 * Property numbering matches the plan's 15-property spec.
 */

// Pull in full types for DaemonContext members. Mirrors the include set
// used by tests/daemon/test_archival_config_defaults.cpp.
#include "consensus/cpu_budget_monitor.h"
#include "storage/disk_space_monitor.h"
#include "p2p/network_limits_monitor.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/services/config_service.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <process.h>
#include <cstdlib>
#define getpid _getpid
static inline int dinero_setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value);
}
static inline int dinero_unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#define setenv dinero_setenv
#define unsetenv dinero_unsetenv
#else
#include <unistd.h>
#endif

using namespace dinero;
namespace fs = std::filesystem;

namespace {

fs::path makeTempConfigFile(const std::string& contents) {
    fs::path tmp = fs::temp_directory_path() /
        ("dinero_conf_test_" + std::to_string(::getpid()) + "_" +
         std::to_string(std::rand()) + ".conf");
    std::ofstream out(tmp, std::ios::trunc);
    out << contents;
    out.close();
    return tmp;
}

void cleanup(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
}

// ─────────────────────────────────────────────────────────────────
// Property #1 — File missing → no error, defaults preserved
// ─────────────────────────────────────────────────────────────────
void test01_FileMissingIsNotError() {
    ConfigService config;
    fs::path nope = fs::temp_directory_path() /
        ("dinero_conf_does_not_exist_" + std::to_string(::getpid()));
    cleanup(nope);  // Ensure absent

    bool result = config.LoadConfigFile(nope.string());
    assert(result == true && "missing file must NOT be an error");
    assert(config.GetString("rpc.port", "DEFAULT") == "DEFAULT" &&
           "no keys set when file missing");
}

// ─────────────────────────────────────────────────────────────────
// Property #2 — Empty file → no error, no keys set
// ─────────────────────────────────────────────────────────────────
void test02_EmptyFile() {
    ConfigService config;
    auto p = makeTempConfigFile("");
    bool result = config.LoadConfigFile(p.string());
    assert(result == true && "empty file must succeed");
    assert(config.GetString("rpc.port", "DEFAULT") == "DEFAULT");
    cleanup(p);
}

// ─────────────────────────────────────────────────────────────────
// Property #3 — Single key=value → GetString returns value
// ─────────────────────────────────────────────────────────────────
void test03_SingleKeyValue() {
    ConfigService config;
    auto p = makeTempConfigFile("rpcport=21000\n");
    assert(config.LoadConfigFile(p.string()));
    assert(config.GetString("rpc.port", "") == "21000" &&
           "rpcport must normalize to rpc.port and store value");
    cleanup(p);
}

// ─────────────────────────────────────────────────────────────────
// Property #4 — Comments and blank lines ignored
// ─────────────────────────────────────────────────────────────────
void test04_CommentsAndBlanks() {
    ConfigService config;
    std::string body =
        "# This is a comment\n"
        "\n"
        "rpcport=22000\n"
        "\n"
        "# another comment\n"
        "rpcuser=alice\n";
    auto p = makeTempConfigFile(body);
    assert(config.LoadConfigFile(p.string()));
    assert(config.GetString("rpc.port", "") == "22000");
    assert(config.GetString("rpc.user", "") == "alice");
    cleanup(p);
}

// ─────────────────────────────────────────────────────────────────
// Property #5 — Whitespace around `=` trimmed
// ─────────────────────────────────────────────────────────────────
void test05_WhitespaceTrimmed() {
    ConfigService config;
    std::string body =
        "  rpcport  =  23000  \n"
        "rpcuser\t=\tbob\n";
    auto p = makeTempConfigFile(body);
    assert(config.LoadConfigFile(p.string()));
    assert(config.GetString("rpc.port", "") == "23000" &&
           "leading/trailing whitespace around key/value must be trimmed");
    assert(config.GetString("rpc.user", "") == "bob" &&
           "tabs around `=` must be treated as whitespace");
    cleanup(p);
}

// ─────────────────────────────────────────────────────────────────
// Property #6 — Value containing `=` → split on FIRST `=` only
// ─────────────────────────────────────────────────────────────────
void test06_ValueContainsEquals() {
    ConfigService config;
    auto p = makeTempConfigFile("rpc.password=p@ss=word=with=equals\n");
    assert(config.LoadConfigFile(p.string()));
    assert(config.GetString("rpc.password", "") == "p@ss=word=with=equals" &&
           "must split on first `=` only; remaining `=` chars stay in value");
    cleanup(p);
}

// ─────────────────────────────────────────────────────────────────
// Property #7 — Empty value → empty string stored
// ─────────────────────────────────────────────────────────────────
void test07_EmptyValue() {
    ConfigService config;
    auto p = makeTempConfigFile("rpc.user=\n");
    assert(config.LoadConfigFile(p.string()));
    assert(config.GetString("rpc.user", "DEFAULT") == "" &&
           "empty value after `=` must store empty string, not default");
    cleanup(p);
}

// ─────────────────────────────────────────────────────────────────
// Property #8 — Multi-value key (p2p.addnode) appends across lines
// ─────────────────────────────────────────────────────────────────
void test08_MultiValueAppends() {
    ConfigService config;
    std::string body =
        "addnode=10.0.0.1\n"
        "addnode=10.0.0.2\n"
        "addnode=10.0.0.3\n";
    auto p = makeTempConfigFile(body);
    assert(config.LoadConfigFile(p.string()));
    std::string nodes = config.GetString("p2p.addnode", "");
    assert(nodes == "10.0.0.1,10.0.0.2,10.0.0.3" &&
           "multi-value key must append all occurrences with commas");
    cleanup(p);
}

// ─────────────────────────────────────────────────────────────────
// Property #9 — Single-value key duplicate → last wins
// ─────────────────────────────────────────────────────────────────
void test09_SingleValueDuplicateLastWins() {
    ConfigService config;
    std::string body =
        "rpcport=20000\n"
        "rpcport=21000\n"
        "rpcport=22000\n";
    auto p = makeTempConfigFile(body);
    assert(config.LoadConfigFile(p.string()));
    assert(config.GetString("rpc.port", "") == "22000" &&
           "duplicate single-value key: last occurrence wins");
    cleanup(p);
}

// ─────────────────────────────────────────────────────────────────
// Property #10 — Section header [main] → warning, skip (not supported in 1.0)
// ─────────────────────────────────────────────────────────────────
void test10_SectionHeaderSkipped() {
    ConfigService config;
    std::string body =
        "rpcport=24000\n"
        "[main]\n"
        "rpcuser=carol\n";
    auto p = makeTempConfigFile(body);
    assert(config.LoadConfigFile(p.string()));
    // Section header line itself is skipped; subsequent keys are still parsed
    // (the section is ignored, but the keys after it are still applied).
    assert(config.GetString("rpc.port", "") == "24000");
    assert(config.GetString("rpc.user", "") == "carol" &&
           "keys after a section header must still be parsed (section is ignored, not boundary)");
    cleanup(p);
}

// ─────────────────────────────────────────────────────────────────
// Property #11 — Malformed line (no `=`) → warning, skip; later lines still parsed
// ─────────────────────────────────────────────────────────────────
void test11_MalformedLineSkipped() {
    ConfigService config;
    std::string body =
        "rpcport=25000\n"
        "this is not a key value pair\n"
        "rpcuser=dave\n";
    auto p = makeTempConfigFile(body);
    assert(config.LoadConfigFile(p.string()));
    assert(config.GetString("rpc.port", "") == "25000");
    assert(config.GetString("rpc.user", "") == "dave" &&
           "lines after malformed line must still be parsed");
    cleanup(p);
}

// ─────────────────────────────────────────────────────────────────
// Property #12 — CLI Set() AFTER LoadConfigFile overrides file value
// ─────────────────────────────────────────────────────────────────
void test12_CLIOverridesFile() {
    ConfigService config;
    auto p = makeTempConfigFile("rpcport=26000\nrpcuser=eve\n");
    assert(config.LoadConfigFile(p.string()));
    // Simulate CLI parse running AFTER file load — Set() should replace
    // single-value keys.
    config.Set("rpcport", "9999");
    assert(config.GetString("rpc.port", "") == "9999" &&
           "CLI Set() after LoadConfigFile must override single-value file values");
    assert(config.GetString("rpc.user", "") == "eve" &&
           "non-overridden keys keep their file values");
    cleanup(p);
}

// ─────────────────────────────────────────────────────────────────
// Property #13 — File overrides default (Init defaults are seeded only if absent)
// ─────────────────────────────────────────────────────────────────
void test13_FileOverridesDefault() {
    ConfigService config;
    auto p = makeTempConfigFile("rpcport=27000\n");
    assert(config.LoadConfigFile(p.string()));
    // Init() seeds defaults only for keys not already set (config_service.cpp:93).
    DaemonContext ctx;
    assert(config.Init(ctx));
    assert(config.GetString("rpc.port", "") == "27000" &&
           "Init() must NOT clobber file-loaded values");
    cleanup(p);
}

// ─────────────────────────────────────────────────────────────────
// Property #14 — Tilde in --conf path → expanded by loader
// ─────────────────────────────────────────────────────────────────
void test14_TildeExpansionInConfPath() {
    // Set HOME to a temp dir, write the conf there, request via "~/<file>"
    fs::path home_dir = fs::temp_directory_path() /
        ("dinero_conf_home_" + std::to_string(::getpid()));
    fs::create_directories(home_dir);
    fs::path conf_in_home = home_dir / "tilde_test.conf";
    {
        std::ofstream out(conf_in_home, std::ios::trunc);
        out << "rpcport=28000\n";
    }

    const char* old_home = std::getenv("HOME");
    // path::c_str() returns wchar_t* on Windows; go through .string() for narrow.
    const std::string home_dir_str = home_dir.string();
    setenv("HOME", home_dir_str.c_str(), 1);

    ConfigService config;
    bool ok = config.LoadConfigFile("~/tilde_test.conf");
    assert(ok && "tilde-prefixed path must be expanded and loaded");
    assert(config.GetString("rpc.port", "") == "28000");

    if (old_home) {
        setenv("HOME", old_home, 1);
    } else {
        unsetenv("HOME");
    }
    cleanup(conf_in_home);
    fs::remove_all(home_dir);
}

// ─────────────────────────────────────────────────────────────────
// Property #15 — DefaultConfigPath() returns <datadir>/dinero.conf
// ─────────────────────────────────────────────────────────────────
void test15_DefaultConfigPath() {
    ConfigService config;
    DaemonContext ctx;
    assert(config.Init(ctx));  // Seeds wallet.datadir = ~/.dinero
    std::string expected_suffix = "/dinero.conf";
    std::string path = config.DefaultConfigPath();
    assert(path.size() >= expected_suffix.size() &&
           path.substr(path.size() - expected_suffix.size()) == expected_suffix &&
           "DefaultConfigPath must end with /dinero.conf");
    // Path should NOT contain literal tilde — it must be expanded.
    assert(path.find('~') == std::string::npos &&
           "DefaultConfigPath must return a tilde-expanded path");
}

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Config File Loader Test (Phase B)" << std::endl;
    std::cout << "========================================" << std::endl;

    test01_FileMissingIsNotError();
    std::cout << "  [✓] #1 file missing is non-error" << std::endl;

    test02_EmptyFile();
    std::cout << "  [✓] #2 empty file" << std::endl;

    test03_SingleKeyValue();
    std::cout << "  [✓] #3 single key=value" << std::endl;

    test04_CommentsAndBlanks();
    std::cout << "  [✓] #4 comments and blank lines ignored" << std::endl;

    test05_WhitespaceTrimmed();
    std::cout << "  [✓] #5 whitespace around = trimmed" << std::endl;

    test06_ValueContainsEquals();
    std::cout << "  [✓] #6 value with = splits on first =" << std::endl;

    test07_EmptyValue();
    std::cout << "  [✓] #7 empty value stores empty string" << std::endl;

    test08_MultiValueAppends();
    std::cout << "  [✓] #8 multi-value key appends" << std::endl;

    test09_SingleValueDuplicateLastWins();
    std::cout << "  [✓] #9 single-value duplicate, last wins" << std::endl;

    test10_SectionHeaderSkipped();
    std::cout << "  [✓] #10 section header skipped" << std::endl;

    test11_MalformedLineSkipped();
    std::cout << "  [✓] #11 malformed line skipped" << std::endl;

    test12_CLIOverridesFile();
    std::cout << "  [✓] #12 CLI overrides file" << std::endl;

    test13_FileOverridesDefault();
    std::cout << "  [✓] #13 file overrides default" << std::endl;

    test14_TildeExpansionInConfPath();
    std::cout << "  [✓] #14 tilde in --conf path expanded" << std::endl;

    test15_DefaultConfigPath();
    std::cout << "  [✓] #15 default config path = <datadir>/dinero.conf" << std::endl;

    std::cout << "\n✅ Config file loader: 15/15 properties hold" << std::endl;
    return 0;
}
