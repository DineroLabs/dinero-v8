#include <string>

class DaemonContext;

namespace dinero::grpc_server {

class SocketWalletServer {
public:
    explicit SocketWalletServer(DaemonContext*, const std::string& = "127.0.0.1:50051");
    ~SocketWalletServer();
    bool Start();
    void Stop();
    bool IsRunning() const { return false; }
    std::string GetAddress() const { return m_address; }
private:
    std::string m_address;
};

SocketWalletServer::SocketWalletServer(DaemonContext*, const std::string& address)
    : m_address(address) {}
SocketWalletServer::~SocketWalletServer() = default;
bool SocketWalletServer::Start() { return false; }
void SocketWalletServer::Stop() {}

} // namespace dinero::grpc_server
