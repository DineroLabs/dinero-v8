#include <gtest/gtest.h>
#include "daemon/http_rpc_server.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
struct HttpRpcDrainTestAccess {
    static auto Active(HttpRpcServer& server) { std::lock_guard<std::mutex> lock(server.connections_mutex_); return server.active_connections_.load(std::memory_order_acquire); }
};
namespace {
struct Socket {
    int fd=-1;
    Socket():fd(::socket(AF_INET,SOCK_STREAM,0)) {if(fd<0)throw std::runtime_error("socket");}
    ~Socket(){if(fd>=0)::close(fd);}
    Socket(const Socket&)=delete;
};
uint16_t Port() {
    Socket reserve;sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(::bind(reserve.fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)))throw std::runtime_error("bind reservation");
    socklen_t size=sizeof(address);if(::getsockname(reserve.fd,reinterpret_cast<sockaddr*>(&address),&size))throw std::runtime_error("port");
    return ntohs(address.sin_port);
}
void Connect(Socket& socket,uint16_t port) {
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
    timeval timeout{15,0};setsockopt(socket.fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int enabled=1;setsockopt(socket.fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled));
#endif
    if(::connect(socket.fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)))throw std::runtime_error("connect");
}
std::string Request(uint16_t port,const char* method) {
    Socket socket;Connect(socket,port);
    std::string body="{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\""+std::string(method)+"\",\"params\":[]}";
    std::string request="POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    size_t sent=0;while(sent<request.size()) {
        int flags=0;
#ifdef MSG_NOSIGNAL
        flags=MSG_NOSIGNAL;
#endif
        auto n=::send(socket.fd,request.data()+sent,request.size()-sent,flags);if(n<=0)throw std::runtime_error("send");sent+=size_t(n);
    }
    std::string response;char bytes[4096];for(;;){auto n=::recv(socket.fd,bytes,sizeof(bytes),0);if(n<=0)break;response.append(bytes,size_t(n));}return response;
}
bool Idle(HttpRpcServer& server) {
    auto deadline=std::chrono::steady_clock::now()+5s;
    while(HttpRpcDrainTestAccess::Active(server) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(5ms);
    return HttpRpcDrainTestAccess::Active(server)==0;
}
TEST(RpcHandlerDrain, WaitsForLiveHandlerBeforeReturning) {
    const auto port=Port();HttpRpcServer server("127.0.0.1",port);
    std::promise<void> entered;std::mutex mutex;std::condition_variable ready;bool release=false;
    server.register_method("test.wait",[&](const Json::Value&){entered.set_value();std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return release;});return Json::Value("finished");});
    server.start();auto client=std::async(std::launch::async,[&]{return Request(port,"test.wait");});
    auto reached=entered.get_future().wait_for(5s);EXPECT_EQ(reached,std::future_status::ready);
    auto stopped=std::async(std::launch::async,[&]{server.stop();});
    // Observe only normal completion timing. Even a copied old implementation
    // stays alive until the benign handler and connection have finished.
    const auto early=stopped.wait_for(8s);EXPECT_EQ(early,std::future_status::timeout);
    {std::lock_guard<std::mutex> lock(mutex);release=true;}ready.notify_all();
    EXPECT_EQ(stopped.wait_for(5s),std::future_status::ready);stopped.get();client.get();EXPECT_TRUE(Idle(server));EXPECT_FALSE(server.is_running());
}
TEST(RpcHandlerDrain, InterruptsIdleSocketsAndRestarts) {
    const auto port=Port();HttpRpcServer server("127.0.0.1",port);server.start();
    {Socket idle;Connect(idle,port);auto deadline=std::chrono::steady_clock::now()+3s;
     while(!HttpRpcDrainTestAccess::Active(server) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(5ms);
     EXPECT_EQ(HttpRpcDrainTestAccess::Active(server),1u);auto before=std::chrono::steady_clock::now();server.stop();
     EXPECT_LT(std::chrono::steady_clock::now()-before,3s);}
    EXPECT_TRUE(Idle(server));server.register_method("test.ping",[](const Json::Value&){return Json::Value("pong");});server.start();
    EXPECT_NE(Request(port,"test.ping").find("pong"),std::string::npos);server.stop();EXPECT_TRUE(Idle(server));
}
TEST(RpcHandlerDrain, RefusesHandlerLifecycleAndDrainsExceptions) {
    const auto port=Port();HttpRpcServer server("127.0.0.1",port);std::atomic<bool> refused=false,start_refused=false;
    server.register_method("test.self",[&](const Json::Value&){try{server.start();}catch(const std::logic_error&){start_refused=true;}try{server.stop();}catch(const std::logic_error&){refused=true;}return Json::Value("self-finished");});
    server.register_method("test.throw",[](const Json::Value&)->Json::Value{throw std::runtime_error("intentional handler exception");});
    server.start();EXPECT_NE(Request(port,"test.self").find("self-finished"),std::string::npos);EXPECT_TRUE(refused);EXPECT_TRUE(start_refused);EXPECT_TRUE(Idle(server));
    // A copied old stop may have ended the listener. Restart only after the
    // connection is observed complete, keeping the server alive throughout.
    if(!server.is_running())server.start();
    EXPECT_NE(Request(port,"test.throw").find("error"),std::string::npos);server.stop();EXPECT_TRUE(Idle(server));
}
}
