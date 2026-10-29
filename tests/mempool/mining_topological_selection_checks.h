// Signed benign packages exercise the patched selector and actual assembler.
// Included in the existing fixture namespace; no alternate ingress or pool seam.
class MiningTopologicalSelection : public MiningPackageSelection {
protected:
    Transaction join(const Transaction& left, uint32_t li,
                     const Transaction& right, uint32_t ri, uint64_t fee) {
        Transaction tx;tx.version=2;
        std::vector<CanonicalWalletUTXO> coins_to_sign;
        for (const auto& source : {std::make_pair(&left,li),std::make_pair(&right,ri)}) {
            const auto& parent=*source.first;const auto index=source.second;
            tx.vin.emplace_back();auto& input=tx.vin.back();
            input.prevout.txid=parent.GetTxid();input.prevout.vout=index;input.sequence=0xfffffffd;
            CanonicalWalletUTXO coin;coin.txid=parent.GetTxid().AsUint256();coin.vout=index;
            coin.value=parent.vout.at(index).value;coin.spk=script;coins_to_sign.push_back(coin);
        }
        tx.vout.emplace_back(AmountUna::Una(coins_to_sign[0].value.GetUna()+coins_to_sign[1].value.GetUna()-fee),script);
        for(size_t i=0;i<tx.vin.size();++i) {
            const auto bytes=TaprootTxSigner::ComputeTaprootSighash(tx,i,coins_to_sign);
            if(bytes.size()!=32)throw std::runtime_error("fixture join sighash");
            std::array<uint8_t,32> digest{};std::copy(bytes.begin(),bytes.end(),digest.begin());
            std::array<uint8_t,64> signature{};
            if(!TaprootKeys::SignSchnorrWithInternalKey(signature,digest,secret,internal))throw std::runtime_error("fixture join signature");
            tx.vin[i].witness.emplace_back(signature.begin(),signature.end());
        }
        return tx;
    }
    void checkOrder(const std::vector<Transaction>& selected,const std::vector<Transaction>& expected) {
        ASSERT_EQ(selected.size(),expected.size());
        std::set<TxId> all,seen;
        for(const auto& tx:expected)all.insert(tx.GetTxid());
        for(const auto& tx:selected) {
            EXPECT_TRUE(all.count(tx.GetTxid()));
            for(const auto& input:tx.vin)
                if(all.count(input.prevout.txid))EXPECT_TRUE(seen.count(input.prevout.txid));
            EXPECT_TRUE(seen.insert(tx.GetTxid()).second);
        }
        EXPECT_EQ(seen,all);
    }
    void checkAssembler(Mempool& pool,const std::vector<Transaction>& expected,uint64_t fees) {
        BlockAssembler assembler(&db);wire(assembler);assembler.setMempool(&pool);
        BlockRelayManager relay(nullptr);assembler.SetBlockRelayManager(&relay);
        for(bool intelligent:{false,true}) {
            assembler.SetIntelligentSelection(intelligent);
            auto block=assembler.CreateNewBlock(address);ASSERT_NE(block,nullptr);
            ASSERT_FALSE(block->vtx.empty());checkOrder({block->vtx.begin()+1,block->vtx.end()},expected);
            EXPECT_EQ(assembler.getBlockTemplateStats().total_fees,fees);
            auto job=assembler.CreateJob();ASSERT_NE(job,nullptr);ASSERT_FALSE(job->transactions.empty());
            checkOrder({job->transactions.begin()+1,job->transactions.end()},expected);EXPECT_EQ(job->total_fees,fees);
        }
    }
};
TEST_F(MiningTopologicalSelection, ThreeGenerationsRemainParentFirst) {
    Mempool pool(&db,&coins);const auto root_tx=payment(61,1000);
    const auto middle=child(root_tx,0,2000),last=child(middle,0,50000);
    const std::vector<Transaction> package{root_tx,middle,last};
    for(const auto& tx:package)ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());
    const auto capture=pool.CaptureBlockSelection(1000000,4000000,1);ASSERT_TRUE(capture.available);
    checkOrder(capture.transactions,package);ASSERT_EQ(capture.transactions.size(),3u);
    EXPECT_EQ(capture.transactions[0].GetTxid(),root_tx.GetTxid());EXPECT_EQ(capture.transactions[1].GetTxid(),middle.GetTxid());
    EXPECT_EQ(capture.transactions[2].GetTxid(),last.GetTxid());
    checkAssembler(pool,package,53000);EXPECT_EQ(pool.size(),3u);
}
TEST_F(MiningTopologicalSelection, DiamondUsesOneSharedAncestorAndDeterministicSiblings) {
    Mempool pool(&db,&coins);const auto root_tx=payment(62,1000,1);
    const auto left=child(root_tx,0,1000),right=child(root_tx,1,1000),last=join(left,0,right,0,50000);
    const std::vector<Transaction> package{root_tx,left,right,last};
    for(const auto& tx:package)ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());
    const auto capture=pool.CaptureBlockSelection(1000000,4000000,1);ASSERT_TRUE(capture.available);
    checkOrder(capture.transactions,package);ASSERT_EQ(capture.transactions.size(),4u);
    EXPECT_EQ(capture.transactions[0].GetTxid(),root_tx.GetTxid());EXPECT_EQ(capture.transactions[3].GetTxid(),last.GetTxid());
    EXPECT_LT(capture.transactions[1].GetTxid(),capture.transactions[2].GetTxid());
    checkAssembler(pool,package,53000);pool.clear();
    for(const auto& tx:std::vector<Transaction>{root_tx,right,left,last})ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());
    const auto reordered=pool.CaptureBlockSelection(1000000,4000000,1);ASSERT_EQ(reordered.transactions.size(),capture.transactions.size());
    for(size_t i=0;i<capture.transactions.size();++i)EXPECT_EQ(reordered.transactions[i].Serialize(),capture.transactions[i].Serialize());
}
TEST_F(MiningTopologicalSelection, SharedParentInputsRespectPackageLimitsAndExclusions) {
    Mempool pool(&db,&coins);const auto root_tx=payment(63,1000,1);
    const auto middle=join(root_tx,0,root_tx,1,2000),last=child(middle,0,50000);
    const std::vector<Transaction> package{root_tx,middle,last};
    for(const auto& tx:package)ASSERT_TRUE(pool.submitTransaction(tx,"fixture",false).accepted());
    checkOrder(pool.CaptureBlockSelection(1000000,4000000,1).transactions,package);
    const auto weight=root_tx.GetWeight()+middle.GetWeight()+last.GetWeight();
    const auto limited=pool.CaptureBlockSelection(1000000,weight-1,1);ASSERT_TRUE(limited.available);
    uint64_t used=0;for(const auto& tx:limited.transactions){used+=tx.GetWeight();EXPECT_NE(tx.GetTxid(),last.GetTxid());}
    EXPECT_LE(used,weight-1);EXPECT_EQ(pool.size(),3u);
    pool.excludeFromBlockTemplates(root_tx.GetTxid().AsUint256(),"fixture",std::chrono::seconds(60));
    const auto excluded=pool.CaptureBlockSelection(1000000,4000000,1);ASSERT_TRUE(excluded.available);EXPECT_TRUE(excluded.transactions.empty());
    EXPECT_EQ(pool.size(),3u);
    pool.excludeFromBlockTemplates(root_tx.GetTxid().AsUint256(),"fixture",std::chrono::seconds(0));
    checkOrder(pool.CaptureBlockSelection(1000000,4000000,1).transactions,package);
    checkAssembler(pool,package,53000);
}
