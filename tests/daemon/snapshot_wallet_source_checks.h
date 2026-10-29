#pragma once
#include "daemon/services/snapshot_wallet_source.h"
#include <sstream>
namespace dinero {
namespace {
struct SnapshotWalletSourceFixture {
    std::vector<Block> blocks;
    assumeutxo::AssumeUtxoReplayEngine replay;
    std::vector<std::pair<OutPoint,consensus::UTXOEntry>> coins;
    explicit SnapshotWalletSourceFixture(uint32_t height=25):blocks(BuildDeterministicChain(height)){
        if(blocks.size()!=height)throw std::runtime_error("source fixture chain");
        std::string error;if(!replay.SeedGenesis(SelectedGenesis(),error))throw std::runtime_error(error);
        for(uint32_t i=0;i<blocks.size();++i)
            if(!replay.ConnectAndAdvance(blocks[i],i+1,blocks[i].GetHash(),error))throw std::runtime_error(error);
        for(const auto& coin:replay.ProvenUtxos())coins.push_back(coin);
        std::sort(coins.begin(),coins.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    }
    static void Number(std::string& out,uint64_t n,unsigned bytes){for(unsigned i=0;i<bytes;++i)out.push_back(char(n>>(8*i)));}
    std::string Bytes(const std::vector<std::pair<OutPoint,consensus::UTXOEntry>>& records)const{
        std::string out;Number(out,consensus::SNAPSHOT_MAGIC,4);Number(out,consensus::SNAPSHOT_VERSION_V5,4);
        const auto base=blocks.back().GetHash();out.append(reinterpret_cast<const char*>(base.data),32);
        Number(out,blocks.size(),4);Number(out,records.size(),8);Number(out,1,8);Number(out,0,8);
        for(const auto& [point,coin]:records){const auto row=consensus::SerializeUtxoRecord(point,coin);
            out.append(reinterpret_cast<const char*>(row.data()),row.size());}
        const auto forest=replay.Forest()->serialize();
        Number(out,consensus::SNAPSHOT_V3_UTREEXO_MAGIC,4);Number(out,consensus::SNAPSHOT_V3_UTREEXO_SECTION_VERSION,4);
        Number(out,forest.size(),8);Number(out,replay.Forest()->getNumLeaves(),8);Number(out,0,8);
        out.append(reinterpret_cast<const char*>(blocks.back().header.utreexo_root.data),32);
        out.append(reinterpret_cast<const char*>(forest.data()),forest.size());return out;
    }
    auto Read(const std::string& bytes)const{std::istringstream input(bytes,std::ios::binary);
        return ReadSnapshotWalletSource(input,blocks.back().header,uint32_t(blocks.size()),SelectedGenesis());}
};
}
TEST(SnapshotWalletSource, AuthenticatesCompleteLiveCoinsAndMarksLegacyMetadataUnproved){
    SnapshotWalletSourceFixture f;const auto source=f.Read(f.Bytes(f.coins));
    EXPECT_EQ(source.base,f.blocks.back().GetHash());EXPECT_EQ(source.height,25u);ASSERT_EQ(source.coins.size(),f.coins.size());
    bool legacy=false,bound=false;
    for(const auto& row:source.coins){
        if(row.height==1){legacy=true;EXPECT_FALSE(row.maturity_bound);}
        if(row.height==25){bound=true;EXPECT_TRUE(row.maturity_bound);EXPECT_TRUE(row.coinbase);}
    }
    EXPECT_TRUE(legacy);EXPECT_TRUE(bound);
}
TEST(SnapshotWalletSource, OmissionsDuplicatesAndCounterfeitCoinFieldsRefuse){
    SnapshotWalletSourceFixture f;auto rows=f.coins;rows.pop_back();EXPECT_THROW(f.Read(f.Bytes(rows)),std::runtime_error);
    rows=f.coins;rows.insert(rows.begin(),rows.front());EXPECT_THROW(f.Read(f.Bytes(rows)),std::runtime_error);
    rows=f.coins;const auto at=std::find_if(rows.begin(),rows.end(),[](const auto& row){return row.second.height==25;});
    ASSERT_NE(at,rows.end());at->second.value=AmountUna::Una(at->second.value.GetUna()+1);
    EXPECT_THROW(f.Read(f.Bytes(rows)),std::runtime_error);
    rows=f.coins;std::find_if(rows.begin(),rows.end(),[](const auto& row){return row.second.height==25;})->second.isCoinbase=false;
    EXPECT_THROW(f.Read(f.Bytes(rows)),std::runtime_error);
}
TEST(SnapshotWalletSource, TruncationBaseMismatchAndOversizedCountsRefuseBeforeUse){
    SnapshotWalletSourceFixture f;auto bytes=f.Bytes(f.coins);
    EXPECT_THROW(f.Read(bytes.substr(0,bytes.size()-1)),std::runtime_error);
    bytes[8]^=1;EXPECT_THROW(f.Read(bytes),std::runtime_error);
    bytes=f.Bytes(f.coins);for(size_t i=44;i<52;++i)bytes[i]=char(255);
    EXPECT_THROW(f.Read(bytes),std::runtime_error);
    bytes=f.Bytes(f.coins);bytes[4]=6;EXPECT_THROW(f.Read(bytes),std::runtime_error);
}
TEST(SnapshotWalletSource, LegacyFlagsCannotBecomeAuthenticatedMetadata){
    SnapshotWalletSourceFixture f;auto rows=f.coins;
    auto at=std::find_if(rows.begin(),rows.end(),[](const auto& row){return row.second.height==1;});ASSERT_NE(at,rows.end());
    const auto id=at->first.txid;at->second.isCoinbase=false; // v1 leaf does not commit this field.
    const auto source=f.Read(f.Bytes(rows));
    const auto found=std::find_if(source.coins.begin(),source.coins.end(),[&](const auto& c){return c.txid==id.AsUint256();});
    ASSERT_NE(found,source.coins.end());EXPECT_FALSE(found->maturity_bound);
}
}
