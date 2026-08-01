/**
 * @file e2e_test_harness.cpp
 * @brief End-to-End Test Infrastructure Implementation (Phase F.5)
 */

#include "e2e_test_harness.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <curl/curl.h>

namespace fs = std::filesystem;

namespace dinero {
namespace test {

// ═══════════════════════════════════════════════════════════════════════
// Daemon path resolution (issue #428)
// ═══════════════════════════════════════════════════════════════════════

std::string ResolveDinerodPath() {
    const char* env = std::getenv("DINEROD");
    if (env && *env) {
        return env;
    }
    // Manual-run fallback only; CI always sets DINEROD.
    return "./dinerod";
}

bool DinerodAvailable(std::string* resolved_path_out, std::string* error_out) {
    const std::string path = ResolveDinerodPath();
    if (resolved_path_out) {
        *resolved_path_out = path;
    }
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        if (error_out) {
            *error_out = "dinerod not found at \"" + path + "\" (DINEROD=" +
                         (std::getenv("DINEROD") ? std::getenv("DINEROD") : "<unset>") +
                         ", cwd=" + fs::current_path().string() +
                         "). CMake sets DINEROD=$<TARGET_FILE:dinerod> and makes "
                         "dinerod a build dependency of this test target, so this "
                         "is a broken environment, not an absent optional feature.";
        }
        return false;
    }
    if (::access(path.c_str(), X_OK) != 0) {
        if (error_out) {
            *error_out = "dinerod at \"" + path + "\" is not executable";
        }
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
// Helper: cURL write callback
// ═══════════════════════════════════════════════════════════════════════

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// ═══════════════════════════════════════════════════════════════════════
// TestDaemon Implementation
// ═══════════════════════════════════════════════════════════════════════

TestDaemon::TestDaemon()
    : datadir_(""),
      rpc_port_(0),
      p2p_port_(0),
      daemon_pid_(-1)
{
}

TestDaemon::~TestDaemon() {
    stop();
}

void TestDaemon::allocatePorts() {
    // Simple port allocation: use random ports in high range
    // In production, would check for availability
    static int base_port = 20000 + (getpid() % 1000) * 10;
    rpc_port_ = base_port++;
    p2p_port_ = base_port++;
}

bool TestDaemon::start(const std::vector<std::string>& args) {
    // Stop any existing daemon
    if (isRunning()) {
        stop();
    }

    // Allocate ports
    allocatePorts();

    // Create temp datadir
    datadir_ = fs::temp_directory_path() / ("dinero_e2e_test_" + std::to_string(getpid()));
    fs::remove_all(datadir_);
    fs::create_directories(datadir_);

    // Save args for restart
    start_args_ = args;

    // Build command
    std::vector<std::string> cmd_args;
    cmd_args.push_back(ResolveDinerodPath());
    cmd_args.push_back("--regtest");
    cmd_args.push_back("--datadir=" + datadir_);
    cmd_args.push_back("--rpcport=" + std::to_string(rpc_port_));
    cmd_args.push_back("--port=" + std::to_string(p2p_port_));
    // NOTE: Don't use --daemon - test harness already forks and manages the process

    // Add custom args
    for (const auto& arg : args) {
        cmd_args.push_back(arg);
    }

    // Fork and exec
    daemon_pid_ = fork();
    if (daemon_pid_ == 0) {
        // Child process: exec dinerod
        std::vector<char*> c_args;
        for (auto& arg : cmd_args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        // Redirect output to log file for debugging
        std::string log_path = datadir_ + "/daemon.log";
        int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd != -1) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        execvp(c_args[0], c_args.data());
        std::cerr << "Failed to exec dinerod" << std::endl;
        exit(1);
    } else if (daemon_pid_ < 0) {
        std::cerr << "Failed to fork" << std::endl;
        return false;
    }

    // Parent process: wait for daemon to be ready
    return waitForReady();
}

void TestDaemon::stop() {
    if (daemon_pid_ > 0) {
        // Send SIGTERM for graceful shutdown
        kill(daemon_pid_, SIGTERM);

        // Wait for process to exit (with timeout)
        for (int i = 0; i < 30; i++) {
            int status;
            pid_t result = waitpid(daemon_pid_, &status, WNOHANG);
            if (result == daemon_pid_) {
                // Process exited
                daemon_pid_ = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Force kill if still running
        if (daemon_pid_ > 0) {
            kill(daemon_pid_, SIGKILL);
            waitpid(daemon_pid_, nullptr, 0);
            daemon_pid_ = -1;
        }
    }

    // Clean up datadir
    if (!datadir_.empty() && fs::exists(datadir_)) {
        fs::remove_all(datadir_);
    }
}

bool TestDaemon::restart() {
    // Save datadir (don't delete it)
    std::string saved_datadir = datadir_;

    // Stop daemon (but don't delete datadir)
    if (daemon_pid_ > 0) {
        kill(daemon_pid_, SIGTERM);
        for (int i = 0; i < 30; i++) {
            int status;
            pid_t result = waitpid(daemon_pid_, &status, WNOHANG);
            if (result == daemon_pid_) {
                daemon_pid_ = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (daemon_pid_ > 0) {
            kill(daemon_pid_, SIGKILL);
            waitpid(daemon_pid_, nullptr, 0);
            daemon_pid_ = -1;
        }
    }

    // Restore datadir path
    datadir_ = saved_datadir;

    // Restart with same args
    std::vector<std::string> cmd_args;
    cmd_args.push_back(ResolveDinerodPath());
    cmd_args.push_back("--regtest");
    cmd_args.push_back("--datadir=" + datadir_);
    cmd_args.push_back("--rpcport=" + std::to_string(rpc_port_));
    cmd_args.push_back("--port=" + std::to_string(p2p_port_));
    // NOTE: Don't use --daemon - test harness already forks and manages the process

    for (const auto& arg : start_args_) {
        cmd_args.push_back(arg);
    }

    daemon_pid_ = fork();
    if (daemon_pid_ == 0) {
        std::vector<char*> c_args;
        for (auto& arg : cmd_args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        // Redirect output to log file for debugging (append mode for restart)
        std::string log_path = datadir_ + "/daemon.log";
        int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd != -1) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        execvp(c_args[0], c_args.data());
        exit(1);
    } else if (daemon_pid_ < 0) {
        return false;
    }

    return waitForReady();
}

std::string TestDaemon::readCookie() const {
    std::string cookie_path = datadir_ + "/.cookie";
    std::ifstream file(cookie_path);
    if (!file.is_open()) {
        return "";
    }

    std::string line;
    std::getline(file, line);

    // Cookie format: __cookie__:actual_cookie_value
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        return line.substr(colon + 1);
    }

    return line;
}

bool TestDaemon::waitForReady(int timeout_sec) {
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > timeout_sec) {
            return false;
        }

        // Check if process still exists
        if (daemon_pid_ > 0) {
            int status;
            pid_t result = waitpid(daemon_pid_, &status, WNOHANG);
            if (result == daemon_pid_) {
                // Process exited unexpectedly
                return false;
            }
        }

        // Try to read cookie (daemon creates it when RPC is ready)
        std::string cookie = readCookie();
        if (!cookie.empty()) {
            // Cookie exists, try a simple RPC call
            try {
                RpcClient rpc("127.0.0.1", rpc_port_, cookie);
                rpc.getblockcount();
                return true;  // Success!
            } catch (...) {
                // RPC not ready yet, continue waiting
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

bool TestDaemon::isRunning() const {
    if (daemon_pid_ <= 0) {
        return false;
    }

    int status;
    pid_t result = waitpid(daemon_pid_, &status, WNOHANG);
    return result == 0;  // 0 means still running
}

// ═══════════════════════════════════════════════════════════════════════
// RpcClient Implementation
// ═══════════════════════════════════════════════════════════════════════

RpcClient::RpcClient(const std::string& host, int port, const std::string& cookie)
    : host_(host),
      port_(port),
      cookie_(cookie),
      request_id_(1)
{
}

std::string RpcClient::httpPost(const std::string& url, const std::string& data, const std::string& auth) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize cURL");
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (!auth.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth.c_str());
    }

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("cURL request failed: " + std::string(curl_easy_strerror(res)));
    }

    return response;
}

Json::Value RpcClient::call(const std::string& method, const Json::Value& params) {
    // Build request using jsoncpp
    Json::Value request;
    request["jsonrpc"] = "2.0";
    request["method"] = method;
    request["params"] = params;
    request["id"] = request_id_++;

    // Serialize request using FastWriter
    Json::FastWriter writer;
    std::string request_str = writer.write(request);

    std::string url = "http://" + host_ + ":" + std::to_string(port_);
    std::string auth = "__cookie__:" + cookie_;

    std::string response_str = httpPost(url, request_str, auth);

    // Parse response using Reader
    Json::Value response;
    Json::Reader reader;
    if (!reader.parse(response_str, response)) {
        throw std::runtime_error("Failed to parse JSON response: " + reader.getFormattedErrorMessages());
    }

    // Check for RPC error (standard JSON-RPC 2.0 format)
    if (response.isMember("error") && !response["error"].isNull()) {
        Json::FastWriter error_writer;
        std::string error_str = error_writer.write(response["error"]);
        throw std::runtime_error("RPC Error: " + error_str);
    }

    // Check result exists
    if (!response.isMember("result")) {
        return Json::Value();  // Return empty value if no result
    }

    // Also check for errors inside result (non-standard but used by some handlers)
    // Only throw if result.error is an object with "code" (more specific check)
    if (response["result"].isObject() &&
        response["result"].isMember("error") &&
        response["result"]["error"].isObject() &&
        response["result"]["error"].isMember("code")) {
        Json::FastWriter error_writer;
        std::string error_str = error_writer.write(response["result"]["error"]);
        throw std::runtime_error("RPC Error: " + error_str);
    }

    return response["result"];
}

// Mining RPC Methods
Json::Value RpcClient::mining_info() {
    Json::Value params(Json::arrayValue);
    return call("mining.info", params);
}

Json::Value RpcClient::mining_start(int threads, const std::string& address) {
    Json::Value params(Json::arrayValue);
    params.append(threads);
    if (!address.empty()) {
        params.append(address);
    }
    return call("mining.start", params);
}

Json::Value RpcClient::mining_stop() {
    Json::Value params(Json::arrayValue);
    return call("mining.stop", params);
}

Json::Value RpcClient::mining_setaddress(const std::string& address) {
    Json::Value params(Json::arrayValue);
    params.append(address);
    return call("mining.setaddress", params);
}

Json::Value RpcClient::mining_getaddress() {
    Json::Value params(Json::arrayValue);
    return call("mining.getaddress", params);
}

// Wallet RPC Methods
Json::Value RpcClient::wallet_createhd(const std::string& name) {
    Json::Value params(Json::arrayValue);
    params.append(name);
    return call("wallet.createhd", params);
}

Json::Value RpcClient::wallet_load(const std::string& name) {
    Json::Value params(Json::arrayValue);
    params.append(name);
    return call("wallet.load", params);
}

Json::Value RpcClient::wallet_encrypt(const std::string& passphrase) {
    Json::Value params(Json::arrayValue);
    params.append(passphrase);
    return call("wallet.encrypt", params);
}

Json::Value RpcClient::wallet_unlock(const std::string& passphrase, int timeout) {
    Json::Value params(Json::arrayValue);
    params.append(passphrase);
    params.append(timeout);
    return call("wallet.unlock", params);
}

Json::Value RpcClient::wallet_getnewaddress(const std::string& label) {
    Json::Value params(Json::arrayValue);
    if (!label.empty()) {
        params.append(label);
    }
    return call("wallet.getnewaddress", params);
}

Json::Value RpcClient::wallet_getbalance() {
    Json::Value params(Json::arrayValue);
    return call("wallet.getbalance", params);
}

// Chain RPC Methods
Json::Value RpcClient::getblockcount() {
    Json::Value params(Json::arrayValue);
    return call("getblockcount", params);
}

Json::Value RpcClient::getbestblockhash() {
    Json::Value params(Json::arrayValue);
    return call("getbestblockhash", params);
}

Json::Value RpcClient::getblockchaininfo() {
    Json::Value params(Json::arrayValue);
    return call("getblockchaininfo", params);
}

Json::Value RpcClient::generatetoaddress(int nblocks, const std::string& address) {
    Json::Value params(Json::arrayValue);
    params.append(nblocks);
    params.append(address);
    return call("generatetoaddress", params);
}

// ═══════════════════════════════════════════════════════════════════════
// WalletHelper Implementation
// ═══════════════════════════════════════════════════════════════════════

std::string WalletHelper::createWallet(const std::string& name) {
    Json::Value result = rpc_.wallet_createhd(name);
    return result["first_address"].asString();
}

void WalletHelper::encryptWallet(const std::string& passphrase) {
    rpc_.wallet_encrypt(passphrase);
}

void WalletHelper::unlockWallet(const std::string& passphrase, int timeout) {
    rpc_.wallet_unlock(passphrase, timeout);
}

std::string WalletHelper::getNewAddress(const std::string& label) {
    Json::Value result = rpc_.wallet_getnewaddress(label);
    // RPC might return string directly or object with "address" field
    if (result.isString()) {
        return result.asString();
    } else if (result.isMember("address")) {
        return result["address"].asString();
    } else {
        throw std::runtime_error("Unexpected wallet.getnewaddress response format");
    }
}

uint64_t WalletHelper::getBalance() {
    Json::Value result = rpc_.wallet_getbalance();
    return result["confirmed"].asUInt64();
}

// ═══════════════════════════════════════════════════════════════════════
// ChainHelper Implementation
// ═══════════════════════════════════════════════════════════════════════

void ChainHelper::mineBlocks(int count, const std::string& address) {
    rpc_.generatetoaddress(count, address);
}

bool ChainHelper::waitForHeight(int target_height, int timeout_sec) {
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > timeout_sec) {
            return false;
        }

        try {
            int height = getHeight();
            if (height >= target_height) {
                return true;
            }
        } catch (...) {
            // Ignore errors, keep waiting
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int ChainHelper::getHeight() {
    Json::Value result = rpc_.getblockcount();
    return result.asInt();
}

}  // namespace test
}  // namespace dinero
