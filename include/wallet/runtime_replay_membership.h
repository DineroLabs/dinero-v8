#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace dinero::wallet::detail {
// Immutable compressed binary trie for 256-bit replay membership. Only the
// changed insertion path is copied; retained branch snapshots share all other
// nodes. No persistence or deserialization/validity interface is provided.
// Branch bit positions strictly increase, bounding traversal/recursion by256
// independently of chain height. This does NOT bound retained snapshot memory.
class RuntimeReplayMembership {
public:
    using Key=std::array<uint8_t,32>;
    bool Contains(const Key& key) const {
        const auto* node=root_.get();
        if(!node)return false;
        while(node->bit<256)node=(Bit(key,node->bit)?node->one:node->zero).get();
        return node->key==key;
    }
    [[nodiscard]] RuntimeReplayMembership With(const Key& key) const {
        if(!root_)return RuntimeReplayMembership(std::make_shared<const Node>(key));
        const auto* leaf=root_.get();
        while(leaf->bit<256)leaf=(Bit(key,leaf->bit)?leaf->one:leaf->zero).get();
        if(leaf->key==key)return *this;
        const auto split=FirstDifference(leaf->key,key);
        return RuntimeReplayMembership(Insert(root_,key,split));
    }
    RuntimeReplayMembership()=default;
private:
    struct Node {
        const uint16_t bit;
        const Key key;
        const std::shared_ptr<const Node> zero,one;
        explicit Node(const Key& k):bit(256),key(k){}
        Node(uint16_t b,std::shared_ptr<const Node> z,std::shared_ptr<const Node> o)
            :bit(b),key{},zero(std::move(z)),one(std::move(o)){}
    };
    std::shared_ptr<const Node> root_;
    explicit RuntimeReplayMembership(std::shared_ptr<const Node> root):root_(std::move(root)){}
    static bool Bit(const Key& key,uint16_t bit) {
        return (key[bit/8]>>(7-bit%8))&1;
    }
    static uint16_t FirstDifference(const Key& a,const Key& b) {
        for(uint16_t bit=0;bit<256;++bit)if(Bit(a,bit)!=Bit(b,bit))return bit;
        throw std::logic_error("Distinct replay membership keys required");
    }
    static std::shared_ptr<const Node> Insert(const std::shared_ptr<const Node>& node,
                                              const Key& key,uint16_t split) {
        if(node->bit>=split) {
            auto leaf=std::make_shared<const Node>(key);
            return Bit(key,split)?std::make_shared<const Node>(split,node,std::move(leaf)):
                std::make_shared<const Node>(split,std::move(leaf),node);
        }
        if(Bit(key,node->bit))return std::make_shared<const Node>(node->bit,node->zero,Insert(node->one,key,split));
        return std::make_shared<const Node>(node->bit,Insert(node->zero,key,split),node->one);
    }
};
} // namespace dinero::wallet::detail
