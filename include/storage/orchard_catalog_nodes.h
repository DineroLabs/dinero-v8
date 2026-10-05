#pragma once
#include "crypto/sha256.h"
#include "consensus/outpoint.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace dinero::storage::catalog {
// Content-addressed records only. Neither a node nor a caller-supplied root
// certifies history. Canonical publication requires the completed replay owner.
using Key=std::array<uint8_t,32>;
using Digest=uint256;
enum class Kind:uint8_t { Transactions=1, LegacyCoins=2 };
inline void Require(bool ok) {if(!ok)throw std::runtime_error("Orchard catalog record inconsistent");}
inline void Number(std::string& s,uint64_t n,unsigned size) {
    for(unsigned i=0;i<size;++i){s.push_back(char(n));n>>=8;}
}
inline uint64_t Number(const std::string& s,size_t at,unsigned size) {
    Require(at<=s.size()&&size<=s.size()-at);uint64_t n=0;
    for(unsigned i=0;i<size;++i)n|=uint64_t(uint8_t(s[at+i]))<<(8*i);return n;
}
inline Digest Hash(const std::string& s) {
    Digest h;crypto::CSHA256().Write(reinterpret_cast<const uint8_t*>(s.data()),s.size()).Finalize(h.data);return h;
}
inline Key TransactionKey(const TxId& id) {
    Key key{};std::copy(id.AsUint256().begin(),id.AsUint256().end(),key.begin());return key;
}
inline std::string OutpointBytes(const OutPoint& p) {
    std::string s(reinterpret_cast<const char*>(p.txid.AsUint256().data),32);Number(s,p.vout,4);return s;
}
inline Key LegacyKey(const std::string& point) {
    Require(point.size()==36);const auto h=Hash("DNOCK01"+point);Key key{};
    std::copy(h.begin(),h.end(),key.begin());return key;
}
struct Node {
    Kind kind;uint16_t bit=256;Key key{};std::string value;Digest zero{},one{};
    std::string Encode() const {
        std::string s="DNOCN01";s.push_back(char(kind));s.push_back(bit==256?1:0);
        if(bit==256){s.append(reinterpret_cast<const char*>(key.data()),32);s+=value;}
        else {Number(s,bit,2);s.append(reinterpret_cast<const char*>(zero.data),32);s.append(reinterpret_cast<const char*>(one.data),32);}
        return s;
    }
    static Node Decode(const std::string& s) {
        Require(s.size()>=9&&s.compare(0,7,"DNOCN01")==0);
        Node n{Kind(uint8_t(s[7]))};Require(n.kind==Kind::Transactions||n.kind==Kind::LegacyCoins);
        const auto tag=uint8_t(s[8]);Require(tag<=1);
        if(tag==1) {
            const size_t extra=n.kind==Kind::LegacyCoins?41:0;Require(s.size()==41+extra);
            std::copy_n(reinterpret_cast<const uint8_t*>(s.data()+9),32,n.key.begin());n.value=s.substr(41);
            if(n.kind==Kind::LegacyCoins)Require(uint8_t(n.value[40])<=1&&LegacyKey(n.value.substr(0,36))==n.key);
        } else {
            Require(s.size()==75);n.bit=uint16_t(Number(s,9,2));Require(n.bit<256);
            std::copy_n(reinterpret_cast<const uint8_t*>(s.data()+11),32,n.zero.begin());
            std::copy_n(reinterpret_cast<const uint8_t*>(s.data()+43),32,n.one.begin());
            Require(!n.zero.IsNull()&&!n.one.IsNull()&&n.zero!=n.one);
        }
        Require(n.Encode()==s);return n;
    }
};
inline bool ValidNode(const Digest& id,const std::string& bytes) noexcept {
    try {return !id.IsNull()&&Hash(bytes)==id&&!Node::Decode(bytes).Encode().empty();}catch(...){return false;}
}
// Immutable Patricia updates with at most 257 nodes on a path. The reader must
// throw on a missing referenced node; nullopt is reserved for proven absence.
class Tree {
public:
    using Read=std::function<std::string(const Digest&)>;
    using Write=std::function<void(const Digest&,const std::string&)>;
    Tree(Kind kind,Read read,Write write={}):kind_(kind),read_(std::move(read)),write_(std::move(write)){}
    std::optional<std::string> Find(const Digest& root,const Key& key) const {
        if(root.IsNull())return {};
        const auto leaf=Leaf(root,key);return leaf.key==key?std::optional(leaf.value):std::nullopt;
    }
    Digest Insert(const Digest& root,const Key& key,const std::string& value) const {
        Require(bool(write_));Node leaf{kind_};leaf.key=key;leaf.value=value;
        // Also enforce canonical legacy key/value binding before any write.
        (void)Node::Decode(leaf.Encode());
        if(root.IsNull())return Put(leaf);
        const auto existing=Leaf(root,key);Require(existing.key!=key); // duplicate input is a producer error
        uint16_t split=0;while(split<256&&Bit(existing.key,split)==Bit(key,split))++split;
        Require(split<256);return InsertAt(root,leaf,split,0);
    }
private:
    Kind kind_;Read read_;Write write_;
    static bool Bit(const Key& k,uint16_t b){Require(b<256);return (k[b/8]>>(7-b%8))&1;}
    Node Get(const Digest& id) const {
        Require(!id.IsNull());const auto bytes=read_(id);Require(Hash(bytes)==id);
        auto n=Node::Decode(bytes);Require(n.kind==kind_);return n;
    }
    Node Leaf(Digest id,const Key& key) const {
        uint16_t minimum=0;Key mask{},prefix{};
        for(unsigned depth=0;depth<=256;++depth) {
            const auto n=Get(id);Require(n.bit>=minimum);
            if(n.bit==256) {
                for(size_t i=0;i<32;++i)Require((n.key[i]&mask[i])==prefix[i]);return n;
            }
            const auto byte=n.bit/8;const auto bit=uint8_t(1u<<(7-n.bit%8));mask[byte]|=bit;
            if(Bit(key,n.bit)){prefix[byte]|=bit;id=n.one;}else{id=n.zero;}
            minimum=n.bit+1;
        }
        throw std::runtime_error("Orchard catalog path exceeds key width");
    }
    Digest Put(const Node& n) const {
        const auto bytes=n.Encode();const auto id=Hash(bytes);Require(!id.IsNull());write_(id,bytes);return id;
    }
    Digest InsertAt(const Digest& id,const Node& leaf,uint16_t split,unsigned depth) const {
        Require(depth<=256);auto n=Get(id);
        if(n.bit>=split) {
            const auto added=Put(leaf);Node parent{kind_};parent.bit=split;
            parent.zero=Bit(leaf.key,split)?id:added;parent.one=Bit(leaf.key,split)?added:id;return Put(parent);
        }
        const bool one=Bit(leaf.key,n.bit);const auto child=one?n.one:n.zero;Require(Get(child).bit>n.bit);
        const auto added=InsertAt(child,leaf,split,depth+1);if(one)n.one=added;else n.zero=added;return Put(n);
    }
};
} // namespace dinero::storage::catalog
