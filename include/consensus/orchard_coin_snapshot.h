#pragma once

#include "consensus/chain_state_view.h"
#include "orchard_transaction.h"
#include <stdexcept>

namespace dinero::consensus {

// A failed database read must not be reported as a consensus-invalid spend.
class OrchardCoinLookupError : public std::runtime_error {
public:
    explicit OrchardCoinLookupError(Status status)
        : std::runtime_error("Orchard coin lookup failed"), status_(status) {}
    Status SourceStatus() const noexcept { return status_; }
private:
    Status status_;
};

// Staged resolution only: NOT a transaction-validity or spendability result.
// The host MUST hold the chainstate lock (or supply an immutable authenticated
// view) throughout resolution and subsequent validation/application. Height is
// diagnostic context, not a state identity or a substitute for that lock.
class OrchardCoinSnapshot {
public:
    [[nodiscard]] static OrchardCoinSnapshot ResolveUnderChainstateLock(
        const orchard::TransactionEnvelope& transaction, const ChainStateView& view);

    const orchard::TransactionEnvelope& Transaction() const noexcept { return transaction_; }
    const std::vector<UTXOEntry>& Coins() const noexcept { return coins_; }
    uint32_t ViewHeight() const noexcept { return view_height_; }
    orchard::Hash SigningDigest(orchard::SigningDomain domain) const;
    [[nodiscard]] orchard::VerifiedEnvelopeAuthorization VerifyOrchardAuthorization(
        orchard::SigningDomain domain) const;

private:
    OrchardCoinSnapshot(orchard::TransactionEnvelope transaction,
                        std::vector<UTXOEntry> coins, uint32_t view_height);
    std::vector<orchard::PreviousOutput> PreviousOutputs() const;
    const orchard::TransactionEnvelope transaction_;
    const std::vector<UTXOEntry> coins_;
    const uint32_t view_height_;
};
} // namespace dinero::consensus
