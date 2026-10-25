#pragma once
#include "consensus/orchard_validation_profile.h"
#include "consensus/orchard_state_error.h"
#include "storage/orchard_activation_origin.h"
#include "storage/orchard_snapshot_base.h"
#include "storage/orchard_storage_mode.h"
#include "common/annotated_mutex.h"

namespace dinero {class OrchardSnapshotStore;}
namespace dinero::consensus {
// Distinct from independent execution of the pre-Orchard history. Only the
// authenticated, completed closed-store handoff may mint this bound owner.
// It preserves the original validated anchor through connect AND disconnect;
// origin rows or matching markers alone cannot construct it.
class OrchardSelectedConsistency final {
public:
    OrchardSelectedConsistency(const OrchardSelectedConsistency&)=delete;
    OrchardSelectedConsistency& operator=(const OrchardSelectedConsistency&)=delete;
    void Check(const ChainDB& db)const {
        mutex_->AssertHeld("authenticated selected Orchard consistency");
        if(&db!=database_||db.open_generation_!=generation_||profile_!=OrchardValidationProfile())
            throw OrchardStateLookupError(Status::Corruption,"selected-origin/owner");
        const auto origin=storage::ReadOrchardActivationOrigin(db);
        const auto marker=db.getValidatedTip();const auto tip=db.getTip();
        if(!origin||origin->Encode()!=origin_||
            !marker.ok()||marker->height!=int(anchor_height_)||marker->hash!=anchor_||
            !tip.ok()||tip->height<int(parent_height_)||storage::ReadOrchardSnapshotBase(db)||
            !storage::ReadOrchardCompactStorageBinding(db))
            throw OrchardStateLookupError(Status::Corruption,"selected-origin/consistency");
    }
private:
    friend class ::dinero::OrchardSnapshotStore;
    OrchardSelectedConsistency(const ChainDB& db,AnnotatedRecursiveMutex& mutex,
        const storage::OrchardActivationOrigin& origin)
        :database_(&db),mutex_(&mutex),generation_(db.open_generation_),origin_(origin.Encode()),
         profile_(OrchardValidationProfile()),anchor_(origin.validated),
         anchor_height_(origin.validated_height),parent_height_(origin.parent_height){Check(db);}
    const ChainDB* database_;AnnotatedRecursiveMutex* mutex_;uint64_t generation_;
    const std::string origin_,profile_;const uint256 anchor_;
    const uint32_t anchor_height_,parent_height_;
};
} // namespace dinero::consensus
