#include "daemon/services/rbf_policy_service.h"
#include "daemon/daemon_context.h"
#include <iostream>

namespace dinero {
namespace daemon {

RBFPolicyService::RBFPolicyService()
    : policy_(std::make_unique<policy::MempoolPolicy>()) {
}

RBFPolicyService::~RBFPolicyService() = default;

bool RBFPolicyService::Init(DaemonContext& ctx) {
    ctx_ = &ctx;
    std::cout << "[RBFPolicyService] Initialized (Replace-By-Fee policy)" << std::endl;
    return true;
}

bool RBFPolicyService::Start() {
    std::cout << "[RBFPolicyService] Started (RBF enabled)" << std::endl;
    return true;
}

void RBFPolicyService::Stop() {
    std::cout << "[RBFPolicyService] Stopped" << std::endl;
}

} // namespace daemon
} // namespace dinero
