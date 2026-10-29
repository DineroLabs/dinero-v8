// W1 wallet forced-link probe (separation gate; spec section 8).
//
// Linked as: dinero_wallet whole-archive + only its declared link deps
// (no wallet_test_deps, no mempool_test_stubs.cpp). Constructs a
// WalletManager on a fresh temp dir, creates and opens a wallet the way the
// wallet tests do (tests/crypto/test_wallet_encryption.cpp), then cleans up.
// Returns nonzero on any failure (no assert: must gate under NDEBUG too).

#include "wallet/wallet_manager.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

int fail(const std::string& what) {
    std::fprintf(stderr, "W1 FAIL: %s\n", what.c_str());
    return 1;
}

fs::path MakeTempDir() {
    std::random_device rd;
    for (int attempt = 0; attempt < 16; ++attempt) {
        fs::path p = fs::temp_directory_path() /
                     ("dinero-sep-w1-" + std::to_string(rd()) + std::to_string(rd()));
        std::error_code ec;
        if (fs::create_directory(p, ec)) return p;
    }
    return {};
}

int Probe(const fs::path& dir) {
    const std::string name = "sep_w1";
    dinero::WalletManager manager(dir);
    manager.create(name);
    if (!manager.exists(name)) return fail("created wallet not in registry");
    manager.open(name);
    if (!manager.hasActiveWallet() || manager.current() != name)
        return fail("open did not activate wallet (current='" + manager.current() + "')");
    manager.unload();
    return 0;
}

}  // namespace

int main() {
    const fs::path dir = MakeTempDir();
    if (dir.empty()) return fail("cannot create temp dir");
    int rc = 0;
    try {
        rc = Probe(dir);
    } catch (const std::exception& e) {
        rc = fail(std::string("exception: ") + e.what());
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec && rc == 0) rc = fail("cleanup " + dir.string() + ": " + ec.message());
    if (rc == 0) std::printf("W1 OK: WalletManager create/open on temp dir\n");
    return rc;
}
