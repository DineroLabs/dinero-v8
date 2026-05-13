#include "rpc/streaming_rpc_handler.h"
#include "common/logger.h"
#include "wallet/wallet_api.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <thread>
#include <future>

namespace dinero {
namespace rpc {

// ═══════════════════════════════════════════════════════════════
// StreamingRpcHandler Implementation
// ═══════════════════════════════════════════════════════════════

StreamingRpcHandler::StreamingRpcHandler(const std::string& operation_id, DaemonContext* daemon_ctx)
    : operation_id_(operation_id)
    , daemon_ctx_(daemon_ctx)
    , ws_server_(daemon_ctx ? daemon_ctx->websocket_server : nullptr)
{
}

StreamingRpcHandler::~StreamingRpcHandler() {
    // Cancel all running operations on cleanup
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : operations_) {
        pair.second->cancelled = true;
    }
}

std::string StreamingRpcHandler::generate_operation_id() const {
    // Generate short operation ID: op_<8 hex chars>
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis;

    uint32_t random = dis(gen);

    std::ostringstream oss;
    oss << "op_" << std::hex << std::setw(8) << std::setfill('0') << random;
    return oss.str();
}

std::string StreamingRpcHandler::start_operation(
    const std::string& client_id,
    std::function<din::Json(ProgressCallback)> work_fn,
    const din::Json& params)
{
    std::string op_id = generate_operation_id();

    // Create operation state
    auto state = std::make_shared<OperationState>();
    state->client_id = client_id;
    state->start_time = std::chrono::system_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        operations_[op_id] = state;
    }

    dinero::g_logger.info("[StreamingRPC] Starting operation: " + op_id +
                          " for client: " + client_id);

    // Create progress callback
    ProgressCallback progress_cb = [this, op_id, client_id, state](
        uint64_t current, uint64_t total, const std::string& message, const din::Json& extra)
    {
        // Check if cancelled
        if (state->cancelled.load()) {
            throw std::runtime_error("Operation cancelled by user");
        }

        // Update state
        state->current_progress = current;
        state->total_work = total;
        state->last_message = message;

        // Send progress event
        this->send_progress(client_id, op_id, current, total, message, extra);
    };

    // Run work function asynchronously
    std::thread worker([this, op_id, client_id, state, work_fn, progress_cb]() {
        try {
            din::Json result = work_fn(progress_cb);

            if (!state->cancelled.load()) {
                state->completed = true;
                this->send_complete(client_id, op_id, result);

                dinero::g_logger.info("[StreamingRPC] Operation completed: " + op_id);
            } else {
                dinero::g_logger.info("[StreamingRPC] Operation cancelled: " + op_id);
            }

        } catch (const std::exception& e) {
            std::string error_msg = std::string("Operation failed: ") + e.what();
            this->send_error(client_id, op_id, error_msg, -1);

            dinero::g_logger.error("[StreamingRPC] Operation error: " + op_id + " - " + error_msg);
        }

        // Cleanup after short delay
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::lock_guard<std::mutex> lock(this->mutex_);
        this->operations_.erase(op_id);
    });

    worker.detach();

    return op_id;
}

bool StreamingRpcHandler::cancel_operation(const std::string& op_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = operations_.find(op_id);
    if (it == operations_.end()) {
        return false;
    }

    it->second->cancelled = true;

    dinero::g_logger.info("[StreamingRPC] Cancelled operation: " + op_id);

    return true;
}

bool StreamingRpcHandler::is_running(const std::string& op_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = operations_.find(op_id);
    if (it == operations_.end()) {
        return false;
    }

    return !it->second->completed && !it->second->cancelled.load();
}

din::Json StreamingRpcHandler::get_status(const std::string& op_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = operations_.find(op_id);
    if (it == operations_.end()) {
        din::Json result;
        result["error"] = "Operation not found";
        result["code"] = -32602;
        return result;
    }

    auto& state = it->second;

    din::Json result;
    result["operation_id"] = op_id;
    result["operation_type"] = operation_id_;
    result["running"] = !state->completed && !state->cancelled.load();
    result["cancelled"] = state->cancelled.load();
    result["completed"] = state->completed;
    result["current_progress"] = static_cast<uint64_t>(state->current_progress.load());
    result["total_work"] = static_cast<uint64_t>(state->total_work.load());
    result["last_message"] = state->last_message;

    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - state->start_time).count();
    result["elapsed_seconds"] = static_cast<int64_t>(elapsed);

    return result;
}

void StreamingRpcHandler::send_progress(
    const std::string& client_id,
    const std::string& op_id,
    uint64_t current,
    uint64_t total,
    const std::string& message,
    const din::Json& extra)
{
    if (!ws_server_) {
        return;
    }

    din::Json event = din::obj();
    event["jsonrpc"] = "2.0";
    event["method"] = "progress";

    din::Json params = din::obj();
    params["operation_id"] = op_id;
    params["operation_type"] = operation_id_;
    params["current"] = current;
    params["total"] = total;
    params["message"] = message;

    if (!extra.isNull() && extra.isObject()) {
        params["extra"] = extra;
    }

    // Calculate percentage
    double percentage = (total > 0) ? (static_cast<double>(current) / total * 100.0) : 0.0;
    params["percentage"] = percentage;

    event["params"] = params;

    ws_server_->send_to_client(client_id, event);
}

void StreamingRpcHandler::send_complete(
    const std::string& client_id,
    const std::string& op_id,
    const din::Json& result)
{
    if (!ws_server_) {
        return;
    }

    din::Json event = din::obj();
    event["jsonrpc"] = "2.0";
    event["method"] = "complete";

    din::Json params = din::obj();
    params["operation_id"] = op_id;
    params["operation_type"] = operation_id_;
    params["result"] = result;

    event["params"] = params;

    ws_server_->send_to_client(client_id, event);
}

void StreamingRpcHandler::send_error(
    const std::string& client_id,
    const std::string& op_id,
    const std::string& error_message,
    int error_code)
{
    if (!ws_server_) {
        return;
    }

    din::Json event = din::obj();
    event["jsonrpc"] = "2.0";
    event["method"] = "error";

    din::Json params = din::obj();
    params["operation_id"] = op_id;
    params["operation_type"] = operation_id_;
    params["error"] = error_message;
    params["code"] = error_code;

    event["params"] = params;

    ws_server_->send_to_client(client_id, event);
}

// ═══════════════════════════════════════════════════════════════
// WalletRescanHandler Implementation
// ═══════════════════════════════════════════════════════════════

WalletRescanHandler::WalletRescanHandler(DaemonContext* daemon_ctx)
    : StreamingRpcHandler("walletrescan", daemon_ctx)
{
}

din::Json WalletRescanHandler::start_rescan(const std::string& client_id, uint64_t start_height) {
    din::Json params = din::obj();
    params["start_height"] = start_height;

    auto work_fn = [this, start_height](ProgressCallback progress_cb) -> din::Json {
        return this->perform_rescan(start_height, progress_cb);
    };

    std::string op_id = start_operation(client_id, work_fn, params);

    din::Json result;
    result["operation_id"] = op_id;
    result["status"] = "started";
    result["message"] = "Wallet rescan started. Progress updates will be sent via WebSocket.";
    result["start_height"] = static_cast<Json::UInt64>(start_height);

    return result;
}

din::Json WalletRescanHandler::perform_rescan(uint64_t start_height, ProgressCallback progress_cb) {
    dinero::g_logger.info("[WalletRescan] Starting rescan from height: " + std::to_string(start_height));

    // Get blockchain height from chainstate service (modern context-aware pattern)
    uint64_t chain_height = 0;
    if (daemon_ctx_ && daemon_ctx_->chainstate) {
        chain_height = static_cast<uint64_t>(daemon_ctx_->chainstate->getBlockHeight());
    } else {
        throw std::runtime_error("Chainstate service not available");
    }

    if (start_height > chain_height) {
        throw std::runtime_error("Start height exceeds current blockchain height");
    }

    din::Json extra = din::obj();
    extra["start_height"] = static_cast<Json::UInt64>(start_height);
    extra["chain_height"] = static_cast<Json::UInt64>(chain_height);
    progress_cb(0, 1, "Wallet rescan backend unavailable in this build", extra);

    throw std::runtime_error("Wallet rescan backend unavailable in this build");
}

} // namespace rpc
} // namespace dinero
