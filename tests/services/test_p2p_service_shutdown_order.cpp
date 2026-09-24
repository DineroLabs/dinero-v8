#include "common/test_logger.h"
#include "daemon/services/p2p_service.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dinero {

struct P2PServiceStopTestAccess {
    static void Install(P2PService& service, std::unique_ptr<::P2PManager> manager,
                        ILogger& logger) {
        service.p2p_mgr_ = std::move(manager);
        service.logger_interface_ = &logger;
    }
};

namespace {

class BlockingStopLogger final : public NullLogger {
public:
    void info(const std::string& message) override {
        if (message != "[P2PService] Stopping P2P networking...") return;
        std::unique_lock<std::mutex> lock(mutex_);
        blocked_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
    }

    bool WaitUntilBlocked() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(5),
                            [this] { return blocked_; });
    }

    void Release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool blocked_{false};
    bool released_{false};
};

class LoopbackListener {
public:
    LoopbackListener() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(fd_, 4) != 0) return;
        socklen_t length = sizeof(address);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            return;
        port_ = ntohs(address.sin_port);
    }

    ~LoopbackListener() {
        if (fd_ >= 0) ::close(fd_);
    }
    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;

    uint16_t port() const { return port_; }

    bool CanConnect() const {
        if (port_ == 0) return false;
        const int client = ::socket(AF_INET, SOCK_STREAM, 0);
        if (client < 0) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port_);
        const bool connected =
            ::connect(client, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)) == 0;
        ::close(client);
        return connected;
    }

private:
    int fd_{-1};
    uint16_t port_{0};
};

TEST(P2PServiceShutdownOrder, QuiescesOutboundBeforeBlockingTeardown) {
    LoopbackListener listener;
    ASSERT_NE(listener.port(), 0);
    ASSERT_TRUE(listener.CanConnect());

    BlockingStopLogger logger;
    P2PService service;
    auto manager = std::make_unique<::P2PManager>(0, "");
    ASSERT_TRUE(manager->start());
    P2PServiceStopTestAccess::Install(service, std::move(manager), logger);

    std::thread stopper([&service] { service.Stop(); });
    const bool blocked_in_teardown = logger.WaitUntilBlocked();
    bool manager_still_running = false;
    bool outbound_dial_accepted = false;
    if (blocked_in_teardown) {
        // manager.stop() has not run: the service is stalled in an earlier
        // teardown step. A live local listener makes a missing cancellation
        // signal observable as a successful outbound dial.
        manager_still_running = service.get().is_running();
        outbound_dial_accepted =
            service.get().connect_to_peer("127.0.0.1", listener.port());
    }
    logger.Release();
    stopper.join();

    ASSERT_TRUE(blocked_in_teardown);
    EXPECT_TRUE(manager_still_running);
    EXPECT_FALSE(outbound_dial_accepted);
}

}  // namespace
}  // namespace dinero
