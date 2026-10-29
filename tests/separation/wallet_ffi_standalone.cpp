// W6 standalone wallet-FFI consumer (separation gate; spec section 8).
//
// Linked the way the Rust bridge links (mobile-tauri/src-tauri/build.rs):
// libdinero_wallet_ffi.a whole-archive plus the static archives build.rs
// names, and no node libraries. Calls one exported dinero_wallet_* function.
// Returns nonzero on failure (no assert: must gate under NDEBUG too).

#include "wallet_ffi.h"

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

int main() {
    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() /
        ("dinero-sep-w6-" + std::to_string(rd()) + std::to_string(rd()));
    std::error_code ec;
    if (!fs::create_directory(dir, ec)) {
        std::fprintf(stderr, "W6 FAIL: cannot create temp dir\n");
        return 1;
    }
    const std::string d = dir.string();
    const int rc = dinero_wallet_init(d.c_str());
    fs::remove_all(dir, ec);
    if (rc != 0) {
        std::fprintf(stderr, "W6 FAIL: dinero_wallet_init rc=%d\n", rc);
        return 1;
    }
    std::printf("W6 OK: dinero_wallet_init on temp dir\n");
    return 0;
}
