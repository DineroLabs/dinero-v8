#ifndef WS_HTTP_SESSION_H_INCLUDED
#define WS_HTTP_SESSION_H_INCLUDED

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <string>

namespace dinero {

// Forward declaration
class WsSession;

// HTTP session that handles initial request and authentication
// before upgrading to WebSocket
class WsHttpSession : public std::enable_shared_from_this<WsHttpSession> {
public:
    WsHttpSession(
        boost::asio::ip::tcp::socket&& socket,
        const std::string& cookie_path);

    // Start reading the HTTP request
    void run();

private:
    void do_read();
    void on_read(boost::beast::error_code ec, std::size_t bytes_transferred);
    void on_write(boost::beast::error_code ec, std::size_t bytes_transferred, bool close);

    // Check if request is authenticated
    bool is_authenticated() const;

    // Send 401 Unauthorized response
    void send_unauthorized();

    // Send 403 Forbidden response
    void send_forbidden();

    boost::beast::tcp_stream stream_;
    boost::beast::flat_buffer buffer_;

    // HTTP request parser
    boost::optional<boost::beast::http::request_parser<boost::beast::http::empty_body>> parser_;

    std::string cookie_path_;
};

} // namespace dinero

#endif // WS_HTTP_SESSION_H_INCLUDED
