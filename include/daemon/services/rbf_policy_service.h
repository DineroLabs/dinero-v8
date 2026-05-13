#pragma once

#include "daemon/iservice.h"
#include "policy/mempool_policy.h"
#include <memory>

namespace dinero {
namespace daemon {

/**
 * Architecture V3: Service wrapper for RBF (Replace-By-Fee) policy
 * Manages transaction replacement rules and fee requirements
 */
class RBFPolicyService : public IService {
public:
    RBFPolicyService();
    ~RBFPolicyService() override;

    std::string Name() const override { return "RBFPolicyService"; }
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Direct access to policy
    policy::MempoolPolicy* getPolicy() { return policy_.get(); }
    const policy::MempoolPolicy* getPolicy() const { return policy_.get(); }

private:
    DaemonContext* ctx_{nullptr};
    std::unique_ptr<policy::MempoolPolicy> policy_;
};

} // namespace daemon
} // namespace dinero
