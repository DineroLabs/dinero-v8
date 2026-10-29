#pragma once
#include "storage/orchard_catalog_nodes.h"
#include "storage/chain_db.h"

namespace dinero::storage {
// Eligibility/before-image DATA, never historical validity or owner authority.
// Both rows are selected atomically in an unpublished conversion candidate.
// Startup/selection must reauthenticate retained proof and original ancestry;
// missing either row must NOT become a fresh activation-parent fallback.
inline constexpr const char* OrchardActivationOriginKey="orchard_activation_handoff:v1:origin";
inline constexpr const char* OrchardActivationSelectionKey="orchard_activation_handoff:v1:selection";
struct OrchardActivationOrigin {
    uint32_t magic=0,parent_height=0,validated_height=0;
    std::string profile;
    uint256 first,parent,parent_catalog,proof,retirement,source,validated;
    std::string Encode()const {
        catalog::Require(profile.size()==64&&parent_height&&validated_height<=parent_height&&
            !first.IsNull()&&!parent.IsNull()&&!parent_catalog.IsNull()&&!proof.IsNull()&&
            !retirement.IsNull()&&!source.IsNull()&&!validated.IsNull());
        std::string out="DNOAO01";catalog::Number(out,magic,4);out+=profile;
        catalog::Number(out,parent_height,4);catalog::Number(out,validated_height,4);
        for(const auto& hash:{first,parent,parent_catalog,proof,retirement,source,validated})
            out.append(reinterpret_cast<const char*>(hash.data),32);
        const auto digest=catalog::Hash(out);out.append(reinterpret_cast<const char*>(digest.data),32);return out;
    }
    static OrchardActivationOrigin Decode(const std::string& bytes){
        catalog::Require(bytes.size()==339&&bytes.compare(0,7,"DNOAO01")==0);
        OrchardActivationOrigin o;o.magic=uint32_t(catalog::Number(bytes,7,4));o.profile=bytes.substr(11,64);
        o.parent_height=uint32_t(catalog::Number(bytes,75,4));o.validated_height=uint32_t(catalog::Number(bytes,79,4));
        size_t at=83;for(auto* hash:{&o.first,&o.parent,&o.parent_catalog,&o.proof,&o.retirement,&o.source,&o.validated}){
            std::copy_n(reinterpret_cast<const uint8_t*>(bytes.data()+at),32,hash->begin());at+=32;
        }
        catalog::Require(o.Encode()==bytes);return o;
    }
    std::string Selection()const {
        const auto digest=catalog::Hash(Encode());return "DNOHS01"+std::string(reinterpret_cast<const char*>(digest.data),32);
    }
};
inline std::optional<OrchardActivationOrigin> ReadOrchardActivationOrigin(const ChainDB& db){
    std::string origin,selection;
    const auto a=db.getRaw(OrchardActivationOriginKey,origin),b=db.getRaw(OrchardActivationSelectionKey,selection);
    if(a==Status::NotFound&&b==Status::NotFound)return {};
    catalog::Require(a==Status::Ok&&b==Status::Ok);
    auto record=OrchardActivationOrigin::Decode(origin);catalog::Require(selection==record.Selection());return record;
}
} // namespace dinero::storage
