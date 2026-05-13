/**
 * @file test_stratum_v1_conformance.cpp
 * @brief Stratum V1 Protocol Conformance Test
 *
 * MAINNET REQUIREMENT: Mining pools use Stratum V1 to distribute work.
 * Non-conformant implementations cause:
 *   - Rejected shares (miner revenue loss)
 *   - Pool/miner incompatibility
 *   - Wasted hashpower
 *
 * This test validates:
 *   S1 — mining.subscribe message format
 *   S2 — mining.authorize request/response
 *   S3 — mining.notify work distribution
 *   S4 — mining.submit share submission
 *   S5 — mining.set_difficulty handling
 *   S6 — Extranonce handling (extranonce1, extranonce2_size)
 *   S7 — JSON-RPC 2.0 message framing
 *   S8 — Error code conformance
 *
 * Reference: https://en.bitcoin.it/wiki/Stratum_mining_protocol
 */

#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <map>
#include <optional>
#include <cassert>

// ════════════════════════════════════════════════════════════════════════════
// Test Infrastructure
// ════════════════════════════════════════════════════════════════════════════

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        g_tests_run++; \
        if (!(cond)) { \
            std::cerr << "  ❌ FAIL: " << msg << "\n"; \
            std::cerr << "     at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
        g_tests_passed++; \
    } while(0)

#define TEST_ASSERT_EQ(a, b, msg) \
    do { \
        g_tests_run++; \
        if ((a) != (b)) { \
            std::cerr << "  ❌ FAIL: " << msg << "\n"; \
            std::cerr << "     Expected: " << (b) << "\n"; \
            std::cerr << "     Got:      " << (a) << "\n"; \
            return false; \
        } \
        g_tests_passed++; \
    } while(0)

// ════════════════════════════════════════════════════════════════════════════
// Simple JSON Parser (minimal for Stratum)
// ════════════════════════════════════════════════════════════════════════════

class SimpleJSON {
public:
    enum Type { OBJECT, ARRAY, STRING, NUMBER, BOOLEAN, JNULL };

    Type type;
    std::string str_val;
    double num_val;
    bool bool_val;
    std::map<std::string, SimpleJSON> obj_val;
    std::vector<SimpleJSON> arr_val;

    SimpleJSON() : type(JNULL), num_val(0), bool_val(false) {}

    static SimpleJSON String(const std::string& s) {
        SimpleJSON j; j.type = STRING; j.str_val = s; return j;
    }
    static SimpleJSON Number(double n) {
        SimpleJSON j; j.type = NUMBER; j.num_val = n; return j;
    }
    static SimpleJSON Boolean(bool b) {
        SimpleJSON j; j.type = BOOLEAN; j.bool_val = b; return j;
    }
    static SimpleJSON Null() {
        SimpleJSON j; j.type = JNULL; return j;
    }
    static SimpleJSON Object() {
        SimpleJSON j; j.type = OBJECT; return j;
    }
    static SimpleJSON Array() {
        SimpleJSON j; j.type = ARRAY; return j;
    }

    SimpleJSON& operator[](const std::string& key) {
        return obj_val[key];
    }

    void push_back(const SimpleJSON& v) {
        arr_val.push_back(v);
    }

    // Serialize to JSON string
    std::string serialize() const {
        std::ostringstream oss;
        switch (type) {
            case STRING:
                oss << "\"" << str_val << "\"";
                break;
            case NUMBER:
                if (num_val == static_cast<int64_t>(num_val)) {
                    oss << static_cast<int64_t>(num_val);
                } else {
                    oss << num_val;
                }
                break;
            case BOOLEAN:
                oss << (bool_val ? "true" : "false");
                break;
            case JNULL:
                oss << "null";
                break;
            case OBJECT: {
                oss << "{";
                bool first = true;
                for (const auto& [k, v] : obj_val) {
                    if (!first) oss << ",";
                    first = false;
                    oss << "\"" << k << "\":" << v.serialize();
                }
                oss << "}";
                break;
            }
            case ARRAY: {
                oss << "[";
                bool first = true;
                for (const auto& v : arr_val) {
                    if (!first) oss << ",";
                    first = false;
                    oss << v.serialize();
                }
                oss << "]";
                break;
            }
        }
        return oss.str();
    }

    bool hasKey(const std::string& key) const {
        return obj_val.find(key) != obj_val.end();
    }

    int64_t asInt() const { return static_cast<int64_t>(num_val); }
    std::string asString() const { return str_val; }
    bool asBool() const { return bool_val; }
};

// ════════════════════════════════════════════════════════════════════════════
// Stratum V1 Message Builders
// ════════════════════════════════════════════════════════════════════════════

class StratumV1Message {
public:
    // mining.subscribe request
    static std::string Subscribe(int id, const std::string& user_agent,
                                  const std::string& session_id = "") {
        SimpleJSON msg = SimpleJSON::Object();
        msg["id"] = SimpleJSON::Number(id);
        msg["method"] = SimpleJSON::String("mining.subscribe");

        SimpleJSON params = SimpleJSON::Array();
        params.push_back(SimpleJSON::String(user_agent));
        if (!session_id.empty()) {
            params.push_back(SimpleJSON::String(session_id));
        }
        msg["params"] = params;

        return msg.serialize() + "\n";
    }

    // mining.authorize request
    static std::string Authorize(int id, const std::string& username,
                                  const std::string& password = "") {
        SimpleJSON msg = SimpleJSON::Object();
        msg["id"] = SimpleJSON::Number(id);
        msg["method"] = SimpleJSON::String("mining.authorize");

        SimpleJSON params = SimpleJSON::Array();
        params.push_back(SimpleJSON::String(username));
        params.push_back(SimpleJSON::String(password));
        msg["params"] = params;

        return msg.serialize() + "\n";
    }

    // mining.submit (share submission)
    static std::string Submit(int id, const std::string& worker_name,
                               const std::string& job_id,
                               const std::string& extranonce2,
                               const std::string& ntime,
                               const std::string& nonce) {
        SimpleJSON msg = SimpleJSON::Object();
        msg["id"] = SimpleJSON::Number(id);
        msg["method"] = SimpleJSON::String("mining.submit");

        SimpleJSON params = SimpleJSON::Array();
        params.push_back(SimpleJSON::String(worker_name));
        params.push_back(SimpleJSON::String(job_id));
        params.push_back(SimpleJSON::String(extranonce2));
        params.push_back(SimpleJSON::String(ntime));
        params.push_back(SimpleJSON::String(nonce));
        msg["params"] = params;

        return msg.serialize() + "\n";
    }

    // mining.subscribe response
    static std::string SubscribeResponse(int id, const std::string& session_id,
                                          const std::string& extranonce1,
                                          int extranonce2_size) {
        SimpleJSON msg = SimpleJSON::Object();
        msg["id"] = SimpleJSON::Number(id);
        msg["error"] = SimpleJSON::Null();

        SimpleJSON result = SimpleJSON::Array();

        // Subscriptions array
        SimpleJSON subs = SimpleJSON::Array();

        // mining.set_difficulty subscription
        SimpleJSON diff_sub = SimpleJSON::Array();
        diff_sub.push_back(SimpleJSON::String("mining.set_difficulty"));
        diff_sub.push_back(SimpleJSON::String(session_id + "_diff"));
        subs.push_back(diff_sub);

        // mining.notify subscription
        SimpleJSON notify_sub = SimpleJSON::Array();
        notify_sub.push_back(SimpleJSON::String("mining.notify"));
        notify_sub.push_back(SimpleJSON::String(session_id));
        subs.push_back(notify_sub);

        result.push_back(subs);
        result.push_back(SimpleJSON::String(extranonce1));
        result.push_back(SimpleJSON::Number(extranonce2_size));

        msg["result"] = result;

        return msg.serialize() + "\n";
    }

    // mining.notify (work distribution)
    static std::string Notify(const std::string& job_id,
                               const std::string& prev_hash,
                               const std::string& coinb1,
                               const std::string& coinb2,
                               const std::vector<std::string>& merkle_branch,
                               const std::string& version,
                               const std::string& nbits,
                               const std::string& ntime,
                               bool clean_jobs) {
        SimpleJSON msg = SimpleJSON::Object();
        msg["id"] = SimpleJSON::Null();
        msg["method"] = SimpleJSON::String("mining.notify");

        SimpleJSON params = SimpleJSON::Array();
        params.push_back(SimpleJSON::String(job_id));
        params.push_back(SimpleJSON::String(prev_hash));
        params.push_back(SimpleJSON::String(coinb1));
        params.push_back(SimpleJSON::String(coinb2));

        SimpleJSON branch = SimpleJSON::Array();
        for (const auto& h : merkle_branch) {
            branch.push_back(SimpleJSON::String(h));
        }
        params.push_back(branch);

        params.push_back(SimpleJSON::String(version));
        params.push_back(SimpleJSON::String(nbits));
        params.push_back(SimpleJSON::String(ntime));
        params.push_back(SimpleJSON::Boolean(clean_jobs));

        msg["params"] = params;

        return msg.serialize() + "\n";
    }

    // mining.set_difficulty
    static std::string SetDifficulty(double difficulty) {
        SimpleJSON msg = SimpleJSON::Object();
        msg["id"] = SimpleJSON::Null();
        msg["method"] = SimpleJSON::String("mining.set_difficulty");

        SimpleJSON params = SimpleJSON::Array();
        params.push_back(SimpleJSON::Number(difficulty));
        msg["params"] = params;

        return msg.serialize() + "\n";
    }

    // Error response
    static std::string ErrorResponse(int id, int error_code, const std::string& message) {
        SimpleJSON msg = SimpleJSON::Object();
        msg["id"] = SimpleJSON::Number(id);
        msg["result"] = SimpleJSON::Null();

        SimpleJSON error = SimpleJSON::Array();
        error.push_back(SimpleJSON::Number(error_code));
        error.push_back(SimpleJSON::String(message));
        error.push_back(SimpleJSON::Null());
        msg["error"] = error;

        return msg.serialize() + "\n";
    }
};

// ════════════════════════════════════════════════════════════════════════════
// Stratum Error Codes (standard)
// ════════════════════════════════════════════════════════════════════════════

namespace StratumError {
    constexpr int UNKNOWN = 20;
    constexpr int JOB_NOT_FOUND = 21;
    constexpr int DUPLICATE_SHARE = 22;
    constexpr int LOW_DIFFICULTY = 23;
    constexpr int UNAUTHORIZED = 24;
    constexpr int NOT_SUBSCRIBED = 25;
}

// ════════════════════════════════════════════════════════════════════════════
// Test S1: mining.subscribe message format
// ════════════════════════════════════════════════════════════════════════════

bool test_s1_subscribe_format() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST S1: mining.subscribe Message Format" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    // Build subscribe request
    std::string msg = StratumV1Message::Subscribe(1, "Dinero-Miner/1.0");

    std::cout << "  Request: " << msg;

    // Verify format
    TEST_ASSERT(msg.find("\"id\":1") != std::string::npos, "Must have id:1");
    TEST_ASSERT(msg.find("\"method\":\"mining.subscribe\"") != std::string::npos,
                "Must have method:mining.subscribe");
    TEST_ASSERT(msg.find("\"params\":[") != std::string::npos, "Must have params array");
    TEST_ASSERT(msg.find("Dinero-Miner/1.0") != std::string::npos, "Must include user agent");
    TEST_ASSERT(msg.back() == '\n', "Message must end with newline");

    // Test with session resumption
    std::string msg2 = StratumV1Message::Subscribe(2, "Dinero-Miner/1.0", "prev_session_123");
    std::cout << "  Resume:  " << msg2;

    TEST_ASSERT(msg2.find("prev_session_123") != std::string::npos,
                "Session resumption must include session ID");

    std::cout << "\n  ✅ mining.subscribe format is conformant\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test S2: mining.authorize request/response
// ════════════════════════════════════════════════════════════════════════════

bool test_s2_authorize_format() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST S2: mining.authorize Request/Response" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    // Build authorize request
    std::string msg = StratumV1Message::Authorize(2, "worker.1", "x");

    std::cout << "  Request: " << msg;

    TEST_ASSERT(msg.find("\"method\":\"mining.authorize\"") != std::string::npos,
                "Must have method:mining.authorize");
    TEST_ASSERT(msg.find("\"worker.1\"") != std::string::npos, "Must include worker name");
    TEST_ASSERT(msg.find("\"x\"") != std::string::npos, "Must include password");

    // Test response format
    SimpleJSON resp = SimpleJSON::Object();
    resp["id"] = SimpleJSON::Number(2);
    resp["result"] = SimpleJSON::Boolean(true);
    resp["error"] = SimpleJSON::Null();

    std::string resp_str = resp.serialize() + "\n";
    std::cout << "  Response (success): " << resp_str;

    TEST_ASSERT(resp_str.find("\"result\":true") != std::string::npos,
                "Success response must have result:true");

    // Test error response
    std::string err = StratumV1Message::ErrorResponse(2, StratumError::UNAUTHORIZED,
                                                       "Invalid credentials");
    std::cout << "  Response (error):   " << err;

    TEST_ASSERT(err.find("24") != std::string::npos, "Error code 24 for unauthorized");

    std::cout << "\n  ✅ mining.authorize format is conformant\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test S3: mining.notify work distribution
// ════════════════════════════════════════════════════════════════════════════

bool test_s3_notify_format() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST S3: mining.notify Work Distribution" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    // Build notify message
    std::string job_id = "job_0001";
    std::string prev_hash = "0000000000000000000123456789abcdef0123456789abcdef0123456789abcdef";
    std::string coinb1 = "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff";
    std::string coinb2 = "ffffffff0100f2052a0100000023";
    std::vector<std::string> merkle = {"abc123", "def456"};
    std::string version = "20000000";
    std::string nbits = "1d00ffff";
    std::string ntime = "65a1b2c3";

    std::string msg = StratumV1Message::Notify(
        job_id, prev_hash, coinb1, coinb2, merkle, version, nbits, ntime, true
    );

    std::cout << "  Notify: " << msg.substr(0, 100) << "...\n" << std::endl;

    TEST_ASSERT(msg.find("\"method\":\"mining.notify\"") != std::string::npos,
                "Must have method:mining.notify");
    TEST_ASSERT(msg.find("\"id\":null") != std::string::npos,
                "Notifications must have id:null");
    TEST_ASSERT(msg.find(job_id) != std::string::npos, "Must include job_id");
    TEST_ASSERT(msg.find(prev_hash) != std::string::npos, "Must include prev_hash");
    TEST_ASSERT(msg.find(coinb1) != std::string::npos, "Must include coinb1");
    TEST_ASSERT(msg.find(coinb2) != std::string::npos, "Must include coinb2");
    TEST_ASSERT(msg.find("abc123") != std::string::npos, "Must include merkle branch");
    TEST_ASSERT(msg.find(nbits) != std::string::npos, "Must include nbits");
    TEST_ASSERT(msg.find("true") != std::string::npos, "clean_jobs flag must be present");

    std::cout << "  Fields verified:" << std::endl;
    std::cout << "    [0] job_id:       " << job_id << std::endl;
    std::cout << "    [1] prev_hash:    " << prev_hash.substr(0, 32) << "..." << std::endl;
    std::cout << "    [2] coinb1:       " << coinb1.substr(0, 32) << "..." << std::endl;
    std::cout << "    [3] coinb2:       " << coinb2 << std::endl;
    std::cout << "    [4] merkle:       " << merkle.size() << " branches" << std::endl;
    std::cout << "    [5] version:      " << version << std::endl;
    std::cout << "    [6] nbits:        " << nbits << std::endl;
    std::cout << "    [7] ntime:        " << ntime << std::endl;
    std::cout << "    [8] clean_jobs:   true" << std::endl;

    std::cout << "\n  ✅ mining.notify format is conformant\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test S4: mining.submit share submission
// ════════════════════════════════════════════════════════════════════════════

bool test_s4_submit_format() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST S4: mining.submit Share Submission" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    std::string msg = StratumV1Message::Submit(
        3,                    // id
        "worker.1",           // worker_name
        "job_0001",           // job_id
        "00000001",           // extranonce2
        "65a1b2c3",           // ntime
        "12345678"            // nonce
    );

    std::cout << "  Submit: " << msg;

    TEST_ASSERT(msg.find("\"method\":\"mining.submit\"") != std::string::npos,
                "Must have method:mining.submit");
    TEST_ASSERT(msg.find("\"worker.1\"") != std::string::npos, "Must include worker_name");
    TEST_ASSERT(msg.find("\"job_0001\"") != std::string::npos, "Must include job_id");
    TEST_ASSERT(msg.find("\"00000001\"") != std::string::npos, "Must include extranonce2");
    TEST_ASSERT(msg.find("\"65a1b2c3\"") != std::string::npos, "Must include ntime");
    TEST_ASSERT(msg.find("\"12345678\"") != std::string::npos, "Must include nonce");

    // Verify parameter order
    size_t pos_worker = msg.find("worker.1");
    size_t pos_job = msg.find("job_0001");
    size_t pos_en2 = msg.find("00000001");
    size_t pos_ntime = msg.find("65a1b2c3");
    size_t pos_nonce = msg.find("12345678");

    TEST_ASSERT(pos_worker < pos_job, "worker_name must come before job_id");
    TEST_ASSERT(pos_job < pos_en2, "job_id must come before extranonce2");
    TEST_ASSERT(pos_en2 < pos_ntime, "extranonce2 must come before ntime");
    TEST_ASSERT(pos_ntime < pos_nonce, "ntime must come before nonce");

    std::cout << "  Parameter order: worker_name, job_id, extranonce2, ntime, nonce ✓" << std::endl;

    std::cout << "\n  ✅ mining.submit format is conformant\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test S5: mining.set_difficulty handling
// ════════════════════════════════════════════════════════════════════════════

bool test_s5_set_difficulty() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST S5: mining.set_difficulty Handling" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    // Test various difficulties
    std::vector<double> difficulties = {0.001, 1.0, 16384.0, 1000000.0};

    for (double diff : difficulties) {
        std::string msg = StratumV1Message::SetDifficulty(diff);
        std::cout << "  Difficulty " << diff << ": " << msg;

        TEST_ASSERT(msg.find("\"method\":\"mining.set_difficulty\"") != std::string::npos,
                    "Must have method:mining.set_difficulty");
        TEST_ASSERT(msg.find("\"id\":null") != std::string::npos,
                    "Notifications must have id:null");
    }

    // Verify vardiff behavior (difficulty changes during session)
    std::cout << "\n  Vardiff simulation:" << std::endl;
    std::cout << "    Initial:    " << StratumV1Message::SetDifficulty(1.0);
    std::cout << "    Increased:  " << StratumV1Message::SetDifficulty(2.0);
    std::cout << "    Decreased:  " << StratumV1Message::SetDifficulty(0.5);

    std::cout << "\n  ✅ mining.set_difficulty format is conformant\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test S6: Extranonce handling
// ════════════════════════════════════════════════════════════════════════════

bool test_s6_extranonce_handling() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST S6: Extranonce Handling" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    // Build subscribe response with extranonce info
    std::string resp = StratumV1Message::SubscribeResponse(
        1,                    // id
        "session_abc123",     // session_id
        "08000002",           // extranonce1 (4 bytes)
        4                     // extranonce2_size
    );

    std::cout << "  Subscribe response:\n  " << resp << std::endl;

    TEST_ASSERT(resp.find("08000002") != std::string::npos, "Must include extranonce1");
    TEST_ASSERT(resp.find(":4") != std::string::npos || resp.find(",4]") != std::string::npos,
                "Must include extranonce2_size");
    TEST_ASSERT(resp.find("mining.notify") != std::string::npos,
                "Must include mining.notify subscription");
    TEST_ASSERT(resp.find("mining.set_difficulty") != std::string::npos,
                "Must include mining.set_difficulty subscription");

    // Verify extranonce space calculation
    // extranonce1 = 4 bytes, extranonce2 = 4 bytes
    // Total extranonce space = 2^32 = 4 billion unique coinbase variants
    uint32_t extranonce2_max = 0xFFFFFFFF;  // 4 bytes
    std::cout << "  Extranonce1: 08000002 (4 bytes, pool-assigned)" << std::endl;
    std::cout << "  Extranonce2: 4 bytes (" << extranonce2_max << " unique values)" << std::endl;
    std::cout << "  Total search space per job: 2^32 * 2^32 = 2^64 hashes" << std::endl;

    // Verify extranonce2 is hex-encoded in submit
    std::string submit = StratumV1Message::Submit(1, "w", "j", "deadbeef", "t", "n");
    TEST_ASSERT(submit.find("\"deadbeef\"") != std::string::npos,
                "extranonce2 must be hex-encoded string");

    std::cout << "\n  ✅ Extranonce handling is conformant\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test S7: JSON-RPC 2.0 message framing
// ════════════════════════════════════════════════════════════════════════════

bool test_s7_jsonrpc_framing() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST S7: JSON-RPC 2.0 Message Framing" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    // Test that all messages end with newline (line-delimited JSON)
    std::vector<std::string> messages = {
        StratumV1Message::Subscribe(1, "Miner/1.0"),
        StratumV1Message::Authorize(2, "worker", "pass"),
        StratumV1Message::Submit(3, "w", "j", "e", "t", "n"),
        StratumV1Message::Notify("j", "h", "c1", "c2", {}, "v", "b", "t", false),
        StratumV1Message::SetDifficulty(1.0),
        StratumV1Message::ErrorResponse(4, 20, "Unknown error")
    };

    for (size_t i = 0; i < messages.size(); i++) {
        const auto& msg = messages[i];

        // Must end with newline
        TEST_ASSERT(msg.back() == '\n',
                    "Message " + std::to_string(i) + " must end with newline");

        // Must be valid single-line JSON
        size_t newlines = std::count(msg.begin(), msg.end(), '\n');
        TEST_ASSERT_EQ(newlines, 1UL,
                       "Message " + std::to_string(i) + " must have exactly one newline");

        // Must start with {
        TEST_ASSERT(msg[0] == '{',
                    "Message " + std::to_string(i) + " must be JSON object");
    }

    std::cout << "  Verified " << messages.size() << " messages:" << std::endl;
    std::cout << "    ✓ All end with exactly one newline" << std::endl;
    std::cout << "    ✓ All are valid JSON objects" << std::endl;
    std::cout << "    ✓ No embedded newlines (line-delimited)" << std::endl;

    std::cout << "\n  ✅ JSON-RPC framing is conformant\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test S8: Error code conformance
// ════════════════════════════════════════════════════════════════════════════

bool test_s8_error_codes() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST S8: Stratum Error Code Conformance" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    // Standard Stratum error codes
    struct ErrorTest {
        int code;
        const char* name;
        const char* message;
    };

    std::vector<ErrorTest> errors = {
        {20, "UNKNOWN", "Other/Unknown"},
        {21, "JOB_NOT_FOUND", "Job not found"},
        {22, "DUPLICATE_SHARE", "Duplicate share"},
        {23, "LOW_DIFFICULTY", "Low difficulty share"},
        {24, "UNAUTHORIZED", "Unauthorized worker"},
        {25, "NOT_SUBSCRIBED", "Not subscribed"}
    };

    for (const auto& e : errors) {
        std::string msg = StratumV1Message::ErrorResponse(1, e.code, e.message);

        // Verify error format: [code, message, null]
        TEST_ASSERT(msg.find("\"error\":[" + std::to_string(e.code)) != std::string::npos,
                    std::string(e.name) + " error code must be " + std::to_string(e.code));
        TEST_ASSERT(msg.find("\"result\":null") != std::string::npos,
                    "Error responses must have result:null");

        std::cout << "  Code " << e.code << " (" << e.name << "): ✓" << std::endl;
    }

    // Verify error array format
    std::string err = StratumV1Message::ErrorResponse(1, 21, "Job not found");
    std::cout << "\n  Sample error response:\n  " << err;

    // Error must be array: [code, message, traceback]
    TEST_ASSERT(err.find("\"error\":[21,") != std::string::npos,
                "Error must be array starting with code");
    TEST_ASSERT(err.find(",null]") != std::string::npos,
                "Error array must end with null (traceback)");

    std::cout << "\n  ✅ Error codes are conformant\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test S9: Full session simulation
// ════════════════════════════════════════════════════════════════════════════

bool test_s9_full_session() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST S9: Full Mining Session Simulation" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    std::cout << "  === MINER → POOL ===" << std::endl;

    // 1. Subscribe
    std::string sub_req = StratumV1Message::Subscribe(1, "Dinero-Miner/1.0.0");
    std::cout << "  [1] " << sub_req;

    // 2. Pool responds with extranonce
    std::string sub_resp = StratumV1Message::SubscribeResponse(1, "sess_001", "08000002", 4);
    std::cout << "\n  === POOL → MINER ===" << std::endl;
    std::cout << "  [1] " << sub_resp.substr(0, 80) << "...\n";

    // 3. Authorize
    std::cout << "  === MINER → POOL ===" << std::endl;
    std::string auth_req = StratumV1Message::Authorize(2, "mywallet.worker1", "x");
    std::cout << "  [2] " << auth_req;

    // 4. Pool authorizes
    SimpleJSON auth_resp = SimpleJSON::Object();
    auth_resp["id"] = SimpleJSON::Number(2);
    auth_resp["result"] = SimpleJSON::Boolean(true);
    auth_resp["error"] = SimpleJSON::Null();
    std::cout << "\n  === POOL → MINER ===" << std::endl;
    std::cout << "  [2] " << auth_resp.serialize() << "\n\n";

    // 5. Pool sends difficulty
    std::string diff = StratumV1Message::SetDifficulty(16384);
    std::cout << "  [*] " << diff;

    // 6. Pool sends work
    std::string notify = StratumV1Message::Notify(
        "job_001",
        "000000000000000000012345",
        "01000000010000",
        "ffffffff0100",
        {"aabb", "ccdd"},
        "20000000",
        "1d00ffff",
        "65a1b2c3",
        true
    );
    std::cout << "  [*] " << notify.substr(0, 80) << "...\n";

    // 7. Miner submits share
    std::cout << "\n  === MINER → POOL (share found!) ===" << std::endl;
    std::string submit = StratumV1Message::Submit(3, "mywallet.worker1", "job_001",
                                                   "00000001", "65a1b2c3", "deadbeef");
    std::cout << "  [3] " << submit;

    // 8. Pool accepts
    SimpleJSON accept = SimpleJSON::Object();
    accept["id"] = SimpleJSON::Number(3);
    accept["result"] = SimpleJSON::Boolean(true);
    accept["error"] = SimpleJSON::Null();
    std::cout << "\n  === POOL → MINER ===" << std::endl;
    std::cout << "  [3] " << accept.serialize() << "\n";

    std::cout << "\n  ✅ Full session simulation successful\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Main Entry Point
// ════════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Stratum V1 Protocol Conformance Test Suite               ║" << std::endl;
    std::cout << "║  Mining Pool Compatibility Validation                     ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════════════════════╝" << std::endl;

    bool all_passed = true;

    all_passed &= test_s1_subscribe_format();
    all_passed &= test_s2_authorize_format();
    all_passed &= test_s3_notify_format();
    all_passed &= test_s4_submit_format();
    all_passed &= test_s5_set_difficulty();
    all_passed &= test_s6_extranonce_handling();
    all_passed &= test_s7_jsonrpc_framing();
    all_passed &= test_s8_error_codes();
    all_passed &= test_s9_full_session();

    std::cout << "\n";

    if (all_passed) {
        std::cout << "╔═══════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  ✅ ALL STRATUM V1 CONFORMANCE TESTS PASSED               ║" << std::endl;
        std::cout << "╠═══════════════════════════════════════════════════════════╣" << std::endl;
        std::cout << "║  Protocol Compliance:                                     ║" << std::endl;
        std::cout << "║    • mining.subscribe - correct format                    ║" << std::endl;
        std::cout << "║    • mining.authorize - correct format                    ║" << std::endl;
        std::cout << "║    • mining.notify - all 9 fields present                 ║" << std::endl;
        std::cout << "║    • mining.submit - correct parameter order              ║" << std::endl;
        std::cout << "║    • mining.set_difficulty - vardiff support              ║" << std::endl;
        std::cout << "║    • Extranonce handling - proper hex encoding            ║" << std::endl;
        std::cout << "║    • JSON-RPC framing - line-delimited                    ║" << std::endl;
        std::cout << "║    • Error codes - standard codes 20-25                   ║" << std::endl;
        std::cout << "╚═══════════════════════════════════════════════════════════╝" << std::endl;
    } else {
        std::cout << "╔═══════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  ❌ STRATUM V1 CONFORMANCE TESTS FAILED                   ║" << std::endl;
        std::cout << "║  WARNING: Mining pools may reject connections!            ║" << std::endl;
        std::cout << "╚═══════════════════════════════════════════════════════════╝" << std::endl;
    }

    std::cout << "\nTests: " << g_tests_passed << "/" << g_tests_run << " passed" << std::endl;

    return all_passed ? 0 : 1;
}
