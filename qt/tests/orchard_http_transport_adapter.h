#pragma once
#include <cstdint>
#include <memory>
#include <string>
// Test-only std ABI: keep Qt headers/macros out of the real node fixtures.
namespace OrchardHttpTransportTest {
// Must run BEFORE constructing fixtures, QApplication or any networking object.
// Requires the future isolated owner environment and explicit execution consent.
std::string ValidateEnvironment();
// Main-scope application owner; no static QApplication destruction at exit.
int RunWithApplication(int (*runTests)());
class Client {
public:
    Client(const std::string& home, const std::string& datadir, uint16_t port);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    // Uses production RpcClient::callNamedAs and real reply signals.
    // Returns {ok, tag, value} or {ok, tag, code, message, data}.
    std::string Call(const std::string& method, const std::string& params,
                     const std::string& tag);
    // Reads the actual widget after its real HTTP requests finish. No injected replies.
    std::string ReadWidget(const std::string& wallet);
    // Persistent actual widget: real review button/dialog and natural RPC polling.
    std::string BeginWidgetPayment(const std::string& wallet, const std::string& mode,
        const std::string& address, uint64_t amount, uint64_t fee);
    std::string WaitWidgetPayment(const std::string& state);
    std::string RetryWidgetPayment();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
