#pragma once

#include <memory>

namespace dinero::vault {
struct VaultStateSnapshot;

// A mutation acquires this owner before the service mutex. Implementations
// must retain the selected wallet/session and a checked transaction until
// destruction. Commit returns only after durable success; callers perform
// only nonthrowing live publication afterward.
class VaultStateWrite {
public:
    virtual ~VaultStateWrite()=default;
    virtual const VaultStateSnapshot& Base() const noexcept=0;
    virtual void Commit(const VaultStateSnapshot& successor)=0;
};
class VaultStateOwner {
public:
    virtual ~VaultStateOwner()=default;
    virtual std::unique_ptr<VaultStateWrite> Begin() const=0;
};
} // namespace dinero::vault
