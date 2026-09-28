// Patched-path ownership invariants only; no unsafe predecessor execution.
#include "daemon/tx_relay_manager.h"
#include "common/ilogger.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <stdexcept>
using namespace dinero;
namespace {
uint256 Id(uint8_t n) { uint256 id; id.data[0]=n; return id; }
void Cooldown() { std::this_thread::sleep_for(std::chrono::milliseconds(550)); }
Transaction Tx(uint8_t n) {
    Transaction tx;tx.version=2;tx.vin.emplace_back();tx.vin[0].prevout.txid=TxId(Id(n));
    tx.vout.emplace_back(AmountUna::Una(1000),std::vector<uint8_t>{0x51});return tx;
}
TEST(RelayRefreshOwner, OperationRetainsCallbackSnapshot) {
    TxRelayManager relay(nullptr);size_t first=0,next=0;
    relay.SetSendMessageCallback([&](const auto&,const auto&,const auto&){++first;relay.SetSendMessageCallback([&](const auto&,const auto&,const auto&){++next;});});
    relay.AnnounceTx(Id(1));relay.AnnounceTx(Id(2));EXPECT_EQ(first,1U);EXPECT_EQ(next,1U);
    relay.SetRetrieveTxCallback([&](const auto&,Transaction& tx){
        relay.SetRetrieveTxCallback({});relay.SetSendMessageCallback([&](const auto&,const auto&,const auto&){++first;});tx=Tx(3);return true;
    });
    relay.HandleGetData("fixture-peer",Id(3));EXPECT_EQ(next,2U);EXPECT_EQ(first,1U);
    size_t validated=0;relay.SetValidateTxCallback([&](const auto&,const auto&){++validated;relay.SetValidateTxCallback({});return true;});
    const auto tx=Tx(4);relay.HandleTx("fixture-peer",tx);EXPECT_EQ(validated,1U);EXPECT_TRUE(relay.IsTxSeen(tx.GetTxid().AsUint256()));
    size_t submitted=0;relay.SetSubmitTxCallback([&](const auto& item,const auto&){++submitted;relay.SetSubmitTxCallback({});return TxAcceptResult::Accepted(item.GetTxid().AsUint256());});
    relay.HandleTx("fixture-peer",Tx(13));EXPECT_EQ(submitted,1U);

}
TEST(RelayRefreshOwner, CompletionInsideSendDoesNotRecreateReservation) {
    TxRelayManager relay(nullptr);relay.SetCsnMode(true);size_t sent=0;const auto id=Id(5);
    relay.SetSendMessageCallback([&](const auto&,const auto& cmd,const auto& payload){
        ++sent;EXPECT_EQ(cmd,"getdata");ASSERT_EQ(payload.size(),37U);EXPECT_EQ(payload[1],1U);EXPECT_EQ(payload[4],0x50U);relay.CompleteRefresh(id);relay.RecordBridgeResponse("fixture-bridge");
        relay.RequestProofRefresh({id}); // Existing cooldown prevents recursion.
    });
    relay.RequestProofRefresh({id});EXPECT_EQ(sent,1U);Cooldown();relay.RequestProofRefresh({id});EXPECT_EQ(sent,2U);
}
TEST(RelayRefreshOwner, TipChangeDiscardsUnsentBatchAndReplacementIsOwned) {
    TxRelayManager relay(nullptr);relay.SetCsnMode(true);size_t old=0,replacement=0;
    relay.SetSendMessageCallback([&](const auto&,const auto&,const auto&){
        ++old;relay.OnTipChanged();relay.SetSendMessageCallback([&](const auto&,const auto&,const auto&){++replacement;});
    });
    relay.RequestProofRefresh({Id(6),Id(7),Id(8)});EXPECT_EQ(old,1U);EXPECT_EQ(replacement,0U);
    Cooldown();relay.RequestProofRefresh({Id(6),Id(7),Id(8)});EXPECT_EQ(replacement,3U);
}
TEST(RelayRefreshOwner, SendExceptionRetainsAttemptAndReleasesOnlyUnsent) {
    TxRelayManager relay(nullptr);relay.SetCsnMode(true);size_t attempts=0;
    relay.SetSendMessageCallback([&](const auto&,const auto&,const auto&){++attempts;throw std::runtime_error("fixture ambiguous send");});
    EXPECT_THROW(relay.RequestProofRefresh({Id(9),Id(10),Id(11)}),std::runtime_error);EXPECT_EQ(attempts,1U);
    relay.SetSendMessageCallback([&](const auto&,const auto&,const auto&){++attempts;});Cooldown();
    relay.RequestProofRefresh({Id(9),Id(10),Id(11)});EXPECT_EQ(attempts,3U); // First attempted request stays pending.
    relay.CompleteRefresh(Id(9));Cooldown();relay.RequestProofRefresh({Id(9),Id(10),Id(11)});EXPECT_EQ(attempts,4U);
}
class FailLogger final:public ILogger {
public:bool fail=false;
    void setLogLevel(LogLevel) override {} void setLogFile(const std::string&) override {} void shutdown() override {}
    void log(LogLevel,const std::string&) override {if(fail)throw std::runtime_error("fixture logger exception");}
    void debug(const std::string& s) override {log(LogLevel::DEBUG,s);} void info(const std::string& s) override {log(LogLevel::INFO,s);}
    void warning(const std::string& s) override {log(LogLevel::WARNING,s);} void error(const std::string& s) override {log(LogLevel::ERROR,s);}
};
TEST(RelayRefreshOwner, PreparedTipAbandonsOrPublishesWithoutExternalCalls) {
    FailLogger logger;TxRelayManager relay(&logger);relay.SetCsnMode(true);size_t sent=0;
    relay.SetSendMessageCallback([&](const auto&,const auto&,const auto&){++sent;});relay.RequestProofRefresh({Id(12)});
    {auto prepared=relay.PrepareTipChanged();ASSERT_TRUE(prepared);}Cooldown();relay.RequestProofRefresh({Id(12)});EXPECT_EQ(sent,1U);
    {auto prepared=relay.PrepareTipChanged();ASSERT_TRUE(prepared);logger.fail=true;prepared->PublishAfterCommit();logger.fail=false;}
    Cooldown();relay.RequestProofRefresh({Id(12)});EXPECT_EQ(sent,2U);
}
}
