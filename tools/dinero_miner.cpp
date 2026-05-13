// Dinero CPU Miner - External mining process
// Communicates with dinerod via RPC (getblocktemplate/submitblock)
// Clean separation: daemon = consensus, miner = worker

#include "crypto/sha256.h"
#include "crypto/sha256_simd.h"
#include "build/build_identity.h"
#include "consensus/chain_identity.h"
#include "../external/bech32/bech32.hpp"
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
#include <curl/curl.h>
#include "compat/jsoncpp_compat.h"

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

using namespace std;
using namespace dinero::crypto;
using namespace std::string_view_literals;

// Global state
atomic<bool> g_shutdown{false};
atomic<uint64_t> g_hashes{0};
atomic<uint32_t> g_blocks_found{0};

static void print_version(ostream& out) {
    out << dinero::build::FormatIdentityMultiline();
}

// Helper: HTTP response writer
size_t write_callback(void* contents, size_t size, size_t nmemb, string* s) {
    s->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Helper: Base64 encode
string base64_encode(const string& input) {
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

// Helper: Read cookie file with strict validation
string read_cookie(const string& cookie_path) {
    ifstream file(cookie_path);
    if (!file.is_open()) {
        return "";
    }
    string cookie;
    getline(file, cookie);

    // Trim whitespace (spaces, tabs, CR, LF)
    cookie.erase(0, cookie.find_first_not_of(" \t\r\n"));
    cookie.erase(cookie.find_last_not_of(" \t\r\n") + 1);

    if (cookie.empty()) {
        cerr << "❌ Empty cookie file: " << cookie_path << endl;
        return "";
    }

    // Validate cookie format: username:password
    size_t colon_pos = cookie.find(':');
    if (colon_pos == string::npos) {
        cerr << "❌ Invalid cookie format (missing ':' separator): " << cookie_path << endl;
        return "";
    }

    string username = cookie.substr(0, colon_pos);
    string password = cookie.substr(colon_pos + 1);

    // Validate username and password are non-empty
    if (username.empty()) {
        cerr << "❌ Invalid cookie format (empty username): " << cookie_path << endl;
        return "";
    }

    if (password.empty()) {
        cerr << "❌ Invalid cookie format (empty password): " << cookie_path << endl;
        return "";
    }

    // Validate no embedded whitespace in credentials
    if (username.find(' ') != string::npos || username.find('\t') != string::npos ||
        password.find(' ') != string::npos || password.find('\t') != string::npos) {
        cerr << "❌ Invalid cookie format (embedded whitespace in credentials): " << cookie_path << endl;
        return "";
    }

    return cookie;
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

static bool parse_hex_byte_strict(const string& hex, uint8_t& out) {
    uint32_t value = 0;
    if (hex.size() != 2 || !parse_hex_u32_strict(hex, value) || value > 0xFF) {
        return false;
    }
    out = static_cast<uint8_t>(value);
    return true;
}

// Helper: Auto-detect cookie path
string find_cookie(const string& datadir) {
    if (!datadir.empty()) {
        string path = datadir + "/.cookie";
        ifstream test(path);
        if (test.good()) return path;
    }
    
    // Try common paths
    vector<string> paths = {
        "./.cookie",
        "./data/.cookie",
        "./data/mainnet/.cookie"
    };
    
    for (const auto& path : paths) {
        ifstream test(path);
        if (test.good()) return path;
    }
    
    return "";
}

// Helper: Reverse byte order of hex string (for endianness conversion)
// Bitcoin stores hashes internally as little-endian but displays as big-endian
string reverse_hex_bytes(const string& hex) {
    if (hex.length() % 2 != 0) return hex; // Invalid hex string

    string reversed;
    for (int i = hex.length() - 2; i >= 0; i -= 2) {
        reversed += hex.substr(i, 2);
    }
    return reversed;
}

// RPC call
bool rpc_call(const string& url, const string& cookie, const string& method,
              const Json::Value& params, Json::Value& result,
              const string* cookie_path = nullptr) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    Json::Value request;
    // Plain const char* keys here (no sv suffix). JsonCpp's
    // Value::operator[] doesn't accept std::string_view directly, and
    // the implicit string_view -> const char* conversion that libstdc++
    // tolerates is rejected by MSVC's STL. const char* works on every
    // platform without any conversion needed.
    request["jsonrpc"] = "2.0";
    request["id"] = "miner";
    request["method"] = method;
    request["params"] = params;
    
    Json::StreamWriterBuilder writer;
    string json_str = Json::writeString(writer, request);
    
    string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    // Submission can legitimately take longer than template polling because
    // the daemon may spend real time validating and connecting the found block.
    const long timeout_seconds =
        (method == "mining.submit" || method == "submitblock" || method == "mining.submitblock")
            ? 120L
            : 30L;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // Use CURLOPT_USERPWD for HTTP Basic auth (same as curl -u)
    const string auth_cookie =
        (cookie_path != nullptr && !cookie_path->empty()) ? read_cookie(*cookie_path) : cookie;
    if (!auth_cookie.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth_cookie.c_str());
    }
    
    CURLcode res = curl_easy_perform(curl);

    // Debug: Check HTTP response code
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        cerr << "DEBUG: CURL error: " << curl_easy_strerror(res) << " (code: " << res << ")" << endl;
        cerr << "DEBUG: URL was: " << url << endl;
        return false;
    }

    if (http_code != 200) {
        cerr << "DEBUG: HTTP error: " << http_code << endl;
        cerr << "DEBUG: Response: " << response << endl;
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

// Helper: hex conversion
string to_hex(const uint8_t* data, size_t len) {
    ostringstream oss;
    oss << hex << setfill('0');
    for (size_t i = 0; i < len; i++) {
        oss << setw(2) << (unsigned)data[i];
    }
    return oss.str();
}

vector<uint8_t> from_hex(const string& hex) {
    vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        string byte_str = hex.substr(i, 2);
        uint8_t byte = 0;
        if (!parse_hex_byte_strict(byte_str, byte)) {
            return {};
        }
        bytes.push_back(byte);
    }
    return bytes;
}

// ============================================================================
// MINING SAFETY CHECKS - PREVENT MINING ON WRONG CHAIN
// ============================================================================

struct ChainSafetyCheck {
    bool safe;
    string error;
    string network;
    string genesis_hash;
    uint64_t height;
    uint64_t peer_count;
    string chainwork;
};

// Verify we're mining on the correct chain before starting
ChainSafetyCheck verify_chain_safety(const string& url, const string& cookie, const string& expected_network) {
    ChainSafetyCheck result;
    result.safe = false;

    // Step 1: Get blockchain info (genesis, network, chainwork)
    Json::Value params(Json::arrayValue);
    Json::Value blockchain_info;

    if (!rpc_call(url, cookie, "blockchain.getinfo", params, blockchain_info)) {
        result.error = "Failed to call blockchain.getinfo - daemon may not be running";
        return result;
    }

    result.network = blockchain_info.get("chain", "unknown").asString();
    result.height = blockchain_info.get("blocks", 0).asUInt64();
    result.chainwork = blockchain_info.get("chainwork", "0x0").asString();

    // Step 2: Get genesis hash by requesting block 0
    Json::Value genesis_params(Json::arrayValue);
    genesis_params.append(0);  // Block height 0 = genesis
    Json::Value genesis_hash_result;

    if (!rpc_call(url, cookie, "blockchain.getblockhash", genesis_params, genesis_hash_result)) {
        result.error = "Failed to get genesis block hash";
        return result;
    }

    result.genesis_hash = genesis_hash_result.asString();

    // Step 3: Validate genesis hash matches expected network
    const auto normalized_network = dinero::consensus::NormalizeNetworkName(expected_network);
    const auto expected_genesis = dinero::consensus::ExpectedGenesisForNetwork(normalized_network);
    if (expected_genesis.empty()) {
        result.error = "Unknown network: " + expected_network;
        return result;
    }

    if (result.genesis_hash != std::string(expected_genesis)) {
        result.error = "CRITICAL: Genesis hash mismatch!\n";
        result.error += "   Expected (" + expected_network + "): " + std::string(expected_genesis) + "\n";
        result.error += "   Actual (daemon):   " + result.genesis_hash + "\n";
        result.error += "   This daemon is running on a different blockchain!\n";
        result.error += "   DO NOT MINE - you would be wasting hashpower on the wrong chain.";
        return result;
    }

    // Step 4: Get peer count (prevent solo mining on isolated chain)
    Json::Value peer_params(Json::arrayValue);
    Json::Value peer_info;

    if (!rpc_call(url, cookie, "getpeerinfo", peer_params, peer_info)) {
        result.error = "Failed to get peer info";
        return result;
    }

    result.peer_count = peer_info.size();

    // Step 5: Validate peer count (allow solo for regtest, require 1+ for mainnet/testnet)
    if (expected_network == "mainnet" || expected_network == "testnet") {
        if (result.peer_count == 0) {
            result.error = "CRITICAL: No peer connections!\n";
            result.error += "   You are mining in isolation (not connected to the network).\n";
            result.error += "   Mining will succeed but blocks will be ORPHANED.\n";
            result.error += "   Wait for peer connections before mining.";
            return result;
        }
    }

    // Step 6: Validate chainwork (prevent mining on trivial self-chain)
    // Convert chainwork hex to decimal for comparison
    // For mainnet, we expect significant work. For regtest, allow anything.
    //
    // NOTE: Disabled for initial mainnet launch (2025-11-10)
    // This check will be re-enabled once the network has established
    // sufficient chainwork (after ~100 blocks mined).
    //
    // if (expected_network == "mainnet") {
    //     // Minimum chainwork check (should be > 0x1 for real mainnet)
    //     // This prevents mining on a self-generated genesis-only chain
    //     if (result.chainwork == "0x0" || result.chainwork == "0x1") {
    //         result.error = "WARNING: Chainwork is trivial (" + result.chainwork + ")\n";
    //         result.error += "   This looks like a freshly initialized chain with no real work.\n";
    //         result.error += "   Are you sure you're connected to mainnet?";
    //         return result;
    //     }
    // }

    // All checks passed!
    result.safe = true;
    return result;
}

// Helper: compact target to full target (Bitcoin compact format)
// Format: 0xAABBCCCC where AA=exponent, BBCCCC=mantissa (3 bytes)
// Target = mantissa * 256^(exponent-3)
// Result is big-endian for hash comparison
void compact_to_target(uint32_t bits, uint8_t target[32]) {
    memset(target, 0, 32);
    uint32_t exp = bits >> 24;
    uint32_t mant = bits & 0x00ffffff;  // 3-byte mantissa

    if (exp <= 3) {
        // Mantissa >> (8 * (3 - exp)) goes in the last few bytes
        uint32_t r = mant >> (8 * (3 - exp));
        for (int i = 0; i < 4 && r; i++) {
            target[31 - i] = (uint8_t)(r & 0xff);
            r >>= 8;
        }
    } else {
        // Place 3-byte mantissa at position (exp-3) from the right (big-endian)
        // For 0x1d3fffff: exp=29, place 0x3fffff at byte position 29-3=26 from right
        // In big-endian array: index 32-29 = 3 (counting from left)
        int idx = 32 - exp;
        if (idx >= 0 && idx <= 29) {
            target[idx] = (uint8_t)((mant >> 16) & 0xff);
            target[idx + 1] = (uint8_t)((mant >> 8) & 0xff);
            target[idx + 2] = (uint8_t)(mant & 0xff);
        }
    }
}

// Compare hash to target (both big-endian)
bool hash_below_target(const uint8_t hash[32], const uint8_t target[32]) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true;
}

// Double SHA256
void sha256d(const uint8_t* data, size_t len, uint8_t out[32]) {
    CSHA256 h1;
    h1.Write(data, len);
    uint8_t tmp[32];
    h1.Finalize(tmp);
    CSHA256 h2;
    h2.Write(tmp, 32);
    h2.Finalize(out);
}

// Write uint32 little-endian
void write_u32_le(uint8_t* p, uint32_t x) {
    p[0] = x & 0xff;
    p[1] = (x >> 8) & 0xff;
    p[2] = (x >> 16) & 0xff;
    p[3] = (x >> 24) & 0xff;
}

// Mining worker thread (now with SIMD!)
void mine_worker(uint32_t thread_id, uint32_t start_nonce, uint32_t stride,
                const vector<uint8_t>& header_template, uint32_t bits,
                atomic<bool>& found, atomic<uint32_t>& winning_nonce,
                SIMDLevel simd_level, int nonce_offset) {
    vector<uint8_t> header = header_template;
    uint8_t target[32];
    compact_to_target(bits, target);

    // DEBUG: Print target only from thread 0
    if (thread_id == 0) {
        cout << "🎯 DEBUG [Thread " << thread_id << "]: bits=0x" << hex << bits << dec << endl;
        cout << "   Target (big-endian): " << to_hex(target, 32) << endl;
    }

    uint32_t nonce = start_nonce;
    uint8_t hash[32];
    uint32_t debug_count = 0;

    while (!found.load() && !g_shutdown.load()) {
        // Write nonce at dynamic offset (from mining.getjob nonce_offset field)
        write_u32_le(&header[nonce_offset], nonce);

        // Double SHA256 with SIMD optimization
        // ✅ CRITICAL: Dinero PoW covers FULL 128 bytes (Phase W.1.1 header)
        // We need to hash all 128 bytes
        sha256d(header.data(), 128, hash);

        // ✅ v0.14.0.4 FIX: sha256d outputs big-endian, compact_to_target outputs big-endian
        // Compare DIRECTLY without reversal. Previous code incorrectly reversed the hash,
        // causing miner to find solutions that daemon would reject with "bad-pow".
        // Both hash and target are already in big-endian format for comparison.

        // DEBUG: Print first few hashes from thread 0
        if (thread_id == 0 && debug_count < 3) {
            cout << "   Hash #" << debug_count << " (nonce=" << nonce << "): " << to_hex(hash, 32) << endl;
            debug_count++;
        }

        // Check if below target (both in big-endian format)
        if (hash_below_target(hash, target)) {
            // SHA-256 raw output is big-endian (MSB at byte[0], leading zeros first for valid PoW)
            // uint256 internal storage is little-endian (data[0]=LSB, GetHex shows MSB first)
            uint8_t hash_le[32];
            for (int i = 0; i < 32; i++) {
                hash_le[i] = hash[31 - i];
            }
            cout << "🎉 DEBUG: FOUND SOLUTION!" << endl;
            cout << "   Nonce: " << nonce << endl;
            cout << "   Hash (display): " << to_hex(hash, 32) << endl;
            cout << "   Hash (uint256 LE): " << to_hex(hash_le, 32) << endl;
            found.store(true);
            winning_nonce.store(nonce);
            return;
        }

        nonce += stride;
        g_hashes.fetch_add(1);

        // Overflow protection
        if (nonce < start_nonce) break;
    }
}

// Stats reporter thread
void stats_reporter() {
    uint64_t last_hashes = 0;
    auto last_time = chrono::steady_clock::now();
    
    while (!g_shutdown.load()) {
        this_thread::sleep_for(chrono::seconds(10));
        
        auto now = chrono::steady_clock::now();
        uint64_t current_hashes = g_hashes.load();
        double elapsed = chrono::duration<double>(now - last_time).count();
        
        double hashrate = (current_hashes - last_hashes) / elapsed;
        
        cout << "⛏️  " << fixed << setprecision(2) << (hashrate / 1000000.0) 
             << " MH/s | Total: " << (current_hashes / 1000000) 
             << " MH | Blocks: " << g_blocks_found.load() << endl;
        
        last_hashes = current_hashes;
        last_time = now;
    }
}

int main(int argc, char** argv) {
    int rpc_port = 20998;  // Default RPC port
    string rpc_url;  // Will be constructed from rpc_port
    string mining_address;
    int threads = 1;
    string cookie_path;
    string datadir;
    bool force_mining = false;  // Allow mining without peer connections (testing only)

    // Parse args
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--rpc" && i + 1 < argc) {
            rpc_url = argv[++i];
        } else if (arg.substr(0, 10) == "--rpcport=") {
            rpc_port = stoi(arg.substr(10));
        } else if (arg == "--rpcport" && i + 1 < argc) {
            rpc_port = stoi(argv[++i]);
        } else if (arg == "--address" && i + 1 < argc) {
            mining_address = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = stoi(argv[++i]);
        } else if (arg == "--cookie" && i + 1 < argc) {
            cookie_path = argv[++i];
        } else if (arg == "--datadir" && i + 1 < argc) {
            datadir = argv[++i];
        } else if (arg == "--force") {
            force_mining = true;
        } else if (arg == "--version") {
            print_version(cout);
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            print_version(cout);
            cout << "\n";
            cout << "Usage: dinero-miner [options]\n";
            cout << "Options:\n";
            cout << "  --rpc <url>       RPC endpoint (overrides --rpcport)\n";
            cout << "  --rpcport <port>  RPC port (default: 20998)\n";
            cout << "  --address <addr>  Mining payout address (din1...)\n";
            cout << "  --threads <n>     Number of threads (default: auto-detect)\n";
            cout << "  --cookie <path>   Path to .cookie file (default: auto-detect)\n";
            cout << "  --datadir <path>  Data directory for auto-detecting cookie\n";
            cout << "  --force           Allow mining without peers (TESTING ONLY)\n";
            cout << "  --version         Show version/build information\n";
            return 0;
        }
    }

    // Construct RPC URL if not explicitly set via --rpc
    if (rpc_url.empty()) {
        rpc_url = "http://127.0.0.1:" + to_string(rpc_port) + "/";
    }

    // Auto-detect threads
    if (threads <= 0) {
        #ifdef __APPLE__
        int mib[2] = {CTL_HW, HW_NCPU};
        int ncpu = 0;
        size_t len = sizeof(ncpu);
        if (sysctl(mib, 2, &ncpu, &len, nullptr, 0) == 0 && ncpu > 0) {
            threads = ncpu;
        } else {
            threads = thread::hardware_concurrency();
        }
        #else
        threads = thread::hardware_concurrency();
        #endif
        if (threads == 0) threads = 1;
    }
    
    // Load cookie for authentication
    string cookie;

    // If explicit cookie file specified, use it
    if (!cookie_path.empty()) {
        cookie = read_cookie(cookie_path);
        if (cookie.empty()) {
            cerr << "❌ Failed to read cookie file: " << cookie_path << endl;
            return 1;
        }
    } else {
        // Auto-detect cookie path
        cookie_path = find_cookie(datadir);
        if (cookie_path.empty()) {
            cerr << "❌ Cookie file not found. Daemon may not be running." << endl;
            cerr << "   Tried: ./.cookie, ./data/.cookie, ./data/mainnet/.cookie" << endl;
            if (!datadir.empty()) {
                cerr << "   Tried: " << datadir << "/.cookie" << endl;
            }
            return 1;
        }

        cookie = read_cookie(cookie_path);
        if (cookie.empty()) {
            cerr << "❌ Failed to read cookie file: " << cookie_path << endl;
            return 1;
        }
    }

    cout << "✅ RPC authenticated via cookie: " << cookie_path << "\n";

    // Validate address
    if (mining_address.empty()) {
        cerr << "Error: --address is required\n";
        return 1;
    }

    // Accept mainnet (din1), testnet (tdin1), and regtest (rdin1) addresses
    bool valid_prefix = (mining_address.substr(0, 4) == "din1") ||
                        (mining_address.substr(0, 5) == "tdin1") ||
                        (mining_address.substr(0, 5) == "rdin1");

    if (!valid_prefix) {
        cerr << "Error: Address must start with 'din1', 'tdin1', or 'rdin1'\n";
        return 1;
    }
    
    // Detect SIMD capabilities
    SIMDContext simd_ctx;
    
    // Banner
    cout << "\n";
    cout << "╔═══════════════════════════════════════╗\n";
    auto build_identity = dinero::build::CurrentIdentity();
    cout << "║     Dinero CPU Miner " << build_identity.version
         << string(max(0, 16 - static_cast<int>(build_identity.version.size())), ' ')
         << "║\n";
    cout << "║     SIMD: " << simd_ctx.name() << string(29 - strlen(simd_ctx.name()), ' ') << "║\n";
    cout << "╚═══════════════════════════════════════╝\n";
    cout << "\n";
    cout << "Dinero: Real Money for Free People - Genesis Block 2025\n";
    cout << "\n";
    cout << "⚙️  Configuration:\n";
    cout << "   RPC:     " << rpc_url << "\n";
    cout << "   Payout:  " << mining_address << "\n";
    cout << "   Threads: " << threads << "\n";
    cout << "   Auth:    cookie\n";
    cout << "   SIMD:    " << simd_ctx.name();
    if (simd_ctx.level() == SIMDLevel::NEON || simd_ctx.level() == SIMDLevel::ARM_SHA) {
        cout << " (Apple Silicon optimized!)";
    }
    cout << "\n\n";
    
    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // ============================================================================
    // CHAIN SAFETY VALIDATION - PREVENT MINING ON WRONG CHAIN
    // ============================================================================

    cout << "\n";
    cout << "🔒 Performing chain safety checks...\n";
    cout << "   (This prevents mining on wrong chain or isolated node)\n\n";

    // Detect expected network from address prefix
    string expected_network;
    if (mining_address.substr(0, 4) == "din1") {
        expected_network = "mainnet";
    } else if (mining_address.substr(0, 5) == "tdin1") {
        expected_network = "testnet";
    } else if (mining_address.substr(0, 5) == "rdin1") {
        expected_network = "regtest";
    }

    ChainSafetyCheck safety = verify_chain_safety(rpc_url, cookie, expected_network);

    if (!safety.safe && !force_mining) {
        cerr << "\n";
        cerr << "╔═══════════════════════════════════════════════════════════╗\n";
        cerr << "║  ❌ MINING SAFETY CHECK FAILED                            ║\n";
        cerr << "╚═══════════════════════════════════════════════════════════╝\n";
        cerr << "\n";
        cerr << safety.error << "\n";
        cerr << "\n";
        cerr << "Mining has been BLOCKED to protect you from:\n";
        cerr << "  • Wasting hashpower on wrong blockchain\n";
        cerr << "  • Mining orphaned blocks (no peer connections)\n";
        cerr << "  • Mining on self-generated test chain\n";
        cerr << "\n";
        cerr << "Please fix the issue above and try again.\n";
        cerr << "Or use --force to bypass (TESTING ONLY).\n";
        cerr << "\n";
        curl_global_cleanup();
        return 1;
    }

    // Safety checks passed (or bypassed with --force) - show confirmation
    if (safety.safe) {
        cout << "✅ Chain Safety Validation PASSED\n";
        cout << "   Network:      " << safety.network << " (expected: " << expected_network << ")\n";
        cout << "   Genesis:      " << safety.genesis_hash.substr(0, 16) << "..." << "\n";
        cout << "   Height:       " << safety.height << "\n";
        cout << "   Peers:        " << safety.peer_count << "\n";
        cout << "   Chainwork:    " << safety.chainwork.substr(0, 18) << "..." << "\n";
        cout << "\n";
        cout << "╔═══════════════════════════════════════════════════════════╗\n";
        cout << "║  ✅ SAFE TO MINE - All validation checks passed          ║\n";
        cout << "╚═══════════════════════════════════════════════════════════╝\n";
        cout << "\n";
    } else {
        cout << "⚠️  Chain Safety Validation BYPASSED (--force enabled)\n";
        cout << "\n";
        cout << "🚨 WARNING: Mining on isolated node\n";
        cout << "   This is for TESTING ONLY.\n";
        cout << "   Blocks may be orphaned if not on the main chain.\n";
        cout << "\n";
    }

    // Start stats reporter
    thread stats_thread(stats_reporter);

    cout << "🚀 Mining started...\n\n";
    
    // Main mining loop — Stratum v2-style: server owns block assembly
    // Miner only grinds nonce on the 128-byte header returned by mining.getjob
    while (!g_shutdown.load()) {
        // Get immutable mining job from daemon
        Json::Value params(Json::arrayValue);
        Json::Value param_obj(Json::objectValue);
        param_obj["address"] = mining_address;
        params.append(param_obj);
        Json::Value job_result;

        if (!rpc_call(rpc_url, cookie, "mining.getjob", params, job_result, &cookie_path)) {
            cerr << "Failed to get mining job, retrying in 5s...\n";
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }

        string job_issue;
        if (describe_job_response_issue(job_result, job_issue)) {
            cerr << job_issue << ", retrying in 5s...\n";
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }

        // Parse job response
        string job_id = job_result.get("job_id", "").asString();
        string header_hex = job_result.get("header_hex", "").asString();
        uint32_t height = job_result.get("height", 0).asUInt();
        uint32_t bits = 0;
        if (!parse_hex_u32_strict(job_result.get("bits", "2100ffff").asString(), bits)) {
            cerr << "Failed to parse job difficulty bits, retrying in 5s...\n";
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }
        int nonce_offset = job_result.get("nonce_offset", 112).asInt();
        string prev_hash = job_result.get("prev_hash", "").asString();

        if (job_id.empty() || header_hex.size() != 256) {
            cerr << "Malformed job response (job_id=" << job_id
                 << ", header_hex.size=" << header_hex.size() << "), retrying in 5s...\n";
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }

        // Convert header hex to bytes
        vector<uint8_t> header = from_hex(header_hex);
        if (header.size() != 128) {
            cerr << "Invalid header size: " << header.size() << " (expected 128), retrying...\n";
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }

        cout << "Mining block " << height << " (job " << job_id
             << ", bits 0x" << hex << bits << dec
             << ", prev " << prev_hash.substr(0, 16) << "...)" << endl;

        // Start mining threads
        atomic<bool> found(false);
        atomic<uint32_t> winning_nonce(0);
        vector<thread> workers;

        for (int i = 0; i < threads; i++) {
            workers.emplace_back(mine_worker, i, i, threads,
                               ref(header), bits, ref(found),
                               ref(winning_nonce), simd_ctx.level(), nonce_offset);
        }

        // Wait for solution or timeout (30 seconds per job)
        auto start_time = chrono::steady_clock::now();
        while (!found.load() && !g_shutdown.load()) {
            auto elapsed = chrono::duration<double>(chrono::steady_clock::now() - start_time).count();
            if (elapsed > 30.0) {
                break;
            }
            this_thread::sleep_for(chrono::milliseconds(100));
        }

        // Stop all workers
        bool had_solution = found.load();
        found.store(true);
        for (auto& w : workers) {
            w.join();
        }

        // Submit solution via mining.submit
        if (had_solution && !g_shutdown.load()) {
            uint32_t nonce = winning_nonce.load();
            // Extract header fields for display
            auto hex_field = [&](int offset, int len) -> string {
                string h;
                for (int i = offset + len - 1; i >= offset; --i)
                    { char buf[3]; snprintf(buf, sizeof(buf), "%02x", header[i]); h += buf; }
                return h;
            };
            cout << "\n"
                 << "╔══════════════════════════════════════════════════════════════════════╗\n"
                 << "║  BLOCK FOUND!  height=" << height << "  nonce=" << nonce << "\n"
                 << "║  prev_hash:    " << hex_field(0x04, 32) << "\n"
                 << "║  merkle_root:  " << hex_field(0x24, 32) << "\n"
                 << "║  utreexo_root: " << hex_field(0x44, 32) << "\n"
                 << "║  nBits:        0x" << hex << bits << dec << "\n"
                 << "╚══════════════════════════════════════════════════════════════════════╝\n"
                 << endl;

            Json::Value submit_params(Json::arrayValue);
            Json::Value submit_obj(Json::objectValue);
            submit_obj["job_id"] = job_id;
            submit_obj["nonce"] = nonce;
            submit_params.append(submit_obj);
            Json::Value submit_result;

            if (rpc_call(rpc_url, cookie, "mining.submit", submit_params, submit_result, &cookie_path)) {
                // Be permissive on success encoding:
                // - null (preferred)
                // - empty object {} (some adapters normalize null this way)
                // - explicit {"status":"accepted"} / {"accepted":true}
                bool accepted = false;
                string reject_code;
                string reject_error;

                if (submit_result.isNull()) {
                    accepted = true;
                } else if (submit_result.isObject()) {
                    const bool has_code = submit_result.isMember("code");
                    const bool has_error = submit_result.isMember("error") &&
                                           !submit_result["error"].isNull() &&
                                           !(submit_result["error"].isString() &&
                                             submit_result["error"].asString().empty());

                    if (!has_code && !has_error && submit_result.empty()) {
                        accepted = true;  // normalized-null success
                    } else if (submit_result.get("accepted", false).asBool() ||
                               submit_result.get("status", "").asString() == "accepted" ||
                               submit_result.get("status", "").asString() == "ok") {
                        accepted = true;
                    } else {
                        reject_code = submit_result.get("code", "unknown").asString();
                        reject_error = submit_result.get("error", "").asString();
                    }
                } else if (submit_result.isBool()) {
                    accepted = submit_result.asBool();
                    if (!accepted) reject_code = "unknown";
                } else if (submit_result.isString()) {
                    string s = submit_result.asString();
                    if (s.empty() || s == "accepted" || s == "ok") {
                        accepted = true;
                    } else {
                        reject_code = "unknown";
                        reject_error = s;
                    }
                } else {
                    reject_code = "unknown";
                }

                if (accepted) {
                    cout << "Block ACCEPTED by daemon! (height " << height << ")" << endl;
                    g_blocks_found.fetch_add(1);
                    // Brief pause for block processing before requesting next job
                    this_thread::sleep_for(chrono::milliseconds(500));
                } else {
                    cerr << "Block REJECTED: " << (reject_code.empty() ? "unknown" : reject_code) << endl;
                    if (!reject_error.empty()) {
                        cerr << "   " << reject_error << endl;
                    }
                    if (reject_code == "unknown") {
                        cerr << "   Raw submit result: " << submit_result.toStyledString() << endl;
                    }
                }
            } else {
                cerr << "Failed to submit block (RPC error)" << endl;
            }
        }
    }
    
    g_shutdown.store(true);
    stats_thread.join();
    
    curl_global_cleanup();
    
    cout << "\n⛏️  Mining stopped. Found " << g_blocks_found.load() << " blocks.\n";
    
    return 0;
}
