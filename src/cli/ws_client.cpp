#include "ws_client.h"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <QFile>
#include <QTextStream>
#include <QUrl>
#include <iostream>
#include <string>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

std::string WsClient::call(const std::string& url, const std::string& auth_header, const std::string& json_request) {
    try {
        // Parse URL using Qt
        QUrl qurl(QString::fromStdString(url));
        std::string host = qurl.host().toStdString();
        std::string port = std::to_string(qurl.port(21001));
        std::string path = qurl.path().toStdString();
        
        if (path.empty()) path = "/ws";

        // IO context and resolver
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        websocket::stream<tcp::socket> ws(ioc);

        // Resolve and connect
        auto const results = resolver.resolve(host, port);
        auto ep = net::connect(ws.next_layer(), results);

        // Update the host string for the handshake
        host += ':' + std::to_string(ep.port());

        // Set WebSocket options
        ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws.set_option(websocket::stream_base::decorator(
            [&auth_header](websocket::request_type& req) {
                if (!auth_header.empty()) {
                    req.set(http::field::authorization, auth_header);
                }
                req.set(http::field::user_agent, "dinero-cli/1.0");
            }));

        // Perform WebSocket handshake
        ws.handshake(host, path);

        // Send JSON-RPC request
        ws.write(net::buffer(json_request));

        // Read response
        beast::flat_buffer buffer;
        ws.read(buffer);

        // Close WebSocket
        ws.close(websocket::close_code::normal);

        // Convert response to string
        return beast::buffers_to_string(buffer.data());

    } catch (const std::exception& e) {
        throw std::runtime_error("WebSocket error: " + std::string(e.what()));
    }
}

bool WsClient::isAvailable(const std::string& url) {
    try {
        // Quick connection test with minimal request
        std::string test_request = R"({"jsonrpc":"2.0","method":"getbestblockhash","id":1})";
        call(url, "", test_request);
        return true;
    } catch (...) {
        return false;
    }
}

Auth Auth::fromCookie(const QString& cookie_path) {
    QFile file(cookie_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return Auth("");
    }
    
    QTextStream stream(&file);
    QString cookie_content = stream.readAll().trimmed();
    
    if (cookie_content.isEmpty()) {
        return Auth("");
    }
    
    // Convert to Basic auth header
    QByteArray cookie_bytes = cookie_content.toUtf8();
    QByteArray base64 = cookie_bytes.toBase64();
    std::string auth_header = "Basic " + base64.toStdString();
    
    return Auth(auth_header);
}
