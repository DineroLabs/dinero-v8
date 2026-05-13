#ifndef WS_SERVER_H_INCLUDED
#define WS_SERVER_H_INCLUDED

#include <memory>
#include <string>

namespace boost {
namespace asio {
  class io_context;
}
}

namespace dinero {

// WebSocket server class
class WsServer {
public:
  WsServer(boost::asio::io_context& ioc,
           const std::string& ip, unsigned short port,
           const std::string& cookie_path = "");

  void run();
  void stop();
  unsigned short effective_port() const;

private:
  class Listener;  // Forward declaration of nested class
  std::shared_ptr<Listener> listener_;
};

// Initialize and start the WebSocket server (called from main)
void start_websocket_server();

} // namespace dinero

#endif // WS_SERVER_H_INCLUDED
