#include "solo_miner/rpc_client.h"
#include "solo_miner/chain_identity.h"
#include <curl/curl.h>
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>

namespace dinero {
namespace solo {

namespace {

// CURL write callback
size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t total = size * nmemb;
    output->append(static_cast<char*>(contents), total);
    return total;
}

// Base64 encoding for basic auth
std::string base64Encode(const std::string& input) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((input.size() + 2) / 3) * 4);

    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            result.push_back(chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) result.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (result.size() % 4) result.push_back('=');
    return result;
}

std::string normalizeNetworkName(const std::string& raw) {
    return std::string(dinero::solo::NormalizeNetworkName(raw));
}

const char* expectedGenesisForNetwork(const std::string& network) {
    const auto expected = dinero::solo::ExpectedGenesisForNetwork(network);
    if (!expected.empty()) return expected.data();
    return nullptr;
}

bool isHexHash64(const std::string& hex) {
    if (hex.size() != 64) return false;
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

} // anonymous namespace

BlockChainStatus classifyBlockChainStatus(
    const std::string& candidate_hash,
    const std::optional<std::string>& active_hash) {
    if (!active_hash.has_value()) {
        return BlockChainStatus::Unknown;
    }
    std::string canonical_active = *active_hash;
    std::string canonical_candidate = candidate_hash;
    std::transform(canonical_active.begin(), canonical_active.end(), canonical_active.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(canonical_candidate.begin(), canonical_candidate.end(), canonical_candidate.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return canonical_active == canonical_candidate
        ? BlockChainStatus::Active
        : BlockChainStatus::ConflictingActiveBlock;
}

RpcClient::RpcClient(const RpcConfig& config) : config_(config) {
    // Initialize CURL globally (thread-safe)
    static bool initialized = false;
    if (!initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        initialized = true;
    }

    // Try to load cookie authentication
    if (!config_.cookie_path.empty()) {
        loadCookie();
    }

    // Fall back to user/password if no cookie
    if (auth_header_.empty() && !config_.user.empty()) {
        std::string auth = config_.user + ":" + config_.password;
        auth_header_ = "Basic " + base64Encode(auth);
    }
}

RpcClient::~RpcClient() {
    // Note: Don't call curl_global_cleanup() - other instances may be using it
}

bool RpcClient::loadCookie() {
    std::ifstream file(config_.cookie_path);
    if (!file.is_open()) {
        return false;
    }

    std::string cookie;
    std::getline(file, cookie);

    if (cookie.empty()) {
        return false;
    }

    // Cookie format is "user:password"
    auth_header_ = "Basic " + base64Encode(cookie);
    cookie_loaded_ = true;
    return true;
}

std::optional<nlohmann::json> RpcClient::call(const std::string& method,
                                               const nlohmann::json& params,
                                               int timeout_override_seconds) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        last_error_ = "Failed to initialize CURL";
        last_call_timed_out_ = false;
        return std::nullopt;
    }

    last_call_timed_out_ = false;

    if (!config_.cookie_path.empty() && !loadCookie()) {
        last_error_ = "Failed to load RPC cookie from " + config_.cookie_path;
        return std::nullopt;
    }

    // Build JSON-RPC request
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = "solo-miner";
    request["method"] = method;
    request["params"] = params;

    std::string post_data = request.dump();
    std::string response;

    // Set up headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!auth_header_.empty()) {
        std::string auth = "Authorization: " + auth_header_;
        headers = curl_slist_append(headers, auth.c_str());
    }

    // Configure CURL
    curl_easy_setopt(curl, CURLOPT_URL, config_.url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     timeout_override_seconds > 0 ? timeout_override_seconds : config_.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Perform request
    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        last_call_timed_out_ = (res == CURLE_OPERATION_TIMEDOUT);
        last_error_ = std::string("CURL error: ") + curl_easy_strerror(res);
        return std::nullopt;
    }

    // Parse response
    try {
        auto json = nlohmann::json::parse(response);

        if (json.contains("error") && !json["error"].is_null()) {
            auto& error = json["error"];
            if (error.is_object() && error.contains("message")) {
                last_error_ = error["message"].get<std::string>();
            } else if (error.is_string()) {
                last_error_ = error.get<std::string>();
            } else {
                last_error_ = "RPC error";
            }
            return std::nullopt;
        }

        if (json.contains("result")) {
            return json["result"];
        }

        return json;

    } catch (const std::exception& e) {
        last_error_ = std::string("JSON parse error: ") + e.what();
        return std::nullopt;
    }
}

bool RpcClient::isConnected() {
    auto result = getBlockchainInfo();
    return result.has_value();
}

std::optional<nlohmann::json> RpcClient::getBlockTemplate(const std::string& address) {
    return getBlockTemplate(address, "");
}

std::optional<nlohmann::json> RpcClient::getBlockTemplate(const std::string& address,
                                                          const std::string& longpollid) {
    nlohmann::json params = nlohmann::json::object();
    params["address"] = address;
    // When longpollid is non-empty AND matches the server's current tip,
    // the server will hold the response open until the tip advances or
    // an ~8s timeout fires (see dinero p2p-fix 8bad44f15 server-side
    // BIP22/BIP23 longpoll implementation). This replaces client-side
    // poll-every-N-seconds with event-driven template refresh — new
    // templates arrive within milliseconds of a block, not after the
    // next poll cycle.
    //
    // Backward-compat: passing an empty longpollid (first-time fetch,
    // or miners that don't track it) makes the server return
    // immediately with the current template — same behavior as before.
    if (!longpollid.empty()) {
        params["longpollid"] = longpollid;
    }
    return call("getblocktemplate", nlohmann::json::array({params}));
}

bool RpcClient::submitBlock(const std::string& block_hex) {
    auto result = call("submitblock",
                       nlohmann::json::array({block_hex}),
                       config_.submit_timeout_seconds);

    if (!result.has_value()) {
        // RPC call failed (network error, etc.)
        return false;
    }

    // Success cases:
    // - null (Bitcoin standard)
    // - {} (empty object - some implementations)
    if (result->is_null()) {
        return true;
    }
    if (result->is_object() && result->empty()) {
        return true;
    }

    // Object with "error" field
    if (result->is_object() && result->contains("error")) {
        auto& err = (*result)["error"];
        if (err.is_null()) {
            // {"error": null} = success
            return true;
        }
        // Non-null error = failure
        if (err.is_string()) {
            last_error_ = err.get<std::string>();
        } else {
            last_error_ = err.dump();
        }
        return false;
    }

    // String response = error message (Bitcoin standard for failures)
    if (result->is_string()) {
        last_error_ = result->get<std::string>();
        return false;
    }

    // Unknown format - fail safe, log for debugging
    last_error_ = "Unknown submitblock response: " + result->dump();
    return false;
}

BlockChainStatus RpcClient::getBlockChainStatus(const std::string& candidate_hash,
                                                uint32_t height) {
    auto result = call("getblockhash", nlohmann::json::array({height}), 5);
    if (!result.has_value()) {
        result = call("blockchain.getblockhash", nlohmann::json::array({height}), 5);
    }
    if (!result.has_value() || !result->is_string()) {
        return BlockChainStatus::Unknown;
    }

    return classifyBlockChainStatus(candidate_hash, result->get<std::string>());
}

std::optional<nlohmann::json> RpcClient::getMiningInfo() {
    return call("getmininginfo");
}

std::optional<nlohmann::json> RpcClient::getBlockchainInfo() {
    return call("getblockchaininfo");
}

std::optional<std::string> RpcClient::getBlockHash(uint32_t height) {
    auto params = nlohmann::json::array({height});

    auto result = call("getblockhash", params);
    if (!result.has_value()) {
        // Fallback for namespaced RPC surfaces.
        result = call("blockchain.getblockhash", params);
    }

    if (!result.has_value()) {
        return std::nullopt;
    }

    if (!result->is_string()) {
        last_error_ = "Invalid getblockhash result type";
        return std::nullopt;
    }

    return result->get<std::string>();
}

ChainSafetyCheck RpcClient::verifyChainSafety() {
    ChainSafetyCheck check;

    auto info = getBlockchainInfo();
    if (!info.has_value() || !info->is_object()) {
        check.error = "Failed to read blockchain info: " + last_error_;
        return check;
    }

    if (!info->contains("chain") || !(*info)["chain"].is_string()) {
        check.error = "Missing 'chain' in getblockchaininfo";
        return check;
    }

    const std::string network_raw = (*info)["chain"].get<std::string>();
    const std::string network = normalizeNetworkName(network_raw);
    if (network.empty()) {
        check.error = "Unknown chain name: " + network_raw;
        return check;
    }
    check.network = network;

    const char* expected_genesis = expectedGenesisForNetwork(network);
    if (!expected_genesis) {
        check.error = "No expected genesis configured for network: " + network;
        return check;
    }

    auto genesis_hash = getBlockHash(0);
    if (!genesis_hash.has_value()) {
        check.error = "Failed to fetch genesis hash: " + last_error_;
        return check;
    }

    check.genesis_hash = *genesis_hash;
    if (!isHexHash64(check.genesis_hash)) {
        check.error = "Invalid genesis hash format from daemon: " + check.genesis_hash;
        return check;
    }

    if (check.genesis_hash != expected_genesis) {
        check.error =
            "Genesis hash mismatch for " + network +
            " (expected " + std::string(expected_genesis) +
            ", got " + check.genesis_hash + ")";
        return check;
    }

    check.safe = true;
    return check;
}

} // namespace solo
} // namespace dinero
