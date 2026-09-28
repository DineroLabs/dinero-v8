#include <gtest/gtest.h>
#include "daemon/http_rpc_server.h"
#include "daemon/daemon_context.h"
#include "mining/block_assembler.h"
#include "daemon/services/mempool_service.h"
#include "rpc/rpc_registry.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/chainparams.h"
#include "storage/chain_db.h"
#include <filesystem>
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
namespace dinero {
class MempoolServiceOwnerTestPeer {
public:
    static std::shared_ptr<MempoolService> Published(ChainDB& db, consensus::ConsensusUTXOSet& coins) {
        auto service=std::make_shared<MempoolService>();
        service->mempool_=std::make_unique<Mempool>(&db,&coins);
        service->accepting_=true;service->started_=true;return service;
    }
};
}
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
// Only patched-path invariants; no unsafe original or shutdown race is run.
class RpcPoolOwner : public ::testing::Test {
protected:
    dinero::ChainDB db;
    dinero::consensus::ConsensusUTXOSet coins;
    std::filesystem::path root;
    std::shared_ptr<dinero::MempoolService> pool;
    DaemonContext context;
    RpcRegistry registry;
    void SetUp() override {
        dinero::SelectParams(dinero::Chain::REGTEST);
        root=std::filesystem::temp_directory_path()/("rpc_pool_owner_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_EQ(db.init(root/"chain"),dinero::Status::Ok);
        pool=dinero::MempoolServiceOwnerTestPeer::Published(db,coins);
        context.mempool=pool;
    }
    void TearDown() override {
        if(pool)pool->Stop();context.mempool.reset();pool.reset();db.close();std::filesystem::remove_all(root);
    }
    void Wire(HttpRpcServer& server) {
        server.set_daemon_context(&context);server.set_rpc_registry(&registry);
    }
};
TEST_F(RpcPoolOwner, HoldsPoolThroughUnifiedHandlerAndAlias) {
    const auto port=Port();HttpRpcServer server("127.0.0.1",port);Wire(server);
    std::atomic<unsigned> calls{0};std::atomic<bool> correct{true};
    ASSERT_TRUE(registry.registerHandler("test.pool",[&](const ExecutionContext& ctx,const din::Json&) {
        bool refused=false;try {pool->Stop();}catch(const std::logic_error&){refused=true;}
        if(!refused || !ctx.mempool_v2 || ctx.mempool_v2!=&pool->mempool())correct=false;
        if(ctx.mempool_v2 && ctx.mempool_v2->size()!=0)correct=false;
        auto nested=dinero::MempoolService::AcquirePoolUse(pool);
        if(&nested->Pool()!=ctx.mempool_v2)correct=false;
        ++calls;return din::Json("pool-owned");
    }));
    registry.registerAlias("test.poolalias","test.pool");server.start();
    EXPECT_NE(Request(port,"test.pool").find("pool-owned"),std::string::npos);
    EXPECT_NE(Request(port,"test.poolalias").find("pool-owned"),std::string::npos);
    server.stop();EXPECT_TRUE(Idle(server));EXPECT_TRUE(correct.load());EXPECT_EQ(calls.load(),2U);
    EXPECT_NO_THROW(pool->Stop());EXPECT_FALSE(pool->isInitialized());
}
TEST_F(RpcPoolOwner, ExceptionAndReturnedErrorReleaseOwner) {
    const auto port=Port();HttpRpcServer server("127.0.0.1",port);Wire(server);
    std::atomic<unsigned> owned{0};
    ASSERT_TRUE(registry.registerHandler("test.throwpool",[&](const ExecutionContext& ctx,const din::Json&)->din::Json {
        try {pool->Stop();}catch(const std::logic_error&){if(ctx.mempool_v2)++owned;}
        throw std::runtime_error("intentional owned handler failure");
    }));
    ASSERT_TRUE(registry.registerHandler("test.errorpool",[&](const ExecutionContext& ctx,const din::Json&) {
        try {pool->Stop();}catch(const std::logic_error&){if(ctx.mempool_v2)++owned;}
        din::Json result;result["error"]="intentional returned handler error";return result;
    }));
    server.start();
    EXPECT_NE(Request(port,"test.throwpool").find("intentional owned handler failure"),std::string::npos);
    EXPECT_NE(Request(port,"test.errorpool").find("intentional returned handler error"),std::string::npos);
    server.stop();EXPECT_TRUE(Idle(server));EXPECT_EQ(owned.load(),2U);
    EXPECT_NO_THROW(pool->Stop());EXPECT_FALSE(pool->isInitialized());
}
TEST_F(RpcPoolOwner, ClosedServiceRefusesBeforeHandler) {
    const auto port=Port();HttpRpcServer server("127.0.0.1",port);Wire(server);
    std::atomic<unsigned> calls{0};
    ASSERT_TRUE(registry.registerHandler("test.closedpool",[&](const ExecutionContext&,const din::Json&) {
        ++calls;return din::Json("must-not-run");
    }));
    pool->Stop();server.start();const auto response=Request(port,"test.closedpool");
    EXPECT_NE(response.find("Mempool service is unavailable"),std::string::npos);
    EXPECT_EQ(response.find("must-not-run"),std::string::npos);
    server.stop();EXPECT_TRUE(Idle(server));EXPECT_EQ(calls.load(),0U);EXPECT_FALSE(pool->isInitialized());
}
}
