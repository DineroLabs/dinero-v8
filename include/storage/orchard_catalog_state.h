#pragma once
#include "storage/orchard_catalog_nodes.h"
#include "consensus/chainwork.h"
#include "consensus/utreexo_stump.h"
#include <bit>

namespace dinero::storage::catalog {
// An immutable per-block record, selected by the EXISTING canonical chain tip.
// Encoding validation is not historical authority. Only the canonical writer
// can enroll the completed replay's initial roots or extend an enrolled parent.
// v2 adds complete nontransparent-output membership. v1 is refused: an old
// record cannot authenticate absence of confidential metadata by defaulting it.
struct State {
    uint8_t network=0xff;Digest genesis{};uint32_t branch=0,activation=0,leaf_activation=0;
    uint32_t height=0;Digest block{},parent{};arith_uint256 work{0};
    Digest transactions{},legacy{},nontransparent{};uint64_t transaction_count=0,legacy_count=0,nontransparent_count=0;
    Digest previous_record{},undo{};std::vector<uint8_t> stump;
    void Validate() const {
        Require(network<=2&&!genesis.IsNull()&&branch&&activation&&activation!=UINT32_MAX);
        Require(uint64_t(height)+1>=activation&&!block.IsNull()&&!work.IsZero());
        Require(transactions.IsNull()==(transaction_count==0)&&legacy.IsNull()==(legacy_count==0)&&
            nontransparent.IsNull()==(nontransparent_count==0));
        const bool initial=uint64_t(height)+1==activation;
        Require(previous_record.IsNull()==initial&&undo.IsNull()==initial);
        Require(stump.size()>=9&&stump.size()<=9+64*33);
        const std::string bytes(stump.begin(),stump.end());const auto leaves=Number(bytes,0,8);
        const auto roots=uint8_t(bytes[8]);Require(roots==std::popcount(leaves)&&stump.size()==9+size_t(roots)*33);
        uint64_t mask=0;
        for(unsigned i=0;i<roots;++i){const auto h=stump[9+i*33];Require(h<64&&!(mask&(uint64_t(1)<<h)));mask|=uint64_t(1)<<h;}
        Require(mask==leaves);
        Require(consensus::UtreexoStump::deserialize(stump).serialize()==stump);
    }
    std::string Encode() const {
        Validate();std::string s="DNOCS02";Number(s,network,1);
        const auto hash=[&](const Digest& h){s.append(reinterpret_cast<const char*>(h.data),32);};
        hash(genesis);Number(s,branch,4);Number(s,activation,4);Number(s,leaf_activation,4);
        Number(s,height,4);hash(block);hash(parent);
        for(unsigned i=0;i<4;++i)Number(s,work.GetWord(i),8);
        hash(transactions);hash(legacy);hash(nontransparent);Number(s,transaction_count,8);Number(s,legacy_count,8);Number(s,nontransparent_count,8);
        hash(previous_record);hash(undo);Number(s,uint64_t(stump.size()),4);s.append(stump.begin(),stump.end());hash(Hash(s));return s;
    }
    static State Decode(const std::string& s) {
        Require(s.size()>=7&&s.size()<=2493&&s.compare(0,7,"DNOCS02")==0);size_t at=7;State v;
        const auto number=[&](unsigned n){auto x=Number(s,at,n);at+=n;return x;};
        const auto hash=[&](Digest& h){Require(at<=s.size()&&32<=s.size()-at);std::copy_n(reinterpret_cast<const uint8_t*>(s.data()+at),32,h.begin());at+=32;};
        v.network=number(1);hash(v.genesis);v.branch=number(4);v.activation=number(4);v.leaf_activation=number(4);
        v.height=number(4);hash(v.block);hash(v.parent);
        for(unsigned i=0;i<4;++i)v.work.SetWord(i,number(8));
        hash(v.transactions);hash(v.legacy);hash(v.nontransparent);v.transaction_count=number(8);v.legacy_count=number(8);v.nontransparent_count=number(8);
        hash(v.previous_record);hash(v.undo);const auto size=number(4);Require(at<=s.size()&&s.size()-at>=32&&size==s.size()-at-32);
        v.stump.assign(s.begin()+at,s.begin()+at+size);at+=size;Digest checksum;hash(checksum);Require(checksum==Hash(s.substr(0,s.size()-32)));v.Validate();Require(v.Encode()==s);return v;
    }
};
} // namespace dinero::storage::catalog
