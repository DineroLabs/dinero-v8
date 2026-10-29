#pragma once
#include "storage/orchard_catalog_nodes.h"
#include "consensus/chainwork.h"
#include "consensus/utreexo_stump.h"
#include <bit>

namespace dinero::storage::catalog {
// Independently reconstructed historical prefix, strictly below the activation
// parent. Encoding/checksum is NOT canonical authorization. This record has no
// retirement certificate and is deliberately rejected by the DNOCS02 decoder.
// Roots become selected only through a writer that binds the exact before-image.
struct HistoricalState {
    uint8_t network=0xff;
    Digest genesis{},block{},parent{};
    uint32_t branch=0,activation=0,leaf_activation=0,height=0,legacy_epoch=0;
    arith_uint256 work{0};
    Digest transactions{},legacy{},nontransparent{};
    uint64_t transaction_count=0,legacy_count=0,nontransparent_count=0;
    Digest legacy_state_root{},tree_root{};
    uint64_t legacy_value=0,tree_size=0,nullifier_count=0;
    std::vector<uint8_t> stump;
    void Validate() const {
        Require(network<=2&&!genesis.IsNull()&&branch&&activation&&activation!=UINT32_MAX);
        Require(uint64_t(height)+1<activation&&!block.IsNull()&&!work.IsZero());
        Require(height==0 ? block==genesis&&parent.IsNull() : !parent.IsNull());
        Require(legacy_epoch<activation&&!legacy_state_root.IsNull()&&!tree_root.IsNull());
        Require(!transactions.IsNull()&&transaction_count>0&&
            legacy.IsNull()==(legacy_count==0)&&nontransparent.IsNull()==(nontransparent_count==0));
        Require(stump.size()>=9&&stump.size()<=9+64*33);
        const std::string bytes(stump.begin(),stump.end());const auto leaves=Number(bytes,0,8);
        const auto roots=uint8_t(bytes[8]);Require(roots==std::popcount(leaves)&&stump.size()==9+size_t(roots)*33);
        uint64_t mask=0;
        for(unsigned i=0;i<roots;++i){const auto h=stump[9+i*33];Require(h<64&&!(mask&(uint64_t(1)<<h)));mask|=uint64_t(1)<<h;}
        Require(mask==leaves&&consensus::UtreexoStump::deserialize(stump).serialize()==stump);
    }
    std::string Encode() const {
        Validate();std::string s="DNHCS01";Number(s,network,1);
        const auto hash=[&](const Digest& h){s.append(reinterpret_cast<const char*>(h.data),32);};
        hash(genesis);Number(s,branch,4);Number(s,activation,4);Number(s,leaf_activation,4);
        Number(s,height,4);hash(block);hash(parent);for(unsigned i=0;i<4;++i)Number(s,work.GetWord(i),8);
        hash(transactions);hash(legacy);hash(nontransparent);
        Number(s,transaction_count,8);Number(s,legacy_count,8);Number(s,nontransparent_count,8);
        Number(s,legacy_epoch,4);hash(legacy_state_root);hash(tree_root);
        Number(s,legacy_value,8);Number(s,tree_size,8);Number(s,nullifier_count,8);
        Number(s,uint64_t(stump.size()),4);s.append(stump.begin(),stump.end());hash(Hash(s));return s;
    }
    static HistoricalState Decode(const std::string& s) {
        Require(s.size()>=7&&s.size()<=4096&&s.compare(0,7,"DNHCS01")==0);size_t at=7;HistoricalState v;
        const auto number=[&](unsigned n){auto x=Number(s,at,n);at+=n;return x;};
        const auto hash=[&](Digest& h){Require(at<=s.size()&&32<=s.size()-at);std::copy_n(reinterpret_cast<const uint8_t*>(s.data()+at),32,h.begin());at+=32;};
        v.network=number(1);hash(v.genesis);v.branch=number(4);v.activation=number(4);v.leaf_activation=number(4);
        v.height=number(4);hash(v.block);hash(v.parent);for(unsigned i=0;i<4;++i)v.work.SetWord(i,number(8));
        hash(v.transactions);hash(v.legacy);hash(v.nontransparent);
        v.transaction_count=number(8);v.legacy_count=number(8);v.nontransparent_count=number(8);
        v.legacy_epoch=number(4);hash(v.legacy_state_root);hash(v.tree_root);
        v.legacy_value=number(8);v.tree_size=number(8);v.nullifier_count=number(8);
        const auto size=number(4);Require(at<=s.size()&&s.size()-at>=32&&size==s.size()-at-32);
        v.stump.assign(s.begin()+at,s.begin()+at+size);at+=size;Digest checksum;hash(checksum);
        Require(checksum==Hash(s.substr(0,s.size()-32)));v.Validate();Require(v.Encode()==s);return v;
    }
};
} // namespace dinero::storage::catalog
