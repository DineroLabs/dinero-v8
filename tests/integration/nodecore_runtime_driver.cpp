// Test transport only: every operation calls the linked production C ABI.
// Replies use a separate file because DaemonApp owns stdout/stderr logging.
#include "nodecore/nodecore_ffi.h"

#include <json/json.h>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
using namespace std::chrono_literals;

Json::Value Parse(const std::string& text) {
    Json::Value value;
    Json::CharReaderBuilder reader;
    std::string error;
    std::istringstream input(text);
    if (!Json::parseFromStream(reader, input, &value, &error)) {
        throw std::runtime_error(error);
    }
    return value;
}

std::string Encode(const Json::Value& value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

Json::Value Owned(char* text) {
    if (!text) return Json::Value();
    const std::string copy(text);
    nodecore_free_string(text);
    return Parse(copy);
}

struct ShutdownBarrier {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    bool timed_out = false;

    static void Callback(int32_t event, const char*, void* context) {
        if (event != NODECORE_EVENT_SHUTDOWN) return;
        auto& gate = *static_cast<ShutdownBarrier*>(context);
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        // No lifecycle API is called from this callback: Stop is joining this
        // emitting thread. Always release, including a broken test driver.
        if (!gate.changed.wait_for(lock, 10s, [&] { return gate.released; })) {
            gate.timed_out = true;
        }
    }
};

Json::Value StopAtCallback(const Json::Value& request) {
    ShutdownBarrier gate;
    nodecore_set_event_callback(ShutdownBarrier::Callback, &gate);
    auto stopped = std::async(std::launch::async, [] { return nodecore_stop(); });
    Json::Value result;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        result["callback_entered"] = gate.changed.wait_for(lock, 10s, [&] { return gate.entered; });
    }
    result["running_during_callback"] = nodecore_is_running();
    result["stop_waits_for_callback"] = stopped.wait_for(100ms) == std::future_status::timeout;
    std::future<int32_t> restarted;
    if (request.isMember("restart_datadir")) {
        const auto path = request["restart_datadir"].asString();
        const auto config = Encode(request["config"]);
        std::promise<void> attempted;
        auto attempt = attempted.get_future();
        restarted = std::async(std::launch::async, [ready = std::move(attempted), path, config]() mutable {
            ready.set_value();
            return nodecore_start(path.c_str(), config.c_str());
        });
        result["start_attempted"] = attempt.wait_for(10s) == std::future_status::ready;
        result["start_waits_for_stop"] = restarted.wait_for(100ms) == std::future_status::timeout;
    }
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.released = true;
        gate.changed.notify_all();
    }
    result["stop_result"] = stopped.get();
    if (restarted.valid()) result["restart_result"] = restarted.get();
    nodecore_set_event_callback(nullptr, nullptr);
    result["callback_timed_out"] = gate.timed_out;
    return result;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ofstream replies(argv[1]);
    if (!replies) return 2;
    std::string line;
    while (std::getline(std::cin, line)) {
        Json::Value result;
        try {
            const auto request = Parse(line);
            const auto operation = request["op"].asString();
            if (operation == "start") {
                const auto path = request["datadir"].asString();
                const auto config = Encode(request["config"]);
                result = nodecore_start(path.c_str(), config.c_str());
            } else if (operation == "stop") {
                result = nodecore_stop();
            } else if (operation == "stop_at_callback") {
                result = StopAtCallback(request);
            } else if (operation == "status") {
                result = Owned(nodecore_get_status_json());
            } else if (operation == "rpc") {
                const auto method = request["method"].asString();
                const auto params = Encode(request["params"]);
                result = Owned(nodecore_rpc_call(method.c_str(), params.c_str()));
            } else {
                throw std::runtime_error("unknown test operation");
            }
        } catch (const std::exception& error) {
            result["driver_error"] = error.what();
        }
        replies << Encode(result) << '\n' << std::flush;
    }
    return nodecore_stop() == NODECORE_OK ? 0 : 1;
}
