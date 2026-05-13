// Dinero Stratum Worker - Dedicated external pool miner
// Connects to dinero-stratum over line-delimited JSON Stratum and hashes locally.

#include "crypto/sha256.h"
#include "crypto/sha256_simd.h"
#include "build/build_identity.h"
#include "compat/jsoncpp_compat.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

using namespace std;
using namespace dinero::crypto;

namespace {

atomic<bool> g_shutdown{false};
atomic<uint64_t> g_hashes{0};
atomic<uint64_t> g_shares_accepted{0};
atomic<uint64_t> g_shares_rejected{0};
atomic<uint64_t> g_blocks_found{0};

void handle_signal(int) {
    g_shutdown.store(true);
}

void print_version(ostream& out) {
    out << dinero::build::FormatIdentityMultiline();
}

string trim_copy(string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool parse_uint32_hex_strict(const string& hex, uint32_t& out) {
    if (hex.empty() || hex.size() > 8) {
        return false;
    }
    for (char c : hex) {
        if (!isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    try {
        unsigned long value = stoul(hex, nullptr, 16);
        if (value > 0xffffffffUL) {
            return false;
        }
        out = static_cast<uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

vector<uint8_t> hex_to_bytes(const string& hex) {
    vector<uint8_t> bytes;
    if ((hex.size() % 2) != 0) {
        return bytes;
    }
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        char* end = nullptr;
        const auto value = strtoul(hex.substr(i, 2).c_str(), &end, 16);
        if (end == nullptr || *end != '\0') {
            return {};
        }
        bytes.push_back(static_cast<uint8_t>(value));
    }
    return bytes;
}

string bytes_to_hex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0f]);
        out.push_back(kHex[data[i] & 0x0f]);
    }
    return out;
}

void sha256d(const uint8_t* data, size_t len, uint8_t out[32]) {
    CSHA256 h1;
    h1.Write(data, len);
    uint8_t tmp[32];
    h1.Finalize(tmp);

    CSHA256 h2;
    h2.Write(tmp, 32);
    h2.Finalize(out);
}

void write_u32_le(uint8_t* p, uint32_t x) {
    p[0] = x & 0xff;
    p[1] = (x >> 8) & 0xff;
    p[2] = (x >> 16) & 0xff;
    p[3] = (x >> 24) & 0xff;
}

void write_u64_le(uint8_t* p, uint64_t x) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((x >> (i * 8)) & 0xff);
    }
}

void bits_to_target(uint32_t bits, uint8_t target[32]) {
    memset(target, 0, 32);
    const uint32_t exponent = bits >> 24;
    const uint32_t mantissa = bits & 0x007fffff;
    if (exponent <= 3) {
        uint32_t shifted = mantissa >> (8 * (3 - exponent));
        target[31] = shifted & 0xff;
        target[30] = (shifted >> 8) & 0xff;
        target[29] = (shifted >> 16) & 0xff;
    } else if (exponent <= 32) {
        const int pos = 32 - static_cast<int>(exponent);
        if (pos >= 0 && pos < 32) {
            target[pos] = (mantissa >> 16) & 0xff;
        }
        if (pos + 1 >= 0 && pos + 1 < 32) {
            target[pos + 1] = (mantissa >> 8) & 0xff;
        }
        if (pos + 2 >= 0 && pos + 2 < 32) {
            target[pos + 2] = mantissa & 0xff;
        }
    }
}

void difficulty_to_target(uint32_t nbits, double difficulty, uint8_t out[32]) {
    uint8_t net_target[32];
    bits_to_target(nbits, net_target);

    if (difficulty <= 0.0) {
        difficulty = 1.0;
    }

    memset(out, 0, 32);
    double remainder = 0.0;
    for (int i = 0; i < 32; ++i) {
        const double value = remainder * 256.0 + net_target[i];
        const double q = value / difficulty;
        out[i] = static_cast<uint8_t>(min(q, 255.0));
        remainder = value - out[i] * difficulty;
    }
}

bool hash_below_target(const uint8_t hash[32], const uint8_t target[32]) {
    for (int i = 0; i < 32; ++i) {
        if (hash[i] < target[i]) {
            return true;
        }
        if (hash[i] > target[i]) {
            return false;
        }
    }
    return true;
}

string calculate_merkle_root_from_txid(const string& txid_hex,
                                       const vector<string>& merkle_branches) {
    vector<uint8_t> current_hash = hex_to_bytes(txid_hex);
    if (current_hash.size() != 32) {
        return {};
    }

    reverse(current_hash.begin(), current_hash.end());

    for (const auto& branch : merkle_branches) {
        vector<uint8_t> branch_bytes = hex_to_bytes(branch);
        if (branch_bytes.size() != 32) {
            return {};
        }
        vector<uint8_t> concat;
        concat.reserve(64);
        concat.insert(concat.end(), current_hash.begin(), current_hash.end());
        concat.insert(concat.end(), branch_bytes.begin(), branch_bytes.end());

        uint8_t new_hash[32];
        sha256d(concat.data(), concat.size(), new_hash);
        current_hash.assign(new_hash, new_hash + 32);
    }

    return bytes_to_hex(current_hash.data(), current_hash.size());
}

string format_hashrate(double hashrate) {
    ostringstream oss;
    oss << fixed << setprecision(2);
    if (hashrate >= 1e9) {
        oss << (hashrate / 1e9) << " GH/s";
    } else if (hashrate >= 1e6) {
        oss << (hashrate / 1e6) << " MH/s";
    } else if (hashrate >= 1e3) {
        oss << (hashrate / 1e3) << " KH/s";
    } else {
        oss << hashrate << " H/s";
    }
    return oss.str();
}

int auto_detect_threads() {
    int threads = 0;
#ifdef __APPLE__
    int mib[2] = {CTL_HW, HW_NCPU};
    size_t len = sizeof(threads);
    if (sysctl(mib, 2, &threads, &len, nullptr, 0) != 0 || threads <= 0) {
        threads = static_cast<int>(thread::hardware_concurrency());
    }
#else
    threads = static_cast<int>(thread::hardware_concurrency());
#endif
    if (threads <= 0) {
        threads = 1;
    }
    return threads;
}

string random_hex_bytes(size_t bytes_len) {
    static random_device rd;
    static mt19937_64 gen(rd());
    uniform_int_distribution<uint32_t> dist(0, 255);
    vector<uint8_t> bytes(bytes_len);
    for (auto& b : bytes) {
        b = static_cast<uint8_t>(dist(gen));
    }
    return bytes_to_hex(bytes.data(), bytes.size());
}

struct EndpointParts {
    string host;
    int port = 3333;
};

optional<EndpointParts> parse_endpoint(string endpoint) {
    endpoint = trim_copy(endpoint);
    if (endpoint.empty()) {
        return nullopt;
    }

    constexpr const char* kScheme = "stratum+tcp://";
    if (endpoint.rfind(kScheme, 0) == 0) {
        endpoint.erase(0, strlen(kScheme));
    }

    if (!endpoint.empty() && endpoint.back() == '/') {
        endpoint.pop_back();
    }

    const auto colon = endpoint.rfind(':');
    if (colon == string::npos || colon == 0 || colon == endpoint.size() - 1) {
        return nullopt;
    }

    EndpointParts parts;
    parts.host = endpoint.substr(0, colon);
    try {
        parts.port = stoi(endpoint.substr(colon + 1));
    } catch (...) {
        return nullopt;
    }
    if (parts.port <= 0 || parts.port > 65535 || parts.host.empty()) {
        return nullopt;
    }
    return parts;
}

class CurlTcpConnection {
public:
    ~CurlTcpConnection() {
        close();
    }

    bool connect_to(const EndpointParts& endpoint, string& error) {
        close();

        curl_ = curl_easy_init();
        if (!curl_) {
            error = "curl_easy_init failed";
            return false;
        }

        const string url = "telnet://" + endpoint.host + ":" + to_string(endpoint.port);
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);

        const CURLcode rc = curl_easy_perform(curl_);
        if (rc != CURLE_OK) {
            error = curl_easy_strerror(rc);
            close();
            return false;
        }
        read_buffer_.clear();
        return true;
    }

    bool send_line(const string& line, string& error) {
        if (!curl_) {
            error = "connection not initialized";
            return false;
        }
        string payload = line;
        if (payload.empty() || payload.back() != '\n') {
            payload.push_back('\n');
        }
        size_t sent_total = 0;
        while (sent_total < payload.size() && !g_shutdown.load()) {
            size_t sent = 0;
            const auto rc = curl_easy_send(curl_,
                                           payload.data() + sent_total,
                                           payload.size() - sent_total,
                                           &sent);
            if (rc == CURLE_AGAIN) {
                this_thread::sleep_for(chrono::milliseconds(10));
                continue;
            }
            if (rc != CURLE_OK) {
                error = curl_easy_strerror(rc);
                return false;
            }
            sent_total += sent;
        }
        return sent_total == payload.size();
    }

    // Cap on a single un-terminated Stratum message. Stratum JSON lines are
    // typically a few hundred bytes; 1 MB is already ~1000x the realistic max.
    // A malicious or buggy pool server that never sends a newline would
    // otherwise let read_buffer_ grow without bound until the process OOMs.
    static constexpr size_t kMaxStratumLineBytes = 1 * 1024 * 1024;  // 1 MB

    bool read_messages(vector<string>& out_lines, string& error, int timeout_ms) {
        const auto deadline = chrono::steady_clock::now() + chrono::milliseconds(timeout_ms);

        auto flush_lines = [&]() {
            size_t newline = string::npos;
            while ((newline = read_buffer_.find('\n')) != string::npos) {
                string line = trim_copy(read_buffer_.substr(0, newline));
                read_buffer_.erase(0, newline + 1);
                if (!line.empty()) {
                    out_lines.push_back(line);
                }
            }
        };

        flush_lines();
        if (!out_lines.empty()) {
            return true;
        }

        while (!g_shutdown.load() && chrono::steady_clock::now() < deadline) {
            char buffer[4096];
            size_t nread = 0;
            const auto rc = curl_easy_recv(curl_, buffer, sizeof(buffer), &nread);
            if (rc == CURLE_AGAIN) {
                this_thread::sleep_for(chrono::milliseconds(25));
                continue;
            }
            if (rc != CURLE_OK) {
                error = curl_easy_strerror(rc);
                return false;
            }
            if (nread == 0) {
                error = "connection closed";
                return false;
            }
            // Size cap: reject BEFORE the append so a malicious pool can't
            // grow read_buffer_ past kMaxStratumLineBytes while waiting for a
            // newline that never comes.
            if (read_buffer_.size() + nread > kMaxStratumLineBytes) {
                error = "stratum message exceeds " +
                        std::to_string(kMaxStratumLineBytes) + " byte cap";
                read_buffer_.clear();
                return false;
            }
            read_buffer_.append(buffer, nread);
            flush_lines();
            if (!out_lines.empty()) {
                return true;
            }
        }

        return true;
    }

    void close() {
        if (curl_) {
            curl_easy_cleanup(curl_);
            curl_ = nullptr;
        }
        read_buffer_.clear();
    }

private:
    CURL* curl_ = nullptr;
    string read_buffer_;
};

struct StratumJob {
    string job_id;
    string prev_hash;
    vector<string> merkle_branches;
    string coinbase_txid;
    string utreexo_root;
    uint32_t version = 0;
    uint32_t nbits = 0;
    uint32_t ntime = 0;
    uint64_t sequence = 0;
};

bool is_pause_job_id(const string& job_id) {
    return job_id.rfind("pause-", 0) == 0;
}

struct PendingShare {
    int request_id = 0;
    string user;
    string job_id;
    string nonce2;
    string ntime_hex;
    string nonce_hex;
    string hash_hex;
    bool block_candidate = false;
    uint64_t job_sequence = 0;
};

struct SubmittedShareMeta {
    string hash_hex;
    bool block_candidate = false;
    uint64_t job_sequence = 0;
};

struct JobControl {
    StratumJob job;
    atomic<bool> stop{false};
};

struct SharedQueue {
    mutex mu;
    deque<PendingShare> shares;
};

void stats_reporter() {
    uint64_t last_hashes = 0;
    auto last = chrono::steady_clock::now();
    while (!g_shutdown.load()) {
        for (int i = 0; i < 40 && !g_shutdown.load(); ++i) {
            this_thread::sleep_for(chrono::milliseconds(250));
        }
        if (g_shutdown.load()) {
            break;
        }
        const auto now = chrono::steady_clock::now();
        const uint64_t current_hashes = g_hashes.load();
        const double elapsed = chrono::duration<double>(now - last).count();
        if (elapsed <= 0.0) {
            continue;
        }
        const double hashrate = static_cast<double>(current_hashes - last_hashes) / elapsed;
        cout << "⛏️  " << format_hashrate(hashrate)
             << " | Shares accepted: " << g_shares_accepted.load()
             << " | Shares rejected: " << g_shares_rejected.load()
             << " | Blocks: " << g_blocks_found.load()
             << endl;
        last_hashes = current_hashes;
        last = now;
    }
}

void mine_worker(int thread_id,
                 int total_threads,
                 shared_ptr<JobControl> control,
                 const string& nonce2_hex,
                 atomic<double>& difficulty,
                 SharedQueue& submit_queue) {
    const auto& job = control->job;

    const string merkle_root = calculate_merkle_root_from_txid(job.coinbase_txid, job.merkle_branches);
    if (merkle_root.empty()) {
        cerr << "❌ Failed to build merkle root for job " << job.job_id << endl;
        return;
    }

    vector<uint8_t> prev_hash = hex_to_bytes(job.prev_hash);
    vector<uint8_t> merkle_bytes = hex_to_bytes(merkle_root);
    vector<uint8_t> utreexo = hex_to_bytes(job.utreexo_root);
    if (prev_hash.size() != 32 || merkle_bytes.size() != 32 || utreexo.size() != 32) {
        cerr << "❌ Invalid job header fields for job " << job.job_id << endl;
        return;
    }
    reverse(prev_hash.begin(), prev_hash.end());
    reverse(utreexo.begin(), utreexo.end());

    vector<uint8_t> header(128, 0);
    write_u32_le(header.data(), job.version);
    memcpy(header.data() + 4, prev_hash.data(), 32);
    memcpy(header.data() + 36, merkle_bytes.data(), 32);
    memcpy(header.data() + 68, utreexo.data(), 32);
    write_u64_le(header.data() + 100, static_cast<uint64_t>(job.ntime));
    write_u32_le(header.data() + 108, job.nbits);

    uint8_t network_target[32];
    bits_to_target(job.nbits, network_target);

    double last_difficulty = -1.0;
    uint8_t share_target[32];
    memset(share_target, 0, sizeof(share_target));

    uint32_t nonce = static_cast<uint32_t>(thread_id);
    uint8_t hash[32];

    while (!control->stop.load() && !g_shutdown.load()) {
        const double current_difficulty = difficulty.load();
        if (fabs(current_difficulty - last_difficulty) > 1e-9) {
            difficulty_to_target(job.nbits, current_difficulty, share_target);
            last_difficulty = current_difficulty;
        }

        write_u32_le(header.data() + 112, nonce);
        sha256d(header.data(), header.size(), hash);
        g_hashes.fetch_add(1, memory_order_relaxed);

        if (hash_below_target(hash, share_target)) {
            PendingShare share;
            share.user = "";
            share.job_id = job.job_id;
            share.nonce2 = nonce2_hex;
            {
                ostringstream ntime_ss;
                ntime_ss << hex << setw(8) << setfill('0') << job.ntime;
                share.ntime_hex = ntime_ss.str();
            }
            {
                ostringstream nonce_ss;
                nonce_ss << hex << setw(8) << setfill('0') << nonce;
                share.nonce_hex = nonce_ss.str();
            }
            share.hash_hex = bytes_to_hex(hash, 32);
            share.block_candidate = hash_below_target(hash, network_target);
            share.job_sequence = job.sequence;

            lock_guard<mutex> lock(submit_queue.mu);
            submit_queue.shares.push_back(std::move(share));
        }

        nonce += static_cast<uint32_t>(total_threads);
        if (nonce < static_cast<uint32_t>(thread_id)) {
            nonce = static_cast<uint32_t>(thread_id);
        }
    }
}

optional<double> json_number_as_double(const Json::Value& value) {
    if (value.isDouble()) {
        return value.asDouble();
    }
    if (value.isInt() || value.isUInt() || value.isInt64() || value.isUInt64()) {
        return value.asDouble();
    }
    if (value.isString()) {
        try {
            return stod(value.asString());
        } catch (...) {
            return nullopt;
        }
    }
    return nullopt;
}

bool parse_notify_job(const Json::Value& params, uint64_t sequence, StratumJob& job, string& error) {
    if (!params.isArray() || params.size() < 10) {
        error = "notify params missing required fields";
        return false;
    }

    uint32_t version = 0;
    uint32_t nbits = 0;
    uint32_t ntime = 0;
    if (!params[0].isString() || !params[1].isString() || !params[4].isArray() ||
        !params[5].isString() || !params[6].isString() || !params[7].isString() ||
        !params[9].isString()) {
        error = "notify params have unexpected types";
        return false;
    }
    if (!parse_uint32_hex_strict(params[5].asString(), version) ||
        !parse_uint32_hex_strict(params[6].asString(), nbits) ||
        !parse_uint32_hex_strict(params[7].asString(), ntime)) {
        error = "notify hex fields are malformed";
        return false;
    }

    vector<string> branches;
    branches.reserve(params[4].size());
    for (const auto& branch : params[4]) {
        if (!branch.isString()) {
            error = "notify merkle branch is not a string";
            return false;
        }
        branches.push_back(branch.asString());
    }

    string coinbase_txid;
    if (params.size() >= 11 && params[10].isString()) {
        coinbase_txid = params[10].asString();
    }
    if (coinbase_txid.size() != 64) {
        error = "notify missing canonical coinbase txid";
        return false;
    }

    job.job_id = params[0].asString();
    job.prev_hash = params[1].asString();
    job.merkle_branches = std::move(branches);
    job.version = version;
    job.nbits = nbits;
    job.ntime = ntime;
    job.utreexo_root = params[9].asString();
    job.coinbase_txid = coinbase_txid;
    job.sequence = sequence;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    string endpoint;
    string user;
    string password = "x";
    int threads = 0;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "--stratum" && i + 1 < argc) {
            endpoint = argv[++i];
        } else if (arg.rfind("--stratum=", 0) == 0) {
            endpoint = arg.substr(10);
        } else if (arg == "--user" && i + 1 < argc) {
            user = argv[++i];
        } else if (arg.rfind("--user=", 0) == 0) {
            user = arg.substr(7);
        } else if (arg == "--password" && i + 1 < argc) {
            password = argv[++i];
        } else if (arg.rfind("--password=", 0) == 0) {
            password = arg.substr(11);
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = stoi(argv[++i]);
        } else if (arg.rfind("--threads=", 0) == 0) {
            threads = stoi(arg.substr(10));
        } else if (arg == "--version") {
            print_version(cout);
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            print_version(cout);
            cout << "\n";
            cout << "Usage: dinero-stratum-worker [options]\n";
            cout << "Options:\n";
            cout << "  --stratum <host:port>   Stratum endpoint (or stratum+tcp://host:port)\n";
            cout << "  --user <identity>       Worker identity (Dinero address or address.worker)\n";
            cout << "  --password <pass>       Worker password (default: x)\n";
            cout << "  --threads <n>           Number of CPU threads (default: auto-detect)\n";
            cout << "  --version               Show version/build information\n";
            return 0;
        }
    }

    if (endpoint.empty()) {
        cerr << "Error: --stratum is required\n";
        return 1;
    }
    if (user.empty()) {
        cerr << "Error: --user is required\n";
        return 1;
    }
    if (threads <= 0) {
        threads = auto_detect_threads();
    }

    auto parsed_endpoint = parse_endpoint(endpoint);
    if (!parsed_endpoint.has_value()) {
        cerr << "Error: invalid Stratum endpoint '" << endpoint << "'\n";
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    SIMDContext simd_ctx;
    cout << "\n";
    cout << "╔══════════════════════════════════════════════╗\n";
    auto build_identity = dinero::build::CurrentIdentity();
    cout << "║   Dinero Stratum Worker " << build_identity.version
         << string(max(0, 11 - static_cast<int>(build_identity.version.size())), ' ')
         << "║\n";
    cout << "║   SIMD: " << simd_ctx.name()
         << string(max(0, 31 - static_cast<int>(strlen(simd_ctx.name()))), ' ')
         << "║\n";
    cout << "╚══════════════════════════════════════════════╝\n\n";
    cout << "⚙️  Configuration:\n";
    cout << "   Stratum: " << parsed_endpoint->host << ":" << parsed_endpoint->port << "\n";
    cout << "   User:    " << user << "\n";
    cout << "   Threads: " << threads << "\n";
    cout << "   SIMD:    " << simd_ctx.name() << "\n\n";

    thread stats_thread(stats_reporter);

    atomic<double> difficulty{1.0};
    SharedQueue submit_queue;
    unordered_map<int, SubmittedShareMeta> pending_responses;
    vector<thread> worker_threads;
    shared_ptr<JobControl> active_job;
    uint64_t next_job_sequence = 1;
    int next_request_id = 1;

    auto stop_workers = [&]() {
        if (active_job) {
            active_job->stop.store(true);
        }
        for (auto& worker : worker_threads) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        worker_threads.clear();
        active_job.reset();
    };

    auto start_workers = [&](const StratumJob& job) {
        stop_workers();
        active_job = make_shared<JobControl>();
        active_job->job = job;

        for (int thread_id = 0; thread_id < threads; ++thread_id) {
            const uint32_t nonce2_seed = static_cast<uint32_t>(thread_id + 1);
            ostringstream nonce2_ss;
            nonce2_ss << hex << setw(8) << setfill('0') << nonce2_seed;
            worker_threads.emplace_back(mine_worker,
                                        thread_id,
                                        threads,
                                        active_job,
                                        nonce2_ss.str(),
                                        ref(difficulty),
                                        ref(submit_queue));
        }

        cout << "📦 New job " << job.job_id
             << " | diff=" << fixed << setprecision(6) << difficulty.load()
             << " | prev=" << job.prev_hash.substr(0, 16) << "..."
             << endl;
    };

    auto send_json = [&](CurlTcpConnection& connection,
                         const Json::Value& value,
                         string& error) -> bool {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return connection.send_line(Json::writeString(builder, value), error);
    };

    while (!g_shutdown.load()) {
        CurlTcpConnection connection;
        string error;

        cout << "🔌 Connecting to Stratum " << parsed_endpoint->host
             << ":" << parsed_endpoint->port << " ..." << endl;
        if (!connection.connect_to(*parsed_endpoint, error)) {
            cerr << "❌ Stratum connect failed: " << error << endl;
            this_thread::sleep_for(chrono::seconds(2));
            continue;
        }

        stop_workers();
        pending_responses.clear();
        difficulty.store(1.0);

        bool subscribed = false;
        bool authorized = false;
        string extra_nonce1;
        int extra_nonce2_size = 0;

        {
            Json::Value subscribe(Json::objectValue);
            subscribe["id"] = next_request_id++;
            subscribe["method"] = "mining.subscribe";
            subscribe["params"] = Json::arrayValue;
            subscribe["params"].append("dinero-stratum-worker/" + build_identity.version);
            if (!send_json(connection, subscribe, error)) {
                cerr << "❌ Failed to send subscribe: " << error << endl;
                this_thread::sleep_for(chrono::seconds(2));
                continue;
            }
        }

        while (!g_shutdown.load()) {
            vector<string> lines;
            if (!connection.read_messages(lines, error, 250)) {
                cerr << "⚠️  Stratum connection lost: " << error << endl;
                break;
            }

            for (const auto& line : lines) {
                Json::CharReaderBuilder builder;
                Json::Value message;
                string parse_errors;
                istringstream iss(line);
                if (!Json::parseFromStream(builder, iss, &message, &parse_errors)) {
                    cerr << "⚠️  Ignoring malformed Stratum message: " << parse_errors << endl;
                    continue;
                }

                if (message.isMember("method") && message["method"].isString()) {
                    const string method = message["method"].asString();
                    const Json::Value params = message["params"];

                    if (method == "mining.set_difficulty") {
                        if (params.isArray() && params.size() >= 1) {
                            auto new_diff = json_number_as_double(params[0]);
                            if (new_diff.has_value() && *new_diff > 0.0) {
                                difficulty.store(*new_diff);
                                cout << "🎯 Difficulty updated: " << fixed << setprecision(6)
                                     << *new_diff << endl;
                            }
                        }
                    } else if (method == "mining.notify") {
                        StratumJob job;
                        if (parse_notify_job(params, next_job_sequence++, job, error)) {
                            if (is_pause_job_id(job.job_id)) {
                                stop_workers();
                                cout << "⏸️  Pool paused work dispatch (" << job.job_id << ")" << endl;
                            } else {
                                start_workers(job);
                            }
                        } else {
                            cerr << "⚠️  Ignoring invalid job notify: " << error << endl;
                        }
                    } else if (method == "client.show_message") {
                        if (params.isArray() && params.size() >= 1 && params[0].isString()) {
                            cout << "ℹ️  Pool message: " << params[0].asString() << endl;
                        }
                    }
                    continue;
                }

                if (message.isMember("id") && message["id"].isInt()) {
                    const int id = message["id"].asInt();

                    if (!subscribed) {
                        const Json::Value result = message["result"];
                        if (!result.isArray() || result.size() < 3 ||
                            !result[1].isString() || !result[2].isInt()) {
                            cerr << "❌ Invalid subscribe response" << endl;
                            error = "invalid subscribe response";
                            break;
                        }
                        extra_nonce1 = result[1].asString();
                        extra_nonce2_size = result[2].asInt();
                        if (extra_nonce1.empty() || extra_nonce2_size <= 0) {
                            cerr << "❌ Subscribe response missing extranonce info" << endl;
                            error = "bad extranonce metadata";
                            break;
                        }
                        subscribed = true;

                        Json::Value authorize(Json::objectValue);
                        authorize["id"] = next_request_id++;
                        authorize["method"] = "mining.authorize";
                        authorize["params"] = Json::arrayValue;
                        authorize["params"].append(user);
                        authorize["params"].append(password);
                        if (!send_json(connection, authorize, error)) {
                            cerr << "❌ Failed to send authorize: " << error << endl;
                            break;
                        }
                        cout << "✅ Subscribed (extranonce1=" << extra_nonce1
                             << ", extranonce2_size=" << extra_nonce2_size << ")" << endl;
                        continue;
                    }

                    if (!authorized) {
                        const bool ok = message["result"].isBool() && message["result"].asBool();
                        if (!ok) {
                            string auth_error = "authorization failed";
                            if (message.isMember("error") && message["error"].isArray() &&
                                message["error"].size() >= 2 && message["error"][1].isString()) {
                                auth_error = message["error"][1].asString();
                            }
                            cerr << "❌ Stratum authorize failed: " << auth_error << endl;
                            error = auth_error;
                            break;
                        }
                        authorized = true;
                        cout << "✅ Authorized as " << user << endl;
                        continue;
                    }

                    auto pending_it = pending_responses.find(id);
                    if (pending_it != pending_responses.end()) {
                        const bool accepted = message["result"].isBool() && message["result"].asBool();
                        if (accepted) {
                            g_shares_accepted.fetch_add(1);
                            if (pending_it->second.block_candidate) {
                                g_blocks_found.fetch_add(1);
                                cout << "🧱 BLOCK FOUND: " << pending_it->second.hash_hex.substr(0, 24)
                                     << "..." << endl;
                            } else {
                                cout << "✅ Share accepted" << endl;
                            }
                        } else {
                            g_shares_rejected.fetch_add(1);
                            string reject_reason = "share rejected";
                            if (message.isMember("error") && message["error"].isArray() &&
                                message["error"].size() >= 2 && message["error"][1].isString()) {
                                reject_reason = message["error"][1].asString();
                            }
                            cerr << "❌ " << reject_reason << endl;
                        }
                        pending_responses.erase(pending_it);
                    }
                }
            }

            if (!error.empty()) {
                break;
            }

            vector<PendingShare> ready;
            ready.reserve(64);
            {
                lock_guard<mutex> lock(submit_queue.mu);
                const size_t batch_size = min<size_t>(submit_queue.shares.size(), 64);
                for (size_t i = 0; i < batch_size; ++i) {
                    ready.push_back(std::move(submit_queue.shares.front()));
                    submit_queue.shares.pop_front();
                }
            }

            for (auto& share : ready) {
                if (!active_job || share.job_sequence != active_job->job.sequence) {
                    continue;
                }

                share.user = user;
                share.request_id = next_request_id++;

                Json::Value submit(Json::objectValue);
                submit["id"] = share.request_id;
                submit["method"] = "mining.submit";
                submit["params"] = Json::arrayValue;
                submit["params"].append(share.user);
                submit["params"].append(share.job_id);
                submit["params"].append(share.nonce2);
                submit["params"].append(share.ntime_hex);
                submit["params"].append(share.nonce_hex);

                if (!send_json(connection, submit, error)) {
                    cerr << "⚠️  Failed to submit share: " << error << endl;
                    break;
                }

                pending_responses.emplace(share.request_id,
                                          SubmittedShareMeta{share.hash_hex,
                                                             share.block_candidate,
                                                             share.job_sequence});

                if (share.block_candidate) {
                    cout << "🧱 Block candidate submitted (nonce=" << share.nonce_hex
                         << ", ntime=" << share.ntime_hex << ")" << endl;
                    if (active_job && share.job_sequence == active_job->job.sequence) {
                        active_job->stop.store(true);
                    }
                    {
                        lock_guard<mutex> lock(submit_queue.mu);
                        submit_queue.shares.clear();
                    }
                    break;
                }
            }

            if (!error.empty()) {
                break;
            }
        }

        stop_workers();
        if (g_shutdown.load()) {
            break;
        }
        this_thread::sleep_for(chrono::seconds(2));
    }

    g_shutdown.store(true);
    stop_workers();
    if (stats_thread.joinable()) {
        stats_thread.join();
    }
    curl_global_cleanup();

    cout << "\n⛏️  Stratum worker stopped."
         << " Shares accepted=" << g_shares_accepted.load()
         << " shares rejected=" << g_shares_rejected.load()
         << " blocks=" << g_blocks_found.load() << endl;
    return 0;
}
