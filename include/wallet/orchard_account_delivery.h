#pragma once
#include <string>
#include <memory>
#include <functional>
#include <stdexcept>
#include "wallet/orchard_account_state.h"
#include "wallet/orchard_account_catalog.h"
#include "wallet/orchard_proof_jobs.h"
namespace dinero { class ChainstateService; class RuntimeOrdinaryDelivery; class RuntimeWalletRecovery; struct RuntimeOutboxCursor; class WalletManager; class RuntimeAccountReplay; struct WalletSigningIdentity; struct UnsignedTransaction; struct PendingPaymentIntent; struct SignResult; }
namespace dinero::wallet {
// An unforgeable, process-local capture of authenticated account bytes restored
// against one retained immutable replay. This carries no durable authority:
// the writer must authenticate the complete capture again in its transaction.
class OrchardCatalogCapture final {
public:
    ~OrchardCatalogCapture();
    OrchardCatalogCapture(const OrchardCatalogCapture&)=delete;
    OrchardCatalogCapture& operator=(const OrchardCatalogCapture&)=delete;
private:
    friend class OrchardAccountDelivery;
    struct Data;
    explicit OrchardCatalogCapture(std::unique_ptr<Data>);
    std::unique_ptr<Data> data_;
};
// A private recovery computation over one fully authenticated catalog capture.
// No lease, seed or SQLite connection survives preparation. Its encoded steps
// are consumed only by the bound writer after a fresh complete-capture check.
class OrchardCatalogRecoveryPlan final {
public:
    ~OrchardCatalogRecoveryPlan();
    OrchardCatalogRecoveryPlan(const OrchardCatalogRecoveryPlan&)=delete;
    OrchardCatalogRecoveryPlan& operator=(const OrchardCatalogRecoveryPlan&)=delete;
private:
    friend class OrchardAccountDelivery;
    struct Data;
    explicit OrchardCatalogRecoveryPlan(std::unique_ptr<Data>);
    std::unique_ptr<Data> data_;
};
// Process-local preparation for one exact Ready transition. It contains no
// wallet lease, seed, SQLite connection or replay callback. Preparation restores
// outside selected ownership; commit consumes it after rechecking the catalog.
class OrchardCatalogFinalizationPlan final {
public:
    ~OrchardCatalogFinalizationPlan();
    OrchardCatalogFinalizationPlan(const OrchardCatalogFinalizationPlan&)=delete;
    OrchardCatalogFinalizationPlan& operator=(const OrchardCatalogFinalizationPlan&)=delete;
private:
    friend class OrchardAccountDelivery;
    struct Data;
    explicit OrchardCatalogFinalizationPlan(std::unique_ptr<Data>);
    std::unique_ptr<Data> data_;
};
// Owned plan returned after a checked reservation commit. The alias below
// retains the existing account API while allowing a source boundary to return
// this owner without exposing the entire account-delivery header.
struct OrchardPreparedSpend {
        uint64_t revision;
        orchard::Hash operation_id;
        OrchardOperationQueue committed_operations;
        orchard::WalletBundlePlan plan;
        orchard::SigningContext signing;
        std::vector<orchard::TransparentOutput> transparent_outputs;
        uint64_t fee_una;
    };
struct OrchardQueuedSpend {
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
// Prepared after durable Reserved, but not yet visible to the executor. The
// selected host releases its source owner before publishing. Dropping this
// ticket never cancels or releases its already committed reservation.
struct OrchardShieldRequest {
    std::unique_ptr<OrchardQueuedSpend> result;
    std::unique_ptr<OrchardProofJobs::Submission> submission;
    [[nodiscard]] std::unique_ptr<OrchardQueuedSpend> Publish() && noexcept {
        if(submission)result->enqueued=submission->Publish();
        return std::move(result);
    }
};
// A bound account consumer, not notification-provider readiness. Source events,
// prepared transitions and immutable restore lookups are acquired/validated
// before entering this owner. Lookups must never acquire chain locks or wait on
// a thread needing the wallet. Transparent baselines and account creation are separate duties.
class OrchardAccountDelivery {
public:
    struct Profile { orchard::SigningDomain domain; uint32_t activation, account; };
    // Ordinary payment with shared reservation ownership. This compatibility
    // entry prepares outside wallet ownership, then commits. The selected
    // service uses the private split below to prepare before its selected lock.
    // Every account must be caught up. Archived reactivation, both reservation
    // sets, signing and pending/history writes share one FULL writer transaction.
    // Unknown recovery catalogs refuse; COMMIT precedes signature publication.
    static SignResult SignAndStageOrdinaryForReplay(WalletManager&,
        const WalletSigningIdentity&,const RuntimeAccountReplay&,
        const UnsignedTransaction&,const PendingPaymentIntent&);
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

    // Generated catalog only: capture and fully restore existing current and
    // retained owners before the writer, then recheck and allocate a never-used
    // number and its encrypted origin
    // snapshot in one FULL transaction. Returns its first external receiver.
    // Origin is actual immutable activation-parent state, not caught-up progress.
    // Legacy/recovery unknown, missing declared accounts, and foreign rows refuse.
    // Both catalog writers require a released caller wallet lease and SQL transaction.
    static IssuedReceiver CreateAccountForReplay(WalletManager&,uint64_t session,
        const Profile&,const RuntimeAccountReplay&);

    // Actual receiving RPC boundary: capture and restore the complete generated
    // catalog outside wallet/seed/SQL owners, then recheck its exact bytes before
    // advancing the requested counter in a FULL transaction. Unknown inventory refuses.
    // This does not assert chain catch-up or change the narrow legacy read API.
    static IssuedReceiver IssueCatalogReceiverForReplay(WalletManager&,uint64_t session,
        const Profile&,const RuntimeAccountReplay&,orchard::WalletScope);

    // A pure Orchard-input spend. The randomized plan escapes only after its
    // nullifiers and any internal change receiver are retained and committed.
    // Caller must prove outside the wallet lease, persist Ready through the
    // bound owner below, then perform fresh selected-chain admission. No relay
    // or readiness claim is made by this captured-source transaction.
    using PreparedSpend = OrchardPreparedSpend;
    static PreparedSpend ReserveCatalogSpendForReplay(WalletManager&,uint64_t session,
        const Profile&,uint64_t expected_revision,const RuntimeAccountReplay&,
        const orchard::Hash& operation_id,std::span<const orchard::WalletPayment>,
        std::span<const orchard::TransparentOutput>,uint64_t fee_una);
    // Transparent-input shielding reservation component. The selected source
    // owner must authenticate exact unspent coins/maturity and retain its head
    // while calling. This owner checks durable wallet coins and BOTH ordinary
    // and reconciled Orchard reservations before a randomized plan may escape.
    // Commit precedes return; caller proves/signs only afterward. No request
    // retry, service/RPC host, transparent signature or admission is implied.
    static std::unique_ptr<PreparedSpend> ReserveCatalogShieldForReplay(WalletManager&,uint64_t session,
        const Profile&,uint64_t expected_revision,const RuntimeAccountReplay&,
        const orchard::Hash& operation_id,std::span<const orchard::ResolvedInput>,
        std::span<const orchard::WalletPayment>,std::span<const orchard::TransparentOutput>,
        uint64_t fee_una);
    using QueuedSpend = OrchardQueuedSpend;
    // Exact ordered input/recipient/memo/change/fee request binding. Reserve
    // under the caller's selected source owner, claim executor capacity before
    // committing, and publish work only after COMMIT. Matching current/archive
    // retries return retained state without generating or re-enqueueing a plan.
    static std::unique_ptr<QueuedSpend> QueueCatalogShieldRequestForReplay(
        WalletManager&,uint64_t session,const Profile&,uint64_t expected_revision,
        const RuntimeAccountReplay&,const orchard::Hash&,std::span<const orchard::ResolvedInput>,
        std::span<const orchard::WalletPayment>,std::span<const orchard::TransparentOutput>,
        uint64_t fee_una,OrchardProofJobs&);
    // Selected service staging: commits Reserved and returns an unpublished
    // executor ticket. The source owner must be released before Publish().
    static OrchardShieldRequest PrepareCatalogShieldRequestForReplay(
        WalletManager&,uint64_t session,const Profile&,uint64_t expected_revision,
        const RuntimeAccountReplay&,const orchard::Hash&,std::span<const orchard::ResolvedInput>,
        std::span<const orchard::WalletPayment>,std::span<const orchard::TransparentOutput>,
        uint64_t fee_una,OrchardProofJobs&);
    // Read-only authenticated retry lookup. Missing is explicit null; errors
    // throw. Never prepares a plan or consults executor/coin availability.
    // Authenticate intent before automatic selection. A match returns its
    // exact retained inputs/change; older unknown details refuse, never infer.
    // Candidate capture is read-only and requires every enrolled account at
    // the captured source head. Selected coin validation remains with service.
    static std::vector<orchard::ResolvedInput> ReadCatalogShieldCandidatesForReplay(
        WalletManager&,uint64_t session,const Profile&,uint64_t expected_revision,
        const RuntimeAccountReplay&);
    static std::unique_ptr<QueuedSpend> FindStoredShieldRequestForReplay(
        WalletManager&,uint64_t session,const Profile&,const RuntimeAccountReplay&,
        const orchard::Hash&,std::span<const orchard::WalletPayment>,uint64_t fee);
    static std::unique_ptr<QueuedSpend> FindCatalogShieldRequestForReplay(
        WalletManager&,uint64_t session,const Profile&,const RuntimeAccountReplay&,
        const orchard::Hash&,std::span<const orchard::ResolvedInput>,
        std::span<const orchard::WalletPayment>,std::span<const orchard::TransparentOutput>,uint64_t fee_una);
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
    // Full catalog restoration releases wallet, seed and SQLite ownership.
    // Borrowed caller leases/transactions refuse. A complete owned recheck and
    // checked read COMMIT precede return of a proof copy; jobs remain retained.
    static OwnedProof ReadCatalogProofForReplay(WalletManager&,uint64_t session,
        const Profile&,const RuntimeAccountReplay&,const orchard::Hash&,OrchardProofJobs&);
    struct RequestProof {
        uint64_t revision;
        OrchardOperationQueue::Entry durable;
        std::optional<OrchardProofJobs::State> state;
        std::unique_ptr<orchard::ProvedWalletBundle> proof;
    };
    // Read-only completion capture. Require an existing current request with
    // the exact ordered recipients/memos/outputs/fee before looking up its job.
    // Authenticate the complete catalog and pin the actual wallet/session.
    // Unknown/archived IDs cannot reserve, requeue or recreate any operation.
    static RequestProof ReadCatalogRequestProofForReplay(WalletManager&,uint64_t session,
        const Profile&,const RuntimeAccountReplay&,const orchard::Hash&,
        std::span<const orchard::WalletPayment>,std::span<const orchard::TransparentOutput>,
        uint64_t fee_una,OrchardProofJobs&);
    // Read-only owned shield proof capture, bound to the same complete request.
    // Missing jobs/archived IDs cannot recreate work or release reservations.
    static RequestProof ReadCatalogShieldRequestProofForReplay(
        WalletManager&,uint64_t session,const Profile&,const RuntimeAccountReplay&,
        const orchard::Hash&,std::span<const orchard::ResolvedInput>,
        std::span<const orchard::WalletPayment>,std::span<const orchard::TransparentOutput>,
        uint64_t fee_una,OrchardProofJobs&);
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
    // Both snapshots authenticated under the same wallet instance and identity,
    // but their catalog content changed while restoration released ownership.
    // Callers may recapture source and catalog; corruption/session errors retain
    // their original failure and are never classified as this retryable result.
    class CatalogChanged final : public std::runtime_error {
    public:
        CatalogChanged():std::runtime_error("Authenticated Orchard catalog changed during read"){}
    };
    struct CatalogEnrolled {
        OrchardAccountCatalog::Snapshot catalog;
        std::vector<Enrolled> accounts;
    };
    // Capture the complete authenticated declared inventory in one transaction,
    // restore owned current/parent payloads after releasing this API's owners,
    // then authenticate/recheck the full capture before return. A caller's outer
    // lease remains its responsibility. Known generated-empty permits no stray
    // rows or schema initialization; unknown recovery/missing owners refuse.
    // This as-of observation is not authority to commit later wallet effects.
    static CatalogEnrolled ReadCatalogForReplay(
        WalletManager&,uint64_t session,const RuntimeAccountReplay&);
    // Same complete catalog/current/reached/retained validation inside the
    // caller's active wallet lease and SQLite transaction. Read-only: never
    // starts, commits or rolls back that transaction, including on refusal.
    // The caller must acquire the immutable replay before wallet ownership and
    // retain its lease through subsequent effects and checked commit. This
    // enables account and transparent-store preflight in one SQL snapshot;
    // it does not itself authorize a receipt replacement or publish readiness.
    static CatalogEnrolled ReadCatalogForReplayInTransaction(
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
    friend struct OrchardDetachedShieldSigningTestAccess;
    static std::unique_ptr<orchard::TransactionEnvelope> SignShieldWithRestorePoints(
        WalletManager&,uint64_t,const Profile&,const RuntimeAccountReplay&,const orchard::Hash&,
        std::span<const orchard::ResolvedInput>,std::span<const orchard::WalletPayment>,
        std::span<const orchard::TransparentOutput>,uint64_t,const consensus::OrchardCoinSnapshot&,OrchardProofJobs&,
        const std::function<RestorePoint(RuntimeOutboxCursor)>&);
    friend struct OrchardDetachedFinalizationTestAccess;
    friend class OrchardCatalogFinalizationPlan;
    static std::unique_ptr<OrchardCatalogFinalizationPlan> PrepareFinalizationWithRestorePoints(
        WalletManager&,uint64_t,const Profile&,const RuntimeAccountReplay&,const orchard::Hash&,
        std::optional<uint64_t>,OrchardProofJobs*,bool shield,
        std::span<const orchard::ResolvedInput>,std::span<const orchard::WalletPayment>,
        std::span<const orchard::TransparentOutput>,uint64_t,
        const std::function<RestorePoint(RuntimeOutboxCursor)>&);
    // Does not acquire or call the source. The selected service retains its
    // already-validated selected owner through this checked Ready/history commit.
    static FinalizedProof CommitCatalogFinalization(WalletManager&,uint64_t,
        std::unique_ptr<OrchardCatalogFinalizationPlan>,const consensus::VerifiedOrchardAuthorizations&);
    friend struct OrchardDetachedIssuanceTestAccess;
    friend struct OrchardDetachedProofReadTestAccess;
    enum class CatalogProofRequest { Any, Spend, Shield };
    static RequestProof ReadCatalogProofWithRestorePoints(
        WalletManager&,uint64_t,const Profile&,const RuntimeAccountReplay&,
        CatalogProofRequest,const orchard::Hash&,std::span<const orchard::ResolvedInput>,
        std::span<const orchard::WalletPayment>,std::span<const orchard::TransparentOutput>,
        uint64_t,OrchardProofJobs&,
        const std::function<RestorePoint(RuntimeOutboxCursor)>&);
    static IssuedReceiver IssueCatalogWithRestorePoints(WalletManager&,uint64_t,
        const Profile&,const RuntimeAccountReplay&,orchard::WalletScope,bool creating,
        const std::function<RestorePoint(RuntimeOutboxCursor)>&);
    friend class OrchardCatalogCapture;
    friend class OrchardCatalogRecoveryPlan;
    friend class dinero::RuntimeWalletRecovery;
    friend struct OrchardDetachedRecoveryTestAccess;
    enum class CatalogRecoveryAction { Observe, Apply, Reconcile };
    static std::unique_ptr<OrchardCatalogRecoveryPlan> PrepareCatalogRecovery(
        WalletManager&,uint64_t,const RuntimeAccountReplay&,CatalogRecoveryAction,uint64_t=0);
    static std::unique_ptr<OrchardCatalogRecoveryPlan> PrepareCatalogRecoveryWithPoints(
        WalletManager&,uint64_t,const RuntimeAccountReplay&,CatalogRecoveryAction,uint64_t,
        const std::function<RestorePoint(RuntimeOutboxCursor)>&);
    static void CompleteCatalogRecoveryWithPoints(OrchardCatalogRecoveryPlan&,CatalogRecoveryAction,uint64_t,
        const std::function<RestorePoint(RuntimeOutboxCursor)>&);
    static const CatalogEnrolled& CatalogRecoveryBefore(const OrchardCatalogRecoveryPlan&);
    static void RecheckCatalogRecoveryInTransaction(WalletManager&,uint64_t,const OrchardCatalogRecoveryPlan&);
    static Applied CommitCatalogRecoveryAccount(WalletManager&,uint64_t,OrchardCatalogRecoveryPlan&,size_t);

    friend class dinero::RuntimeOrdinaryDelivery;
    friend struct OrchardDetachedCoverageTestAccess;
    static std::unique_ptr<OrchardCatalogCapture> PrepareCatalogForReplay(
        WalletManager&,uint64_t,std::shared_ptr<const RuntimeAccountReplay>);
    static std::unique_ptr<OrchardCatalogCapture> PrepareCatalogWithRestorePoints(
        WalletManager&,uint64_t,std::shared_ptr<const RuntimeAccountReplay>,
        const std::function<RestorePoint(RuntimeOutboxCursor)>&);
    // No restoration or source lookup occurs here. The caller owns FULL SQL
    // and selected-source checks through its subsequent effects and commit.
    static void RecheckCatalogCaptureInTransaction(WalletManager&,uint64_t,
        const std::shared_ptr<const RuntimeAccountReplay>&,const OrchardCatalogCapture&);
    friend struct OrchardDetachedCatalogTestAccess;
    // Shared implementation for the immutable replay point provider. A narrow
    // friend fixture wraps lookups to observe same-thread ownership; normal
    // callers always obtain every point from the supplied validated view.
    static CatalogEnrolled ReadCatalogWithRestorePoints(WalletManager&,uint64_t,
        const RuntimeAccountReplay&,const std::function<RestorePoint(RuntimeOutboxCursor)>&);
    friend class dinero::ChainstateService;
    friend struct OrchardDetachedOrdinaryPaymentTestAccess;
    static std::unique_ptr<OrchardCatalogRecoveryPlan> PrepareOrdinaryPaymentForReplay(
        WalletManager&,const WalletSigningIdentity&,const RuntimeAccountReplay&);
    static std::unique_ptr<OrchardCatalogRecoveryPlan> PrepareOrdinaryPaymentWithPoints(
        WalletManager&,const WalletSigningIdentity&,const RuntimeAccountReplay&,
        const std::function<RestorePoint(RuntimeOutboxCursor)>&);
    static SignResult CommitOrdinaryPaymentForReplay(WalletManager&,const WalletSigningIdentity&,
        std::unique_ptr<OrchardCatalogRecoveryPlan>,const UnsignedTransaction&,const PendingPaymentIntent&);
    // Only the actual selected source service can request signatures and Ready
    // promotion. It must keep signed intermediate bytes private until commit.
    static std::unique_ptr<orchard::TransactionEnvelope> SignShieldProofForReplay(
        WalletManager&,uint64_t,const Profile&,const RuntimeAccountReplay&,const orchard::Hash&,
        std::span<const orchard::ResolvedInput>,std::span<const orchard::WalletPayment>,
        std::span<const orchard::TransparentOutput>,uint64_t,const consensus::OrchardCoinSnapshot&,OrchardProofJobs&);
    static std::unique_ptr<OrchardCatalogFinalizationPlan> PrepareShieldFinalizationForReplay(
        WalletManager&,uint64_t,const Profile&,const RuntimeAccountReplay&,const orchard::Hash&,
        std::span<const orchard::ResolvedInput>,std::span<const orchard::WalletPayment>,
        std::span<const orchard::TransparentOutput>,uint64_t,OrchardProofJobs&);
    struct SpendResult { std::unique_ptr<PreparedSpend> direct; std::unique_ptr<QueuedSpend> queued; };
    static SpendResult ReserveSpendForReplay(WalletManager&,uint64_t,const Profile&,uint64_t,
        const RuntimeAccountReplay&,const orchard::Hash&,std::span<const orchard::WalletPayment>,
        std::span<const orchard::TransparentOutput>,uint64_t,OrchardProofJobs*,bool bind_request=false);

    friend struct OrchardDetachedSpendReservationTestAccess;
    static SpendResult ReserveSpendWithRestorePoints(WalletManager&,uint64_t,const Profile&,uint64_t,
        const RuntimeAccountReplay&,const orchard::Hash&,std::span<const orchard::WalletPayment>,
        std::span<const orchard::TransparentOutput>,uint64_t,OrchardProofJobs*,bool,
        const std::function<RestorePoint(RuntimeOutboxCursor)>&);

    struct ShieldResult { std::unique_ptr<PreparedSpend> direct; OrchardShieldRequest queued; };
    friend struct OrchardDetachedShieldReservationTestAccess;
    enum class ShieldRequestMode { Unbound, Exact, Stored };
    static std::unique_ptr<OrchardCatalogRecoveryPlan> PrepareShieldReservationForReplay(
        WalletManager&,uint64_t,const Profile&,uint64_t,const RuntimeAccountReplay&,const orchard::Hash&,
        std::span<const orchard::ResolvedInput>,std::span<const orchard::WalletPayment>,
        std::span<const orchard::TransparentOutput>,uint64_t,ShieldRequestMode,bool existing_only=false);
    static std::unique_ptr<OrchardCatalogRecoveryPlan> PrepareShieldReservationWithPoints(
        WalletManager&,uint64_t,const Profile&,uint64_t,const RuntimeAccountReplay&,const orchard::Hash&,
        std::span<const orchard::ResolvedInput>,std::span<const orchard::WalletPayment>,
        std::span<const orchard::TransparentOutput>,uint64_t,ShieldRequestMode,bool,
        const std::function<RestorePoint(RuntimeOutboxCursor)>&);
    static std::unique_ptr<QueuedSpend> TakePreparedShieldRetry(WalletManager&,uint64_t,OrchardCatalogRecoveryPlan&);
    static std::vector<orchard::ResolvedInput> ReadPreparedShieldCandidates(
        WalletManager&,uint64_t,const Profile&,uint64_t,const OrchardCatalogRecoveryPlan&);
    static ShieldResult CommitShieldReservation(WalletManager&,uint64_t,std::unique_ptr<OrchardCatalogRecoveryPlan>,
        std::span<const orchard::ResolvedInput>,std::span<const orchard::TransparentOutput>,OrchardProofJobs*);
    static ShieldResult ReserveShieldForReplay(WalletManager&,uint64_t,const Profile&,uint64_t,
        const RuntimeAccountReplay&,const orchard::Hash&,std::span<const orchard::ResolvedInput>,
        std::span<const orchard::WalletPayment>,std::span<const orchard::TransparentOutput>,uint64_t,OrchardProofJobs*,bool existing_only=false);
    struct Owner;
};
} // namespace dinero::wallet
