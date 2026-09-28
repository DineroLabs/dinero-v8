// Benign patched-path cache ownership checks with a real isolated forest.
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "network/bridge_node.h"
#include <gtest/gtest.h>
#include <functional>
#include <memory>
#include <utility>
namespace {
using namespace dinero;
using namespace dinero::consensus;
class HookProvider final : public IUTXOProvider {
public:
    ConsensusUTXOSet coins;
    mutable std::function<void()> before_read;
    mutable size_t reads = 0;
    std::optional<UTXOEntry> GetUTXO(const OutPoint& out) const override {
        ++reads;
        auto action = std::exchange(before_read, {});
        if (action) action();
        const auto* coin = coins.GetCoin(out);
        return coin ? std::optional<UTXOEntry>(*coin) : std::nullopt;
    }
    bool AddUTXO(const OutPoint& out,const UTXOEntry& coin) override { return coins.AddCoin(out,coin); }
    bool SpendUTXO(const OutPoint& out,uint32_t) override { return bool(coins.SpendCoin(out)); }
    bool DeleteUTXO(const OutPoint& out) override { return coins.DeleteCoin(out); }
    bool HasUTXO(const OutPoint& out) const override { return coins.HaveCoin(out); }
};
class BridgeCacheOwner : public ::testing::Test {
protected:
    std::shared_ptr<HookProvider> provider = std::make_shared<HookProvider>();
    std::unique_ptr<network::BridgeNode> bridge;
    Transaction tx;
    UtreexoHash leaf;
    void SetUp() override {
        SelectParams(Chain::MAINNET);
        uint256 id;id.data[0]=37;OutPoint out{TxId(id),0};
        UTXOEntry coin{AmountUna::Una(10000),{0x51},1,false};
        ASSERT_TRUE(provider->coins.AddCoin(out,coin));
        leaf=HashUTXOForCreationHeight(id,0,10000,{0x51},1,false);
        uint256 second_id;second_id.data[0]=38;
        ASSERT_TRUE(provider->coins.AddCoin({TxId(second_id),0},coin));
        const auto second_leaf=HashUTXOForCreationHeight(second_id,0,10000,{0x51},1,false);
        provider->coins.MutateForestGuarded([&](UtreexoForest& forest){forest.add(leaf);forest.add(second_leaf);});
        tx.version=2;tx.vin.emplace_back();tx.vin[0].prevout.txid=out.txid;tx.vin[0].prevout.vout=0;
        tx.vout.emplace_back(AmountUna::Una(9000),std::vector<uint8_t>{0x51});
        bridge=std::make_unique<network::BridgeNode>(provider,&provider->coins.GetForest(),nullptr,nullptr,nullptr,&provider->coins);
    }
    void checkProof() {
        const auto result=bridge->GenerateProofsForTransaction(tx);
        ASSERT_TRUE(result);ASSERT_EQ(result->size(),1U);
        EXPECT_EQ(result->front().second.value,10000U);
        EXPECT_FALSE(result->front().first.siblings.empty());
        const auto held=provider->coins.LockForestShared();
        EXPECT_TRUE(result->front().first.verify(leaf,provider->coins.GetForest().getRoots()));
    }
};
TEST_F(BridgeCacheOwner, AbandonPreservesCacheAndPublicationInvalidates) {
    checkProof();ASSERT_EQ(bridge->GetCacheSnapshot().tx_entries,1U);const auto reads=provider->reads;
    const auto before=bridge->GetCacheSnapshot();
    {auto prepared=bridge->PrepareTxProofCacheInvalidation();ASSERT_TRUE(prepared);}
    checkProof();EXPECT_EQ(provider->reads,reads);EXPECT_EQ(bridge->GetCacheSnapshot().evictions,before.evictions);
    {auto prepared=bridge->PrepareTxProofCacheInvalidation();ASSERT_TRUE(prepared);prepared->PublishAfterCommit();}
    EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,0U);EXPECT_EQ(bridge->GetCacheSnapshot().evictions,before.evictions+1);
    checkProof();EXPECT_EQ(provider->reads,reads+1);checkProof();EXPECT_EQ(provider->reads,reads+1);
}
TEST_F(BridgeCacheOwner, InvalidationDuringGenerationRefusesObsoleteResult) {
    const auto root=bridge->GetCurrentForestCommitment();
    provider->before_read=[&]{bridge->InvalidateTxProofCache();};
    EXPECT_FALSE(bridge->GenerateProofsForTransaction(tx));EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,0U);
    EXPECT_EQ(bridge->GetCurrentForestCommitment(),root);checkProof();
    bridge->InvalidateTxProofCache();
    provider->before_read=[&]{bridge->ClearCache();};
    EXPECT_FALSE(bridge->GenerateProofsForTransaction(tx));EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,0U);
    checkProof();EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,1U);
}
TEST_F(BridgeCacheOwner, AbandonedInvalidationDoesNotCancelGeneration) {
    provider->before_read=[&]{auto prepared=bridge->PrepareTxProofCacheInvalidation();};
    checkProof();EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,1U);const auto reads=provider->reads;
    checkProof();EXPECT_EQ(provider->reads,reads);
    {auto prepared=bridge->PrepareTxProofCacheInvalidation();prepared->PublishAfterCommit();}
    EXPECT_EQ(bridge->GetCacheSnapshot().tx_entries,0U);
    {auto prepared=bridge->PrepareTxProofCacheInvalidation();prepared->PublishAfterCommit();}
    EXPECT_EQ(bridge->GetCacheSnapshot().evictions,1U);checkProof();
}
}
