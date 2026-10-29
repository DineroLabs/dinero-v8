#include <gtest/gtest.h>
#include "daemon/http_rpc_server.h"
#include "daemon/rpc_auth.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/config_service.h"
#include "wallet/wallet_manager.h"
#include "wallet/utxo_index.h"
#include "consensus/chainparams.h"
#include "rpc/rpc_registry.h"
#include "common/ilogger.h"
#include <filesystem>
#include <cstdlib>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

struct HttpRpcRequestTestAccess {
    static Json::Value Dispatch(HttpRpcServer& server,const std::string& method){
        Json::Value request;request["jsonrpc"]="2.0";request["id"]=71;
        request["method"]=method;return server.process_rpc_call(request);
    }
    // A captured refusal exercises the real frame reader without changing the
    // token bucket, limits, or production admission policy.
    static void RefusedConnection(HttpRpcServer& server, int fd) {
        server.configure_client_socket(fd);
        server.handle_connection(fd, false);
    }
};
namespace dinero {
struct ReopenRpcGateTestAccess {
    static auto Recover(const std::shared_ptr<ReopenRpcGate>& gate){return ReopenRpcGate::Recover(gate);}
    static void Hold(ReopenRpcGate::Pause& pause){pause.HoldClosed();}
    static void Resume(ReopenRpcGate::Pause& pause){pause.Resume();}
    static void Epoch(ReopenRpcGate& gate,uint64_t epoch){gate.epoch_.store(epoch);}
};
}
namespace {
struct Socket {
    int fd;
    Socket() : fd(::socket(AF_INET, SOCK_STREAM, 0)) { if (fd < 0) throw std::runtime_error("socket"); }
    explicit Socket(int value) : fd(value) {}
    ~Socket() { if (fd >= 0) ::close(fd); }
    Socket(const Socket&) = delete;
};
void Configure(int fd) {
    timeval timeout{15, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) throw std::runtime_error("receive timeout");
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) throw std::runtime_error("send timeout");
#ifdef SO_NOSIGPIPE
    int one = 1; if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one))) throw std::runtime_error("signal option");
#endif
}
uint16_t Port() {
    Socket reservation; sockaddr_in address{};
    address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (::bind(reservation.fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))) throw std::runtime_error("reserve port");
    socklen_t length=sizeof(address);
    if (::getsockname(reservation.fd,reinterpret_cast<sockaddr*>(&address),&length)) throw std::runtime_error("read port");
    return ntohs(address.sin_port);
}
void Connect(Socket& socket,uint16_t port) {
    Configure(socket.fd); sockaddr_in address{}; address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons(port);
    if (::connect(socket.fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))) throw std::runtime_error("connect");
}
void Send(int fd,const std::string& bytes) {
    size_t offset=0;
    while(offset<bytes.size()) {
        int flags=0;
#ifdef MSG_NOSIGNAL
        flags=MSG_NOSIGNAL;
#endif
        const auto count=::send(fd,bytes.data()+offset,bytes.size()-offset,flags);
        if(count<=0) throw std::runtime_error("send");
        offset+=static_cast<size_t>(count);
    }
}
std::string Read(int fd) {
    std::string out; char bytes[4096];
    for (;;) {
        const auto count=::recv(fd,bytes,sizeof(bytes),0);
        if(count==0) return out;
        if(count<0) throw std::runtime_error("receive");
        out.append(bytes,static_cast<size_t>(count));
    }
}
std::string Body(const std::string& value, const std::string& method="test.frame") {
    Json::Value request;request["jsonrpc"]="2.0";request["id"]=17;
    request["method"]=method;request["params"].append(value);
    return Json::writeString(Json::StreamWriterBuilder{},request);
}
void SendFrame(int fd,const std::string& body,const std::string& authorization={}) {
    // Benign ordinary request in separate writes, including a header name split
    // across writes and a body larger than a receive buffer.
    Send(fd,"POST / HTTP/1.1\r\nHost: localhost\r\nCon");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Send(fd,"tent-LenGth: "+std::to_string(body.size())+" \t\r\n"+authorization+"\r\n");
    const auto split=body.size()/2;
    Send(fd,body.substr(0,split));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Send(fd,body.substr(split));
}
Json::Value Response(const std::string& raw,int status) {
    EXPECT_EQ(raw.find("HTTP/1.1 "+std::to_string(status)),0U);
    const auto end=raw.find("\r\n\r\n");
    if(end==std::string::npos) throw std::runtime_error("response headers");
    const auto start=raw.find("Content-Length: ");
    if(start==std::string::npos || start>=end) throw std::runtime_error("response length");
    const auto count=std::stoull(raw.substr(start+16));
    EXPECT_EQ(count,raw.size()-end-4);
    Json::Value value;Json::CharReaderBuilder builder;std::string error;
    auto reader=std::unique_ptr<Json::CharReader>(builder.newCharReader());
    if(!reader->parse(raw.data()+end+4,raw.data()+raw.size(),&value,&error)) throw std::runtime_error("response JSON");
    return value;
}
TEST(RpcRequestFraming, SegmentedHeadersAndBodyReachHandlerExactlyOnce) {
    const auto port=Port();HttpRpcServer server("127.0.0.1",port);
    std::atomic<unsigned> calls{0};
    server.register_method("test.frame",[&](const Json::Value& params){++calls;return params[0];});
    server.start();Socket client;Connect(client,port);
    const std::string value(20000,'x');SendFrame(client.fd,Body(value));
    const auto response=Response(Read(client.fd),200);
    EXPECT_EQ(response["result"].asString(),value);EXPECT_EQ(response["id"].asInt(),17);
    server.stop();EXPECT_EQ(calls.load(),1U);
}
void RequireRestarting(const Json::Value& response){
    EXPECT_EQ(response["id"].asInt(),71);EXPECT_FALSE(response.isMember("result"));
    ASSERT_TRUE(response["error"].isObject());EXPECT_EQ(response["error"]["code"].asInt(),-28);
    EXPECT_EQ(response["error"]["message"].asString(),"Core restarting; retry request");
    EXPECT_TRUE(response["error"]["data"]["retryable"].asBool());
    EXPECT_EQ(response["error"]["data"]["reason"].asString(),"restarting");
}
TEST(RpcReopenPause, PausedQueuesNeverReachHandlersAndAbortInvalidatesOldEpoch){
    DaemonContext context;HttpRpcServer server("127.0.0.1",0);server.set_daemon_context(&context);
    unsigned calls=0;const auto gate=context.reopen_rpc_gate;const auto old=gate->Snapshot();
    for(const auto* method:{"wallet.orchard.queueshield","wallet.orchard.queuespend","wallet.orchard.queuesend"})
        server.register_method(method,[&](const Json::Value&){++calls;Json::Value refused;refused["accepted"]=false;return refused;});
    {auto pause=dinero::ReopenRpcGate::Begin(gate);
     EXPECT_ANY_THROW(dinero::ReopenRpcGate::Begin(gate));
     for(const auto* method:{"wallet.orchard.queueshield","wallet.orchard.queuespend","wallet.orchard.queuesend"})
         RequireRestarting(HttpRpcRequestTestAccess::Dispatch(server,method));
     EXPECT_EQ(calls,0u);}
    EXPECT_FALSE(gate->Allows(old));EXPECT_TRUE(gate->Allows(gate->Snapshot()));
    const auto result=HttpRpcRequestTestAccess::Dispatch(server,"wallet.orchard.queueshield");
    EXPECT_FALSE(result["result"]["accepted"].asBool());EXPECT_EQ(calls,1u);
}
TEST(RpcReopenPause, CompletePauseInsideDispatchSuppressesBothRefusalAndSuccess){
    DaemonContext context;HttpRpcServer server("127.0.0.1",0);server.set_daemon_context(&context);
    bool refuse=true;unsigned calls=0;
    server.register_method("test.overlap",[&](const Json::Value&){
        ++calls;{auto pause=dinero::ReopenRpcGate::Begin(context.reopen_rpc_gate);}
        Json::Value result;result["accepted"]=!refuse;if(refuse)result["error"]="must not escape";return result;
    });
    RequireRestarting(HttpRpcRequestTestAccess::Dispatch(server,"test.overlap"));refuse=false;
    RequireRestarting(HttpRpcRequestTestAccess::Dispatch(server,"test.overlap"));EXPECT_EQ(calls,2u);
    EXPECT_TRUE(context.reopen_rpc_gate->Allows(context.reopen_rpc_gate->Snapshot()));
}
TEST(RpcReopenPause, AbandonedCommittedTokenCanBeRecoveredButLiveTokenCannotBeStolen){
    using Gate=dinero::ReopenRpcGate;using Access=dinero::ReopenRpcGateTestAccess;
    const auto gate=std::make_shared<Gate>();const auto original=gate->Snapshot();
    EXPECT_ANY_THROW(Access::Recover(gate));EXPECT_EQ(gate->Snapshot(),original);
    auto token=Gate::Begin(gate);Access::Hold(*token);const auto owned=gate->Snapshot();
    EXPECT_ANY_THROW(Access::Recover(gate));EXPECT_EQ(gate->Snapshot(),owned);
    EXPECT_ANY_THROW(Gate::Begin(gate));EXPECT_FALSE(gate->Allows(owned));
    token.reset();const auto abandoned=gate->Snapshot();EXPECT_GT(abandoned,owned);EXPECT_FALSE(gate->Allows(abandoned));
    auto recovered=Access::Recover(gate);const auto reclaimed=gate->Snapshot();EXPECT_GT(reclaimed,abandoned);
    EXPECT_ANY_THROW(Access::Recover(gate));EXPECT_FALSE(gate->Allows(reclaimed));
    recovered.reset(); // An abandoned recovered receipt also stays CLOSED.
    EXPECT_FALSE(gate->Allows(gate->Snapshot()));
    recovered=Access::Recover(gate);Access::Resume(*recovered);
    EXPECT_TRUE(gate->Allows(gate->Snapshot()));
    for(const auto stale:{original,owned,abandoned,reclaimed})EXPECT_FALSE(gate->Allows(stale));
}
TEST(RpcReopenPause, UnownedLatchStillReturnsRestartingWithoutDispatch){
    using Access=dinero::ReopenRpcGateTestAccess;DaemonContext context;
    HttpRpcServer server("127.0.0.1",0);server.set_daemon_context(&context);unsigned calls=0;
    server.register_method("wallet.orchard.queuesend",[&](const Json::Value&){++calls;return Json::Value(false);});
    {auto pause=dinero::ReopenRpcGate::Begin(context.reopen_rpc_gate);Access::Hold(*pause);}
    RequireRestarting(HttpRpcRequestTestAccess::Dispatch(server,"wallet.orchard.queuesend"));EXPECT_EQ(calls,0u);
    auto pause=Access::Recover(context.reopen_rpc_gate);
    RequireRestarting(HttpRpcRequestTestAccess::Dispatch(server,"wallet.orchard.queuesend"));EXPECT_EQ(calls,0u);
    Access::Resume(*pause);EXPECT_FALSE(HttpRpcRequestTestAccess::Dispatch(server,"wallet.orchard.queuesend")["result"].asBool());
    EXPECT_EQ(calls,1u);
}
TEST(RpcReopenPause, ExhaustedGenerationNeverWrapsOrReopens){
    using Access=dinero::ReopenRpcGateTestAccess;const auto gate=std::make_shared<dinero::ReopenRpcGate>();
    const auto maximum=std::numeric_limits<uint64_t>::max();
    Access::Epoch(*gate,maximum-3);EXPECT_ANY_THROW(dinero::ReopenRpcGate::Begin(gate));EXPECT_EQ(gate->Snapshot(),maximum-3);
    Access::Epoch(*gate,maximum-4);EXPECT_ANY_THROW(Access::Recover(gate));EXPECT_EQ(gate->Snapshot(),maximum-4);
    EXPECT_FALSE(gate->Allows(gate->Snapshot()));EXPECT_FALSE(gate->Allows(0));
}
TEST(RpcRequestFraming, CapturedRateRefusalConsumesOneFrameWithoutDispatch) {
    HttpRpcServer server("127.0.0.1",0);std::atomic<unsigned> calls{0};
    server.register_method("test.frame",[&](const Json::Value&){++calls;return Json::Value("unexpected");});
    int pair[2];ASSERT_EQ(::socketpair(AF_UNIX,SOCK_STREAM,0,pair),0);
    Socket client(pair[0]),owned(pair[1]);Configure(client.fd);
    auto handler=std::async(std::launch::async,[&]{HttpRpcRequestTestAccess::RefusedConnection(server,owned.fd);});
    SendFrame(client.fd,Body(std::string(20000,'r')));
    const auto response=Response(Read(client.fd),429);handler.get();
    EXPECT_TRUE(response["error"].isObject());EXPECT_EQ(response["error"]["code"].asInt(),-32000);
    EXPECT_TRUE(response["id"].isNull());EXPECT_EQ(calls.load(),0U);
}
TEST(RpcRequestFraming, AuthenticationStillGatesCompleteRequests) {
    const auto port=Port();HttpRpcServer server("127.0.0.1",port);
    auto auth=std::make_shared<RpcAuth>("/unused-rpc-framing-fixture");
    auth->set_static_credentials("test","secret");server.set_auth(auth);
    std::atomic<unsigned> calls{0};server.register_method("test.frame",[&](const Json::Value& params){++calls;return params[0];});server.start();
    {Socket client;Connect(client,port);SendFrame(client.fd,Body("unauthenticated"));Response(Read(client.fd),401);}
    EXPECT_EQ(calls.load(),0U);
    {Socket client;Connect(client,port);SendFrame(client.fd,Body("authenticated"),"Authorization: Basic dGVzdDpzZWNyZXQ=\r\n");
     EXPECT_EQ(Response(Read(client.fd),200)["result"].asString(),"authenticated");}
    {Socket client;Connect(client,port);SendFrame(client.fd,Body("ordinary header spacing"),"aUtHoRiZaTiOn:\tBasic dGVzdDpzZWNyZXQ= \t\r\n");
     EXPECT_EQ(Response(Read(client.fd),200)["result"].asString(),"ordinary header spacing");}
    server.stop();EXPECT_EQ(calls.load(),2U);
}
struct WalletLogger final:dinero::ILogger {
    void log(dinero::LogLevel,const std::string&) override {}
    void info(const std::string&) override {} void error(const std::string&) override {}
    void debug(const std::string&) override {} void warning(const std::string&) override {}
    void setLogLevel(dinero::LogLevel) override {} void setLogFile(const std::string&) override {}
    void shutdown() override {}
};
struct WalletFixture {
    dinero::ChainParams previous=dinero::Params();
    WalletLogger logger;DaemonContext context;RpcRegistry registry;
    std::filesystem::path root;std::unique_ptr<dinero::UTXOIndex> index;
    std::shared_ptr<dinero::WalletService> service;
    WalletFixture() {
        dinero::SelectParams(dinero::Chain::REGTEST);
        char path[]="/tmp/dinero-rpc-wallet-owner-XXXXXX";
        if(!mkdtemp(path))throw std::runtime_error("fixture directory");root=path;
        auto config=std::make_shared<dinero::ConfigService>();config->Set("datadir",root.string());
        context.config=config;context.logger_interface=&logger;
        service=std::make_shared<dinero::WalletService>();context.wallet=service;
        if(!service->Init(context))throw std::runtime_error("actual wallet Init");
        index=std::make_unique<dinero::UTXOIndex>((root/"index.db").string());
        if(!index->Initialize())throw std::runtime_error("actual wallet index");
        auto use=dinero::WalletService::AcquireWalletUse(service);
        use->Wallet().setUTXOIndex(index.get());use->Wallet().create("rpc-owned");
    }
    ~WalletFixture() {
        if(service)service->Stop();context.wallet.reset();service.reset();index.reset();
        dinero::MutableParams()=previous;std::filesystem::remove_all(root);
    }
    void Wire(HttpRpcServer& server){server.set_daemon_context(&context);server.set_rpc_registry(&registry);}
};
Json::Value Call(uint16_t port,const std::string& method) {
    Socket client;Connect(client,port);SendFrame(client.fd,Body("payload",method));return Response(Read(client.fd),200);
}
TEST(RpcRequestFraming, UnifiedHandlerRetainsWalletAndAlias) {
    WalletFixture f;const auto port=Port();HttpRpcServer server("127.0.0.1",port);f.Wire(server);
    std::atomic<unsigned> calls{0};std::atomic<bool> owned{true};
    ASSERT_TRUE(f.registry.registerHandler("test.walletowner",[&](const ExecutionContext& ctx,const din::Json&){
        bool refused=false;try{f.service->Stop();}catch(const std::logic_error&){refused=true;}
        auto nested=dinero::WalletService::AcquireWalletUse(f.service);
        if(!refused || ctx.wallet_manager!=&nested->Wallet() || nested->Wallet().getCurrentWalletName()!="rpc-owned")owned=false;
        ++calls;return din::Json("wallet-owned");
    }));
    f.registry.registerAlias("test.walletalias","test.walletowner");server.start();
    EXPECT_EQ(Call(port,"test.walletowner")["result"].asString(),"wallet-owned");
    EXPECT_EQ(Call(port,"test.walletalias")["result"].asString(),"wallet-owned");
    server.stop();EXPECT_TRUE(owned.load());EXPECT_EQ(calls.load(),2U);EXPECT_NO_THROW(f.service->Stop());
}
TEST(RpcRequestFraming, ReturnedAndThrownFailuresReleaseWallet) {
    WalletFixture f;const auto port=Port();HttpRpcServer server("127.0.0.1",port);f.Wire(server);
    std::atomic<unsigned> owned{0};
    const auto inspect=[&](const ExecutionContext& ctx){
        bool refused=false;try{f.service->Stop();}catch(const std::logic_error&){refused=true;}
        auto nested=dinero::WalletService::AcquireWalletUse(f.service);
        if(refused && ctx.wallet_manager==&nested->Wallet())++owned;
    };
    ASSERT_TRUE(f.registry.registerHandler("test.throwwallet",[&](const ExecutionContext& ctx,const din::Json&)->din::Json {
        inspect(ctx);throw std::runtime_error("owned wallet handler exception");
    }));
    ASSERT_TRUE(f.registry.registerHandler("test.errorwallet",[&](const ExecutionContext& ctx,const din::Json&){
        inspect(ctx);din::Json result;result["error"]="owned wallet handler error";return result;
    }));
    server.start();EXPECT_FALSE(Call(port,"test.throwwallet")["error"].isNull());
    EXPECT_FALSE(Call(port,"test.errorwallet")["error"].isNull());server.stop();
    EXPECT_EQ(owned.load(),2U);EXPECT_NO_THROW(f.service->Stop());
}
TEST(RpcRequestFraming, ClosedWalletRefusesBeforeUnifiedHandler) {
    WalletFixture f;const auto port=Port();HttpRpcServer server("127.0.0.1",port);f.Wire(server);
    std::atomic<unsigned> calls{0};
    ASSERT_TRUE(f.registry.registerHandler("test.closedwallet",[&](const ExecutionContext&,const din::Json&){++calls;return din::Json("unexpected");}));
    server.register_method("test.legacycontrol",[](const Json::Value&){return Json::Value("control-available");});
    f.service->Stop();server.start();EXPECT_FALSE(Call(port,"test.closedwallet")["error"].isNull());
    EXPECT_EQ(Call(port,"test.legacycontrol")["result"].asString(),"control-available");
    server.stop();EXPECT_EQ(calls.load(),0U);
}
} // namespace

#include "vault_runtime_readonly_checks.h"

#include "vault_explicit_readonly_checks.h"
