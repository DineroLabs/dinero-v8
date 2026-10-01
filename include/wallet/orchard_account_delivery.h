#pragma once
#include <string>
#include <memory>
#include "wallet/orchard_account_state.h"
#include "wallet/orchard_account_catalog.h"
#include "wallet/orchard_proof_jobs.h"
namespace dinero { class WalletManager; class RuntimeAccountReplay; }
namespace dinero::wallet {
// A bound account consumer, not notification-provider readiness. Source events,
// prepared transitions and immutable restore lookups are acquired/validated
// before entering this owner. Lookups must never acquire chain locks or wait on
// a thread needing the wallet. Transparent baselines and account creation are separate duties.
class OrchardAccountDelivery {
public:
    struct Profile { orchard::SigningDomain domain; uint32_t activation, account; };
    struct RestorePoint {
        storage::OrchardStoredState checkpoint;
        OrchardWalletRestoreLookups lookups;
    };
    struct Applied { uint64_t revision; OrchardAccountState account; };
    // Requires an existing encrypted account under the real database identity.
    // No cursor setter, implicit enrollment or rescan reset is exposed here.
    static Applied Read(WalletManager&, uint64_t session, const Profile&, const RestorePoint&);
    // Authenticated metadata chooses a cursor only inside this owner; full
    // account restoration against its immutable branch view must then succeed.
    // No metadata-only spendable account or unauthenticated cursor is exposed.
    static Applied ReadForReplay(WalletManager&,uint64_t session,const Profile&,const RuntimeAccountReplay&);
    struct IssuedReceiver { uint64_t revision; std::string address; };
    // Existing authenticated account only. Capture replay material before
    // wallet ownership. The FULL transaction retains the previous envelope
    // and advances just its issuance counter before returning the address.
    // Never creates identity/schema/account or resets scan/pending state.
    // An authenticated captured prefix is not current-chain spend readiness.
    static IssuedReceiver IssueReceiverForReplay(WalletManager&,uint64_t session,
        const Profile&,const RuntimeAccountReplay&,orchard::WalletScope);

    // Generated catalog only: fully authenticate every existing current and
    // retained owner, allocate a never-used number and its encrypted origin
    // snapshot in one FULL transaction. Returns its first external receiver.
    // Origin is actual immutable activation-parent state, not caught-up progress.
    // Legacy/recovery unknown, missing declared accounts, and foreign rows refuse.
    static IssuedReceiver CreateAccountForReplay(WalletManager&,uint64_t session,
        const Profile&,const RuntimeAccountReplay&);

    // Actual receiving RPC boundary: reconcile the authenticated generated
    // catalog against every current/reached account and retained predecessor
    // before advancing the requested existing account. Unknown inventory refuses.
    // This does not assert chain catch-up or change the narrow legacy read API.
    static IssuedReceiver IssueCatalogReceiverForReplay(WalletManager&,uint64_t session,
        const Profile&,const RuntimeAccountReplay&,orchard::WalletScope);

    // A pure Orchard-input spend. The randomized plan escapes only after its
    // nullifiers and any internal change receiver are retained and committed.
    // Caller must prove outside the wallet lease, persist Ready through the
    // bound owner below, then perform fresh selected-chain admission. No relay
    // or readiness claim is made by this captured-source transaction.
    struct PreparedSpend {
        uint64_t revision;
        orchard::Hash operation_id;
        OrchardOperationQueue committed_operations;
        orchard::WalletBundlePlan plan;
        orchard::SigningContext signing;
        std::vector<orchard::TransparentOutput> transparent_outputs;
        uint64_t fee_una;
    };
    static PreparedSpend ReserveCatalogSpendForReplay(WalletManager&,uint64_t session,
        const Profile&,uint64_t expected_revision,const RuntimeAccountReplay&,
        const orchard::Hash& operation_id,std::span<const orchard::WalletPayment>,
        std::span<const orchard::TransparentOutput>,uint64_t fee_una);
    struct QueuedSpend {
        uint64_t revision;
        orchard::Hash operation_id;
        std::vector<orchard::TransparentOutput> transparent_outputs;
        uint64_t fee_una;
        bool enqueued = false;
        bool existing_request = false;
        bool archived = false;
        std::optional<OrchardOperationQueue::Entry> durable{};
        std::optional<OrchardAccountState::OperationObservation> observation{};
    };
    // Service lifetime must outlive this call. Capacity/task/context/result
    // allocation happens before Reserved commits. Returns a preallocated owner;
    // stopped executor is explicit enqueued=false, never a cancellation permit.
    static std::unique_ptr<QueuedSpend> QueueCatalogSpendForReplay(
        WalletManager&,uint64_t session,const Profile&,uint64_t expected_revision,
        const RuntimeAccountReplay&,const orchard::Hash&,
        std::span<const orchard::WalletPayment>,std::span<const orchard::TransparentOutput>,
        uint64_t fee_una,OrchardProofJobs&);
    // Request ID is bound to the exact ordered recipients/memos/outputs/fee
    // and authenticated wallet/domain/account. Matching retries return saved
    // current/archive bytes without preparing or publishing another proof.
    // Legacy unknown bindings and changed request contents refuse. Revision
    // precondition applies to new work; a retry may carry its original revision.
    // enqueued means THIS call published a task, not durable delivery/readiness.
    static std::unique_ptr<QueuedSpend> QueueCatalogRequestForReplay(
        WalletManager&,uint64_t session,const Profile&,uint64_t expected_revision,
        const RuntimeAccountReplay&,const orchard::Hash& request_id,
        std::span<const orchard::WalletPayment>,std::span<const orchard::TransparentOutput>,
        uint64_t fee_una,OrchardProofJobs&);
    struct OwnedProof {
        uint64_t revision;
        std::optional<OrchardProofJobs::State> state;
        std::unique_ptr<orchard::ProvedWalletBundle> proof;
    };
    // Fully authenticate all declared/reached owners in the caller-captured
    // source before inspecting a proof job. Pin persistent wallet identity,
    // manager instance, live session, network/branch and account. Copy only;
    // checked read COMMIT precedes return. Missing job never authorizes retry,
    // cancellation, regeneration or release of a durable reservation.
    static OwnedProof ReadCatalogProofForReplay(WalletManager&,uint64_t session,
        const Profile&,const RuntimeAccountReplay&,const orchard::Hash&,OrchardProofJobs&);
    // Exact existing reservation and authorization only. Signed bytes must not
    // leave the host for admission/relay until this checked commit returns.
    // Ready retries preserve identical bytes; no cancellation/release API is
    // exposed here because proof exposure cannot be inferred from absence.
    static Applied ReadyCatalogSpendForReplay(WalletManager&,uint64_t session,
        const Profile&,uint64_t expected_revision,const RuntimeAccountReplay&,
        const orchard::Hash&,const consensus::VerifiedOrchardAuthorizations&);

    struct FinalizedProof {
        Applied state;
        bool retired_job;
    };
    // Authenticate the current catalog and exact owned completed job, retain
    // its verified signed envelope as Ready, then retire only that captured
    // executor job after checked COMMIT. No proof bytes escape on failure.
    // Exact Ready retry may succeed without a job; it performs no new proof,
    // selection or reservation. Retirement is optional cleanup, not admission.
    // Caller verifies authorizations outside wallet ownership and still needs
    // fresh selected-chain admission before relay.
    static FinalizedProof FinalizeCatalogProofForReplay(WalletManager&,uint64_t session,
        const Profile&,const RuntimeAccountReplay&,const orchard::Hash&,
        const consensus::VerifiedOrchardAuthorizations&,OrchardProofJobs&);

    struct Enrolled {
        uint32_t number; Applied state;
        // Reached, authenticated archive identities/revisions in head order.
        // Recovery compares these again before effects and final completion.
        std::vector<std::pair<orchard::Hash,uint64_t>> archive_revisions;
    };
    // Discover current rows, then authenticate and fully restore every account
    // in one SQLite snapshot under key ownership. Row locators are not an
    // authenticated catalog: this does not detect prior deletion or certify
    // account completeness. A zero receipt must fully restore at the exact
    // source origin; recovery scans every event to enroll it. Only archive rows reached from authenticated
    // account heads may use derived identities; unrelated rows still refuse.
    static std::vector<Enrolled> ReadEnrolledForReplay(
        WalletManager&,uint64_t session,const RuntimeAccountReplay&);
    struct CatalogEnrolled {
        OrchardAccountCatalog::Snapshot catalog;
        std::vector<Enrolled> accounts;
    };
    // Complete declared inventory under one checked transaction. Known generated
    // empty is distinct from unknown recovery/missing metadata; it permits no
    // stray rows and performs no schema/account initialization. Nonempty catalogs
    // require every declared current/reached/retained owner and recorded parent.
    static CatalogEnrolled ReadCatalogForReplay(
        WalletManager&,uint64_t session,const RuntimeAccountReplay&);
    // Recovery entry points use an immutable selected source view captured
    // before wallet ownership. Reconcile every referenced archive before/after
    // effects in the SAME account transaction. Missing ancestry/capacity refuses.
    static Applied ApplyForReplay(WalletManager&,uint64_t session,const Profile&,
        uint64_t expected_revision,const RuntimeAccountReplay&,uint64_t sequence);
    static Applied ReconcileForReplay(WalletManager&,uint64_t session,const Profile&,
        uint64_t expected_revision,const RuntimeAccountReplay&);
    static Applied Connect(WalletManager&, uint64_t session, const Profile&,
        uint64_t expected_revision, const RestorePoint&, const RuntimeOutboxEvent&,
        const OrchardBlockCandidate&, const consensus::PreparedOrchardState&,
        std::span<const consensus::VerifiedOrchardAuthorizations>);
    // The parent revision comes from the current authenticated account payload,
    // never a caller-selected locator. Authenticate the retained revision and
    // check the immediate-parent scan before writes. Missing history refuses.
    static Applied Disconnect(WalletManager&, uint64_t session, const Profile&,
        uint64_t expected_revision, const RestorePoint&, const RuntimeOutboxEvent&,
        const OrchardBlockCandidate&, const RestorePoint& parent);
    static Applied Historical(WalletManager&, uint64_t session, const Profile&,
        uint64_t expected_revision, const RestorePoint&, const RuntimeOutboxEvent&);
private:
    struct SpendResult { std::unique_ptr<PreparedSpend> direct; std::unique_ptr<QueuedSpend> queued; };
    static SpendResult ReserveSpendForReplay(WalletManager&,uint64_t,const Profile&,uint64_t,
        const RuntimeAccountReplay&,const orchard::Hash&,std::span<const orchard::WalletPayment>,
        std::span<const orchard::TransparentOutput>,uint64_t,OrchardProofJobs*,bool bind_request=false);

    struct Owner;
};
} // namespace dinero::wallet
