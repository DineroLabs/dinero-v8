#pragma once
#include "wallet/wallet_manager.h"
#include <functional>

namespace dinero {class ChainstateService;namespace wallet {
class OrchardAccountDelivery;
class ShieldFundingPending final:public std::runtime_error {
public:ShieldFundingPending():std::runtime_error("Authenticated shield funding body pending"){}
};
// Lexical CURRENT input authority, never serialized or returned by an RPC.
// Only the selected source can construct it, after both historical inclusion
// (legacy maturity metadata) and current accumulator membership. This grants
// only these exact inputs to one atomic reservation, not a general coin view,
// history/index baseline, signature authorization or Ready certificate.
class AuthenticatedShieldInputs final {
    friend class dinero::ChainstateService;
    friend class OrchardAccountDelivery;
    WalletManager* const wallet_;
    const uint64_t session_;
    const uint32_t height_;
    const uint256 base_;
    const std::string root_;
    const std::vector<PendingPaymentInput> inputs_;
    const std::function<void()> current_;
    const std::vector<bool> snapshot_candidates_;
    AuthenticatedShieldInputs(WalletManager& wallet,uint64_t session,uint32_t height,uint256 base,
        std::string root,std::vector<PendingPaymentInput> inputs,std::function<void()> current,std::vector<bool> snapshot_candidates={})
        :wallet_(&wallet),session_(session),height_(height),base_(base),root_(std::move(root)),
         inputs_(std::move(inputs)),current_(std::move(current)),snapshot_candidates_(snapshot_candidates.empty()?
             std::vector<bool>(inputs_.size(),true):std::move(snapshot_candidates)){}
public:
    AuthenticatedShieldInputs(const AuthenticatedShieldInputs&)=delete;
    AuthenticatedShieldInputs& operator=(const AuthenticatedShieldInputs&)=delete;
};
}}
