#pragma once
#include "wallet/orchard_account_state.h"
namespace dinero { class WalletManager; class RuntimeAccountReplay; }
namespace dinero::wallet {
// A bound account consumer, not notification-provider readiness. Source events,
// prepared transitions and immutable restore lookups are acquired/validated
// before entering this owner. Lookups must never acquire chain locks or wait on
// a thread needing the wallet. Initial baseline enrollment is a separate duty.
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
    struct Enrolled {
        uint32_t number; Applied state;
        // Reached, authenticated archive identities/revisions in head order.
        // Recovery compares these again before effects and final completion.
        std::vector<std::pair<orchard::Hash,uint64_t>> archive_revisions;
    };
    // Discover current rows, then authenticate and fully restore every account
    // in one SQLite snapshot under key ownership. Row locators are not an
    // authenticated catalog: this does not detect prior deletion or certify
    // initial/late-account enrollment. Only archive rows reached from authenticated
    // account heads may use derived identities; unrelated rows still refuse.
    static std::vector<Enrolled> ReadEnrolledForReplay(
        WalletManager&,uint64_t session,const RuntimeAccountReplay&);
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
};
} // namespace dinero::wallet
