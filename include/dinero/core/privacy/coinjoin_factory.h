#include "privacy/coinjoin_adapter.h"
#include <memory>
#include <stdexcept>

namespace din {

// Forward declarations for adapter implementations
class CoinJoinAdapterGeneric;
class CoinJoinAdapterJM;

// Adapter factory
std::unique_ptr<ICoinJoinAdapter> make_cj_adapter(const std::string& type, const std::string& base_url);

// Configuration structure
struct CoinJoinConfig {
    std::string type = "generic";        // "generic" | "jm"
    std::string base_url = "http://127.0.0.1:8080";
    bool require_proxy = false;
    std::string proxy_host = "127.0.0.1";
    int proxy_port = 9050;
    int timeout_seconds = 30;
    int max_retries = 3;
};

} // namespace din
