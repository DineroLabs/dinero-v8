#pragma once
#include "wallet/runtime_replay_spool.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace dinero::wallet::detail {
// Append-only Patricia nodes in the replay's private spool. A root is local to
// this instance; never persist/export it or interpret it as proof authority.
// Insertion copies only the changed path. Reads use at most 257 fixed-size
// nodes, with no history-size in-memory membership table or retained tree.
class RuntimeReplayDiskMembership {
public:
    using Key=std::array<uint8_t,32>;
    using Root=uint64_t;
    explicit RuntimeReplayDiskMembership(RuntimeReplaySpool& spool):spool_(spool){}
    RuntimeReplayDiskMembership(const RuntimeReplayDiskMembership&)=delete;
    RuntimeReplayDiskMembership& operator=(const RuntimeReplayDiskMembership&)=delete;
    bool Contains(Root root,const Key& key) const {
        if(!root)return false;
        auto node=Read(root);uint16_t previous=0;bool first=true;
        for(;;) {
            Check(first||node.bit>previous);first=false;
            if(node.bit==256)return node.key==key;
            previous=node.bit;node=Read(Bit(key,node.bit)?node.one:node.zero);
        }
    }
    // Complete, ordered traversal from an owned root, bounded by the number
    // of successful unique insertions recorded by the caller. SQL enumeration
    // is insufficient: deleted rows must fail, never look like end-of-input.
    // Every node is MAC-checked by Read/Get, including terminal SQLITE_DONE.
    // Visitors run after Get has released its statement and spool mutex. A
    // visitor may observe a prefix before an error; stage privately and publish
    // only after this call returns the exact expected count successfully.
    template<class Visitor>
    uint64_t ForEach(Root root,uint64_t expected_count,Visitor&& visitor) const {
        Check(bool(root)==bool(expected_count));
        if(!root)return 0;
        struct Frame {Root id;uint16_t minimum_bit;Key mask,prefix;};
        // At most one pending sibling per key bit, plus the current node.
        std::array<Frame,257> pending{};size_t size=1;
        pending[0]={root,0,{}, {}};
        uint64_t count=0;Key previous{};
        while(size) {
            const auto frame=pending[--size];const auto node=Read(frame.id);
            Check(node.bit>=frame.minimum_bit);
            if(node.bit==256) {
                for(size_t i=0;i<node.key.size();++i)
                    Check((node.key[i]&frame.mask[i])==frame.prefix[i]);
                Check(count<expected_count && (!count||previous<node.key));
                Check(visitor(node.key));previous=node.key;++count;
                continue;
            }
            Check(size+2<=pending.size());
            auto mask=frame.mask;auto zero=frame.prefix;auto one=frame.prefix;
            const auto byte=node.bit/8;const uint8_t bit=uint8_t(1u<<(7-node.bit%8));
            mask[byte]|=bit;one[byte]|=bit;
            const auto next=uint16_t(node.bit+1);
            pending[size++]={node.one,next,mask,one};
            pending[size++]={node.zero,next,mask,zero};
        }
        Check(count==expected_count);return count;
    }
    // Caller owns a RuntimeReplaySpool::Batch. A failed batch can leave gaps
    // in allocated IDs but cannot publish a partial root to a later node.
    Root With(Root root,const Key& key) {
        if(!root)return Write(Node{256,key,0,0});
        auto leaf=Read(root);uint16_t previous=0;bool first=true;
        while(leaf.bit<256) {
            Check(first||leaf.bit>previous);first=false;previous=leaf.bit;
            leaf=Read(Bit(key,leaf.bit)?leaf.one:leaf.zero);
        }
        Check(first||leaf.bit>previous);
        if(leaf.key==key)return root;
        uint16_t split=0;while(split<256&&Bit(leaf.key,split)==Bit(key,split))++split;
        Check(split<256);return Insert(root,key,split,0);
    }
private:
    struct Node {uint16_t bit;Key key;Root zero,one;};
    RuntimeReplaySpool& spool_;Root next_=1;
    static void Check(bool ok){if(!ok)throw std::runtime_error("Runtime replay membership spool inconsistent");}
    static bool Bit(const Key& key,uint16_t bit){Check(bit<256);return (key[bit/8]>>(7-bit%8))&1;}
    static void Number(RuntimeReplaySpool::Bytes& bytes,uint64_t value,unsigned size) {
        for(unsigned i=0;i<size;++i){bytes.push_back(uint8_t(value));value>>=8;}
    }
    static uint64_t Number(const RuntimeReplaySpool::Bytes& bytes,size_t at,unsigned size) {
        Check(at<=bytes.size()&&size<=bytes.size()-at);uint64_t value=0;
        for(unsigned i=0;i<size;++i)value|=uint64_t(bytes[at+i])<<(8*i);return value;
    }
    static RuntimeReplaySpool::Bytes RecordKey(Root id) {
        Check(id!=0);RuntimeReplaySpool::Bytes key{'t'};Number(key,id,8);return key;
    }
    Node Read(Root id) const {
        Check(id>0&&id<next_);const auto bytes=spool_.Get(RecordKey(id));Check(bytes&&bytes->size()==50);
        Node result{uint16_t(Number(*bytes,0,2)),{},Number(*bytes,34,8),Number(*bytes,42,8)};
        std::copy_n(bytes->begin()+2,32,result.key.begin());Check(result.bit<=256);
        if(result.bit==256)Check(result.zero==0&&result.one==0);
        else Check(result.key==Key{}&&result.zero>0&&result.one>0&&result.zero<id&&result.one<id&&result.zero!=result.one);
        return result;
    }
    Root Write(const Node& node) {
        Check(next_!=UINT64_MAX&&node.bit<=256);const Root id=next_++;
        RuntimeReplaySpool::Bytes bytes;bytes.reserve(50);Number(bytes,node.bit,2);
        bytes.insert(bytes.end(),node.key.begin(),node.key.end());Number(bytes,node.zero,8);Number(bytes,node.one,8);
        spool_.Insert(RecordKey(id),bytes);return id;
    }
    Root Insert(Root id,const Key& key,uint16_t split,unsigned depth) {
        Check(depth<=256);const auto node=Read(id);
        if(node.bit>=split) {
            const auto leaf=Write(Node{256,key,0,0});
            return Write(Bit(key,split)?Node{split,{},id,leaf}:Node{split,{},leaf,id});
        }
        // Read validates monotonically allocated children; additionally check
        // increasing bit positions before descending through persisted data.
        const auto child=Bit(key,node.bit)?node.one:node.zero;
        Check(Read(child).bit>node.bit);const auto inserted=Insert(child,key,split,depth+1);
        return Write(Bit(key,node.bit)?Node{node.bit,{},node.zero,inserted}:Node{node.bit,{},inserted,node.one});
    }
};
} // namespace dinero::wallet::detail
