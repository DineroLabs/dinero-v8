#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
struct CompactPoolInputs {
    MempoolTransaction legacy,modern;
    std::vector<uint8_t> legacy_wire,modern_wire;
    void Capture(CompactStartupFixture& f) {
        legacy=CanonicalPoolSurvivor(f);
        const auto& last=f.second->Transactions().back();
        modern=MempoolTransaction(SelectionSpend(f.f,OutPoint(last.GetTxid(),0),last.OutputCoin(0,103),100000));
        auto first=CanonicalPoolUnverifiedProof(f,legacy),second=CanonicalPoolUnverifiedProof(f,modern);
        OrchardAdmissionFixture::Require(bool(first)&&bool(second));legacy_wire=*first;modern_wire=*second;
    }
    auto Entries()const{return std::vector<MempoolProofView>{{legacy,legacy_wire},{modern,modern_wire}};}
};
consensus::OrchardTransactionContext CompactPoolContext(CompactStartupFixture& f) {
    const auto c=*consensus::SelectedOrchardBlockContext(f.second->Header(),f.second->Height());
    return {c.height+1,f.second->Header().GetHash(),c.activation_height,c.domain};
}
}
TEST(OrchardPoolCatalogView, ReopenedCompactOwnerAuthenticatesLegacyAndModernInputs) {
    CompactPoolInputs inputs;CompactStartupFixture f([&](auto& fixture){inputs.Capture(fixture);});
    auto owner=f.Restore();ASSERT_TRUE(owner);const auto rows=f.Rows();const auto archives=f.ArchiveBytes();
    const auto context=CompactPoolContext(f);const auto entries=inputs.Entries();
    std::lock_guard<AnnotatedRecursiveMutex> held(f.startup_mutex);
    const auto view=owner->CapturePoolCoinsUnderLock(f.startup_mutex,context,f.second->Header(),entries);
    EXPECT_EQ(view.getHeight(),103u);EXPECT_EQ(view.ParentHash(),context.parent_hash);EXPECT_EQ(view.CapturedInputs(),2u);
    const auto old=view.getCoin(inputs.legacy.Inputs().front()),modern=view.getCoin(inputs.modern.Inputs().front());
    ASSERT_TRUE(old.ok());ASSERT_TRUE(modern.ok());EXPECT_TRUE(old->isCoinbase);EXPECT_EQ(old->height,2u);
    EXPECT_FALSE(modern->isCoinbase);EXPECT_EQ(modern->height,103u);
    EXPECT_EQ(view.ProvedOutputAbsences(),inputs.legacy.OutputCount()+inputs.modern.OutputCount());
    EXPECT_EQ(view.getCoin(OutPoint(inputs.legacy.GetTxid(),0)).status(),Status::NotFound);
    uint256 unknown_hash;unknown_hash.data[0]=0xa5;unknown_hash.data[31]=0x5a;
    const OutPoint unknown(TxId(unknown_hash),4);
    EXPECT_EQ(view.getCoin(unknown).status(),Status::Internal);EXPECT_THROW(view.hasCoin(unknown),consensus::OrchardCoinLookupError);
    std::vector<ParsedTransaction> parsed;
    for(const auto& entry:entries)parsed.push_back(ParsedTransaction::DecodeExact(entry.body.Serialize(),TransactionReadMode::StagedOrchard));
    const auto checked=consensus::CheckOrchardTransactionCoinsUnderChainstateLock(parsed,context,view,[&](uint32_t h){return f.Mtp(h);});
    EXPECT_EQ(checked.Transactions().size(),2u);EXPECT_EQ(checked.TotalFees(),300000u);
    size_t full_coins=0;ASSERT_EQ(f.reopened.forEachUTXO([&](const uint256&,uint32_t,const Coin&){++full_coins;return true;}),Status::Ok);
    EXPECT_EQ(full_coins,0u);EXPECT_EQ(f.Rows(),rows);EXPECT_EQ(f.ArchiveBytes(),archives);
}
TEST(OrchardPoolCatalogView, MetadataProofAndSelectionFailuresDoNotPublishInputs) {
    CompactPoolInputs inputs;CompactStartupFixture f([&](auto& fixture){inputs.Capture(fixture);});auto owner=f.Restore();ASSERT_TRUE(owner);
    const auto context=CompactPoolContext(f);const auto rows=f.Rows();
    std::lock_guard<AnnotatedRecursiveMutex> held(f.startup_mutex);
    const auto capture=[&](const auto& entries){return owner->CapturePoolCoinsUnderLock(f.startup_mutex,context,f.second->Header(),entries);};
    const auto good=capture(inputs.Entries());
    ASSERT_EQ(inputs.legacy_wire.front(),2u);ASSERT_EQ(inputs.legacy.Inputs().size(),1u);
    auto forged=inputs.legacy_wire;ASSERT_EQ(forged.at(forged.size()-33),1u);forged[forged.size()-33]=0;
    EXPECT_THROW((void)capture(std::vector<MempoolProofView>{{inputs.modern,inputs.modern_wire},{inputs.legacy,forged}}),std::exception);
    auto modern=inputs.modern_wire;ASSERT_GT(modern.size(),37u);modern[modern.size()-37]^=1;
    EXPECT_THROW((void)capture(std::vector<MempoolProofView>{{inputs.modern,modern}}),std::exception);
    auto wrong_root=inputs.modern_wire;wrong_root.back()^=1;
    EXPECT_THROW((void)capture(std::vector<MempoolProofView>{{inputs.modern,wrong_root}}),std::exception);
    EXPECT_THROW((void)capture(std::vector<MempoolProofView>{{inputs.modern,inputs.legacy_wire}}),std::exception);
    EXPECT_THROW((void)capture(std::vector<MempoolProofView>{{inputs.modern,{}}}),std::exception);
    const auto historical=MempoolTransaction(f.second->Transactions().back().Historical());
    EXPECT_THROW((void)capture(std::vector<MempoolProofView>{{historical,{}}}),std::exception);
    auto duplicate=inputs.Entries();duplicate.push_back(duplicate.front());EXPECT_THROW((void)capture(duplicate),std::exception);
    auto wrong_context=context;++wrong_context.domain.branch_id;
    EXPECT_THROW((void)owner->CapturePoolCoinsUnderLock(f.startup_mutex,wrong_context,f.second->Header(),inputs.Entries()),std::exception);
    auto wrong_header=f.second->Header();++wrong_header.nonce;
    EXPECT_THROW((void)owner->CapturePoolCoinsUnderLock(f.startup_mutex,context,wrong_header,inputs.Entries()),std::exception);
    EXPECT_EQ(good.CapturedInputs(),2u);EXPECT_TRUE(good.getCoin(inputs.legacy.Inputs().front())->isCoinbase);
    EXPECT_EQ(capture(inputs.Entries()).CapturedInputs(),2u);EXPECT_EQ(f.Rows(),rows);
}
TEST(OrchardPoolCatalogView, DurableSelectionAndCatalogReadFailureRefuseThenRetry) {
    CompactPoolInputs inputs;CompactStartupFixture f([&](auto& fixture){inputs.Capture(fixture);});auto owner=f.Restore();ASSERT_TRUE(owner);
    const auto context=CompactPoolContext(f);const auto entries=inputs.Entries();
    const auto capture=[&]{std::lock_guard<AnnotatedRecursiveMutex> held(f.startup_mutex);
        return owner->CapturePoolCoinsUnderLock(f.startup_mutex,context,f.second->Header(),entries);};
    ASSERT_EQ(capture().CapturedInputs(),2u);
    const auto tip=f.reopened.getTip();ASSERT_TRUE(tip.ok());auto other=tip->hash;other.data[0]^=1;
    ASSERT_EQ(f.reopened.setTip(f.f.token,other,tip->height,tip->work),Status::Ok);
    EXPECT_THROW((void)capture(),std::exception);
    ASSERT_EQ(f.reopened.setTip(f.f.token,tip->hash,tip->height,tip->work),Status::Ok);
    const auto catalog=storage::catalog::State::Decode(f.expected_catalog.at(context.parent_hash));
    const auto id=catalog.legacy;ASSERT_FALSE(id.IsNull());const auto original=f.reopened.getOrchardCatalogNode(id);ASSERT_TRUE(original.ok());
    const auto key=std::string("Morchard_catalog_v1/node/")+std::string(reinterpret_cast<const char*>(id.data),32);
    f.Mutate([&](auto& raw){rocksdb::WriteOptions o;o.sync=true;OrchardAdmissionFixture::Require(raw.db->Delete(o,raw.cf("utreexo"),key).ok());});
    const auto missing=f.Rows();EXPECT_THROW((void)capture(),std::exception);EXPECT_EQ(f.Rows(),missing);
    f.Mutate([&](auto& raw){raw.put("utreexo",key,"corrupt");});
    EXPECT_THROW((void)capture(),std::exception);
    f.Mutate([&](auto& raw){raw.put("utreexo",key,*original);});
    EXPECT_EQ(capture().CapturedInputs(),2u);
    f.reopened.close();EXPECT_THROW((void)capture(),std::exception);ASSERT_EQ(f.reopened.init(f.f.path),Status::Ok);
    EXPECT_EQ(capture().CapturedInputs(),2u);
}
TEST(OrchardPoolCatalogView, RetirementMarkerMatchesSelectedParent) {
    CompactPoolInputs inputs;CompactStartupFixture f([&](auto& fixture){inputs.Capture(fixture);});
    auto owner=f.Restore();ASSERT_TRUE(owner);
    const auto context=CompactPoolContext(f);const auto entries=inputs.Entries();
    const auto capture=[&]{std::lock_guard<AnnotatedRecursiveMutex> held(f.startup_mutex);
        return owner->CapturePoolCoinsUnderLock(f.startup_mutex,context,f.second->Header(),entries);};
    const auto good=capture();const auto rows=f.Rows();const auto archives=f.ArchiveBytes();
    const auto retired=f.reopened.getLegacyRetirementState();ASSERT_TRUE(retired.ok());
    std::string original;
    f.Mutate([&](auto& raw){OrchardAdmissionFixture::Require(raw.db->Get(rocksdb::ReadOptions(),
        raw.cf(shielded_store_fixture::shielded),"R1S",&original).ok());});
    ASSERT_EQ(original.size(),237u);ASSERT_EQ(original.substr(0,4),"DLR1");
    // Preserve the entire authenticated record. Alter one well-formed marker
    // field at a time, so a decoder rejection cannot satisfy this regression.
    for(size_t field=0;field<3;++field) {
        SCOPED_TRACE(field);auto changed=original;
        if(field==0)for(size_t i=0;i<4;++i)changed[169+i]=char((retired->height+1)>>(8*i));
        else changed[field==1?173:205]^=1;
        f.Mutate([&](auto& raw){raw.put(shielded_store_fixture::shielded,"R1S",changed);});
        const auto decoded=f.reopened.getLegacyRetirementState();ASSERT_TRUE(decoded.ok());
        EXPECT_EQ(decoded->record,retired->record);
        const auto damaged=f.Rows();EXPECT_THROW((void)capture(),std::exception);
        EXPECT_EQ(f.Rows(),damaged);EXPECT_EQ(f.ArchiveBytes(),archives);
        EXPECT_EQ(good.ParentHash(),context.parent_hash);EXPECT_EQ(good.CapturedInputs(),2u);
        EXPECT_TRUE(good.getCoin(inputs.legacy.Inputs().front())->isCoinbase);
        f.Mutate([&](auto& raw){raw.put(shielded_store_fixture::shielded,"R1S",original);});
        EXPECT_EQ(capture().CapturedInputs(),2u);EXPECT_EQ(f.Rows(),rows);
    }
    f.Undo(*owner,f.second);f.Undo(*owner,f.first);
    const auto boundary_rows=f.Rows();const auto boundary_archives=f.ArchiveBytes();
    const auto header=f.reopened.getHeader(f.parent->hash);ASSERT_TRUE(header.ok());
    auto boundary=context;boundary.height=f.parent->height+1;boundary.parent_hash=f.parent->hash;
    const auto capture_boundary=[&]{std::lock_guard<AnnotatedRecursiveMutex> held(f.startup_mutex);
        return owner->CapturePoolCoinsUnderLock(f.startup_mutex,boundary,*header,{});};
    ASSERT_EQ(f.reopened.getLegacyRetirementState().status(),Status::NotFound);
    EXPECT_EQ(capture_boundary().CapturedInputs(),0u);
    f.Mutate([&](auto& raw){raw.put(shielded_store_fixture::shielded,"R1S",original);});
    ASSERT_TRUE(f.reopened.getLegacyRetirementState().ok());const auto damaged=f.Rows();
    EXPECT_THROW((void)capture_boundary(),std::exception);EXPECT_EQ(f.Rows(),damaged);
    EXPECT_EQ(f.ArchiveBytes(),boundary_archives);
    f.Mutate([&](auto& raw){rocksdb::WriteOptions options;options.sync=true;
        OrchardAdmissionFixture::Require(raw.db->Delete(options,raw.cf(shielded_store_fixture::shielded),"R1S").ok());});
    EXPECT_EQ(capture_boundary().CapturedInputs(),0u);EXPECT_EQ(f.Rows(),boundary_rows);
}
} // namespace dinero
#else
TEST(OrchardPoolCatalogView, UnenrolledOwnerRefuses) {
    dinero::OrchardCompactChainstate owner;dinero::AnnotatedRecursiveMutex mutex;
    std::lock_guard<dinero::AnnotatedRecursiveMutex> held(mutex);
    EXPECT_THROW((void)owner.CapturePoolCoinsUnderLock(mutex,{},dinero::BlockHeader{},{}),std::exception);
}
#endif
