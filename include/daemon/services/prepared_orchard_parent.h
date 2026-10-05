#pragma once
#include "storage/legacy_retirement.h"
#include <memory>
namespace dinero {
class ChainstateService;
// The actual service captures real storage owners and validates outside the
// selected lock. This opaque owner has the same public type in ON/OFF builds;
// backend-specific state is constructed only by an enabled service. Thread
// affinity is enforced by the replay owner. No result authorizes activation.
class PreparedOrchardParent final {
public:
    ~PreparedOrchardParent();
    PreparedOrchardParent(const PreparedOrchardParent&) = delete;
    PreparedOrchardParent& operator=(const PreparedOrchardParent&) = delete;
    const storage::LegacyRetirementRecord& Record() const;
private:
    friend class ChainstateService;
    struct State;
    explicit PreparedOrchardParent(std::unique_ptr<State> state);
    const std::unique_ptr<State> state_;
};
}
