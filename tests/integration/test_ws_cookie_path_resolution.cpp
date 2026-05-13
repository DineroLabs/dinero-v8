#include "daemon/auth_cookie.h"
#include "daemon/rpc_authcookie.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << std::endl;
        std::exit(1);
    }
}

void write_cookie(const fs::path& path, const std::string& token) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    require(out.is_open(), "unable to open cookie file " + path.string());
    out << "__cookie__:" << token << "\n";
}

}  // namespace

int main() {
    const auto nonce = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const fs::path root = fs::temp_directory_path() / ("dinero_ws_cookie_path_" + nonce);
    const fs::path datadir = root / "node";
    const fs::path default_cookie = datadir / ".cookie";
    const fs::path custom_cookie = root / "auth" / "custom.cookie";

    fs::create_directories(datadir);

    const std::string default_resolved = GetAuthCookiePath(datadir.string(), "");
    const std::string custom_resolved = GetAuthCookiePath(datadir.string(), custom_cookie.string());

    require(default_resolved == default_cookie.string(), "default cookie path should resolve under datadir");
    require(custom_resolved == custom_cookie.string(), "explicit rpccookiefile should be preserved");

    write_cookie(default_cookie, "wrong-token");
    write_cookie(custom_cookie, "right-token");

    std::unordered_map<std::string, std::string> headers{
        {"authorization", dinero::make_basic_value("right-token")}
    };

    require(
        !dinero::check_basic_authorization(headers, default_cookie.string()),
        "default cookie must not authorize a request signed for the custom cookie"
    );
    require(
        dinero::check_basic_authorization(headers, custom_cookie.string()),
        "custom rpccookiefile must authorize immediately after a path change"
    );

    fs::remove_all(root);
    std::cout << "[PASS] WS cookie path resolution and auth cache honor rpccookiefile" << std::endl;
    return 0;
}
