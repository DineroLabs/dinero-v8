// dinero-swap-tower-intake: the door of a public watchtower
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 14).
//
//   dinero-swap-tower-intake --inbox DIR --listen 127.0.0.1:PORT
//                            [--network main|testnet|regtest]
//                            [--max-packages N] [--max-bytes N] [--workers N]
//
// It answers the three requests of wallet/swap/tower_http.h and writes verified
// packages into DIR, the inbox a dinero-swap-tower guards. It holds no key and
// never touches a chain.
//
// It speaks plain HTTP and is meant to sit behind a web server that ends TLS,
// buffers requests and limits how often one address may ask (see the runbook
// in docs/swaps). It still protects itself: one request per connection, ten
// seconds for the whole exchange, a body no larger than --max-bytes, a fixed
// number of connections at a time.
#include "wallet/swap/tower_http.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <csignal>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using namespace dinero::swap;

namespace {

struct Door {
    std::string inbox;
    TowerIntakeLimits limits;
    std::mutex store_mu;  // one package is counted and written at a time
};

void Serve(tcp::socket socket, Door& door) {
    beast::tcp_stream stream(std::move(socket));
    stream.expires_after(std::chrono::seconds(10));
    beast::error_code ec;
    beast::flat_buffer buffer;
    http::request_parser<http::string_body> parser;
    parser.header_limit(8 * 1024);
    parser.body_limit(door.limits.max_package_bytes);
    http::read(stream, buffer, parser, ec);

    TowerHttpReply reply;
    if (ec == http::error::body_limit) {
        reply = TowerHttpReply{413, "{\"error\":\"the package is too large\"}"};
    } else if (ec) {
        return;  // not a request we could read in time: nothing to say
    } else {
        const auto& request = parser.get();
        const std::string method(request.method_string());
        const std::string target(request.target());
        std::lock_guard<std::mutex> lock(door.store_mu);
        reply = HandleTowerHttpRequest(method, target, request.body(), door.inbox, door.limits,
                                       static_cast<uint32_t>(std::time(nullptr)));
        if (reply.status == 500) std::cerr << "intake: could not serve " << method << " " << target.substr(0, 80) << std::endl;
    }
    http::response<http::string_body> response(static_cast<http::status>(reply.status), 11);
    response.set(http::field::content_type, "application/json");
    response.set(http::field::cache_control, "no-store");
    response.keep_alive(false);
    response.body() = reply.body;
    response.prepare_payload();
    http::write(stream, response, ec);
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
}

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> opt{{"--max-packages", "20000"}, {"--max-bytes", "262144"}, {"--workers", "8"}};
    for (int i = 1; i + 1 < argc; i += 2) opt[argv[i]] = argv[i + 1];
    if (!opt.count("--inbox") || !opt.count("--listen")) {
        std::cerr << "usage: see tools/dinero_swap_tower_intake.cpp (--inbox DIR --listen HOST:PORT)\n";
        return 2;
    }
    Door door;
    try {
        door.inbox = opt["--inbox"];
        door.limits.max_packages = std::stoul(opt["--max-packages"]);
        door.limits.max_package_bytes = std::stoul(opt["--max-bytes"]);
        if (opt.count("--network")) {
            const std::string n = opt["--network"];
            if (n == "main") door.limits.network = SwapNetwork::Mainnet;
            else if (n == "testnet") door.limits.network = SwapNetwork::Testnet;
            else if (n == "regtest") door.limits.network = SwapNetwork::Regtest;
            else throw std::invalid_argument("--network must be main, testnet or regtest");
        }
        const std::string listen = opt["--listen"];
        const auto colon = listen.rfind(':');
        if (colon == std::string::npos) throw std::invalid_argument("--listen must be HOST:PORT");
        const size_t workers = std::max<size_t>(1, std::min<size_t>(64, std::stoul(opt["--workers"])));

        std::signal(SIGPIPE, SIG_IGN);
        net::io_context io;
        tcp::acceptor acceptor(io, tcp::endpoint(net::ip::make_address(listen.substr(0, colon)),
                                                 static_cast<unsigned short>(std::stoul(listen.substr(colon + 1)))));
        std::cout << "intake: " << door.inbox << " behind " << acceptor.local_endpoint() << std::endl;
        std::mutex accept_mu;
        std::vector<std::thread> pool;
        for (size_t i = 0; i < workers; ++i) {
            pool.emplace_back([&] {
                for (;;) {
                    tcp::socket socket(io);
                    beast::error_code ec;
                    {
                        std::lock_guard<std::mutex> lock(accept_mu);
                        acceptor.accept(socket, ec);
                    }
                    if (ec) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }
                    try {
                        Serve(std::move(socket), door);
                    } catch (const std::exception& e) {
                        std::cerr << "intake: " << e.what() << std::endl;
                    }
                }
            });
        }
        for (auto& t : pool) t.join();
    } catch (const std::exception& e) {
        std::cerr << "intake: " << e.what() << "\n";
        return 2;
    }
    return 0;
}
