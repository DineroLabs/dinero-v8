#pragma once
#include "consensus/orchard_authorization.h"
#include "orchard_wallet.h"
#include "orchard_wallet_storage.h"
#include <map>
namespace dinero::wallet {
// Wallet-local pending operations, NOT consensus or mempool admission. The host
// must select owned/unspent inputs under its wallet/chain locks. Every returned
// replacement must be staged in the same SQLite transaction as ordinary wallet
// reservations and published only after commit. Never broadcast before the
// Ready state is durable. Restored Ready bytes require fresh node admission.
class OrchardOperationQueue {
public:
  enum class Phase : uint8_t { Reserved = 0, Ready = 1 };
  struct Entry {
    Phase phase;
    orchard::Hash message;
    std::vector<orchard::ResolvedInput> inputs;
    std::vector<orchard::Hash> nullifiers; // All action NFs, including padding.
    std::vector<uint8_t> transaction; // Empty until Ready, frozen thereafter.
  };
  static constexpr size_t kMaxPending = 128;
  [[nodiscard]] static OrchardOperationQueue Empty(orchard::SigningDomain);
  [[nodiscard]] OrchardOperationQueue
  Reserve(const orchard::Hash &operation_id,
          const orchard::WalletProvingIntent &) const;
  [[nodiscard]] OrchardOperationQueue
  SetReady(const orchard::Hash &operation_id,
           const consensus::VerifiedOrchardAuthorizations &) const;
  // Safe only before a signed transaction is exposed. Ready entries cannot
  // be cancelled or overwritten through this API. Confirmation/conflict
  // archival and reorg reconciliation must be supplied by the wallet host.
  [[nodiscard]] OrchardOperationQueue
  CancelReserved(const orchard::Hash &) const;
  const std::map<orchard::Hash, Entry> &Entries() const noexcept {
    return entries_;
  }
  [[nodiscard]] orchard::WalletStateBytes Encode() const;
  [[nodiscard]] static OrchardOperationQueue
  Restore(const orchard::WalletStateBytes &, orchard::SigningDomain);

private:
  explicit OrchardOperationQueue(orchard::SigningDomain domain)
      : domain_(domain) {}
  void CheckEntry(const Entry &, bool verify_proof) const;
  void CheckUniqueReservations() const;
  orchard::SigningDomain domain_;
  std::map<orchard::Hash, Entry> entries_;
};
} // namespace dinero::wallet
