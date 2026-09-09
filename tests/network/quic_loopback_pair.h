#pragma once

#include "network/quic_session.h"
#include <memory>
#include <mutex>

namespace dinero::network::test {

// Own both endpoints until no writer can deliver to either one. Close() only
// requests shutdown; it does not join, and handshake readiness does not mean
// the session thread has stopped sending packets. A callback must never read
// a shared_ptr variable concurrently with reset(), including on early returns.
class QuicLoopbackPair {
    struct Routing {
        std::mutex mutex;
        QuicSession* client{nullptr};
        QuicSession* server{nullptr};
        void deliver(bool to_server, std::vector<uint8_t> bytes) {
            std::lock_guard<std::mutex> lock(mutex);
            auto* target = to_server ? server : client;
            if (target) target->EnqueueIncomingPacket(std::move(bytes));
        }
    };
    std::shared_ptr<Routing> routing_ = std::make_shared<Routing>();
public:
    std::unique_ptr<QuicSession> client;
    std::unique_ptr<QuicSession> server;

    QuicLoopbackPair() {
        server = std::make_unique<QuicSession>([route=routing_](std::vector<uint8_t> bytes) {
            route->deliver(false, std::move(bytes));
        });
        client = std::make_unique<QuicSession>([route=routing_](std::vector<uint8_t> bytes) {
            route->deliver(true, std::move(bytes));
        });
        std::lock_guard<std::mutex> lock(routing_->mutex);
        routing_->client = client.get();
        routing_->server = server.get();
    }
    ~QuicLoopbackPair() {
        // Wait for in-flight enqueue callbacks, then disconnect both directions
        // before either endpoint can be destroyed. Never hold this lock while
        // joining: session threads may still invoke their now-disconnected writers.
        {
            std::lock_guard<std::mutex> lock(routing_->mutex);
            routing_->client = nullptr;
            routing_->server = nullptr;
        }
        client->Close();
        server->Close();
    }
};

} // namespace dinero::network::test
