#pragma once
#include "consensus/orchard_block_coins.h"
#include "consensus/utreexo_delta.h"
#include <memory>

namespace dinero::consensus {
enum class OrchardForestErrorCode { Context, ParentCommitment, MissingLeaf, Delete, Add, Undo };
class OrchardForestError : public std::runtime_error {
public:
    explicit OrchardForestError(OrchardForestErrorCode code)
        :std::runtime_error("Orchard forest transition rejected"),code_(code){}
    OrchardForestErrorCode Code()const noexcept{return code_;}
private:
    OrchardForestErrorCode code_;
};
class PreparedOrchardForest;
[[nodiscard]] PreparedOrchardForest PrepareOrchardForestTransition(
    const PreparedOrchardBlockCoins&, const BlockHeader& parent, const UtreexoForest&);
[[nodiscard]] UtreexoForest UndoOrchardForestTransition(const UtreexoForest&,const PreparedOrchardForest&);

// Stateful computation on one private forest clone. No network-proof bypass:
// this requires the authenticated full parent forest under the caller's lock.
// Peer proof payload validation and durable forest/undo integration are separate.
class PreparedOrchardForest {
public:
    const UtreexoForest& After()const noexcept{return *after_;}
    const UtreexoDelta& Delta()const noexcept{return delta_;}
    const uint256& Root()const noexcept{return root_;}
    const uint256& BlockHash()const noexcept{return block_hash_;}
    bool MatchesHeader(const BlockHeader& header)const{return header.GetHash()==block_hash_ && header.utreexo_root==root_;}
private:
    friend PreparedOrchardForest PrepareOrchardForestTransition(
        const PreparedOrchardBlockCoins&,const BlockHeader&,const UtreexoForest&);
    friend UtreexoForest UndoOrchardForestTransition(const UtreexoForest&,const PreparedOrchardForest&);
    PreparedOrchardForest(uint256 block_hash,uint256 root,uint256 parent_root,bool parent_canonical,
        UtreexoDelta delta,std::unique_ptr<UtreexoForest> after)
        :block_hash_(block_hash),root_(root),parent_root_(parent_root),parent_canonical_(parent_canonical),
         delta_(std::move(delta)),after_(std::move(after)){}
    uint256 block_hash_,root_,parent_root_;
    bool parent_canonical_;
    UtreexoDelta delta_;
    std::unique_ptr<UtreexoForest> after_;
};
} // namespace dinero::consensus
