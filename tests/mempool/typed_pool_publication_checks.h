// Patched-path publication and replacement policy checks. Orchard rows below
// are structural policy fixtures, not authorized or admitted transactions.
#include "policy/rbf_policy.h"
namespace {
class TypedPoolPublication : public MempoolTypedPackage {
protected:
    struct RefusingLogger final : ILogger {
        bool refuse=false;
        void info(const std::string& message) override {
            if(refuse && message.find("Added transaction to mempool:")!=std::string::npos)
                throw std::runtime_error("fixture publication observer refusal");
        }
        void log(LogLevel,const std::string& text) override {info(text);}
        void setLogLevel(LogLevel) override {}
        void setLogFile(const std::string&) override {}
        void shutdown() override {}
        void warning(const std::string&) override {}
        void error(const std::string&) override {}
        void debug(const std::string&) override {}
    };
#ifdef DINERO_TEST_ORCHARD_BODY
    static std::vector<uint8_t> fixture(const char* name) {
        std::ifstream file(std::filesystem::path(DINERO_ORCHARD_BODY_FIXTURES)/name,std::ios::binary);
        if(!file.good())throw std::runtime_error("missing typed policy fixture");
        return {std::istreambuf_iterator<char>(file),{}};
    }
    static MempoolTransaction orchardChild(const OutPoint& parent) {
        const auto source=orchard::TransactionEnvelope::DecodeExact(fixture("candidate-envelope.bin"));
        auto inputs=source.Inputs();if(inputs.empty())throw std::runtime_error("missing fixture input");
        inputs.resize(1);std::copy(parent.txid.AsUint256().begin(),parent.txid.AsUint256().end(),inputs[0].txid_wire.begin());
        inputs[0].output_index=parent.vout;
        return MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(source.LockTime(),inputs,source.Outputs(),source.ExplicitFee(),fixture("candidate-spend.bundle")));
    }
#endif
};
TEST_F(TypedPoolPublication, SignedPublicationKeepsExactOverlayAndAncestorOwners) {
    const auto parent=split(fund(81));Mempool pool(&db,&coins);pool.setRBFEnabled(true);
    ASSERT_TRUE(pool.submitTransaction(parent,"typed-publication",false).accepted());
    const auto child=spend({parent.GetTxid(),0},499500,1000);
    ASSERT_TRUE(pool.submitTransaction(child,"typed-publication",false).accepted());
    const auto entry=pool.getMempoolEntry(child.GetTxid().AsUint256());ASSERT_TRUE(entry);
    EXPECT_EQ(entry->tx.Serialize(),child.Serialize());EXPECT_EQ(entry->depends,std::vector<uint256>{parent.GetTxid().AsUint256()});
    EXPECT_EQ(entry->ancestor_fee,2000U);EXPECT_EQ(entry->ancestor_size,parent.GetSize()+child.GetSize());
    EXPECT_TRUE(pool.isOutputSpentInMempool({parent.GetTxid(),0}));
    for(size_t i=0;i<parent.vout.size();++i) {
        const auto coin=pool.getCoinsView().getCoin({parent.GetTxid(),static_cast<uint32_t>(i)});
        if(i==0){EXPECT_EQ(coin.status(),Status::NotFound);continue;}
        ASSERT_EQ(coin.status(),Status::Ok);EXPECT_EQ(coin.value().value,parent.vout[i].value);EXPECT_EQ(coin.value().scriptPubKey,parent.vout[i].scriptPubKey);EXPECT_EQ(coin.value().height,110U);EXPECT_FALSE(coin.value().isCoinbase);
    }
    const auto replacement=spend({parent.GetTxid(),0},499500,10000);
    const auto before=pool.getTransactionIds();ASSERT_TRUE(pool.submitTransactionTestOnly(replacement,"typed-publication").accepted());EXPECT_EQ(pool.getTransactionIds(),before);
    ASSERT_TRUE(pool.submitTransaction(replacement,"typed-publication",false).accepted());EXPECT_FALSE(pool.hasTransaction(child.GetTxid().AsUint256()));
    EXPECT_EQ(pool.getCoinsView().getCoin({child.GetTxid(),0}).status(),Status::NotFound);
    const auto replaced=pool.getCoinsView().getCoin({replacement.GetTxid(),0});ASSERT_EQ(replaced.status(),Status::Ok);EXPECT_EQ(replaced.value().value,replacement.vout[0].value);EXPECT_EQ(replaced.value().scriptPubKey,script);
}
TEST_F(TypedPoolPublication, PublicationFailureRestoresReplacementAndAllowsRetry) {
    const auto out=fund(82);const auto original=spend(out,1000000,1000);const auto replacement=spend(out,1000000,10000);
    RefusingLogger logger;Mempool pool(&db,&coins);pool.setRBFEnabled(true);pool.setLogger(&logger);
    ASSERT_TRUE(pool.submitTransaction(original,"typed-publication",false).accepted());const auto before=pool.getTransactionIds();const auto fees=pool.getTotalFees();unsigned observed=0;
    pool.setTxBodyAcceptedCallback([&](const MempoolTransaction&){++observed;});logger.refuse=true;
    EXPECT_THROW(pool.submitTransaction(replacement,"typed-publication",false),std::runtime_error);logger.refuse=false;
    EXPECT_EQ(pool.getTransactionIds(),before);EXPECT_EQ(pool.getTotalFees(),fees);EXPECT_EQ(observed,0U);EXPECT_TRUE(pool.isOutputSpentInMempool(out));
    ASSERT_TRUE(pool.getMempoolEntry(original.GetTxid().AsUint256()));EXPECT_EQ(pool.getMempoolEntry(original.GetTxid().AsUint256())->tx.Serialize(),original.Serialize());
    EXPECT_EQ(pool.getCoinsView().getCoin({original.GetTxid(),0}).status(),Status::Ok);EXPECT_EQ(pool.getCoinsView().getCoin({replacement.GetTxid(),0}).status(),Status::NotFound);
    ASSERT_TRUE(pool.submitTransaction(replacement,"typed-publication",false).accepted());EXPECT_EQ(observed,1U);EXPECT_FALSE(pool.hasTransaction(original.GetTxid().AsUint256()));
}
#ifdef DINERO_TEST_ORCHARD_BODY
TEST_F(TypedPoolPublication, MixedConflictInventoryRefusesDirectAndDescendantFamilies) {
    const auto out=fund(83);const auto original=spend(out,1000000,1000);const auto replacement=spend(out,1000000,10000);
    const auto child=orchardChild({original.GetTxid(),0});const auto grandchild=structural({{child.GetTxid(),0}},91);
    const std::vector<MempoolEntry> rows{MempoolEntry(MempoolTransaction(grandchild),1000,110),MempoolEntry(child,*child.ExplicitFee(),110),MempoolEntry(MempoolTransaction(original),1000,110)};
    const auto conflicts=policy::RBFPolicy::buildConflictSet(replacement,rows,mining::CTSelectionConfig{});
    EXPECT_EQ(conflicts.direct_conflicts,std::unordered_set<uint256>{original.GetTxid().AsUint256()});
    const std::unordered_set<uint256> descendants{child.GetTxid().AsUint256(),grandchild.GetTxid().AsUint256()};EXPECT_EQ(conflicts.descendant_conflicts,descendants);EXPECT_EQ(conflicts.conflict_count,3U);EXPECT_TRUE(conflicts.contains_unsupported_family);
    EXPECT_EQ(conflicts.total_fee,2000U+*child.ExplicitFee());EXPECT_EQ(conflicts.total_virtual_size,original.GetVirtualSize()+child.GetVirtualSize()+grandchild.GetVirtualSize());
    policy::RBFPolicy policy;std::string error;
    EXPECT_EQ(policy.validateReplacement(replacement,1000000,conflicts,{}, {},{rows.back()},error),policy::RBFValidationResult::UNSUPPORTED_FAMILY);
    const auto direct=structural(child.Inputs(),92);const auto direct_set=policy::RBFPolicy::buildConflictSet(direct,rows,{});
    EXPECT_TRUE(direct_set.contains_unsupported_family);EXPECT_EQ(direct_set.direct_conflicts,std::unordered_set<uint256>{child.GetTxid().AsUint256()});
    EXPECT_EQ(policy.validateReplacement(direct,1000000,direct_set,{}, {},{rows[1]},error),policy::RBFValidationResult::UNSUPPORTED_FAMILY);
    // Exercise the actual preflight/admission refusal with the structural graph.
    Mempool pool(&db,&coins);pool.setRBFEnabled(true);MempoolOrchardConflictTestPeer::Install(pool,{MempoolTransaction(original),child,MempoolTransaction(grandchild)});
    const auto before=pool.getTransactionIds();const auto fees=pool.getTotalFees();unsigned observed=0;pool.setTxBodyAcceptedCallback([&](const MempoolTransaction&){++observed;});
    EXPECT_EQ(pool.submitTransactionTestOnly(replacement,"typed-policy").code,TxRejectCode::RBF_REJECTED);
    const auto result=pool.submitTransaction(replacement,"typed-policy",false);EXPECT_EQ(result.code,TxRejectCode::RBF_REJECTED);EXPECT_NE(result.message.find("family without replacement policy"),std::string::npos);
    EXPECT_EQ(pool.getTransactionIds(),before);EXPECT_EQ(pool.getTotalFees(),fees);EXPECT_EQ(observed,0U);EXPECT_EQ(pool.getMempoolEntry(child.GetTxid().AsUint256())->tx.Serialize(),child.Serialize());
}
TEST_F(TypedPoolPublication, SignedReplacementPreservesUnrelatedTypedBody) {
    const auto orchard=MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::DecodeExact(fixture("candidate-envelope.bin")));
    Mempool pool(&db,&coins);pool.setRBFEnabled(true);MempoolOrchardConflictTestPeer::Install(pool,{orchard});
    const auto out=fund(84);const auto original=spend(out,1000000,1000);const auto replacement=spend(out,1000000,10000);
    ASSERT_TRUE(pool.submitTransaction(original,"typed-policy",false).accepted());
    const auto conflicts=policy::RBFPolicy::buildConflictSet(replacement,{*pool.getMempoolEntry(orchard.GetTxid().AsUint256()),*pool.getMempoolEntry(original.GetTxid().AsUint256())},{});
    EXPECT_FALSE(conflicts.contains_unsupported_family);EXPECT_EQ(conflicts.conflict_count,1U);
    ASSERT_TRUE(pool.submitTransaction(replacement,"typed-policy",false).accepted());EXPECT_EQ(pool.size(),2U);EXPECT_FALSE(pool.hasTransaction(original.GetTxid().AsUint256()));
    const auto retained=pool.getMempoolEntry(orchard.GetTxid().AsUint256());ASSERT_TRUE(retained);EXPECT_EQ(retained->tx.Serialize(),orchard.Serialize());EXPECT_EQ(retained->tx.GetWtxid(),orchard.GetWtxid());
    EXPECT_EQ(pool.getCoinsView().getCoin({replacement.GetTxid(),0}).status(),Status::Ok);
    // Orchard remains an unvalidated structural fixture; only Historical
    // submissions above underwent the actual admission authorization path.
}
#endif
}
