#include "consensus/orchard_state_root.h"
#include "crypto/sha256.h"
#include <algorithm>

namespace dinero::consensus {
std::vector<uint8_t> BuildOrchardStateRootPreimage(const OrchardStateRootContext& c,
    const storage::LegacyRetirementRecord& r, const storage::OrchardStoredState& s,
    const storage::OrchardCommitmentSets& sets) {
    (void)orchard::SigningContext::Create(c.domain,0,{}, {},0);
    if (!c.activation_height || c.activation_height==UINT32_MAX ||
        c.height<c.activation_height || c.height>uint32_t(INT32_MAX) || c.parent_hash.IsNull() ||
        r.network_code!=c.domain.network_code || !std::equal(c.domain.genesis_wire.begin(),c.domain.genesis_wire.end(),r.genesis.begin()) ||
        r.branch_id!=c.domain.branch_id || r.activation_height!=c.activation_height ||
        r.legacy_epoch_height>=r.activation_height || r.boundary_parent.IsNull() ||
        (c.height==c.activation_height && c.parent_hash!=r.boundary_parent) ||
        r.retired_value>orchard::kMaxMoneyUna || s.height!=c.height ||
        s.pool_balance>orchard::kMaxMoneyUna-r.retired_value ||
        s.frontier.size()>storage::ORCHARD_STORED_FRONTIER_LIMIT ||
        sets.nullifier_count!=s.tree_size || !sets.anchor_count ||
        sets.anchor_count>sets.anchor_references ||
        sets.anchor_references!=uint64_t(c.height)-c.activation_height+1)
        throw std::invalid_argument("invalid Orchard state-root context");
    const auto frontier=orchard::OrchardFrontier::Decode({
        reinterpret_cast<const uint8_t*>(s.frontier.data()),s.frontier.size()});
    if (frontier.Size()!=s.tree_size || !std::equal(frontier.Root().begin(),frontier.Root().end(),s.anchor.begin()))
        throw std::invalid_argument("inconsistent Orchard state-root frontier");
    std::vector<uint8_t> b{'D','N','O','R','S','T','0','1'};
    const auto number=[&](uint64_t n,size_t width){for(size_t i=0;i<width;++i)b.push_back(uint8_t(n>>(8*i)));};
    const auto hash=[&](const uint256& h){b.insert(b.end(),h.begin(),h.end());};
    number(orchard::kTransactionVersion,4);number(orchard::kBundleWireProfile,1);
    number(orchard::kOrchardPoolProfile,1);number(orchard::kCircuitProfile,1);
    number(orchard::kEffectCommitmentVersion,4);
    number(r.network_code,1);hash(r.genesis);number(r.branch_id,4);number(r.activation_height,4);
    number(r.legacy_epoch_height,4);hash(r.boundary_parent);hash(r.legacy_state_root);number(r.retired_value,8);
    hash(r.tree_root);number(r.tree_size,8);number(r.nullifier_count,8);
    number(c.height,4);hash(c.parent_hash);
    hash(s.anchor);number(s.tree_size,8);number(s.pool_balance,8);
    hash(sets.nullifiers);number(sets.nullifier_count,8);hash(sets.anchors);
    number(sets.anchor_count,8);number(sets.anchor_references,8);
    return b;
}
uint256 ComputeOrchardStateRoot(const OrchardStateRootContext& c,
    const storage::LegacyRetirementRecord& r, const storage::OrchardStoredState& s,
    const storage::OrchardCommitmentSets& sets) {
    const auto bytes=BuildOrchardStateRootPreimage(c,r,s,sets);
    crypto::CSHA256 hash;hash.Write(bytes.data(),bytes.size());uint256 root;hash.Finalize(root.data);return root;
}
std::vector<uint8_t> BuildOrchardCatalogStateRootPreimage(const OrchardStateRootContext& c,
    const storage::LegacyRetirementRecord& r,const storage::OrchardStoredState& s,
    const storage::OrchardCommitmentSets& sets,const storage::catalog::State& parent) {
    const auto record=parent.Encode();
    if(parent.network!=c.domain.network_code || parent.branch!=c.domain.branch_id ||
        !std::equal(parent.genesis.begin(),parent.genesis.end(),c.domain.genesis_wire.begin()) ||
        parent.activation!=c.activation_height || uint64_t(parent.height)+1!=c.height ||
        parent.block!=c.parent_hash)
        throw std::invalid_argument("invalid Orchard parent catalog context");
    auto bytes=BuildOrchardStateRootPreimage(c,r,s,sets);
    bytes.at(7)='2'; // Explicit preimage domain; DNORST01 stays byte-identical.
    const auto digest=storage::catalog::Hash(record);
    bytes.insert(bytes.end(),digest.begin(),digest.end());
    return bytes;
}
uint256 ComputeOrchardCatalogStateRoot(const OrchardStateRootContext& c,
    const storage::LegacyRetirementRecord& r,const storage::OrchardStoredState& s,
    const storage::OrchardCommitmentSets& sets,const storage::catalog::State& parent) {
    const auto bytes=BuildOrchardCatalogStateRootPreimage(c,r,s,sets,parent);
    crypto::CSHA256 hash;hash.Write(bytes.data(),bytes.size());uint256 root;hash.Finalize(root.data);return root;
}
uint256 ComputeOrchardSelectedStateRoot(const OrchardStateRootContext& c,
    const storage::LegacyRetirementRecord& r,const storage::OrchardStoredState& s,
    const storage::OrchardCommitmentSets& sets,uint32_t activation,
    const storage::catalog::State* parent) {
    if(activation!=UINT32_MAX && (activation<c.activation_height || activation>uint32_t(INT32_MAX)))
        throw std::invalid_argument("invalid Orchard catalog commitment activation");
    if(activation==UINT32_MAX || c.height<activation)return ComputeOrchardStateRoot(c,r,s,sets);
    if(!parent)throw std::invalid_argument("missing authenticated Orchard parent catalog");
    return ComputeOrchardCatalogStateRoot(c,r,s,sets,*parent);
}
} // namespace dinero::consensus
