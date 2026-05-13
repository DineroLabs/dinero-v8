#pragma once
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <mutex>
#include <vector>
#include <deque>
#include <memory>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include "events/event_sink.h"
#include "metrics/metrics_registry.h"

namespace net = boost::asio;
namespace beast = boost::beast;
using tcp = net::ip::tcp;

/**
 * @brief Simple WebSocket session with backpressure handling
 */
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    explicit WsSession(tcp::socket&& socket)
        : ws_(std::move(socket)) {}

    void start() {
        ws_.set_option(beast::websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(beast::websocket::stream_base::decorator(
            [](beast::websocket::response_type& res) {
                res.set(beast::http::field::server, "dinero-ws");
            }));
        
        ws_.async_accept([self=shared_from_this()](beast::error_code ec) {
            if (ec) return;
            self->doRead();
        });
    }

    // Thread-safe entry point for sending messages
    void send(std::string msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outq_.size() >= MAX_QUEUE_SIZE) {
            // Drop oldest message to prevent memory exhaustion
            outq_.pop_front();
        }
        outq_.push_back(std::move(msg));
        if (!writing_) {
            doWrite();
        }
    }

    bool isOpen() const { return ws_.is_open(); }

private:
    void doRead() {
        auto self = shared_from_this();
        ws_.async_read(buf_, [self](beast::error_code ec, std::size_t) {
            if (ec) return;
            // Handle incoming message (could be JSON-RPC requests)
            self->buf_.consume(self->buf_.size());
            self->doRead();
        });
    }

    void doWrite() {
        if (outq_.empty()) {
            writing_ = false;
            return;
        }
        
        writing_ = true;
        ws_.text(true);
        ws_.async_write(net::buffer(outq_.front()),
            [self=shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) { 
                    self->writing_ = false; 
                    self->outq_.clear(); 
                    return; 
                }
                self->outq_.pop_front();
                self->doWrite(); // Continue with next message
            });
    }

    beast::websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buf_;
    std::deque<std::string> outq_;
    std::mutex mutex_;
    bool writing_ = false;
    
    static constexpr size_t MAX_QUEUE_SIZE = 256; // Backpressure limit
};

/**
 * @brief Simple WebSocket hub that implements IEventSink
 */
class WsHub : public IEventSink {
public:
    void add(std::shared_ptr<WsSession> session) {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.push_back(session);
        pruneUnlocked();
    }
    
    void removeClosed() { 
        std::lock_guard<std::mutex> lock(mutex_); 
        pruneUnlocked(); 
    }

    // IEventSink implementation
    void publish(std::string_view topic, const nlohmann::json& payload) override {
        nlohmann::json msg{
            {"jsonrpc", "2.0"},
            {"method", topic},            // JSON-RPC notification style
            {"params", payload},
            {"ts", std::time(nullptr)}
        };
        auto text = msg.dump();

        std::vector<std::shared_ptr<WsSession>> targets;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pruneUnlocked();
            targets.reserve(clients_.size());
            for (auto const& w : clients_) {
                if (auto s = w.lock()) {
                    targets.push_back(s);
                }
            }
        }
        
        // Send to all active sessions (outside the lock)
        for (auto& session : targets) {
            session->send(text);
        }

        // Update client count metrics
        dinero::metrics::MetricsRegistry::SetWebSocketClients(targets.size());

        // Add schema versioning and deduplication
        addSchemaAndDedup(text, topic, payload);

        // Update metrics
        dinero::metrics::MetricsRegistry::IncrementWebSocketMessages(std::string(topic));
    }

private:
    void pruneUnlocked() {
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                [](const std::weak_ptr<WsSession>& w) {
                    return w.expired() || !w.lock()->isOpen();
                }),
            clients_.end());
    }

    void addSchemaAndDedup(std::string& text, std::string_view topic, const nlohmann::json& payload) {
        try {
            // Parse the JSON to add schema fields
            auto msg = nlohmann::json::parse(text);

            // Add schema versioning
            msg["schema"] = "din.ws.v1";
            msg["rev"] = 1;
            msg["event_id"] = generateEventId();

            // Add deduplication check
            if (shouldEmit(msg)) {
                text = msg.dump();
            } else {
                text.clear(); // Skip this message
            }
        } catch (const std::exception& e) {
            // If parsing fails, send original message
            std::cerr << "Failed to add schema to WS message: " << e.what() << std::endl;
        }
    }

    std::string generateEventId() {
        static std::atomic<uint64_t> counter{0};
        return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               "_" + std::to_string(counter.fetch_add(1));
    }

    bool shouldEmit(const nlohmann::json& msg) {
        static std::unordered_map<std::string, uint64_t> last_seen;
        static std::mutex dedup_mutex;

        try {
            std::string topic = msg["method"];
            uint64_t ts = msg["ts"];

            // Create deduplication key
            std::string key = topic;
            if (msg.contains("params") && msg["params"].contains("hash")) {
                key += "#" + std::string(msg["params"]["hash"]);
            } else if (msg.contains("params") && msg["params"].contains("height")) {
                key += "#" + std::to_string(int64_t(msg["params"]["height"]));
            }

            std::lock_guard<std::mutex> lock(dedup_mutex);
            auto it = last_seen.find(key);
            if (it != last_seen.end() && it->second >= ts) {
                return false; // Duplicate
            }
            last_seen[key] = ts;
            return true;
        } catch (const std::exception& e) {
            // If we can't parse for dedup, allow the message
            return true;
        }
    }

    std::mutex mutex_;
    std::vector<std::weak_ptr<WsSession>> clients_;
};
