#pragma once
#include "vault/state_snapshot.h"
#include <memory>

namespace dinero { class WalletManager; class WalletService; }
namespace dinero::vault {
using VaultIdentity=std::array<uint8_t,32>;
struct VaultStateDomain {
    uint8_t network{0};
    std::array<uint8_t,32> genesis{};
};
struct StoredVaultState {
    VaultIdentity identity{};
    std::array<uint8_t,32> predecessor{};
    std::array<uint8_t,32> digest{};
    VaultStateSnapshot state;
};

// Authenticated summaries of all PRESENT rows in one wallet transaction.
// Missing storage is an error. This is not an authenticated catalog or proof
// against deletion/whole-backup rollback; it never authorizes recreation.
struct VaultStateSummary {
    VaultIdentity identity{};
    uint64_t revision{0};
    std::optional<VaultOperatorBinding> operator_binding;
};

// One wallet-bound FULL SQLite transaction with the selected session and
// recovery seed pinned for its entire lifetime. Acquire before the vault
// mutex; acquire selected-chain observations before this owner. Never call
// wallet signing/admission or chain callbacks while holding this transaction.
//
// This does not certify catalog/deletion/whole-backup completeness. Missing
// existing owners always refuse. Only an explicit CreateNew operation may
// allocate a fresh identity and insert an empty initial state; it never
// reconstructs or replaces a historical vault from absent storage.
class VaultStateTransaction {
public:
    static std::unique_ptr<VaultStateTransaction> CreateNew(
        WalletManager&,uint64_t expected_session,const VaultStateDomain&,
        const VaultServiceConfig& initial_config);
    // Explicit new creation requires a presently resolvable exact operator
    // signing key under the same pinned wallet/SQLite owner, before insertion.
    // Existing CreateNew remains the narrow component factory; no old record
    // is silently enrolled or relabelled by either path.
    static std::unique_ptr<VaultStateTransaction> CreateNewOwned(
        WalletManager&,uint64_t,const VaultStateDomain&,const VaultServiceConfig&);
    static std::vector<VaultStateSummary> ListExisting(
        WalletManager&,uint64_t,const VaultStateDomain&);
    static std::unique_ptr<VaultStateTransaction> OpenExisting(
        WalletManager&,uint64_t expected_session,const VaultStateDomain&,const VaultIdentity&);
    ~VaultStateTransaction();
    VaultStateTransaction(const VaultStateTransaction&)=delete;
    VaultStateTransaction& operator=(const VaultStateTransaction&)=delete;
    const StoredVaultState& Current() const noexcept;
    // One successor only; exact current ciphertext/revision is compared before
    // replacement. It is not published or acknowledged until checked Commit.
    void Stage(const VaultStateSnapshot& successor);
    void Commit();
    const StoredVaultState& Committed() const;
private:
    static std::unique_ptr<VaultStateTransaction> CreateNewImpl(
        WalletManager&,uint64_t,const VaultStateDomain&,const VaultServiceConfig&,bool);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit VaultStateTransaction(std::unique_ptr<Impl>);
};

using VaultWithdrawalDispatcherFactory=std::function<std::shared_ptr<VaultWithdrawalDispatcher>(const VaultIdentity&)>;
struct BoundVaultService {
    VaultIdentity identity{};
    std::shared_ptr<VaultService> service;
};

// Retains the daemon WalletService, acquires its real WalletUse for every
// operation, and requires the selected session captured by this attachment.
// A restart/switch must explicitly reopen the same authenticated identity.
class WalletVaultStateOwner final : public VaultStateOwner {
public:
    static BoundVaultService CreateNewService(
        std::shared_ptr<WalletService>,uint64_t,const VaultStateDomain&,
        const VaultServiceConfig&,std::unique_ptr<SigningBackend>,
        VaultService::BlockHashAtHeightFn,VaultService::TxIncludedAtFn,VaultTipSnapshotFn,
        VaultWithdrawalDispatcherFactory = {});
    static BoundVaultService OpenExistingService(
        std::shared_ptr<WalletService>,uint64_t,const VaultStateDomain&,const VaultIdentity&,
        std::unique_ptr<SigningBackend>,VaultService::BlockHashAtHeightFn,
        VaultService::TxIncludedAtFn,VaultTipSnapshotFn, VaultWithdrawalDispatcherFactory = {});
    std::unique_ptr<VaultStateWrite> Begin() const override;
private:
    WalletVaultStateOwner(std::shared_ptr<WalletService>,uint64_t,
                         const VaultStateDomain&,const VaultIdentity&);
    std::shared_ptr<WalletService> wallet_;
    uint64_t session_;
    VaultStateDomain domain_;
    VaultIdentity identity_;
};
} // namespace dinero::vault
