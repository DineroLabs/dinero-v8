#pragma once
#include "daemon/services/assumeutxo_lifecycle.h"
namespace {
// Uses the actual isolated service, validated coinbase history, encrypted wallet
// and payment owner. This installs a synthetic snapshot storage layout for
// refusal coverage; the separate HTTP late-import fixture exercises LoadSnapshot.
class SnapshotPaymentSource : public WalletBatchRpc {
protected:
    std::vector<dinero::ChainDB::PreBaseCoinRecord> frozen;
    std::vector<uint8_t> checkpoint;
    void SetUp() override {
        WalletBatchRpc::SetUp();if(HasFatalFailure())return;
        for(const auto& [point,entry]:payment_source->replay.ProvenUtxos()) {
            dinero::Coin coin;coin.amount=entry.value.GetUna();coin.script_pubkey=util::hex(entry.scriptPubKey);
            coin.height=entry.height;coin.coinbase=entry.isCoinbase;coin.is_confidential=entry.is_confidential;coin.commitment=entry.commitment;
            frozen.push_back({point.txid.AsUint256(),point.vout,coin});
        }
        checkpoint=payment_source->replay.Forest()->serialize();
        auto& db=payment_source->db;const auto& token=payment_source->token;
        ASSERT_EQ(db.replacePreBaseCoins(token,payment_source->tip.hash,101,frozen),dinero::Status::Ok);
        ASSERT_EQ(db.putUtreexoCheckpointWithChecksum(token,101,checkpoint),dinero::Status::Ok);
        ASSERT_EQ(db.setTip(token,payment_source->blocks[0].GetHash(),0,dinero::arith_uint256{0}),dinero::Status::Ok);
        ASSERT_TRUE(chain->GetAssumeUtxoLifecycle()->OnSnapshotLoaded(payment_source->tip.hash,101));
    }
    auto Pay() {
        auto use=dinero::ChainstateService::AcquireWalletIndexUse(chain);
        auto& wallet=service->get();auto input=unsigned_tx({hd});
        input.tx.vout[0].scriptPubKey=modern.spk;
        return chain->signAndStageWalletPayment(wallet,dinero::CaptureWalletSigningIdentity(wallet,"owner"),
            input,{modern_address,99000,"snapshot source fixture"});
    }
    void Refused(const std::string& reason) {
        const auto result=Pay();EXPECT_FALSE(result.success);EXPECT_NE(result.error.find(reason),std::string::npos)<<result.error;
        EXPECT_TRUE(result.signed_tx.tx.vin.empty());EXPECT_TRUE(service->get().getPendingPayments().empty());
        EXPECT_EQ(count(service->get().getCurrentDatabase(),"transactions"),0);
        EXPECT_TRUE(service->get().getLockedUTXOs().empty());
    }
};
TEST_F(SnapshotPaymentSource, ExactBaseRetainsSignedPayment) {
    const auto result=Pay();ASSERT_TRUE(result.success)<<result.error;verify(result.signed_tx.tx,{hd});
    const auto payments=service->get().getPendingPayments();ASSERT_EQ(payments.size(),1u);
    EXPECT_EQ(payments[0].signed_body,result.signed_tx.tx.Serialize(dinero::TxSerializationMode::WithWitness));
    EXPECT_TRUE(service->get().isUTXOLocked(hd.GetTxIdHex(),hd.vout));
    const auto durable=payment_source->db.getTip();ASSERT_TRUE(durable.ok());EXPECT_EQ(durable->height,0);
}
TEST_F(SnapshotPaymentSource, IdentitiesAndCheckpointRefuseBeforeWalletEffects) {
    auto& db=payment_source->db;const auto& token=payment_source->token;const auto base=payment_source->tip.hash;
    const auto wrong=payment_source->blocks[1].GetHash();
    ASSERT_EQ(db.replacePreBaseCoins(token,wrong,101,frozen),dinero::Status::Ok);Refused("identities disagree");
    ASSERT_EQ(db.replacePreBaseCoins(token,base,101,frozen),dinero::Status::Ok);
    dinero::WalletBatchPaymentTestAccess::SetPaymentFixtureUtxoTip(*chain,wrong,101);Refused("identities disagree");
    dinero::WalletBatchPaymentTestAccess::SetPaymentFixtureUtxoTip(*chain,base,101);
    ASSERT_EQ(db.deleteUtreexoCheckpointWithChecksum(token,101),dinero::Status::Ok);Refused("checkpoint unavailable");
    ASSERT_EQ(db.putUtreexoCheckpointWithChecksum(token,101,checkpoint),dinero::Status::Ok);
    auto corrupt=checkpoint;corrupt.back()^=1;
    ASSERT_EQ(db.putUtreexoCheckpoint(token,101,corrupt),dinero::Status::Ok);Refused("checksum mismatch");
    dinero::consensus::UtreexoForest empty;
    ASSERT_EQ(db.putUtreexoCheckpointWithChecksum(token,101,empty.serialize()),dinero::Status::Ok);Refused("forest disagrees");
    ASSERT_EQ(db.putUtreexoCheckpointWithChecksum(token,101,checkpoint),dinero::Status::Ok);
    const auto retry=Pay();EXPECT_TRUE(retry.success)<<retry.error;
}
TEST_F(SnapshotPaymentSource, LifecycleAndOrchardRouteNeverBypass) {
    auto* lifecycle=chain->GetAssumeUtxoLifecycle();lifecycle->Disable();Refused("lifecycle unavailable");
    ASSERT_TRUE(lifecycle->OnSnapshotLoaded(payment_source->blocks[1].GetHash(),101));Refused("lifecycle unavailable");
    lifecycle->Disable();ASSERT_TRUE(lifecycle->OnSnapshotLoaded(payment_source->tip.hash,101));
    const auto previous=dinero::Params();dinero::MutableParams().orchard_activation_height=101;
    dinero::MutableParams().orchard_branch_id=0x12345678;Refused("selected and durable tips disagree");dinero::MutableParams()=previous;
    // Even an ordinary source cannot bypass the wallet owner's retained-row gate.
    auto* db=service->get().getCurrentDatabase();
    sql(db,"CREATE TABLE orchard_wallet_retained(fixture INTEGER)");
    sql(db,"INSERT INTO orchard_wallet_retained VALUES(1)");Refused("inventory unavailable");
    sql(db,"DELETE FROM orchard_wallet_retained");
    lifecycle->ForceFatal("snapshot payment fixture");Refused("lifecycle unavailable");
}
} // namespace
