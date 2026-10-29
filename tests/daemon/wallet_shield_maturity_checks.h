#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
struct ShieldMaturityFixture : ShieldReservationFixture {
    static constexpr uint64_t ShieldAmount = 20000;
    static constexpr uint64_t ShieldFee = 10000;
    ShieldMaturityFixture() {
        // Mine a real coinbase to the wallet's already issued BIP86 address.
        // No synthetic height, coin metadata, key, path or account replacement.
        BlockAssembler assembler(&f.db);WireOrchardAssembler(assembler,f);
        const auto block=assembler.CreateOrchardBlock(address);Need(bool(block));
        Need(block->Height()==103 && block->Transactions().size()==1);
        const auto coinbase=OrchardBlockCandidate::DecodeExact(block->WireBytes()).Transactions().front().Historical();
        Need(coinbase.IsCoinbase() && !coinbase.vout.empty());
        Need(coinbase.vout.front().scriptPubKey==script);
        const auto accepted=Submit(block->WireBytes());Need(accepted.accepted()&&accepted.connected);
        funding.txid=coinbase.GetTxid().AsUint256();funding.vout=0;
        funding.value=coinbase.vout.front().value;funding.height=103;
        Need(funding.value.GetUna()>ShieldAmount+ShieldFee);
        const auto observed=f.service->getCanonicalOutputInclusion(funding.txid,0,103);
        Need(observed.ok()&&observed->MatchesTransparent(funding.value.GetUna(),script));
    }
    void AdvanceToNextSpendAge(uint32_t age) {
        Need(age>0);
        const auto wanted=uint64_t(funding.height)+age-1;
        Need(f.service->GetActiveTip()->height<=wanted);
        while(f.service->GetActiveTip()->height<wanted)MineEmpty();
        Need(uint64_t(f.service->GetActiveTip()->height)+1-funding.height==age);
        // Normal source-proved recovery must discover the real wallet coin and
        // advance both enrolled accounts; addUTXO is not used for this coin.
        Need(wallet->RecoverActiveWalletFromCanonicalSource()==Recovery::AppliedPrefix);
        auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        struct Statement {sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} row;
        Need(sqlite3_prepare_v2(lease->Database(),
            "SELECT amount,height,is_coinbase,is_spent FROM utxos WHERE txid=? AND vout=0",-1,&row.p,nullptr)==SQLITE_OK);
        const auto txid=funding.GetTxIdHex();Need(sqlite3_bind_text(row.p,1,txid.data(),int(txid.size()),SQLITE_TRANSIENT)==SQLITE_OK);
        Need(sqlite3_step(row.p)==SQLITE_ROW);
        for(int column=0;column<4;++column)Need(sqlite3_column_type(row.p,column)==SQLITE_INTEGER);
        Need(sqlite3_column_int64(row.p,0)==static_cast<sqlite3_int64>(funding.value.GetUna()));
        Need(sqlite3_column_int64(row.p,1)==103 && sqlite3_column_int64(row.p,2)==1 && sqlite3_column_int64(row.p,3)==0);
        Need(sqlite3_step(row.p)==SQLITE_DONE);
    }
    auto CoinbaseOutputs() {
        return std::vector<orchard::TransparentOutput>{{funding.value.GetUna()-ShieldAmount-ShieldFee,script}};
    }
    auto QueueCoinbase(wallet::OrchardProofJobs& jobs) {
        const auto revision=Account(3).revision;const auto inputs=Inputs();const auto payments=Payments();const auto outputs=CoinbaseOutputs();
        auto use=WalletService::AcquireWalletUse(wallet);auto source=ChainstateService::AcquireWalletIndexUse(f.service);
        return f.service->queueRuntimeWalletShield(use->Wallet(),Selected().session,3,revision,orchard::Hash{81},inputs,payments,outputs,ShieldFee,jobs);
    }
    auto ReserveCoinbase() {
        const auto revision=Account(3).revision;const auto inputs=Inputs();const auto payments=Payments();const auto outputs=CoinbaseOutputs();
        auto use=WalletService::AcquireWalletUse(wallet);auto source=ChainstateService::AcquireWalletIndexUse(f.service);
        return f.service->reserveRuntimeWalletShield(use->Wallet(),Selected().session,3,revision,orchard::Hash{81},inputs,payments,outputs,ShieldFee);
    }
    auto FinishCoinbase(wallet::OrchardProofJobs& jobs) {
        const auto inputs=Inputs();const auto payments=Payments();const auto outputs=CoinbaseOutputs();
        auto use=WalletService::AcquireWalletUse(wallet);auto source=ChainstateService::AcquireWalletIndexUse(f.service);
        return f.service->finalizeRuntimeWalletShield(use->Wallet(),Selected().session,3,orchard::Hash{81},inputs,payments,outputs,ShieldFee,jobs);
    }
};
}
TEST(WalletShieldMaturity, OneBlockBeforeMaturityRefusesWithoutReservation) {
    ShieldMaturityFixture f;
    ASSERT_EQ(consensus::UTREEXO_STATELESS_COINBASE_MATURITY,100u);
    f.AdvanceToNextSpendAge(99);ASSERT_EQ(f.f.service->GetActiveTip()->height,201u);
    const auto before=f.Snapshot();auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    try {(void)f.QueueCoinbase(jobs);FAIL()<<"Immature coinbase request was queued";}
    catch(const std::runtime_error& error){EXPECT_STREQ(error.what(),"Shield coinbase input is immature");}
    EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(jobs.Query(orchard::Hash{81}));
    try {(void)f.ReserveCoinbase();FAIL()<<"Immature coinbase was reserved";}
    catch(const std::runtime_error& error){EXPECT_STREQ(error.what(),"Shield coinbase input is immature");}
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
}
TEST(WalletShieldMaturity, ExactMaturitySignsMinesAndRecovers) {
    ShieldMaturityFixture f;
    ASSERT_EQ(consensus::UTREEXO_STATELESS_COINBASE_MATURITY,100u);
    f.AdvanceToNextSpendAge(100);ASSERT_EQ(f.f.service->GetActiveTip()->height,202u);
    std::vector<uint8_t> body;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
        const auto queued=f.QueueCoinbase(jobs);ASSERT_TRUE(queued);ASSERT_TRUE(queued->enqueued);
        ASSERT_EQ(ServiceProofTerminal(jobs,orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
        body=f.FinishCoinbase(jobs);EXPECT_FALSE(jobs.Query(orchard::Hash{81}));
    }
    const auto ready=f.Snapshot();const auto account=f.Account(3);
    const auto& operation=account.account.Operations().Entries().at(orchard::Hash{81});
    EXPECT_EQ(operation.phase,wallet::OrchardOperationQueue::Phase::Ready);EXPECT_EQ(operation.transaction,body);
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);
    ASSERT_EQ(envelope.Inputs().size(),1u);ASSERT_EQ(envelope.Inputs()[0].witness.size(),1u);
    EXPECT_EQ(envelope.Inputs()[0].witness[0].size(),64u);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    wallet::OrchardProofJobs absent;EXPECT_EQ(f.FinishCoinbase(absent),body);EXPECT_EQ(f.Snapshot(),ready);
    const auto included=f.Mine(MempoolTransaction::FromOrchard(envelope));ASSERT_TRUE(included);
    EXPECT_EQ(included->Context()->height,203u);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),ShieldMaturityFixture::ShieldAmount);
    EXPECT_EQ(f.f.ingress->mempool().size(),0u);
}
} // namespace dinero
#endif
