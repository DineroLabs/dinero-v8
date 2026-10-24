#pragma once
#include "storage/chain_db.h"
#include "storage/orchard_catalog_nodes.h"

namespace dinero::storage {
// Durable eligibility/floor metadata, NOT an authentication certificate. A
// startup owner must authenticate the retained proof against its selected
// header chain before enrolling state. Unknown/malformed metadata fails closed.
inline constexpr const char* OrchardSnapshotBaseKey="orchard_snapshot_v6:base";
// Factory eligibility only, consumed with genesis or the snapshot head. It is
// never a state-validity certificate or permission to reset an existing store.
inline constexpr const char* OrchardSnapshotFreshKey="orchard_snapshot_v6:fresh";
inline constexpr uint64_t OrchardSnapshotProofChunkBytes=1024*1024;
inline std::string OrchardSnapshotProofKey(const uint256& digest,uint64_t part) {
    return "orchard_snapshot_v6:proof:"+digest.GetHex()+":"+std::to_string(part);
}
inline uint256 OrchardSnapshotRetirementFingerprint(const LegacyRetirementRecord& r) {
    std::string b="DNOSR01";catalog::Number(b,r.network_code,1);
    const auto hash=[&](const uint256& h){b.append(reinterpret_cast<const char*>(h.data),32);};
    hash(r.genesis);catalog::Number(b,r.branch_id,4);catalog::Number(b,r.activation_height,4);
    catalog::Number(b,r.legacy_epoch_height,4);hash(r.boundary_parent);hash(r.legacy_state_root);
    catalog::Number(b,r.retired_value,8);hash(r.tree_root);catalog::Number(b,r.tree_size,8);
    catalog::Number(b,r.nullifier_count,8);return catalog::Hash(b);
}
struct OrchardSnapshotBase {
    bool operator==(const OrchardSnapshotBase&)const=default;
    uint8_t profile=0;uint32_t height=0;uint256 block,proof_digest,retirement;
    uint64_t proof_bytes=0;
    std::string Encode()const {
        catalog::Require((profile==1||profile==2)&&height&&height<=uint32_t(INT32_MAX)&&
            !block.IsNull()&&!proof_digest.IsNull()&&proof_bytes&&proof_bytes<=(uint64_t(1)<<30));
        std::string b="DNOSB01";b.push_back(char(profile));catalog::Number(b,height,4);
        b.append(reinterpret_cast<const char*>(block.data),32);b.append(reinterpret_cast<const char*>(proof_digest.data),32);
        catalog::Number(b,proof_bytes,8);b.append(reinterpret_cast<const char*>(retirement.data),32);return b;
    }
    static OrchardSnapshotBase Decode(const std::string& b) {
        catalog::Require(b.size()==116&&b.compare(0,7,"DNOSB01")==0);OrchardSnapshotBase s;
        s.profile=uint8_t(b[7]);s.height=uint32_t(catalog::Number(b,8,4));
        std::copy_n(reinterpret_cast<const uint8_t*>(b.data()+12),32,s.block.begin());
        std::copy_n(reinterpret_cast<const uint8_t*>(b.data()+44),32,s.proof_digest.begin());
        s.proof_bytes=catalog::Number(b,76,8);
        std::copy_n(reinterpret_cast<const uint8_t*>(b.data()+84),32,s.retirement.begin());
        catalog::Require(s.Encode()==b);return s;
    }
};
inline std::optional<OrchardSnapshotBase> ReadOrchardSnapshotBase(const ChainDB& db) {
    std::string bytes;const auto status=db.getRaw(OrchardSnapshotBaseKey,bytes);
    if(status==Status::NotFound)return {};
    catalog::Require(status==Status::Ok);return OrchardSnapshotBase::Decode(bytes);
}
} // namespace dinero::storage
