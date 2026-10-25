#pragma once
#include "wallet/runtime_replay_spool.h"
#include <openssl/sha.h>
#include <algorithm>

namespace dinero::wallet::detail {
// Private content-addressed Patricia index. Only the root remains in memory.
// Unlike the scratch database's storage MAC, this root binds the exact metadata
// inserted by the verifier: changing a row AND its storage MAC cannot mint a
// different retained authorization token. This is not an import/reopen format.
class RuntimeReplayIndex {
public:
    using Key=std::array<uint8_t,33>;
    using Hash=std::array<uint8_t,32>;
    using Bytes=RuntimeReplaySpool::Bytes;
    explicit RuntimeReplayIndex(RuntimeReplaySpool& spool):spool_(spool){}
    std::optional<Bytes> Get(const Key& key) const {
        if(root_==Hash{})return std::nullopt;
        auto hash=root_;unsigned minimum=0;
        for(unsigned depth=0;depth<=264;++depth){
            const auto node=Read(hash);Check(node.bit>=minimum);
            if(node.bit==264)return node.key==key?std::optional(node.value):std::nullopt;
            minimum=node.bit+1;hash=Bit(key,node.bit)?node.one:node.zero;
        }
        Check(false);return std::nullopt;
    }
    // Owns its batch: publish the new root only after successful commit.
    void Set(const Key& key,std::span<const uint8_t> value){
        Check(!frozen_&&value.size()<=RuntimeReplaySpool::MaximumRecordBytes-100);
        RuntimeReplaySpool::Batch batch(spool_);
        const auto next=root_==Hash{}?Write({264,key,Bytes(value.begin(),value.end()),{}, {}}):Replace(root_,key,value,0);
        batch.Commit();root_=next;
    }
    void Freeze(){Check(!frozen_);frozen_=true;}
private:
    struct Node {unsigned bit;Key key;Bytes value;Hash zero,one;};
    RuntimeReplaySpool& spool_;Hash root_{};bool frozen_=false;
    static void Check(bool ok){if(!ok)throw std::runtime_error("Runtime replay index inconsistent");}
    static bool Bit(const Key& key,unsigned bit){Check(bit<264);return (key[bit/8]>>(7-bit%8))&1;}
    static Hash Digest(std::span<const uint8_t> bytes){Hash h{};Check(SHA256(bytes.data(),bytes.size(),h.data()));return h;}
    static Bytes StorageKey(const Hash& hash){Bytes k{'i'};k.insert(k.end(),hash.begin(),hash.end());return k;}
    Node Read(const Hash& hash) const {
        Check(hash!=Hash{});const auto bytes=spool_.Get(StorageKey(hash));
        Check(bytes&&bytes->size()>=2&&Digest(*bytes)==hash);
        const unsigned bit=(*bytes)[0]|(unsigned((*bytes)[1])<<8);Check(bit<=264);
        Node n{bit,{},{},{},{}};
        if(bit==264){
            Check(bytes->size()>=35);std::copy_n(bytes->begin()+2,33,n.key.begin());
            n.value.assign(bytes->begin()+35,bytes->end());
        }else{
            Check(bytes->size()==66);std::copy_n(bytes->begin()+2,32,n.zero.begin());
            std::copy_n(bytes->begin()+34,32,n.one.begin());
            Check(n.zero!=Hash{}&&n.one!=Hash{}&&n.zero!=n.one);
        }
        return n;
    }
    Hash Write(const Node& n){
        Bytes bytes{uint8_t(n.bit),uint8_t(n.bit>>8)};
        if(n.bit==264){bytes.insert(bytes.end(),n.key.begin(),n.key.end());bytes.insert(bytes.end(),n.value.begin(),n.value.end());}
        else {bytes.insert(bytes.end(),n.zero.begin(),n.zero.end());bytes.insert(bytes.end(),n.one.begin(),n.one.end());}
        const auto h=Digest(bytes);const auto key=StorageKey(h);
        const auto old=spool_.Get(key);if(old)Check(*old==bytes);else spool_.Insert(key,bytes);
        return h;
    }
    Hash Insert(const Hash& hash,const Key& key,std::span<const uint8_t> value,unsigned split,unsigned minimum){
        auto n=Read(hash);Check(n.bit>=minimum);
        if(n.bit>=split){
            const auto leaf=Write({264,key,Bytes(value.begin(),value.end()),{}, {}});
            return Write(Bit(key,split)?Node{split,{},{},hash,leaf}:Node{split,{},{},leaf,hash});
        }
        const auto child=Bit(key,n.bit)?n.one:n.zero;
        const auto next=Insert(child,key,value,split,n.bit+1);
        if(Bit(key,n.bit))n.one=next;else n.zero=next;return Write(n);
    }
    Hash Replace(const Hash& hash,const Key& key,std::span<const uint8_t> value,unsigned minimum){
        auto n=Read(hash);Check(n.bit>=minimum);
        if(n.bit==264){
            if(n.key==key)return Write({264,key,Bytes(value.begin(),value.end()),{}, {}});
            unsigned split=0;while(split<264&&Bit(n.key,split)==Bit(key,split))++split;
            return Insert(hash,key,value,split,minimum);
        }
        // The search leaf determines the first differing bit. Insert from this
        // subtree's root, not a child, when that bit precedes its current split.
        auto leaf=n;unsigned next_minimum=n.bit+1;
        while(leaf.bit<264){
            const auto child=Read(Bit(key,leaf.bit)?leaf.one:leaf.zero);
            Check(child.bit>=next_minimum);leaf=child;next_minimum=leaf.bit+1;
        }
        unsigned split=0;while(split<264&&Bit(leaf.key,split)==Bit(key,split))++split;
        if(split<264)return Insert(hash,key,value,split,minimum);
        const auto next=Replace(Bit(key,n.bit)?n.one:n.zero,key,value,n.bit+1);
        if(Bit(key,n.bit))n.one=next;else n.zero=next;return Write(n);
    }
};
} // namespace dinero::wallet::detail
