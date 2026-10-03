#pragma once
// Included by the real replay/service fixture inside namespace dinero.
// Patched-path storage and selected-state checks; no alternate validator.
namespace {
using CoinRows=std::map<std::string,std::string>;
void RewriteFixtureCoins(ChainDB& db,const std::filesystem::path& path,
    const std::function<void(CoinRows&)>& edit) {
    db.close();
    {
        struct RawOwner {
            std::unique_ptr<rocksdb::DB> db;
            std::vector<rocksdb::ColumnFamilyHandle*> handles;
            ~RawOwner(){for(auto* h:handles)db->DestroyColumnFamilyHandle(h);}
        } owner;
        const auto require=[](bool ok){if(!ok)throw std::runtime_error("fixture raw coins");};
        rocksdb::Options options;std::vector<std::string> names;
        require(rocksdb::DB::ListColumnFamilies(options,path.string(),&names).ok());
        std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
        for(const auto& name:names) {
            rocksdb::ColumnFamilyOptions cf(options);
            if(name=="shielded_state_v1")cf=storage::ShieldedStateColumnFamilyOptions(std::move(cf));
            descriptors.emplace_back(name,std::move(cf));
        }
        rocksdb::DB* raw=nullptr;
        const auto opened=rocksdb::DB::Open(options,path.string(),descriptors,&owner.handles,&raw);
        owner.db.reset(raw);require(opened.ok());
        auto found=std::find_if(owner.handles.begin(),owner.handles.end(),[](auto* h){return h->GetName()=="utxo";});
        require(found!=owner.handles.end());auto* cf=*found;
        CoinRows rows;
        std::unique_ptr<rocksdb::Iterator> it(owner.db->NewIterator(rocksdb::ReadOptions(),cf));
        for(it->SeekToFirst();it->Valid();it->Next())rows.emplace(it->key().ToString(),it->value().ToString());
        require(it->status().ok());const auto old=rows;edit(rows);
        rocksdb::WriteBatch batch;
        for(const auto& [key,value]:old)batch.Delete(cf,key);
        for(const auto& [key,value]:rows)batch.Put(cf,key,value);
        rocksdb::WriteOptions writes;writes.sync=true;require(owner.db->Write(writes,&batch).ok());
    }
    if(db.init(path)!=Status::Ok)throw std::runtime_error("fixture coin reopen");
}
}

#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
class SelectedParentFixture {
public:
    ChainParams previous_params = Params();
    bool previous_stateless = GetConfig().utreexo_stateless;
    std::filesystem::path path;
    ChainDB db;
    ChainstateService service;
    CBlockIndex tip;
    std::vector<Block> blocks;
    std::unique_ptr<assumeutxo::AssumeUtxoReplayEngine> replay;
    const ChainWriteToken token = ChainWriteToken::CreateForTesting();
    static void Require(bool value) { if (!value) throw std::runtime_error("selected parent fixture setup"); }
    SelectedParentFixture() {
        MutableParams().orchard_activation_height=4;
        MutableParams().orchard_branch_id=1;
        MutableParams().shielded_activation_height=1;
        MutableParams().shielded_epoch_reset_height=UINT32_MAX;
        MutableParams().shielded_spend_auth_epoch_reset_height=UINT32_MAX;
        GetConfig().utreexo_stateless=false;
        auto name=(std::filesystem::temp_directory_path()/"selected_parent_XXXXXX").string();
        Require(mkdtemp(name.data())!=nullptr);path=name;
        // Generated separated-layout fixture. Normal ChainDB open correctly
        // creates the legacy layout and must never silently migrate it.
        {
            auto families=shielded_store_fixture::legacy;
            families.push_back(shielded_store_fixture::shielded);
            shielded_store_fixture::Raw raw(path,families);
            const uint32_t schema=4;
            raw.put("meta","schema_version",std::string(reinterpret_cast<const char*>(&schema),4));
            raw.put("meta","storage_layout_v1",shielded_store_fixture::ready);
            consensus::shielded::CommitmentTree tree;consensus::shielded::AnchorHistory anchors;
            const auto frontier=tree.SerializeFrontier();const auto history=anchors.SerializePersistenceBytes();
            raw.put(shielded_store_fixture::shielded,"Mshielded_frontier",{frontier.begin(),frontier.end()});
            raw.put(shielded_store_fixture::shielded,"Mshielded_anchor_history",{history.begin(),history.end()});
            raw.put("meta","shielded_tip",std::string(84,'\0'));
        }
        Require(db.init(path)==Status::Ok);
        blocks={SelectedGenesis()};const auto rest=BuildDeterministicChain(3);
        Require(rest.size()==3);blocks.insert(blocks.end(),rest.begin(),rest.end());
        replay=std::make_unique<assumeutxo::AssumeUtxoReplayEngine>();
        std::string error;Require(replay->SeedGenesis(blocks.front(),error));
        arith_uint256 work{0};
        for(uint32_t h=0;h<blocks.size();++h) {
            const auto& b=blocks[h];
            if(h) Require(replay->ConnectAndAdvance(b,h,b.GetHash(),error));
            work+=GetBlockProof(b.header.difficulty);
            Require(db.putHeader(token,b.GetHash(),b.header,h,work)==Status::Ok);
            Require(db.putHeightIndex(token,h,b.GetHash())==Status::Ok);
            Require(db.putBlock(token,b.GetHash(),b)==Status::Ok);
            for(uint32_t i=0;i<b.vtx.size();++i)
                Require(db.putTxIndex(token,b.vtx[i].GetTxid().AsUint256(),b.GetHash(),i)==Status::Ok);
        }
        tip=CBlockIndex(blocks.back().header,3);tip.chainwork=work.GetHex();
        Require(db.setTip(token,tip.hash,tip.height,work)==Status::Ok);
        Require(db.setValidatedTip(token,tip.hash,tip.height)==Status::Ok);
        for(const auto& [point,entry]:replay->ProvenUtxos()) {
            Coin c;c.amount=entry.value.GetUna();c.script_pubkey=util::hex(entry.scriptPubKey);
            c.height=entry.height;c.coinbase=entry.isCoinbase;c.is_confidential=entry.is_confidential;c.commitment=entry.commitment;
            Require(db.putCoin(token,point.txid.AsUint256(),point.vout,c)==Status::Ok);
        }
        Require(db.putForestTipMarker(token,{3,tip.hash,uint256::FromHexUnsafe(replay->UtreexoRootHex())})==Status::Ok);
        const auto frontier=replay->ShieldedTree()->SerializeFrontier();
        const auto anchors=replay->ShieldedAnchors()->SerializePersistenceBytes();
        Require(db.putShieldedState(token,ChainDB::ShieldedStateRecord::Frontier,{frontier.begin(),frontier.end()})==Status::Ok);
        Require(db.putShieldedState(token,ChainDB::ShieldedStateRecord::AnchorHistory,{anchors.begin(),anchors.end()})==Status::Ok);
        const auto root=replay->ShieldedTree()->Root();uint256 tree_root;std::copy(root.begin(),root.end(),tree_root.begin());
        Require(db.putShieldedTipMarker(token,{3,tip.hash,tree_root,replay->ShieldedTree()->Size(),0})==Status::Ok);
        service.setChainDB(&db);ShieldedStateStartupTestAccess::BoundaryState(service,tip,*replay);
    }
    ~SelectedParentFixture() {
        service.setChainDB(nullptr);db.close();std::error_code ec;std::filesystem::remove_all(path,ec);
        MutableParams()=previous_params;GetConfig().utreexo_stateless=previous_stateless;
    }
    auto Read() {return ShieldedStateStartupTestAccess::Boundary(service);}
    void CheckUnpublished() {
        ASSERT_EQ(db.getLegacyRetirementState().status(),Status::NotFound);
        ASSERT_TRUE(db.getTip().ok());EXPECT_EQ(db.getTip()->hash,tip.hash);
    }
};
}

TEST(SelectedParentHistory, IndependentReplayMatchesDurableAndLiveState) {
    SelectedParentFixture f;
    const auto record=f.Read();ASSERT_TRUE(record);
    EXPECT_EQ(record->boundary_parent,f.tip.hash);EXPECT_EQ(record->activation_height,4u);
    EXPECT_EQ(record->retired_value,0u);EXPECT_EQ(record->legacy_epoch_height,1u);
    f.CheckUnpublished();
    f.db.close();ASSERT_EQ(f.db.init(f.path),Status::Ok);
    EXPECT_EQ(f.Read(),record);f.CheckUnpublished();
}

TEST(SelectedParentHistory, StateAndAncestryMismatchRefuseWithoutPublication) {
    SelectedParentFixture f;ASSERT_TRUE(f.Read());
    const auto point=f.replay->ProvenUtxos().begin()->first;
    const auto original=f.db.getCoin(point.txid.AsUint256(),point.vout);ASSERT_TRUE(original.ok());
    auto wrong=*original;++wrong.amount;
    ASSERT_EQ(f.db.putCoin(f.token,point.txid.AsUint256(),point.vout,wrong),Status::Ok);
    EXPECT_FALSE(f.Read());f.CheckUnpublished();
    ASSERT_EQ(f.db.putCoin(f.token,point.txid.AsUint256(),point.vout,*original),Status::Ok);
    EXPECT_TRUE(f.Read());
    ShieldedStateStartupTestAccess::RemoveBoundaryCoin(f.service,point);
    EXPECT_FALSE(f.Read());f.CheckUnpublished();
    ShieldedStateStartupTestAccess::BoundaryState(f.service,f.tip,*f.replay);
    EXPECT_TRUE(f.Read());
    ASSERT_EQ(f.db.putHeightIndex(f.token,1,f.blocks[0].GetHash()),Status::Ok);
    EXPECT_FALSE(f.Read());f.CheckUnpublished();
    ASSERT_EQ(f.db.putHeightIndex(f.token,1,f.blocks[1].GetHash()),Status::Ok);
    EXPECT_TRUE(f.Read());
    const auto marker=f.db.getForestTipMarker();ASSERT_TRUE(marker.ok());auto bad=*marker;bad.forest_root.SetNull();
    ASSERT_EQ(f.db.putForestTipMarker(f.token,bad),Status::Ok);EXPECT_FALSE(f.Read());f.CheckUnpublished();
    ASSERT_EQ(f.db.putForestTipMarker(f.token,*marker),Status::Ok);EXPECT_TRUE(f.Read());
    auto changed=f.blocks.front();changed.vtx.front().vout.front().value=AmountUna::Una(1);
    ASSERT_EQ(f.db.putBlock(f.token,changed.GetHash(),changed),Status::Ok);
    EXPECT_FALSE(f.Read());f.CheckUnpublished();
    ASSERT_EQ(f.db.putBlock(f.token,f.blocks.front().GetHash(),f.blocks.front()),Status::Ok);
    EXPECT_TRUE(f.Read());
    CoinRows original_rows;
    RewriteFixtureCoins(f.db,f.path,[&](CoinRows& rows){original_rows=rows;rows["zz-malformed-late-key"]="unreadable";});
    EXPECT_FALSE(f.Read());f.CheckUnpublished();
    RewriteFixtureCoins(f.db,f.path,[&](CoinRows& rows){rows=original_rows;});
    EXPECT_TRUE(f.Read());
}
#else
TEST(SelectedParentHistory, UnavailableWithoutBackend) {
    ChainstateService service;
    EXPECT_FALSE(ShieldedStateStartupTestAccess::Boundary(service));
}
#endif

TEST(CheckedCoinInventory, VisitorFailureAndEarlyStopRemainDistinct) {
    auto name=(std::filesystem::temp_directory_path()/"checked_coin_inventory_XXXXXX").string();
    ASSERT_NE(mkdtemp(name.data()),nullptr);ChainDB db;ASSERT_EQ(db.init(name),Status::Ok);
    const auto token=ChainWriteToken::CreateForTesting();uint256 txid;txid.begin()[0]=1;
    Coin coin;coin.amount=100;coin.script_pubkey="51";coin.height=1;
    ASSERT_EQ(db.putCoin(token,txid,0,coin),Status::Ok);
    ASSERT_EQ(db.putCoin(token,txid,1,coin),Status::Ok);
    unsigned count=0;
    EXPECT_EQ(db.forEachUTXO([&](const uint256&,uint32_t,const Coin&){++count;return false;}),Status::Ok);
    EXPECT_EQ(count,1u);
    EXPECT_THROW(db.forEachUTXO([](const uint256&,uint32_t,const Coin&)->bool{
        throw std::runtime_error("visitor refused");
    }),std::runtime_error);
    count=0;EXPECT_EQ(db.forEachUTXO([&](const uint256&,uint32_t,const Coin&){++count;return true;}),Status::Ok);
    EXPECT_EQ(count,2u);
    db.close();std::error_code ec;std::filesystem::remove_all(name,ec);
}

TEST(CheckedCoinInventory, MalformedLateRowsRefuseAndLegacyEncodingsRemainReadable) {
    auto name=(std::filesystem::temp_directory_path()/"checked_coin_rows_XXXXXX").string();
    ASSERT_NE(mkdtemp(name.data()),nullptr);ChainDB db;ASSERT_EQ(db.init(name),Status::Ok);
    const auto token=ChainWriteToken::CreateForTesting();uint256 txid;txid.begin()[0]=1;
    Coin coin;coin.amount=100;coin.script_pubkey="51";coin.height=1;
    ASSERT_EQ(db.putCoin(token,txid,0,coin),Status::Ok);ASSERT_EQ(db.putCoin(token,txid,1,coin),Status::Ok);
    CoinRows original;RewriteFixtureCoins(db,name,[&](CoinRows& rows){original=rows;});ASSERT_EQ(original.size(),2u);
    const auto encoded=[](uint32_t height,uint8_t coinbase,std::optional<uint8_t> confidential,bool commitment) {
        VectorWriter w;w.write<uint64_t>(100);w.writeString("51");w.write(height);w.write(coinbase);
        if(confidential)w.write(*confidential);
        if(commitment)w.writeBytes({});
        return w.release_string();
    };
    for(auto value:{encoded(1,0,std::nullopt,false),encoded(1,0,0,false),encoded(1,0,0,true)}) {
        RewriteFixtureCoins(db,name,[&](CoinRows& rows){rows=original;rows.rbegin()->second=value;});
        unsigned count=0;
        EXPECT_EQ(db.forEachUTXO([&](const uint256&,uint32_t,const Coin& c){
            EXPECT_EQ(c.amount,100u);EXPECT_FALSE(c.coinbase);EXPECT_FALSE(c.is_confidential);++count;return true;
        }),Status::Ok);EXPECT_EQ(count,2u);
    }
    for(auto value:{std::string("short"),encoded(UINT32_MAX,0,0,true),encoded(1,2,0,true),
                    encoded(1,0,2,true),encoded(1,0,0,true)+"trailing"}) {
        RewriteFixtureCoins(db,name,[&](CoinRows& rows){rows=original;rows.rbegin()->second=value;});
        unsigned count=0;
        EXPECT_EQ(db.forEachUTXO([&](const uint256&,uint32_t,const Coin&){++count;return true;}),Status::Corruption);
        EXPECT_EQ(count,1u);
    }
    RewriteFixtureCoins(db,name,[&](CoinRows& rows){rows=original;rows["zz-malformed-late-key"]="value";});
    unsigned count=0;EXPECT_EQ(db.forEachUTXO([&](const uint256&,uint32_t,const Coin&){++count;return true;}),Status::Corruption);
    EXPECT_EQ(count,2u);
    RewriteFixtureCoins(db,name,[&](CoinRows& rows){rows=original;});count=0;
    EXPECT_EQ(db.forEachUTXO([&](const uint256&,uint32_t,const Coin&){++count;return true;}),Status::Ok);EXPECT_EQ(count,2u);
    db.close();std::error_code ec;std::filesystem::remove_all(name,ec);
}
