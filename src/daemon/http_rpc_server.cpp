#include "http_rpc_server.h"
#include "rpc_auth.h"
#include "version_config.h"
#include "daemon/rpc_utils.h"
#include "rpc/rpc_registry.h"
#include "metrics/metrics_registry.h"  // Week 5: Metrics export endpoint
#include "daemon/daemon_context.h"     // Phase 3A: Full definition needed for wallet access
#include "daemon/services/wallet_service.h"  // Phase 3A: WalletService definition
#include "daemon/services/mempool_service.h" // STEP 3.1: MempoolService for stats
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <vector>
#include <cctype>
#include <limits>
#include <unordered_set>
#include <cstdlib>  // std::getenv — DINERO_RPC_DEBUG gate (issue #538)

// Phase 2A/3A: We get mempool/utxo/wallet_manager from DaemonContext instead of globals

// Platform-specific includes
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
#endif
#include <cerrno>
#include <cstring>

#include "compat/net_compat.h"  // compat_set_cloexec (#295)

// Admin-only RPC methods that can corrupt state or expose secrets.
// These are rejected when the server runs in read-only mode (--rpc-readonly).
static const std::unordered_set<std::string> ADMIN_METHODS = {
    "stop",
    "wallet.importprivkey",
    "wallet.importmnemonic",
    "wallet.exportmnemonic",
    "wallet.rescanblockchain",
    "wallet.abortrescan",
    "wallet.backup",
    "wallet.sendtoaddress",
    "wallet.sendmany",
    "wallet.consolidate",
    "wallet.shield",
    "wallet.unshield",
    "wallet.transfer",
    "wallet.createfundedpsbt",
    "wallet.processpsbt",
    "wallet.signpsbt",
    "wallet.signrawtransaction",
    "wallet.signp2mr",
    "wallet.signcommitment",
    "wallet.fundchannel",
    "wallet.sendrawtransaction",
    "sendrawtransaction",
    "wallet.recordsend",
    "mining.submitblock",
    "submitblock",
    "invalidateblock",
    "reconsiderblock",
    "blockchain.invalidateblock",
    "blockchain.reconsiderblock",
    "blockchain.submitblock",
    "blockchain.loadtxoutset",
    "blockchain.resetassumeutxofatal",
    "descriptor.import",
    "descriptor.setactive",
    "hwallet.signpsbt",
    "hwallet.importpsbtfromfile",
    "ln.sendpayment",
    "pool.authorizeworker",
    "pool.submitshare",
    "pool.disconnectworker",
    "pool.setconfig",
    "pool.processpayouts",
    // Vault (liquidity custody) — mutating methods move funds / credit the ledger.
    // Must NOT be callable by read-only RPC clients (security fix 2026-05-29, F-CRIT-03).
    "vault.observe",
    "vault.withdraw",
    "vault.processnext",
    "vault.setoperator",
    // Contract registry and spending methods. v8.1.9 keeps spending disabled,
    // but these remain admin-classified so future registration cannot silently
    // expose them to read-only credentials.
    "contract.createescrow",
    "contract.setlocktx",
    "contract.release",
    "contract.refund",
    "contract.broadcastrelease",
    "contract.broadcastrefund",
    "multiasset.createescrow",
    "multiasset.releaseescrow",
    "multiasset.refundescrow",
    // Seeder process lifecycle — fork/execv a child process. Must NOT be callable
    // by read-only RPC clients (security fix: seeder.start RCE vector).
    "seeder.start",
    "seeder.stop",
    // Changes externally reachable service state and persists an operator
    // preference. Read-only RPC credentials must never invoke it.
    "network.setonionservice",
    "network.setrelayservice",
};

static const std::unordered_set<std::string> DEV_ONLY_METHODS = {
    "debug.computesighash",
    "debug.attachwitness",
    "blockchain.debugclearundoflag",
};

static bool MatchesCanonicalOrFlatAlias(
    const std::unordered_set<std::string>& methods, const std::string& method) {
    if (methods.count(method) > 0) return true;
    for (const auto& canonical : methods) {
        const auto dot = canonical.find('.');
        if (dot != std::string::npos && canonical.substr(dot + 1) == method) return true;
    }
    return false;
}

bool HttpRpcServer::isAdminMethod(const std::string& method) {
    // RpcRegistry creates flat aliases directly in handlers_ rather than in its
    // explicit aliases_ table. Therefore getAliasInfo() cannot canonicalize
    // aliases such as `withdraw`, `sendtoaddress`, or `transfer`. Derive every
    // generated flat spelling here so aliases inherit the canonical policy.
    return MatchesCanonicalOrFlatAlias(ADMIN_METHODS, method);
}

namespace {

bool ParseContentLengthValue(const std::string& raw_value, size_t& out_value) {
    out_value = 0;
    if (raw_value.empty()) {
        return false;
    }
    for (char ch : raw_value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!std::isdigit(uch)) {
            return false;
        }
        const size_t digit = static_cast<size_t>(uch - '0');
        if (out_value > (std::numeric_limits<size_t>::max() - digit) / 10) {
            return false;
        }
        out_value = out_value * 10 + digit;
    }
    return true;
}

bool TryParseContentLength(const std::string& request, size_t& content_length) {
    auto cl_pos = request.find("Content-Length:");
    if (cl_pos == std::string::npos) {
        cl_pos = request.find("content-length:");
    }
    if (cl_pos == std::string::npos) {
        content_length = 0;
        return true;
    }

    size_t cl_val_start = cl_pos + 15;  // strlen("Content-Length:")
    while (cl_val_start < request.size() &&
           (request[cl_val_start] == ' ' || request[cl_val_start] == '\t')) {
        ++cl_val_start;
    }

    const auto cl_val_end = request.find("\r\n", cl_val_start);
    if (cl_val_end == std::string::npos || cl_val_end == cl_val_start) {
        return false;
    }

    const std::string raw_value = request.substr(cl_val_start, cl_val_end - cl_val_start);
    return ParseContentLengthValue(raw_value, content_length);
}

bool SendAll(int socket_fd, const char* data, size_t length) {
    size_t total_sent = 0;
    while (total_sent < length) {
#ifdef _WIN32
        const int to_send = static_cast<int>(
            std::min(length - total_sent, static_cast<size_t>(std::numeric_limits<int>::max())));
        const int sent = send(socket_fd, data + total_sent, to_send, 0);
#else
        const ssize_t sent = send(socket_fd, data + total_sent, length - total_sent, 0);
#endif
        if (sent <= 0) {
            return false;
        }
        total_sent += static_cast<size_t>(sent);
    }
    return true;
}

void ShutdownSend(int socket_fd) {
#ifdef _WIN32
    shutdown(socket_fd, SD_SEND);
#else
    shutdown(socket_fd, SHUT_WR);
#endif
}

}  // namespace

HttpRpcServer::HttpRpcServer(const std::string& bind_address, uint16_t port)
    : bind_address_(bind_address), port_(port) {
    register_builtin_methods();
}

HttpRpcServer::~HttpRpcServer() {
    stop();
}

void HttpRpcServer::start() {
    if (running_) {
        std::cout << "RPC server already running" << std::endl;
        return;
    }
    
    shutdown_requested_ = false;
    server_thread_ = std::make_unique<std::thread>(&HttpRpcServer::server_loop, this);
    
    // Wait a bit for server to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "HTTP RPC server started on " << bind_address_ << ":" << port_ << std::endl;

    // Security warning: RPC has no TLS — credentials sent as cleartext Base64
    if (bind_address_ == "0.0.0.0" || bind_address_ == "*") {
        std::cerr << "WARNING: RPC server bound to all interfaces (" << bind_address_
                  << "). RPC uses HTTP Basic Auth without TLS — credentials are NOT encrypted. "
                  << "The daemon does NOT enforce a source-network allowlist. "
                  << "Bind to localhost or protect this port with a firewall / SSH tunnel."
                  << std::endl;
    }
}

void HttpRpcServer::stop() {
    const bool had_server_thread = static_cast<bool>(server_thread_);
    const bool was_running = running_.load();
    shutdown_requested_ = true;

    if (server_thread_ && server_thread_->joinable()) {
        server_thread_->join();
    }
    server_thread_.reset();

    if (!had_server_thread && !was_running) {
        return;
    }

    // Detached connection handlers capture `this`. Wait briefly for them to
    // drain before service teardown to avoid use-after-free during shutdown.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (active_connections_.load(std::memory_order_relaxed) > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    const auto remaining = active_connections_.load(std::memory_order_relaxed);
    const auto active_handlers = active_rpc_handlers_.load(std::memory_order_relaxed);
    if (remaining > 0) {
        std::cerr << "RPC server stop timeout with " << remaining
                  << " active connection(s) and " << active_handlers
                  << " active RPC handler(s); continuing shutdown" << std::endl;
    }
    
    running_ = false;
    std::cout << "HTTP RPC server stopped" << std::endl;
}

void HttpRpcServer::register_method(const std::string& method, RpcHandler handler) {
    methods_[method] = handler;
}

std::vector<std::string> HttpRpcServer::get_registered_methods() const {
    std::vector<std::string> methods;
    methods.reserve(methods_.size());
    for (const auto& pair : methods_) {
        methods.push_back(pair.first);
    }
    std::sort(methods.begin(), methods.end());
    return methods;
}

void HttpRpcServer::register_builtin_methods() {
    register_method("getinfo", [this](const Json::Value& params) {
        return handle_getinfo(params);
    });
    
    register_method("help", [this](const Json::Value& params) {
        return handle_help(params);
    });
    
    register_method("stop", [this](const Json::Value& params) {
        return handle_stop(params);
    });
    
    register_method("listtransactions", [this](const Json::Value& params) {
        // Return empty array - use wallet-specific RPC methods for real transaction data
        Json::Value result(Json::arrayValue);
        return result;
    });
}

void HttpRpcServer::server_loop() {
    int server_socket = create_server_socket();
    if (server_socket < 0) {
        std::cerr << "Failed to create server socket" << std::endl;
        return;
    }
    
    running_ = true;
    
    while (!shutdown_requested_) {
        // Accept connections with timeout. Use poll() rather than select():
        // select() cannot handle a file descriptor >= FD_SETSIZE (1024), and a
        // daemon with a large chaindb / many wallets can push the listen socket
        // past that, making select() fail immediately ("Select error") so RPC
        // never serves. poll() has no such limit. (Pairs with the bounded
        // chaindb max_open_files; either alone fixes the symptom.)
#ifndef _WIN32
        struct pollfd pfd;
        pfd.fd = server_socket;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int activity = poll(&pfd, 1, 1000);  // 1000 ms timeout

        if (activity < 0) {
            if (errno == EINTR) continue;
            if (!shutdown_requested_) {
                std::cerr << "poll() error in RPC server: " << std::strerror(errno) << std::endl;
            }
            break;
        }
        if (activity == 0) {
            continue;  // timeout — re-check shutdown flag
        }
        if (pfd.revents & POLLIN) {
#else
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        int activity = select(server_socket + 1, &read_fds, nullptr, nullptr, &timeout);
        if (activity < 0) {
            if (!shutdown_requested_) {
                std::cerr << "Select error in RPC server" << std::endl;
            }
            break;
        }
        if (activity == 0) {
            continue;
        }
        if (FD_ISSET(server_socket, &read_fds)) {
#endif
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            
            int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
            if (client_socket >= 0) {
                configure_client_socket(client_socket);

                // Security note: RPC uses HTTP Basic Auth without TLS.
                // External connections are allowed (DineroDPI needs them) but
                // authentication is still required — cookie auth gates all
                // endpoints except /serverinfo.

                if (active_connections_.load(std::memory_order_relaxed) >= kMaxConcurrentConnections) {
                    // Well-formed JSON-RPC error (error is an OBJECT, not a bare
                    // string) so strict clients don't choke. id is null: the
                    // request is rejected before it is parsed.
                    const std::string response = build_http_response(
                        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32000,"
                        "\"message\":\"RPC server busy: too many open connections\"}}",
                        "application/json",
                        503);
                    SendAll(client_socket, response.c_str(), response.size());
                    close_socket(client_socket);
                    continue;
                }

                // RPC rate limiting: per-IP token bucket (50 req/sec default)
                {
                    char rate_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, rate_ip, sizeof(rate_ip));
                    if (!checkRpcRate(std::string(rate_ip))) {
                        // Well-formed JSON-RPC error (error is an OBJECT). The old
                        // reply was a bare-string error with a hand-counted
                        // Content-Length, which made strict clients (incl.
                        // dinero-cli) abort instead of surfacing the limit.
                        const std::string response = build_http_response(
                            "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32000,"
                            "\"message\":\"Rate limit exceeded. Try again shortly.\"}}",
                            "application/json",
                            429);
                        SendAll(client_socket, response.c_str(), response.size());
                        close_socket(client_socket);
                        continue;
                    }
                }

                active_connections_.fetch_add(1, std::memory_order_relaxed);
                try {
                    std::thread([this, client_socket]() {
                        try {
                            handle_connection(client_socket);
                        } catch (const std::exception& e) {
                            std::cerr << "Unhandled RPC connection error: " << e.what() << std::endl;
                        } catch (...) {
                            std::cerr << "Unhandled RPC connection error: unknown exception" << std::endl;
                        }
                        close_socket(client_socket);
                        active_connections_.fetch_sub(1, std::memory_order_relaxed);
                    }).detach();
                } catch (...) {
                    active_connections_.fetch_sub(1, std::memory_order_relaxed);
                    close_socket(client_socket);
                }
            }
        }
    }
    
    close_socket(server_socket);
    running_ = false;
}

void HttpRpcServer::handle_connection(int client_socket) {
    // Read headers first (up to 8KB should be plenty for HTTP headers)
    char header_buf[8192];
    int bytes_received = recv(client_socket, header_buf, sizeof(header_buf) - 1, 0);

    if (bytes_received <= 0) return;

    header_buf[bytes_received] = '\0';
    std::string request(header_buf, bytes_received);

    // Check if we need to read more body data based on Content-Length
    auto header_end = request.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        constexpr size_t kMaxTotalRead = 2 * 1024 * 1024;
        size_t content_length = 0;
        if (!TryParseContentLength(request, content_length)) {
            const std::string response = build_http_response(
                "{\"error\":\"Invalid Content-Length header\"}",
                "application/json",
                400);
            SendAll(client_socket, response.c_str(), response.length());
            return;
        }

        if (content_length > kMaxTotalRead) {
            const std::string response = build_http_response(
                "{\"error\":\"Request body too large\"}",
                "application/json",
                413);
            SendAll(client_socket, response.c_str(), response.length());
            return;
        }

        // How much body we already have vs how much we need
        size_t body_start = header_end + 4;
        size_t body_received = request.size() - body_start;
        size_t body_remaining = (content_length > body_received) ? content_length - body_received : 0;

        // Read remaining body in a loop
        while (body_remaining > 0) {
            char chunk[8192];
            size_t to_read = std::min(body_remaining, sizeof(chunk));
            int n = recv(client_socket, chunk, to_read, 0);
            if (n <= 0) break;
            request.append(chunk, n);
            body_remaining -= n;
        }
    }

    std::string response = process_http_request(request);
    if (SendAll(client_socket, response.c_str(), response.length())) {
        ShutdownSend(client_socket);
    }
}

std::string HttpRpcServer::process_http_request(const std::string& request) {
    // Special handling for GET /serverinfo (no auth required - for autodiscovery)
    if (request.find("GET /serverinfo") == 0) {
        // Read serverinfo.json from g_data_dir global
        extern std::string g_data_dir;
        std::string serverinfo_path = g_data_dir + "/serverinfo.json";

        std::ifstream file(serverinfo_path);
        if (!file.is_open()) {
            return build_http_response("{\"error\":\"serverinfo.json not found\"}", "application/json", 404);
        }

        std::string serverinfo_content((std::istreambuf_iterator<char>(file)),
                                       std::istreambuf_iterator<char>());
        file.close();

        return build_http_response(serverinfo_content, "application/json", 200);
    }

    // --- Authentication gate for all endpoints except /serverinfo ---
    // Helper lambda: returns 401 response if auth fails, empty string if OK
    auto check_auth = [&]() -> std::string {
        if (auth_) {
            std::string auth_header = extract_authorization_header(request);
            if (!auth_->validate_request(auth_header)) {
                Json::Value error;
                error["error"]["code"] = -32600;
                error["error"]["message"] = "Unauthorized - valid RPC cookie required";
                error["id"] = Json::nullValue;

                Json::StreamWriterBuilder builder;
                std::string json_response = Json::writeString(builder, error);

                std::string response = build_http_response(json_response, "application/json", 401);
                size_t header_end = response.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    response.insert(header_end, "\r\nWWW-Authenticate: Basic realm=\"Dinero RPC\"");
                }
                return response;
            }
        }
        return {};
    };

    auto build_busy_response = [&]() -> std::string {
        Json::Value error;
        error["error"]["code"] = dinero::rpc::RPC_INTERNAL_ERROR;
        error["error"]["message"] = "RPC server busy: too many concurrent requests";
        error["id"] = Json::nullValue;

        Json::StreamWriterBuilder builder;
        std::string json_response = Json::writeString(builder, error);
        return build_http_response(json_response, "application/json", 503);
    };

    auto run_with_rpc_slot = [&](auto&& fn) -> std::string {
        if (!try_acquire_rpc_execution_slot()) {
            return build_busy_response();
        }

        try {
            std::string result = fn();
            release_rpc_execution_slot();
            return result;
        } catch (...) {
            release_rpc_execution_slot();
            throw;
        }
    };

    // GET /metrics — requires auth (exposes operational data)
    if (request.find("GET /metrics") == 0) {
        auto denied = check_auth();
        if (!denied.empty()) return denied;

        return run_with_rpc_slot([&]() -> std::string {
            bool json_format = false;
            if (request.find("Accept: application/json") != std::string::npos ||
                request.find("accept: application/json") != std::string::npos) {
                json_format = true;
            }

            std::string metrics_content;
            if (json_format) {
                metrics_content = dinero::metrics::MetricsRegistry::ExportMetricsJSON();
                return build_http_response(metrics_content, "application/json", 200);
            }

            metrics_content = dinero::metrics::MetricsRegistry::ExportMetrics();
            return build_http_response(metrics_content, "text/plain; version=0.0.4", 200);
        });
    }

    // Only POST allowed beyond this point
    if (request.find("POST") != 0) {
        return build_http_response("Method not allowed", "text/plain", 405);
    }

    // Check authentication for RPC calls
    {
        auto denied = check_auth();
        if (!denied.empty()) return denied;
    }
    
    return run_with_rpc_slot([&]() -> std::string {
        std::string body = extract_http_body(request);
        if (body.empty()) {
            return build_http_response("Bad request", "text/plain", 400);
        }

        // Parse JSON RPC request (use safe CharReader, cap payload to prevent abuse).
        // submitblock can legitimately carry multi-megabyte full-block hex payloads,
        // especially once private-lane transactions and proofs make blocks denser.
        // Keep a guardrail, but make it large enough for valid block submissions.
        constexpr size_t kMaxRpcBody = 16 * 1024 * 1024; // 16 MiB guardrail
        if (body.size() > kMaxRpcBody) {
            Json::Value error;
            error["error"]["code"] = -32700;
            error["error"]["message"] = "Parse error: payload too large";
            error["id"] = Json::nullValue;

            Json::StreamWriterBuilder builder;
            std::string json_response = Json::writeString(builder, error);
            return build_http_response(json_response, "application/json", 413);
        }

        Json::CharReaderBuilder reader_builder;
        reader_builder["collectComments"] = false;
        std::string parse_errors;
        Json::Value json_request;

        std::unique_ptr<Json::CharReader> reader(reader_builder.newCharReader());
        if (!reader->parse(body.data(), body.data() + body.size(), &json_request, &parse_errors)) {
            Json::Value error;
            error["error"]["code"] = -32700;
            error["error"]["message"] = "Parse error";
            error["error"]["details"] = parse_errors;
            error["id"] = Json::nullValue;

            Json::StreamWriterBuilder builder;
            std::string json_response = Json::writeString(builder, error);
            return build_http_response(json_response);
        }

        Json::Value json_response = process_rpc_call(json_request);

        Json::StreamWriterBuilder builder;
        std::string response_body = Json::writeString(builder, json_response);

        return build_http_response(response_body);
    });
}

Json::Value HttpRpcServer::process_rpc_call(const Json::Value& request) {
    Json::Value response;
    response["jsonrpc"] = "2.0";
    response["id"] = request.get("id", Json::nullValue);

    if (!request.isMember("method")) {
        return dinero::rpc::RpcError(dinero::rpc::RPC_INVALID_REQUEST, "Invalid Request - missing method");
    }

    std::string method = request["method"].asString();
    std::string dispatch_method = method;
    Json::Value params = request.get("params", Json::Value());

    auto invoke_legacy = [&](const std::string& legacy_method) -> Json::Value {
        auto it = methods_.find(legacy_method);
        if (it == methods_.end()) {
            return dinero::rpc::RpcError(dinero::rpc::RPC_METHOD_NOT_FOUND,
                                         "Method not found: " + legacy_method);
        }
        Json::Value result = it->second(params);
        const bool preserve_null_result =
            (legacy_method == "gettxout" || legacy_method == "blockchain.gettxout");
        response["result"] =
            (result.isNull() && !preserve_null_result)
                ? Json::Value(Json::objectValue)
                : result;
        response["error"] = Json::nullValue;
        return response;
    };

    try {
        if (!dev_mode_ && MatchesCanonicalOrFlatAlias(DEV_ONLY_METHODS, method)) {
            return dinero::rpc::RpcError(dinero::rpc::RPC_FORBIDDEN,
                "Method '" + method + "' is available only in explicit development mode");
        }
        // RPC access control: reject admin methods in read-only mode.
        //
        // SECURITY (readonly-bypass fix): the admin check must run on the CANONICAL
        // method name, not the raw request method. Short aliases (e.g.
        // "sendtoaddress" -> "wallet.sendtoaddress", "consolidate" ->
        // "wallet.consolidate") would otherwise slip past ADMIN_METHODS, which only
        // lists the canonical "wallet.*" names. Resolve the alias FIRST, then check
        // both the raw and the canonical name.
        std::string admin_check_method = method;
        if (rpc_registry_ != nullptr) {
            if (const RpcAliasInfo* alias_info = rpc_registry_->getAliasInfo(method)) {
                admin_check_method = alias_info->canonical_name;
            }
        }
        if (readonly_mode_ && (isAdminMethod(method) || isAdminMethod(admin_check_method))) {
            return dinero::rpc::RpcError(dinero::rpc::RPC_FORBIDDEN,
                "Method '" + method + "' is admin-only (server running in read-only mode)");
        }

        // Reserve core daemon control/introspection builtins so auto-flat RPC
        // aliases such as mining.stop never shadow daemon stop.
        if (method == "stop" || method == "help" || method == "getinfo" ||
            method == "listtransactions") {
            auto it = methods_.find(method);
            if (it != methods_.end()) {
                return invoke_legacy(method);
            }
        }

        // UNIFIED RPC ARCHITECTURE (vNext):
        // Priority 1: Check global RpcRegistry first (new unified system)
        if (rpc_registry_ != nullptr) {
            // Compatibility fallback: some older builds may expose only the
            // namespaced method while callers still use the short alias.
            if (dispatch_method == "gettxout" &&
                rpc_registry_->lookup(dispatch_method) == nullptr &&
                rpc_registry_->lookup("blockchain.gettxout") != nullptr) {
                dispatch_method = "blockchain.gettxout";
            }

            // Note: Registry's RpcHandler has different signature than HTTP's local RpcHandler
            // Registry: std::function<Json::Value(const ExecutionContext&, const Json::Value&)>
            // HTTP:     std::function<Json::Value(const Json::Value&)>
            ::RpcHandler* handler_ptr = rpc_registry_->lookup(dispatch_method);  // Use global RpcHandler
            if (handler_ptr) {
                // Found in unified registry - use vNext handler
                ExecutionContext ctx;
                ctx.walletName = ""; // HTTP doesn't specify wallet name
                ctx.user = "";
                ctx.cookie = "";
                ctx.client_id = "http"; // Mark as HTTP client
                ctx.daemon = daemon_context_; // Week 2: Inject DaemonContext for service access

                // Dependency Injection: Logger interface (prefer wallet_logger for wallet.* methods, fallback to logger_interface)
                if (daemon_context_) {
                    // Resolve alias (e.g. sendtoaddress -> wallet.sendtoaddress) for logger routing.
                    std::string canonical_method = dispatch_method;
                    if (const RpcAliasInfo* alias_info = rpc_registry_->getAliasInfo(dispatch_method)) {
                        canonical_method = alias_info->canonical_name;
                    }

                    // For wallet.* methods, use wallet-specific logger; otherwise use general logger.
                    if (canonical_method.rfind("wallet.", 0) == 0 && daemon_context_->wallet_logger) {
                        ctx.logger = daemon_context_->wallet_logger;
                    } else {
                        ctx.logger = daemon_context_->logger_interface;
                    }
                }

                // Phase 2A: Inject mempool and utxo_view
                // TODO: Wire legacy TxMempool once refactoring is complete
                ctx.mempool = nullptr;
                ctx.utxo_view = nullptr;

                // STEP 3.1: Inject NEW policy-aware mempool for stats/policy queries
                if (daemon_context_ && daemon_context_->mempool) {
                    // Cast MempoolService to get underlying Mempool*
                    auto mempool_service = std::dynamic_pointer_cast<dinero::MempoolService>(daemon_context_->mempool);
                    if (mempool_service) {
                        ctx.mempool_v2 = const_cast<dinero::Mempool*>(&mempool_service->mempool());
                    } else {
                        ctx.mempool_v2 = nullptr;
                    }
                } else {
                    ctx.mempool_v2 = nullptr;
                }

                // Phase 3A: Inject wallet_manager from WalletService
                ctx.wallet_manager = (daemon_context_ && daemon_context_->wallet) ?
                    &daemon_context_->wallet->get() : nullptr;

                // Issue #538 — these four lines fired on EVERY RPC, unconditionally,
                // in every daemon.  At ~4 lines per call they drown daemon.log: a
                // harness polling getblockcount for 60s emits ~115 of them, which is
                // what evicted the reorg evidence from a failure dump and made the
                // diagnostic useless under exactly the condition it exists to
                // diagnose.  Gated behind DINERO_RPC_DEBUG (default off), matching
                // the DINERO_* env-hook convention already used in block_relay_manager
                // and block_acceptor.  Read once: this is on the per-request path.
                static const bool rpc_debug_enabled = (std::getenv("DINERO_RPC_DEBUG") != nullptr);

                // Call the unified handler
                if (rpc_debug_enabled) {
                    std::cout << "[RPC DEBUG] Calling handler for method: " << dispatch_method << std::endl;
                    std::cout << "[RPC DEBUG] handler_ptr address: " << (void*)handler_ptr << std::endl;
                    std::cout << "[RPC DEBUG] daemon_context_ address: " << (void*)daemon_context_ << std::endl;
                }

                din::Json result;
                try {
                    result = (*handler_ptr)(ctx, params);
                    if (rpc_debug_enabled) {
                        std::cout << "[RPC DEBUG] Handler completed successfully for: " << dispatch_method << std::endl;
                    }
                } catch (const std::bad_function_call& e) {
                    std::cerr << "[RPC ERROR] bad_function_call for method: " << dispatch_method << std::endl;
                    std::cerr << "[RPC ERROR] This means the RpcHandler function object is empty/null" << std::endl;
                    throw;
                } catch (const std::exception& e) {
                    std::cerr << "[RPC ERROR] Exception in handler for " << dispatch_method << ": " << e.what() << std::endl;
                    throw;
                }

                // Issue #458 — promote a handler-reported error to the
                // top-level JSON-RPC error field.
                //
                // Handlers signal failure by RETURNING an object containing
                // "error" rather than throwing (e.g. generatetoaddress'
                // "Block not activated"). Placing that object under "result"
                // while setting the top-level "error" to null produces a
                // malformed envelope in which a hard failure is
                // indistinguishable from success:
                //
                //   {"error": null, "result": {"error": {"code": -32000, ...}}}
                //
                // RPCServer::handleSingleRequest (rpc_server.cpp:699) already
                // promotes exactly this way, but that path is not the live one;
                // this dispatcher did not, so every caller had to know to look
                // inside "result". None did — a totally failed mine read as
                // success across the integration suite.
                //
                // Stricter than the legacy path in two respects.
                //
                // 1. It promotes only a NON-NULL error, so a handler that
                //    returns an explicit "error": null alongside real data
                //    keeps its result rather than becoming an empty error.
                //
                // 2. It NORMALISES the promoted value. Handlers in this repo
                //    return both shapes:
                //        {"error": {"code": -32000, "message": "..."}}
                //        {"error": "some message"}
                //    Promoting a bare string verbatim would produce another
                //    invalid JSON-RPC envelope — the error member must be an
                //    object with numeric "code" and string "message". A string
                //    is therefore wrapped as an internal error, and so is any
                //    other malformed shape (number, array, object missing
                //    code/message).
                if (result.isObject() && result.isMember("error") &&
                    !result["error"].isNull()) {
                    const Json::Value& raw = result["error"];
                    const bool well_formed =
                        raw.isObject() && raw.isMember("code") &&
                        raw["code"].isIntegral() && raw.isMember("message") &&
                        raw["message"].isString();

                    if (well_formed) {
                        response["error"] = raw;
                    } else {
                        Json::Value normalized;
                        normalized["code"] = -32603;  // Internal error
                        normalized["message"] =
                            raw.isString()
                                ? raw.asString()
                                : std::string("Handler reported a malformed "
                                              "error value");
                        // Keep the original for debuggability without letting a
                        // malformed shape escape into the envelope contract.
                        if (!raw.isString()) {
                            normalized["data"] = raw;
                        }
                        response["error"] = normalized;
                    }
                    return response;
                }

                // Preserve JSON null for gettxout to match Bitcoin-style contract:
                // null means "spent or never existed".
                const bool preserve_null_result =
                    (method == "gettxout" || dispatch_method == "blockchain.gettxout");
                response["result"] =
                    (result.isNull() && !preserve_null_result)
                        ? Json::Value(Json::objectValue)
                        : result;
                response["error"] = Json::nullValue;
                return response;
            }
        }

        // Priority 2: Fall back to legacy methods_ map (backwards compatibility)
        if (methods_.find(method) != methods_.end()) {
            return invoke_legacy(method);
        }

        // Method not found in either registry or legacy map
        return dinero::rpc::RpcError(dinero::rpc::RPC_METHOD_NOT_FOUND, "Method not found: " + method);

    } catch (const std::exception& e) {
        return dinero::rpc::RpcError(dinero::rpc::RPC_INTERNAL_ERROR, std::string("Internal error: ") + e.what());
    }
}

std::string HttpRpcServer::build_http_response(const std::string& content, 
                                              const std::string& content_type, 
                                              int status_code) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status_code;
    
    switch (status_code) {
        case 200: response << " OK"; break;
        case 401: response << " Unauthorized"; break;
        case 404: response << " Not Found"; break;
        case 400: response << " Bad Request"; break;
        case 413: response << " Payload Too Large"; break;
        case 429: response << " Too Many Requests"; break;
        case 405: response << " Method Not Allowed"; break;
        case 503: response << " Service Unavailable"; break;
        case 500: response << " Internal Server Error"; break;
        default: response << " Unknown"; break;
    }
    
    response << "\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << content.length() << "\r\n";
    response << "Access-Control-Allow-Origin: http://127.0.0.1\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << content;
    
    return response.str();
}

std::string HttpRpcServer::extract_http_body(const std::string& request) {
    size_t body_start = request.find("\r\n\r\n");
    if (body_start == std::string::npos) return "";
    
    return request.substr(body_start + 4);
}

std::string HttpRpcServer::extract_authorization_header(const std::string& request) {
    // Find Authorization header (case-insensitive)
    std::string request_lower = request;
    std::transform(request_lower.begin(), request_lower.end(), request_lower.begin(), ::tolower);
    
    size_t auth_pos = request_lower.find("\nauthorization:");
    if (auth_pos == std::string::npos) {
        auth_pos = request_lower.find("\r\nauthorization:");
        if (auth_pos == std::string::npos) {
            return "";
        }
        auth_pos += 2; // Skip \r\n
    } else {
        auth_pos += 1; // Skip \n
    }
    
    // Find the end of the header line
    size_t line_end = request.find("\r\n", auth_pos);
    if (line_end == std::string::npos) {
        line_end = request.find("\n", auth_pos);
        if (line_end == std::string::npos) {
            return "";
        }
    }
    
    // Extract the header value
    std::string header_line = request.substr(auth_pos, line_end - auth_pos);
    size_t colon_pos = header_line.find(':');
    if (colon_pos == std::string::npos) {
        return "";
    }
    
    std::string value = header_line.substr(colon_pos + 1);

    // Trim ALL whitespace (including \r\n)
    value.erase(0, value.find_first_not_of(" \t\r\n"));
    value.erase(value.find_last_not_of(" \t\r\n") + 1);

    return value;
}

std::string HttpRpcServer::extract_client_ip(int client_socket) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    if (getpeername(client_socket, (struct sockaddr*)&client_addr, &addr_len) == 0) {
        return inet_ntoa(client_addr.sin_addr);
    }
    
    return "unknown";
}

void HttpRpcServer::configure_client_socket(int client_socket) {
#ifdef _WIN32
    const DWORD timeout_ms = static_cast<DWORD>(kClientSocketTimeout.count());
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval timeout;
    timeout.tv_sec = static_cast<time_t>(kClientSocketTimeout.count() / 1000);
    timeout.tv_usec = static_cast<suseconds_t>((kClientSocketTimeout.count() % 1000) * 1000);
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

bool HttpRpcServer::try_acquire_rpc_execution_slot() {
    uint32_t current = active_rpc_handlers_.load(std::memory_order_relaxed);
    while (current < kMaxConcurrentRpcHandlers) {
        if (active_rpc_handlers_.compare_exchange_weak(
                current,
                current + 1,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void HttpRpcServer::release_rpc_execution_slot() {
    active_rpc_handlers_.fetch_sub(1, std::memory_order_acq_rel);
}

Json::Value HttpRpcServer::handle_getinfo(const Json::Value& params) {
    Json::Value result;
    result["version"] = DINERO_CLI_GIT_SHA;
    result["build_time"] = DINERO_CLI_BUILD_DATE;
    result["protocol_version"] = 1;
    result["blocks"] = 0;  // Use getblockcount RPC for real blockchain height
    result["connections"] = 0;  // Use getconnectioncount RPC for real peer count
    result["difficulty"] = "0x1d3fffff";  // CPU-friendly difficulty (regtest)
    result["testnet"] = false;
    result["balance"] = 0.0;  // Use getbalance RPC for real wallet balance
    result["rpc_methods"] = Json::Value(Json::arrayValue);

    for (const auto& method : methods_) {
        result["rpc_methods"].append(method.first);
    }

    // Include methods from the unified RPC registry
    if (rpc_registry_) {
        for (const auto& name : rpc_registry_->methodNames()) {
            result["rpc_methods"].append(name);
        }
    }

    return result;
}

Json::Value HttpRpcServer::handle_help(const Json::Value& params) {
    // Method-specific help: help "<method>"
    if (!params.empty()) {
        if (params.size() != 1 || !params[0].isString()) {
            Json::Value error(Json::objectValue);
            error["code"] = -32602;
            error["message"] = "help expects 0 or 1 string parameter";
            return dinero::rpc::RpcError(dinero::rpc::RPC_INVALID_PARAMS, error["message"].asString());
        }

        const std::string requested_method = params[0].asString();
        std::string canonical_method = requested_method;

        if (rpc_registry_) {
            if (const RpcAliasInfo* alias_info = rpc_registry_->getAliasInfo(requested_method)) {
                canonical_method = alias_info->canonical_name;
            } else if (requested_method == "gettxout" &&
                       rpc_registry_->has("blockchain.gettxout")) {
                // Compatibility fallback for builds that expose only namespaced method.
                canonical_method = "blockchain.gettxout";
            }
        }

        const RpcMethodMeta* meta = nullptr;
        if (rpc_registry_) {
            meta = rpc_registry_->getMethodMeta(requested_method);
            if (!meta && canonical_method != requested_method) {
                meta = rpc_registry_->getMethodMeta(canonical_method);
            }
        }

        Json::Value method_help(Json::objectValue);
        method_help["requested_method"] = requested_method;
        method_help["method"] = canonical_method;

        if (meta) {
            method_help["namespace"] = meta->ns;
            method_help["description"] = meta->description;

            Json::Value method_params(Json::arrayValue);
            for (const auto& p : meta->params) {
                Json::Value param(Json::objectValue);
                param["name"] = p.name;
                param["type"] = p.type;
                param["required"] = p.required;
                param["description"] = p.desc;
                method_params.append(param);
            }
            method_help["params"] = method_params;

            Json::Value result_meta(Json::objectValue);
            result_meta["type"] = meta->result.type;
            result_meta["description"] = meta->result.desc;
            method_help["result"] = result_meta;

            if (!meta->help.empty()) {
                method_help["example"] = meta->help;
            }

            if (rpc_registry_) {
                const auto aliases = rpc_registry_->getAliases(canonical_method);
                if (!aliases.empty()) {
                    Json::Value alias_list(Json::arrayValue);
                    for (const auto& alias : aliases) {
                        alias_list.append(alias);
                    }
                    method_help["aliases"] = alias_list;
                }
            }

            return method_help;
        }

        // Fallback when method exists but has no metadata attached.
        const bool in_registry = rpc_registry_ &&
            (rpc_registry_->has(requested_method) || rpc_registry_->has(canonical_method));
        const bool in_legacy = methods_.count(requested_method) > 0 || methods_.count(canonical_method) > 0;
        if (in_registry || in_legacy) {
            method_help["description"] = "Method is registered but has no detailed metadata.";
            method_help["params"] = Json::Value(Json::arrayValue);
            method_help["result"] = Json::Value(Json::objectValue);
            return method_help;
        }

        return dinero::rpc::RpcError(dinero::rpc::RPC_METHOD_NOT_FOUND,
                                     "Method not found: " + requested_method);
    }

    // General help: list available methods.
    Json::Value result;
    result["available_methods"] = Json::Value(Json::arrayValue);

    for (const auto& method : methods_) {
        result["available_methods"].append(method.first);
    }

    // Include methods from the unified RPC registry
    if (rpc_registry_) {
        for (const auto& name : rpc_registry_->methodNames()) {
            result["available_methods"].append(name);
        }
    }

    result["description"] = "Dinero headless daemon RPC server";
    result["version"] = DINERO_CLI_VERSION;
    result["tip"] = "Use help \"<method>\" for method-specific documentation";

    return result;
}

Json::Value HttpRpcServer::handle_stop(const Json::Value& params) {
    (void)params;
    Json::Value result;
    result["message"] = "Shutdown requested";

    // Stop accepting new work immediately on the RPC thread. The actual daemon
    // shutdown callback still runs asynchronously so the current request can
    // complete without re-entrancy issues.
    shutdown_requested_ = true;

    auto request_shutdown = daemon_context_ ? daemon_context_->request_shutdown
                                            : std::function<void()>{};

    // Never capture `this` here: RPCService::Stop() destroys HttpRpcServer
    // during shutdown, and a detached thread holding a raw `this` can crash or
    // call std::terminate() after the server object is gone.
    std::thread([request_shutdown = std::move(request_shutdown)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        try {
            if (request_shutdown) {
                request_shutdown();
            }
        } catch (const std::exception& e) {
            std::cerr << "RPC stop callback failed: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "RPC stop callback failed: unknown exception" << std::endl;
        }
    }).detach();

    return result;
}

int HttpRpcServer::create_server_socket() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return -1;
    }
#endif
    
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return -1;
    }

    // #295: don't let spawned children (e.g. dinero-seeder) inherit the RPC
    // listen socket — an orphaned child would keep port 20998 bound after the
    // daemon exits and block the next daemon from starting. This is the LIVE RPC
    // server (RPCService::Start -> HttpRpcServer); rpc_server_clean.cpp is unused.
    compat_set_cloexec(server_socket);

    // Set socket options
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, 
                   (char*)&opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set socket options" << std::endl;
        close_socket(server_socket);
        return -1;
    }
    
    // Bind socket
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_);
    
    if (bind_address_ == "0.0.0.0" || bind_address_ == "*") {
        server_addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, bind_address_.c_str(), &server_addr.sin_addr) <= 0) {
            std::cerr << "Invalid bind address: " << bind_address_ << std::endl;
            close_socket(server_socket);
            return -1;
        }
    }
    
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to bind socket to " << bind_address_ << ":" << port_ << std::endl;
        close_socket(server_socket);
        return -1;
    }
    
    // Listen for connections
    if (listen(server_socket, 5) < 0) {
        std::cerr << "Failed to listen on socket" << std::endl;
        close_socket(server_socket);
        return -1;
    }
    
    return server_socket;
}

bool HttpRpcServer::checkRpcRate(const std::string& client_ip) {
    std::lock_guard<std::mutex> lock(rpc_rate_mutex_);
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    auto& bucket = rpc_rate_buckets_[client_ip];
    if (bucket.last_refill == 0) {
        bucket.last_refill = now;
        bucket.tokens = static_cast<double>(RPC_BUCKET_CAPACITY);
    }

    // Refill tokens based on elapsed time
    double elapsed_sec = static_cast<double>(now - bucket.last_refill) / 1000.0;
    bucket.tokens = std::min(static_cast<double>(RPC_BUCKET_CAPACITY),
                             bucket.tokens + elapsed_sec * RPC_MAX_REQUESTS_PER_SEC);
    bucket.last_refill = now;

    // Consume one token
    if (bucket.tokens >= 1.0) {
        bucket.tokens -= 1.0;
        return true;
    }
    return false;
}

void HttpRpcServer::close_socket(int socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}
