#pragma once
#include "daemon/runtime_block_outbox.h"
#include "wallet/runtime_recovery_limits.h"
#include <optional>
#include <memory>
#include <string>
#include <functional>
#include <map>
#include <vector>
#include "wallet/wallet_manager.h"

namespace dinero {
namespace wallet { class OrchardCatalogCapture; }
class UTXOIndex;
class WalletManager;
class RuntimeWalletOriginProjection;
class RuntimeWalletCoverageProjection;
class RuntimeWalletBalanceSnapshot;
struct RuntimeIndexProgress {
    RuntimeOutboxCursor cursor;
    uint256 origin_hash, tip_hash;
    uint32_t origin_height = 0, tip_height = 0;
};
// Owns real UTXOIndex effects and one progress row in its SQLite transaction.
// The event MUST come from the selected service's checked delivery reader.
// This receipt covers only this index after the source origin, not historical
// baseline completeness, other wallet stores or all-consumer readiness.
class RuntimeIndexDelivery {
public:
    // Pins the selected wallet and durably establishes its database binding
    // before touching the index. Source acquisition precedes this call; the
    // caller captures the intended session before releasing wallet ownership.
    static std::optional<RuntimeIndexProgress> ReadForWallet(WalletManager&, UTXOIndex&, uint64_t expected_session);
    static RuntimeIndexProgress ApplyForWallet(WalletManager&, UTXOIndex&, uint64_t expected_session, const RuntimeOutboxEvent&);
private:
    friend struct RuntimeIndexDeliveryTestAccess;
    friend class RuntimeOrdinaryDelivery;
    using HistoricalScripts = std::map<std::vector<uint8_t>,std::string>;
    static bool CoverageRequired(UTXOIndex&,const std::string&,const HistoricalScripts* = nullptr);
    static void ReconcileCoverage(UTXOIndex&,const RuntimeWalletCoverageProjection&,
        const std::function<void()>& finish,const std::function<void()>& source_before_commit);
    static void CheckCoverageImportedCohort(UTXOIndex&,const RuntimeWalletOriginProjection&);
    static void WithCoverageBalance(UTXOIndex&,const RuntimeWalletCoverageProjection&,
        const std::function<void()>& capture);
    static void CheckIncrementalRows(UTXOIndex&,const RuntimeWalletCoverageProjection&);
    static void CaptureOriginDomain(UTXOIndex&,RuntimeWalletOriginProjection&);
    static void CheckOriginDomain(UTXOIndex&,const RuntimeWalletOriginProjection&);
    static std::optional<RuntimeIndexProgress> Read(UTXOIndex&, const std::string& wallet_identity, const HistoricalScripts* = nullptr);
    static RuntimeIndexProgress Apply(UTXOIndex&, const std::string& wallet_identity,
                                      const RuntimeOutboxEvent&, const RuntimeWalletOriginProjection* origin = nullptr, const std::function<void()>& finish = {}, const HistoricalScripts* = nullptr,
                                      const RuntimeWalletCoverageProjection* previous = nullptr,
                                      const RuntimeWalletCoverageProjection* next = nullptr,const std::function<void()>& source_before_commit = {});
};
// Ordinary wallet UTXOs/history and their source progress share the selected
// wallet SQLite transaction. This is independent of the index/account receipts:
// a coordinator must reconcile every store before declaring wallet readiness.
// Acquire the selected checked source before this call. Initial adoption does
// not certify the pre-origin baseline or key ownership.
class RuntimeOrdinaryDelivery {
    friend struct RuntimeOriginProjectionTestAccess;
    static WalletManager::BalanceSummary ReadCoverageBalance(WalletManager&,UTXOIndex&,
        const RuntimeWalletCoverageProjection&,const wallet::OrchardCatalogCapture&,
        const std::string& expected_wallet,const std::function<void()>& source_before_capture);
    static RuntimeIndexProgress Apply(WalletManager&,uint64_t,const RuntimeOutboxEvent&,const RuntimeWalletOriginProjection*);
    static RuntimeIndexProgress ApplyInTransaction(WalletManager&,WalletManager::DatabaseLease*,const std::string&,
        const RuntimeOutboxEvent&,const RuntimeWalletOriginProjection*);
public:
    // Producer-facing capture/check/adopt/advance surface. None of these trust
    // their caller: every one re-reads and reauthenticates the wallet and
    // index owners it touches against the projections it is handed, in its
    // own transaction, and throws on any divergence.
    static std::unique_ptr<RuntimeWalletOriginProjection> CaptureOriginDomain(WalletManager&,uint64_t,UTXOIndex* = nullptr,
        bool existing_identity_only = false,size_t material_limit_bytes = kWalletRecoveryMaterialLimitBytes);
    static void CheckOriginDomain(WalletManager&,const RuntimeWalletOriginProjection&,UTXOIndex* = nullptr);
    static void AdoptOrigin(WalletManager&,UTXOIndex&,const RuntimeWalletOriginProjection&);
    // Capture and restore before the producer takes its selected-source lock.
    // ReconcileCoverage reauthenticates the captured bytes in the write owner.
    static std::unique_ptr<wallet::OrchardCatalogCapture> PrepareCoverage(
        WalletManager&,const RuntimeWalletCoverageProjection&);
    static void ReconcileCoverage(WalletManager&,UTXOIndex&,const RuntimeWalletCoverageProjection&,
        const wallet::OrchardCatalogCapture&,const std::function<void()>& source_before_commit = {});
    // Strict classification: only BOTH genuinely absent is the Orchard-only
    // composition. Invalidated/corrupt/one-store partial coverage throws.
    static std::optional<std::pair<RuntimeIndexProgress,RuntimeIndexProgress>> ExistingCoverage(
        WalletManager&,UTXOIndex&,uint64_t);
    static RuntimeIndexProgress AdvanceCoverage(WalletManager&,UTXOIndex&,const RuntimeWalletCoverageProjection&,
        const wallet::OrchardCatalogCapture&,const std::function<void()>& source_before_commit);
    // Read-only classification of missing/invalidated/script-changed receipts.
    // Corrupt metadata and unavailable reads throw; they are not a reset flag.
    static bool CoverageRequiredForWallet(WalletManager&,UTXOIndex&,uint64_t session);
    static std::optional<RuntimeIndexProgress> ReadForWallet(WalletManager&, uint64_t expected_session);
    static RuntimeIndexProgress ApplyForWallet(WalletManager&, uint64_t expected_session, const RuntimeOutboxEvent&);
    // One owned read of the covered balance, minted as an immutable snapshot.
    // Same read and source fence as the private summary read; the fence runs
    // again after the read and the snapshot's cursor/tip come from the given
    // coverage, never from the caller. Only this read mints the snapshot.
    static std::shared_ptr<const RuntimeWalletBalanceSnapshot> ReadCoverageBalanceSnapshot(
        WalletManager&,UTXOIndex&,const RuntimeWalletCoverageProjection&,
        const wallet::OrchardCatalogCapture&,const std::string& expected_wallet,
        const std::function<void()>& source_before_capture);
};
} // namespace dinero
