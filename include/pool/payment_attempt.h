#pragma once
#include "pool/pool_types.h"
#include <memory>
namespace dinero::pool {
class PayoutProcessor;
class PoolDB;
struct PoolPaymentWalletBinding {
    PoolPaymentFunding funding;
    std::array<uint8_t,32> wallet{};
    uint8_t network{0};
    std::array<uint8_t,32> genesis{};
    bool operator==(const PoolPaymentWalletBinding&) const = default;
};
struct PoolPaymentMember {
    uint64_t payout_id{0},block_id{0},amount{0};
    std::string worker;
    std::array<uint8_t,32> origin{};
    bool operator==(const PoolPaymentMember&) const = default;
};
// A retained body is an authenticated wallet origin, never chain settlement.
struct PoolPaymentRetained {
    std::array<uint8_t,32> txid{},body_sha256{};
    uint64_t fee_una{0};
    uint32_t vout{0};
    bool operator==(const PoolPaymentRetained&) const = default;
};
// An as-of canonical inclusion, reversible when its exact block disconnects.
struct PoolPaymentSettlement {
    std::array<uint8_t,32> block{};
    uint32_t height{0};
    uint64_t block_time{0};
    bool operator==(const PoolPaymentSettlement&) const = default;
};
struct PoolPaymentAttempt {
    std::array<uint8_t,16> id{};
    PoolPaymentWalletBinding binding;
    std::string address;
    uint64_t amount{0};
    std::vector<PoolPaymentMember> members; // strictly ordered by origin
    std::optional<PoolPaymentRetained> retained;
    std::optional<PoolPaymentSettlement> settlement;
    bool operator==(const PoolPaymentAttempt&) const = default;
};
// Only an active processor can call the bound wallet adapter. The processor
// commits a new attempt before DispatchNew. Existing attempts call Resolve
// only, including after any exception or restart. No wallet callback may run
// while the pool SQLite/manager owner is held.
class PoolPaymentDispatcher {
public:
    virtual ~PoolPaymentDispatcher()=default;
private:
    friend class PayoutProcessor;
    virtual const PoolPaymentWalletBinding& Binding() const=0;
    virtual std::optional<PoolPaymentRetained> DispatchNew(const PoolPaymentAttempt&)=0;
    virtual std::optional<PoolPaymentRetained> Resolve(const PoolPaymentAttempt&)=0;
    virtual bool Reconcile(PoolDB&,const PoolPaymentAttempt&)=0;
};
class PoolPaymentBackend {
public:
    virtual ~PoolPaymentBackend()=default;
private:
    friend class PayoutProcessor;
    virtual std::unique_ptr<PoolPaymentDispatcher> Bind(const PoolPaymentFunding&)=0;
};
} // namespace dinero::pool
