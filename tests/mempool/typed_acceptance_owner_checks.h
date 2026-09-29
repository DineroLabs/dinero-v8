// Patched-path immutable observer ownership only. No unsafe-original controls.
namespace {
class TypedAcceptanceOwner : public MempoolTypedPackage {
protected:
    struct Guard final : Mempool::ChainstateReadGuard {
        bool& held;
        explicit Guard(bool& value):held(value){EXPECT_FALSE(held);held=true;}
        ~Guard() override {held=false;}
    };
};
TEST_F(TypedAcceptanceOwner, SignedAdmissionRetainsTypedBodyAfterLocksAndReplacement) {
    auto tx=spend(fund(71),1000000,1000);const auto expected=tx.Serialize();const auto id=tx.GetTxid();
    Mempool pool(&db,&coins);bool held=false;pool.setChainstateReadGuardFactory([&]{return std::make_unique<Guard>(held);});
    unsigned accepted=0,broadcast=0;MempoolTransaction retained;
    auto capture=std::make_shared<int>(19);std::weak_ptr<int> weak=capture;
    pool.setTxBodyAcceptedCallback([&,capture](const MempoolTransaction& body){
        EXPECT_FALSE(held);if(held)return;++accepted;EXPECT_EQ(*capture,19);EXPECT_EQ(body.Serialize(),expected);EXPECT_EQ(body.GetTxid(),id);EXPECT_FALSE(body.IsOrchard());EXPECT_TRUE(pool.hasTransaction(id.AsUint256()));
        retained=body;tx.vout[0].value=AmountUna::Una(1);pool.setTxBodyAcceptedCallback({});EXPECT_FALSE(weak.expired());
    });capture.reset();
    pool.setTxBroadcastCallback([&](const uint256& observed){EXPECT_FALSE(held);EXPECT_EQ(observed,id.AsUint256());++broadcast;});
    ASSERT_TRUE(pool.submitTransaction(tx,"typed-observer",true).accepted());EXPECT_EQ(accepted,1U);EXPECT_EQ(broadcast,1U);EXPECT_TRUE(weak.expired());EXPECT_FALSE(held);
    const auto entry=pool.getMempoolEntry(id.AsUint256());ASSERT_TRUE(entry);EXPECT_EQ(entry->tx.Serialize(),expected);ASSERT_TRUE(pool.removeTransaction(id.AsUint256()));EXPECT_EQ(retained.Serialize(),expected);
}
TEST_F(TypedAcceptanceOwner, PreflightAndObserverFailureKeepActualAdmissionOutcome) {
    const auto tx=spend(fund(72),1000000,1000);Mempool pool(&db,&coins);bool held=false;pool.setChainstateReadGuardFactory([&]{return std::make_unique<Guard>(held);});
    unsigned calls=0,broadcasts=0;MempoolTransaction retained;
    pool.setTxBodyAcceptedCallback([&](const MempoolTransaction& body){EXPECT_FALSE(held);++calls;retained=body;throw std::runtime_error("fixture ambiguous observer outcome");});
    pool.setTxBroadcastCallback([&](const uint256&){++broadcasts;});
    ASSERT_TRUE(pool.submitTransactionTestOnly(tx,"typed-observer").accepted());EXPECT_EQ(calls,0U);EXPECT_EQ(pool.size(),0U);
    EXPECT_THROW(pool.submitTransaction(tx,"typed-observer",true),std::runtime_error);EXPECT_FALSE(held);EXPECT_EQ(calls,1U);EXPECT_EQ(broadcasts,0U);EXPECT_TRUE(pool.hasTransaction(tx.GetTxid().AsUint256()));EXPECT_EQ(retained.Serialize(),tx.Serialize());EXPECT_TRUE(pool.isOutputSpentInMempool({tx.vin[0].prevout.txid,tx.vin[0].prevout.vout}));
}
TEST_F(TypedAcceptanceOwner, CaptureFailureAndLegacyCompatibilityPreserveBoundary) {
    struct CopyRefusal {
        std::shared_ptr<bool> armed;unsigned* calls;
        CopyRefusal(std::shared_ptr<bool> a,unsigned* c):armed(std::move(a)),calls(c){}
        CopyRefusal(const CopyRefusal& other):armed(other.armed),calls(other.calls){if(*armed)throw std::runtime_error("fixture observer copy refusal");}
        void operator()(const MempoolTransaction&) const {++*calls;}
    };
    const auto tx=spend(fund(73),1000000,1000);Mempool pool(&db,&coins);auto armed=std::make_shared<bool>(false);unsigned typed=0,legacy=0;
    pool.setTxBodyAcceptedCallback(CopyRefusal(armed,&typed));*armed=true;
    EXPECT_THROW(pool.submitTransaction(tx,"typed-observer",false),std::runtime_error);EXPECT_EQ(pool.size(),0U);EXPECT_EQ(typed,0U);EXPECT_FALSE(pool.isOutputSpentInMempool({tx.vin[0].prevout.txid,tx.vin[0].prevout.vout}));*armed=false;
    pool.setTxAcceptedCallback([&](const Transaction& body){++legacy;EXPECT_EQ(body.Serialize(),tx.Serialize());});
    ASSERT_TRUE(pool.submitTransaction(tx,"legacy-observer",false).accepted());EXPECT_EQ(legacy,1U);EXPECT_EQ(typed,0U);
    EXPECT_THROW(MempoolAcceptanceObserver{}.Prepare(MempoolTransaction{}),std::invalid_argument);
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(TypedAcceptanceOwner, OrchardCapabilityRefusesLegacyAndRetainsExactRepresentation) {
    std::ifstream file(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES)/"candidate-envelope.bin",std::ios::binary);ASSERT_TRUE(file.good());
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
    auto body=MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::DecodeExact(bytes));const auto wire=body.Serialize();const auto id=body.GetTxid();const auto witness=body.GetWtxid();const auto inputs=body.Inputs();const auto nullifiers=body.OrchardNullifiers();
    unsigned legacy=0,typed=0;auto compatibility=MempoolAcceptanceObserver::ForHistorical([&](const Transaction&){++legacy;});
    EXPECT_THROW(compatibility.Prepare(body),std::logic_error);EXPECT_EQ(legacy,0U);
    auto owner=std::make_shared<int>(23);std::weak_ptr<int> weak=owner;
    auto observer=MempoolAcceptanceObserver::ForBody([&,owner](const MempoolTransaction& captured){++typed;EXPECT_EQ(*owner,23);EXPECT_TRUE(captured.IsOrchard());EXPECT_EQ(captured.Serialize(),wire);EXPECT_EQ(captured.GetTxid(),id);EXPECT_EQ(captured.GetWtxid(),witness);EXPECT_EQ(captured.Inputs(),inputs);EXPECT_EQ(captured.OrchardNullifiers(),nullifiers);});owner.reset();
    auto notification=observer.Prepare(body);observer=MempoolAcceptanceObserver{};body=MempoolTransaction{};EXPECT_FALSE(weak.expired());notification();EXPECT_EQ(typed,1U);EXPECT_EQ(legacy,0U);notification={};EXPECT_TRUE(weak.expired());
    // Structural observer representation only: no pool insertion/admission.
}
#endif
}
