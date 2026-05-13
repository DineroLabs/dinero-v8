// Dinero GPU Miner - Standalone GPU mining process
// Communicates with dinerod via RPC (mining.getjob / mining.submit)
// Supports: Metal (Apple Silicon), CUDA (NVIDIA), OpenCL (AMD/Intel)
//
// Usage:
//   dinero-gpu-miner --address din1p... [--backend auto|metal|cuda|opencl] [--rpcport 20998]

#include "mining/gpu/compute_backend.h"
#include "mining/gpu/gpu_device_manager.h"
#include "build/build_identity.h"
#include "consensus/chain_identity.h"
#include "crypto/sha256.h"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <csignal>
#include <curl/curl.h>
#include "compat/jsoncpp_compat.h"

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

using namespace std;
using namespace dinero::gpu;
using namespace std::string_view_literals;

// Forward declaration for OpenCL kernel source loader (defined in opencl_backend.cpp)
namespace dinero::gpu { std::string loadOpenCLKernelSource(); }

// ============================================================================
// Global state
// ============================================================================

static atomic<bool> g_shutdown{false};
static atomic<uint64_t> g_total_hashes{0};
static atomic<uint32_t> g_blocks_found{0};
static atomic<double> g_gpu_hashrate{0.0};

// Stale-job signal set by the longpoll watcher thread when the daemon
// notifies that the chain tip has advanced. The main mining loop checks
// this between GPU batch dispatches and, if set, aborts the current job
// so it can pick up fresh work immediately. Without this, a GPU batch
// running on a stale template keeps hashing for up to BATCH_SIZE / hashrate
// seconds after the network has already moved on — that hashrate is wasted
// on work guaranteed to be orphaned.
//
// The watcher thread uses the server-side longpoll on getblocktemplate
// (dinero p2p-fix 8bad44f15) purely as a tip-change NOTIFICATION — the
// template body it returns is discarded. Actual job fetching still goes
// through mining.getjob in the main loop. This decoupling is deliberate:
// getjob has stricter safety gates (mining readiness, utreexo state,
// strict block assembly) and getblocktemplate is fine as a pure signal.
static atomic<bool> g_stale_job{false};

static void print_version(FILE* out) {
    std::string version = dinero::build::FormatIdentityMultiline();
    fputs(version.c_str(), out);
}

// Signal handler for clean shutdown
static void signal_handler(int) {
    g_shutdown.store(true);
}

// ============================================================================
// HTTP/RPC helpers (same as dinero-miner)
// ============================================================================

static size_t write_callback(void* contents, size_t size, size_t nmemb, string* s) {
    s->append((char*)contents, size * nmemb);
    return size * nmemb;
}

static string base64_encode(const string& input) {
    static const char* base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string output;
    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            output.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) output.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (output.size() % 4) output.push_back('=');
    return output;
}

static string read_cookie(const string& cookie_path) {
    ifstream file(cookie_path);
    if (!file.is_open()) return "";
    string cookie;
    getline(file, cookie);
    cookie.erase(0, cookie.find_first_not_of(" \t\r\n"));
    cookie.erase(cookie.find_last_not_of(" \t\r\n") + 1);
    if (cookie.empty()) return "";
    size_t colon_pos = cookie.find(':');
    if (colon_pos == string::npos) return "";
    if (cookie.substr(0, colon_pos).empty()) return "";
    if (cookie.substr(colon_pos + 1).empty()) return "";
    return cookie;
}

static string find_cookie(const string& datadir) {
    if (!datadir.empty()) {
        string path = datadir + "/.cookie";
        ifstream test(path);
        if (test.good()) return path;
    }
    vector<string> paths = { "./.cookie", "./data/.cookie", "./data/mainnet/.cookie" };
    for (const auto& path : paths) {
        ifstream test(path);
        if (test.good()) return path;
    }
    return "";
}

static bool parse_hex_u32_strict(const string& hex, uint32_t& out) {
    if (hex.empty() || hex.size() > 8) {
        return false;
    }
    uint32_t value = 0;
    for (char ch : hex) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!std::isxdigit(uch)) {
            return false;
        }
        value <<= 4;
        if (uch >= '0' && uch <= '9') value |= static_cast<uint32_t>(uch - '0');
        else if (uch >= 'a' && uch <= 'f') value |= static_cast<uint32_t>(uch - 'a' + 10);
        else value |= static_cast<uint32_t>(uch - 'A' + 10);
    }
    out = value;
    return true;
}

static bool rpc_call(const string& url, const string& cookie, const string& method,
                     const Json::Value& params, Json::Value& result,
                     long timeout_sec = 30,
                     const string* cookie_path = nullptr) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    Json::Value request;
    // Plain const char* keys (no sv suffix). JsonCpp's operator[] doesn't
    // accept std::string_view; the implicit conversion libstdc++ tolerates
    // is rejected by MSVC. const char* works on every platform.
    request["jsonrpc"] = "2.0";
    request["id"] = "gpu-miner";
    request["method"] = method;
    request["params"] = params;

    Json::StreamWriterBuilder writer;
    string json_str = Json::writeString(writer, request);

    string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    const string auth_cookie =
        (cookie_path != nullptr && !cookie_path->empty()) ? read_cookie(*cookie_path) : cookie;
    if (!auth_cookie.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth_cookie.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        cerr << "RPC error: " << curl_easy_strerror(res) << endl;
        return false;
    }
    if (http_code != 200) {
        cerr << "RPC HTTP " << http_code << ": " << response << endl;
        return false;
    }

    Json::CharReaderBuilder reader;
    istringstream iss(response);
    string errs;
    if (!Json::parseFromStream(reader, iss, &result, &errs)) {
        cerr << "JSON parse error: " << errs << endl;
        return false;
    }
    if (result.isMember("error") && !result["error"].isNull()) {
        cerr << "RPC error: " << result["error"]["message"].asString() << endl;
        return false;
    }
    result = result["result"];
    return true;
}

static bool describe_job_response_issue(const Json::Value& job_result, string& message) {
    if (!job_result.isObject()) {
        return false;
    }

    string code = job_result.get("code", "").asString();
    string reason = job_result.get("reason", "").asString();
    string detail = job_result.get("error", "").asString();

    if (detail.empty() && job_result.isMember("message") && job_result["message"].isString()) {
        detail = job_result["message"].asString();
    }
    if ((reason.empty() || detail.empty()) &&
        job_result.isMember("mining_safety") && job_result["mining_safety"].isObject()) {
        const Json::Value& safety = job_result["mining_safety"];
        if (reason.empty()) {
            reason = safety.get("reason", "").asString();
        }
        if (detail.empty()) {
            detail = safety.get("error", "").asString();
        }
    }

    if (code == "mining-safety-gate") {
        message = "Daemon paused mining work";
        if (!reason.empty()) {
            message += " (" + reason + ")";
        }
        if (!detail.empty()) {
            message += ": " + detail;
        }
        return true;
    }

    if (code == "stale-job") {
        message = "Daemon returned stale job";
        if (!detail.empty()) {
            message += ": " + detail;
        }
        return true;
    }

    if (job_result.isMember("error") && job_result["error"].isString()) {
        message = "Daemon rejected mining job: " + job_result["error"].asString();
        return true;
    }

    return false;
}

// ============================================================================
// Hex helpers
// ============================================================================

static string to_hex(const uint8_t* data, size_t len) {
    ostringstream oss;
    oss << hex << setfill('0');
    for (size_t i = 0; i < len; i++) oss << setw(2) << (unsigned)data[i];
    return oss.str();
}

static vector<uint8_t> from_hex(const string& hex) {
    vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.length(); i += 2) {
        bytes.push_back((uint8_t)stoul(hex.substr(i, 2), nullptr, 16));
    }
    return bytes;
}

// ============================================================================
// Target conversion
// ============================================================================

// Convert compact difficulty (nBits) to 32-byte big-endian target
static void compact_to_target(uint32_t bits, uint8_t target[32]) {
    memset(target, 0, 32);
    uint32_t exp = bits >> 24;
    uint32_t mant = bits & 0x00ffffff;

    if (exp <= 3) {
        uint32_t r = mant >> (8 * (3 - exp));
        for (int i = 0; i < 4 && r; i++) {
            target[31 - i] = (uint8_t)(r & 0xff);
            r >>= 8;
        }
    } else {
        int idx = 32 - exp;
        if (idx >= 0 && idx <= 29) {
            target[idx]     = (uint8_t)((mant >> 16) & 0xff);
            target[idx + 1] = (uint8_t)((mant >> 8) & 0xff);
            target[idx + 2] = (uint8_t)(mant & 0xff);
        }
    }
}

// Convert 32-byte big-endian target to 8x uint32 words
// target_words[0] = MSW (matches SHA-256 output where state[0] = H0 = MSW)
static void target_bytes_to_words(const uint8_t target_be[32], uint32_t target_words[8]) {
    for (int i = 0; i < 8; i++) {
        int byte_idx = i * 4;
        target_words[i] = ((uint32_t)target_be[byte_idx] << 24) |
                          ((uint32_t)target_be[byte_idx + 1] << 16) |
                          ((uint32_t)target_be[byte_idx + 2] << 8) |
                          ((uint32_t)target_be[byte_idx + 3]);
    }
}

// ============================================================================
// Chain safety (same as CPU miner)
// ============================================================================

struct ChainSafetyCheck {
    bool safe;
    string error;
    string network;
    string genesis_hash;
    uint64_t height;
    uint64_t peer_count;
};

static ChainSafetyCheck verify_chain_safety(const string& url, const string& cookie, const string& expected_network) {
    ChainSafetyCheck result;
    result.safe = false;

    Json::Value params(Json::arrayValue);
    Json::Value blockchain_info;
    if (!rpc_call(url, cookie, "blockchain.getinfo", params, blockchain_info)) {
        result.error = "Failed to call blockchain.getinfo - daemon may not be running";
        return result;
    }
    result.network = blockchain_info.get("chain", "unknown").asString();
    result.height = blockchain_info.get("blocks", 0).asUInt64();

    Json::Value genesis_params(Json::arrayValue);
    genesis_params.append(0);
    Json::Value genesis_hash_result;
    if (!rpc_call(url, cookie, "blockchain.getblockhash", genesis_params, genesis_hash_result)) {
        result.error = "Failed to get genesis block hash";
        return result;
    }
    result.genesis_hash = genesis_hash_result.asString();

    const auto normalized_network = dinero::consensus::NormalizeNetworkName(expected_network);
    const auto expected_genesis = dinero::consensus::ExpectedGenesisForNetwork(normalized_network);
    if (!expected_genesis.empty() && result.genesis_hash != std::string(expected_genesis)) {
        result.error = "Genesis hash mismatch - wrong chain!";
        return result;
    }

    Json::Value peer_params(Json::arrayValue);
    Json::Value peer_info;
    if (rpc_call(url, cookie, "getpeerinfo", peer_params, peer_info)) {
        result.peer_count = peer_info.size();
    }

    if (expected_network == "mainnet" && result.peer_count == 0) {
        result.error = "No peer connections - mining would produce orphan blocks";
        return result;
    }

    result.safe = true;
    return result;
}

// ============================================================================
// GPU Backend initialization
// ============================================================================

struct GPUContext {
    unique_ptr<IComputeBackend> backend;
    BackendType type;
    string device_name;
    bool ready;
};

static GPUContext init_gpu(const string& requested_backend) {
    GPUContext ctx;
    ctx.ready = false;
    ctx.type = BackendType::NONE;

    fprintf(stderr, "[GPU] Detecting devices...\n");

    GPUDeviceManager dm;
    auto devices = dm.detectAllDevices();

    fprintf(stderr, "[GPU] Found %zu device(s):\n", devices.size());
    for (auto& d : devices) {
        fprintf(stderr, "  [%u] %s (%s, %zu MB, %u CUs)\n",
                d.device_id, d.name.c_str(),
                backendToString(d.backend).c_str(),
                d.global_memory_mb, d.compute_units);
    }

    if (devices.empty()) {
        fprintf(stderr, "[GPU] ERROR: No GPU devices found!\n");
        return ctx;
    }

    // Select backend
    BackendType selected = BackendType::NONE;
    if (requested_backend == "auto") {
        selected = dm.getBestAvailableBackend();
    } else if (requested_backend == "metal") {
        selected = BackendType::METAL;
    } else if (requested_backend == "cuda") {
        selected = BackendType::CUDA;
    } else if (requested_backend == "opencl") {
        selected = BackendType::OPENCL;
    }

    fprintf(stderr, "[GPU] Selected backend: %s\n", backendToString(selected).c_str());

    if (selected == BackendType::NONE) {
        fprintf(stderr, "[GPU] ERROR: No suitable backend available!\n");
        return ctx;
    }

    // Create backend
    ctx.backend = createBackend(selected);
    if (!ctx.backend) {
        fprintf(stderr, "[GPU] ERROR: createBackend(%s) returned nullptr\n",
                backendToString(selected).c_str());
        return ctx;
    }
    fprintf(stderr, "[GPU] Backend created OK\n");

    // Find device for this backend
    uint32_t dev_id = 0;
    for (auto& d : devices) {
        if (d.backend == selected) {
            dev_id = d.device_id;
            ctx.device_name = d.name;
            break;
        }
    }

    // Init device
    if (!ctx.backend->initDevice(dev_id)) {
        fprintf(stderr, "[GPU] ERROR: initDevice(%u) failed\n", dev_id);
        return ctx;
    }
    fprintf(stderr, "[GPU] initDevice(%u) OK — %s\n", dev_id, ctx.device_name.c_str());

    // Compile kernel — OpenCL needs source loaded from file; Metal uses precompiled .metallib
    std::string kernel_src;
    if (selected == BackendType::OPENCL) {
        kernel_src = loadOpenCLKernelSource();
        if (kernel_src.empty()) {
            fprintf(stderr, "[GPU] ERROR: Failed to load OpenCL kernel source\n");
            return ctx;
        }
    }
    if (!ctx.backend->compileKernel(kernel_src)) {
        fprintf(stderr, "[GPU] ERROR: compileKernel failed\n");
        return ctx;
    }
    fprintf(stderr, "[GPU] compileKernel OK\n");

    ctx.type = selected;
    ctx.ready = true;
    fprintf(stderr, "[GPU] Ready to mine on %s (%s)\n",
            ctx.device_name.c_str(), backendToString(selected).c_str());
    return ctx;
}

// ============================================================================
// Longpoll watcher — tip-change notifier via server-side longpoll
// ============================================================================
//
// Purpose: decouple tip-change detection from GPU batch dispatch. Without
// this, the main loop relies on polling blockchain.getbestblockhash every
// 2 seconds, which burns up to 2 seconds of GPU hash-work on stale templates
// after every competing block lands on the network.
//
// Pattern (matches your design request):
//   - ONE long-poll request open at a time (single watcher thread,
//     single blocking RPC call in flight)
//   - Remember longpollid across iterations
//   - On wake (tip change detected via server-side longpoll), set the
//     stale-job flag; the main loop picks it up between GPU batch
//     dispatches and retires the current job cleanly
//   - Avoid overlapping requests (serialized by the single thread)
//
// The returned template body is intentionally discarded — this thread
// is a PURE SIGNAL. Actual job fetching goes through mining.getjob in
// the main loop, which has stricter safety gating (mining readiness,
// utreexo state, strict assembly).
//
// Compatible with older daemons: pre-8bad44f15 daemons don't block on
// longpollid (return immediately). In that case the watcher spins in a
// fast loop — we throttle with a short sleep on same-longpollid returns
// so we don't hammer the RPC on daemons that don't support the feature.

static void longpoll_watcher(const string& rpc_url,
                             const string& cookie_in,
                             const string& cookie_path,
                             const string& mining_address) {
    string last_longpollid;
    string cookie = cookie_in;

    while (!g_shutdown.load()) {
        Json::Value params(Json::arrayValue);
        Json::Value param_obj(Json::objectValue);
        param_obj["address"] = mining_address;
        if (!last_longpollid.empty()) {
            param_obj["longpollid"] = last_longpollid;
        }
        params.append(param_obj);

        Json::Value result;
        // 30s timeout on the RPC itself is safe: the server-side longpoll
        // is bounded at ~8s, so any response is already in well under 30s.
        // If the server is dead, we'll time out and retry after a short
        // backoff below.
        bool ok = rpc_call(rpc_url, cookie, "mining.getblocktemplate",
                           params, result, 30, &cookie_path);

        if (!ok) {
            // Transient RPC failure — don't hammer
            this_thread::sleep_for(chrono::seconds(2));
            continue;
        }

        string new_longpollid = result.get("longpollid", "").asString();

        if (new_longpollid.empty()) {
            // Daemon doesn't expose longpollid (pre-8bad44f15). Fall back
            // to 2s polling — matches the old in-main-loop tip check cadence
            // so behavior doesn't regress against older daemons.
            this_thread::sleep_for(chrono::seconds(2));
            // Heuristic: re-read best-block-hash as our poor-man's tip token
            Json::Value tip_result;
            if (rpc_call(rpc_url, cookie, "blockchain.getbestblockhash",
                         Json::Value(Json::arrayValue), tip_result, 30, &cookie_path)) {
                string current_tip = tip_result.isString() ? tip_result.asString() : "";
                if (!current_tip.empty() && !last_longpollid.empty() &&
                    current_tip != last_longpollid) {
                    g_stale_job.store(true);
                }
                last_longpollid = current_tip;
            }
            continue;
        }

        // Server supports longpoll. On first call, last_longpollid is empty
        // and the server returns immediately with the current tip token.
        // On subsequent calls, the server holds the request open until the
        // tip advances (~8s max).
        if (!last_longpollid.empty() && new_longpollid != last_longpollid) {
            // Tip changed — signal main loop to drop current job
            g_stale_job.store(true);
        }
        last_longpollid = new_longpollid;
        // No sleep on success: the server already parked us for up to ~8s.
        // Looping back immediately just re-parks us against the new tip.
    }
}

// ============================================================================
// Stats reporter
// ============================================================================

static void stats_reporter() {
    uint64_t last_hashes = 0;
    auto last_time = chrono::steady_clock::now();

    while (!g_shutdown.load()) {
        this_thread::sleep_for(chrono::seconds(5));

        auto now = chrono::steady_clock::now();
        uint64_t current_hashes = g_total_hashes.load();
        double elapsed = chrono::duration<double>(now - last_time).count();

        if (elapsed > 0 && current_hashes > last_hashes) {
            double hashrate = (current_hashes - last_hashes) / elapsed;
            g_gpu_hashrate.store(hashrate);

            // Pick best unit
            const char* unit;
            double display_rate;
            if (hashrate >= 1e9) {
                display_rate = hashrate / 1e9;
                unit = "GH/s";
            } else if (hashrate >= 1e6) {
                display_rate = hashrate / 1e6;
                unit = "MH/s";
            } else if (hashrate >= 1e3) {
                display_rate = hashrate / 1e3;
                unit = "KH/s";
            } else {
                display_rate = hashrate;
                unit = "H/s";
            }

            fprintf(stderr, "GPU: %.2f %s | Total: %llu MH | Blocks: %u\n",
                    display_rate, unit,
                    (unsigned long long)(current_hashes / 1000000),
                    g_blocks_found.load());
        }

        last_hashes = current_hashes;
        last_time = now;
    }
}

// ============================================================================
// Block submission
// ============================================================================

static bool submit_solution(const string& url, const string& cookie,
                            const string& cookie_path,
                            const string& job_id, uint32_t nonce, uint32_t height) {
    Json::Value submit_params(Json::arrayValue);
    Json::Value submit_obj(Json::objectValue);
    submit_obj["job_id"] = job_id;
    submit_obj["nonce"] = nonce;
    submit_params.append(submit_obj);
    Json::Value submit_result;

    if (!rpc_call(url, cookie, "mining.submit", submit_params, submit_result, 120, &cookie_path)) {
        fprintf(stderr, "SUBMIT FAILED: RPC error for height %u, nonce %u\n", height, nonce);
        return false;
    }

    // Accept various success encodings
    bool accepted = false;
    if (submit_result.isNull()) {
        accepted = true;
    } else if (submit_result.isObject()) {
        bool has_error = submit_result.isMember("error") &&
                         !submit_result["error"].isNull() &&
                         !(submit_result["error"].isString() && submit_result["error"].asString().empty());
        if (!has_error && submit_result.empty()) {
            accepted = true;
        } else if (submit_result.get("accepted", false).asBool() ||
                   submit_result.get("status", "").asString() == "accepted") {
            accepted = true;
        }
    } else if (submit_result.isBool()) {
        accepted = submit_result.asBool();
    }

    if (accepted) {
        fprintf(stderr, "BLOCK ACCEPTED at height %u (nonce %u)\n", height, nonce);
        g_blocks_found.fetch_add(1);
        return true;
    } else {
        fprintf(stderr, "BLOCK REJECTED at height %u: %s\n", height,
                submit_result.toStyledString().c_str());
        return false;
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int rpc_port = 20998;
    string rpc_url;
    string mining_address;
    string cookie_path;
    string datadir;
    string backend_str = "auto";
    bool force_mining = false;

    // Parse args
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if ((arg == "--rpc" || arg == "-r") && i + 1 < argc) {
            rpc_url = argv[++i];
        } else if (arg.substr(0, 10) == "--rpcport=") {
            rpc_port = stoi(arg.substr(10));
        } else if (arg == "--rpcport" && i + 1 < argc) {
            rpc_port = stoi(argv[++i]);
        } else if ((arg == "--address" || arg == "-a") && i + 1 < argc) {
            mining_address = argv[++i];
        } else if ((arg == "--backend" || arg == "-b") && i + 1 < argc) {
            backend_str = argv[++i];
            // Normalize
            transform(backend_str.begin(), backend_str.end(), backend_str.begin(), ::tolower);
        } else if (arg == "--cookie" && i + 1 < argc) {
            cookie_path = argv[++i];
        } else if (arg == "--datadir" && i + 1 < argc) {
            datadir = argv[++i];
        } else if (arg == "--force") {
            force_mining = true;
        } else if (arg == "--version") {
            print_version(stdout);
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            print_version(stderr);
            fprintf(stderr, "\n");
            fprintf(stderr, "Usage: dinero-gpu-miner [options]\n\n");
            fprintf(stderr, "Options:\n");
            fprintf(stderr, "  --address, -a <addr>   Mining payout address (din1...)\n");
            fprintf(stderr, "  --backend, -b <type>   GPU backend: auto|metal|cuda|opencl (default: auto)\n");
            fprintf(stderr, "  --rpcport <port>       RPC port (default: 20998)\n");
            fprintf(stderr, "  --rpc, -r <url>        Full RPC URL (overrides --rpcport)\n");
            fprintf(stderr, "  --cookie <path>        Path to .cookie file\n");
            fprintf(stderr, "  --datadir <path>       Data directory for cookie auto-detection\n");
            fprintf(stderr, "  --force                Allow mining without peers\n");
            fprintf(stderr, "  --version              Show version/build information\n");
            fprintf(stderr, "  --help, -h             Show this help\n");
            fprintf(stderr, "\nBackends:\n");
            fprintf(stderr, "  metal   - Apple Silicon GPU (macOS)\n");
            fprintf(stderr, "  cuda    - NVIDIA GPU (Linux/Windows)\n");
            fprintf(stderr, "  opencl  - AMD/Intel/NVIDIA GPU (cross-platform)\n");
            fprintf(stderr, "  auto    - Auto-detect best available (default)\n");
            return 0;
        }
    }

    // Validate backend string
    if (backend_str != "auto" && backend_str != "metal" &&
        backend_str != "cuda" && backend_str != "opencl") {
        fprintf(stderr, "ERROR: Unknown backend '%s'. Use: auto, metal, cuda, opencl\n",
                backend_str.c_str());
        return 1;
    }

    // Construct RPC URL
    if (rpc_url.empty()) {
        rpc_url = "http://127.0.0.1:" + to_string(rpc_port) + "/";
    }

    // Validate address
    if (mining_address.empty()) {
        fprintf(stderr, "ERROR: --address is required\n");
        return 1;
    }
    bool valid_prefix = (mining_address.substr(0, 4) == "din1") ||
                        (mining_address.substr(0, 5) == "tdin1") ||
                        (mining_address.substr(0, 5) == "rdin1");
    if (!valid_prefix) {
        fprintf(stderr, "ERROR: Address must start with din1, tdin1, or rdin1\n");
        return 1;
    }

    // Load cookie
    string cookie;
    if (!cookie_path.empty()) {
        cookie = read_cookie(cookie_path);
    } else {
        cookie_path = find_cookie(datadir);
        if (!cookie_path.empty()) cookie = read_cookie(cookie_path);
    }
    if (cookie.empty()) {
        fprintf(stderr, "ERROR: Cookie file not found. Is dinerod running?\n");
        fprintf(stderr, "  Tried: ./.cookie, ./data/.cookie, ./data/mainnet/.cookie\n");
        if (!datadir.empty()) fprintf(stderr, "  Tried: %s/.cookie\n", datadir.c_str());
        return 1;
    }

    // Init libcurl
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // ========================================================================
    // Banner
    // ========================================================================

    fprintf(stderr, "\n");
    fprintf(stderr, "==============================================\n");
    auto build_identity = dinero::build::CurrentIdentity();
    fprintf(stderr, "       Dinero GPU Miner %s\n", build_identity.version.c_str());
    fprintf(stderr, "       Backend: %s\n", backend_str.c_str());
    fprintf(stderr, "==============================================\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  RPC:     %s\n", rpc_url.c_str());
    fprintf(stderr, "  Address: %s\n", mining_address.c_str());
    fprintf(stderr, "  Cookie:  %s\n", cookie_path.c_str());
    fprintf(stderr, "\n");

    // ========================================================================
    // Chain safety check
    // ========================================================================

    fprintf(stderr, "Verifying chain safety...\n");
    string expected_network = "mainnet";
    if (mining_address.substr(0, 5) == "tdin1") expected_network = "testnet";
    else if (mining_address.substr(0, 5) == "rdin1") expected_network = "regtest";

    ChainSafetyCheck safety = verify_chain_safety(rpc_url, cookie, expected_network);
    if (!safety.safe && !force_mining) {
        fprintf(stderr, "CHAIN SAFETY FAILED: %s\n", safety.error.c_str());
        fprintf(stderr, "Use --force to bypass (testing only)\n");
        curl_global_cleanup();
        return 1;
    }
    fprintf(stderr, "Chain OK: %s, height %llu, %llu peers\n",
            safety.network.c_str(),
            (unsigned long long)safety.height,
            (unsigned long long)safety.peer_count);

    // ========================================================================
    // Initialize GPU
    // ========================================================================

    GPUContext gpu = init_gpu(backend_str);
    if (!gpu.ready) {
        fprintf(stderr, "\nFATAL: GPU initialization failed.\n");
        fprintf(stderr, "Check that your GPU supports %s.\n", backend_str.c_str());
        curl_global_cleanup();
        return 1;
    }

    fprintf(stderr, "\n");
    fprintf(stderr, "==============================================\n");
    fprintf(stderr, "  GPU READY: %s (%s)\n",
            gpu.device_name.c_str(), backendToString(gpu.type).c_str());
    fprintf(stderr, "  Mining started!\n");
    fprintf(stderr, "==============================================\n");
    fprintf(stderr, "\n");

    // Start stats reporter
    thread stats_thread(stats_reporter);

    // Start longpoll watcher — sets g_stale_job when the chain tip advances.
    // Pure signaling thread; discards the template body it receives from
    // getblocktemplate. Actual job fetching stays in the main loop via
    // mining.getjob.
    thread longpoll_thread(longpoll_watcher, rpc_url, cookie, cookie_path, mining_address);

    // GPU batch size: 16M nonces per dispatch
    const uint32_t BATCH_SIZE = 0x01000000;  // 16,777,216

    // Nonce cursor — walks through entire 32-bit space
    uint32_t nonce_cursor = 0;

    // ========================================================================
    // Main mining loop
    // ========================================================================

    while (!g_shutdown.load()) {
        // Safety: refuse to mine if daemon has no peers (prevents local fork).
        // --force bypasses the in-loop check as well as the startup check;
        // without that the flag is misleading (its --help text promises
        // "Allow mining without peers" but the unconditional in-loop check
        // would still pause mining and block dev/regtest workflows).
        if (!force_mining)
        {
            Json::Value net_result;
            if (rpc_call(rpc_url, cookie, "getconnectioncount", Json::Value(Json::arrayValue), net_result, 30, &cookie_path)) {
                int connections = net_result.isInt() ? net_result.asInt() : 0;
                if (connections == 0) {
                    fprintf(stderr, "No peers connected — pausing mining to prevent local fork...\n");
                    while (!g_shutdown.load()) {
                        this_thread::sleep_for(chrono::seconds(5));
                        Json::Value retry_result;
                        if (rpc_call(rpc_url, cookie, "getconnectioncount", Json::Value(Json::arrayValue), retry_result, 30, &cookie_path)) {
                            int c = retry_result.isInt() ? retry_result.asInt() : 0;
                            if (c > 0) {
                                fprintf(stderr, "Peers reconnected (%d), resuming mining\n", c);
                                break;
                            }
                        }
                    }
                    continue;
                }
            }
        }

        // Get mining job from daemon
        Json::Value params(Json::arrayValue);
        Json::Value param_obj(Json::objectValue);
        param_obj["address"] = mining_address;
        params.append(param_obj);
        Json::Value job_result;

        if (!rpc_call(rpc_url, cookie, "mining.getjob", params, job_result, 30, &cookie_path)) {
            fprintf(stderr, "Failed to get job, retrying in 5s...\n");
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }

        string job_issue;
        if (describe_job_response_issue(job_result, job_issue)) {
            fprintf(stderr, "%s, retrying in 5s...\n", job_issue.c_str());
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }

        string job_id = job_result.get("job_id", "").asString();
        string header_hex = job_result.get("header_hex", "").asString();
        uint32_t height = job_result.get("height", 0).asUInt();
        uint32_t bits = 0;
        if (!parse_hex_u32_strict(job_result.get("bits", "2100ffff").asString(), bits)) {
            fprintf(stderr, "Failed to parse job difficulty bits, retrying in 5s...\n");
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }
        int nonce_offset = job_result.get("nonce_offset", 112).asInt();
        string prev_hash = job_result.get("prev_hash", "").asString();

        if (job_id.empty() || header_hex.size() != 256) {
            fprintf(stderr, "Malformed job response (id=%s, hex_len=%zu), retrying in 5s...\n",
                    job_id.c_str(), header_hex.size());
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }

        // Convert header hex to bytes
        vector<uint8_t> header_bytes = from_hex(header_hex);
        if (header_bytes.size() != 128) {
            fprintf(stderr, "Invalid header size %zu, retrying...\n", header_bytes.size());
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }

        fprintf(stderr, "Job: height=%u bits=0x%08x prev=%s...\n",
                height, bits, prev_hash.substr(0, 16).c_str());

        // Compute target
        uint8_t target_be[32];
        compact_to_target(bits, target_be);
        uint32_t target_words[8];
        target_bytes_to_words(target_be, target_words);

        // Prepare WorkPackage header as uint32_t words
        // The header is 128 bytes = 32 uint32_t words (little-endian)
        uint32_t header_words[32];
        for (int i = 0; i < 32; i++) {
            int off = i * 4;
            header_words[i] = ((uint32_t)header_bytes[off]) |
                              ((uint32_t)header_bytes[off + 1] << 8) |
                              ((uint32_t)header_bytes[off + 2] << 16) |
                              ((uint32_t)header_bytes[off + 3] << 24);
        }

        // Reset nonce cursor for each new job.
        // Also clear any stale-job signal left over from BEFORE this job
        // was fetched — the fresh getjob call above already rebased us to
        // the new tip, so any pending stale signal from the watcher is
        // describing state we've already reacted to.
        nonce_cursor = 0;
        bool solution_found = false;
        g_stale_job.store(false);

        // Job timeout: request fresh job every 120 seconds (failsafe only;
        // the longpoll watcher should normally trigger retires much sooner
        // than this whenever the tip actually moves).
        auto job_start = chrono::steady_clock::now();
        auto last_peer_check = job_start;

        // Dispatch GPU batches until solution found or job expires or
        // watcher signals stale.
        while (!g_shutdown.load() && !solution_found) {
            // Event-driven stale-job signal from the longpoll watcher.
            // This is the FAST path — the watcher trips this within
            // milliseconds of the daemon's notifyBlockConnected firing.
            if (g_stale_job.load()) {
                fprintf(stderr, "Tip changed during job — aborting stale work (event-driven)\n");
                break;
            }

            // Check job timeout (failsafe; should not normally trigger
            // with longpoll watcher active)
            auto now = chrono::steady_clock::now();
            auto elapsed = chrono::duration<double>(now - job_start).count();
            if (elapsed > 120.0) break;  // Get fresh job

            // Peer-loss check every ~10s: abort if daemon lost all peers
            // (prevents local fork). This is a SAFETY gate, not a tip check —
            // tip changes are handled by g_stale_job above.
            auto since_peer_check = chrono::duration<double>(now - last_peer_check).count();
            if (since_peer_check >= 10.0) {
                last_peer_check = now;
                Json::Value net_chk;
                if (rpc_call(rpc_url, cookie, "getconnectioncount", Json::Value(Json::arrayValue), net_chk, 30, &cookie_path)) {
                    if ((net_chk.isInt() ? net_chk.asInt() : 0) == 0) {
                        fprintf(stderr, "Peers lost mid-job — aborting job to prevent local fork\n");
                        break;
                    }
                }
            }

            // Calculate batch range
            uint32_t nonce_start = nonce_cursor;
            uint64_t nonce_end_64 = (uint64_t)nonce_cursor + BATCH_SIZE - 1;
            uint32_t nonce_end = (nonce_end_64 > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)nonce_end_64;

            // Prepare work package
            WorkPackage work;
            memcpy(work.header, header_words, sizeof(header_words));
            memcpy(work.target, target_words, sizeof(target_words));
            work.nonce_start = nonce_start;
            work.nonce_end = nonce_end;
            work.backend = gpu.type;

            // Dispatch to GPU
            MiningResult result;
            auto t0 = chrono::high_resolution_clock::now();
            bool ok = gpu.backend->mine(work, result);
            auto t1 = chrono::high_resolution_clock::now();

            if (!ok) {
                fprintf(stderr, "GPU mine() error, retrying...\n");
                this_thread::sleep_for(chrono::milliseconds(100));
                continue;
            }

            // Update stats
            g_total_hashes.fetch_add(result.hashes_tried);

            // Check for solution
            if (result.found) {
                // Compute real hash on CPU for display + verification
                // Write nonce into header at nonce_offset (little-endian)
                header_bytes[nonce_offset]     = (result.nonce)       & 0xFF;
                header_bytes[nonce_offset + 1] = (result.nonce >> 8)  & 0xFF;
                header_bytes[nonce_offset + 2] = (result.nonce >> 16) & 0xFF;
                header_bytes[nonce_offset + 3] = (result.nonce >> 24) & 0xFF;

                // Double-SHA256 of 128-byte header
                uint8_t hash1[32], hash2[32];
                dinero::crypto::CSHA256().Write(header_bytes.data(), 128).Finalize(hash1);
                dinero::crypto::CSHA256().Write(hash1, 32).Finalize(hash2);

                fprintf(stderr, "\n╔══════════════════════════════════════════════════════════════════════╗\n");
                fprintf(stderr, "║  *** BLOCK FOUND! ***\n");
                fprintf(stderr, "║  Height: %u\n", height);
                fprintf(stderr, "║  Nonce:  %u (0x%08x)\n", result.nonce, result.nonce);
                // Display hash (SHA256 output: byte 0 = MSB, leading zeros first)
                fprintf(stderr, "║  Hash:         ");
                for (int i = 0; i < 32; i++) fprintf(stderr, "%02x", hash2[i]);
                fprintf(stderr, "\n");
                // Full header fields (uint256 stored little-endian in wire; reverse for display)
                fprintf(stderr, "║  prev_hash:    %s\n", prev_hash.c_str());
                fprintf(stderr, "║  merkle_root:  ");
                for (int i = 67; i >= 36; i--) fprintf(stderr, "%02x", header_bytes[i]);
                fprintf(stderr, "\n");
                fprintf(stderr, "║  utreexo_root: ");
                for (int i = 99; i >= 68; i--) fprintf(stderr, "%02x", header_bytes[i]);
                fprintf(stderr, "\n");
                fprintf(stderr, "║  nBits:        0x%08x\n", bits);
                fprintf(stderr, "╚══════════════════════════════════════════════════════════════════════╝\n");

                submit_solution(rpc_url, cookie, cookie_path, job_id, result.nonce, height);
                solution_found = true;

                // Wait for daemon to advance past this height before mining again.
                // Without this, we get a stale template and "chain tip changed" rejections.
                auto submit_time = chrono::steady_clock::now();
                while (!g_shutdown.load()) {
                    this_thread::sleep_for(chrono::milliseconds(500));
                    Json::Value check_params(Json::arrayValue);
                    Json::Value check_obj(Json::objectValue);
                    check_obj["address"] = mining_address;
                    check_params.append(check_obj);
                    Json::Value check_result;
                    if (rpc_call(rpc_url, cookie, "mining.getjob", check_params, check_result, 30, &cookie_path)) {
                        uint32_t new_height = check_result.get("height", 0).asUInt();
                        if (new_height > height) {
                            fprintf(stderr, "  Daemon advanced to height %u\n", new_height);
                            break;
                        }
                    }
                    double waited = chrono::duration<double>(chrono::steady_clock::now() - submit_time).count();
                    if (waited > 10.0) {
                        fprintf(stderr, "  Timeout waiting for daemon, resuming\n");
                        break;
                    }
                }
                break;
            }

            // Advance cursor
            nonce_cursor = nonce_end + 1;
            if (nonce_cursor < nonce_start) break;  // Overflow — exhausted nonce space
        }
    }

    // Shutdown
    g_shutdown.store(true);
    if (gpu.backend) gpu.backend->stop();
    stats_thread.join();
    // Longpoll watcher may be parked in a server-side longpoll call for
    // up to ~8s. We join it after curl_global_cleanup to let any
    // in-flight request complete and release the connection cleanly.
    if (longpoll_thread.joinable()) {
        longpoll_thread.join();
    }
    curl_global_cleanup();

    fprintf(stderr, "\nGPU Miner stopped. Found %u blocks.\n", g_blocks_found.load());
    return 0;
}
