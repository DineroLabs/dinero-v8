#pragma once
#include "wallet/orchard_account_metadata.h"
// Called from the existing real account fixture after encrypted-store reopen.
// No selected-chain callback, reset, writes or new account are needed here.
static void CheckOrchardAccountMetadata(
    const dinero::wallet::OrchardAccountState &reserved,
    const dinero::wallet::OrchardAccountState &ready,
    const dinero::wallet::OrchardAccountState &delivered,
    const dinero::orchard::WalletStateBytes &authenticated_reopened,
    dinero::orchard::SigningDomain domain,
    const dinero::orchard::FullViewingKeyBytes &fvk, uint32_t activation) {
  using Metadata = dinero::wallet::OrchardAccountMetadata;
  const auto same = [](const auto &a, const auto &b) {
    return std::equal(a.Bytes().begin(), a.Bytes().end(),
                      b.Bytes().begin(), b.Bytes().end());
  };
  // Exact queue encoding checks every ordered transparent outpoint, sequence,
  // amount, script, intent digest, nullifier and Ready body, not just row count.
  auto r = Metadata::Read(reserved.Encode(), domain, fvk, activation);
  Require(same(r.Operations().Encode(), reserved.Operations().Encode()));
  Require(!r.Operations().Entries().empty());
  Require(!r.Operations().Entries().begin()->second.inputs.empty());
  auto signed_owner = Metadata::Read(ready.Encode(), domain, fvk, activation);
  Require(same(signed_owner.Operations().Encode(), ready.Operations().Encode()));
  // The input temporaries have already been destroyed; the returned value owns
  // all its data and holds no span into the decrypted account buffer.
  Require(!signed_owner.Operations().Entries().begin()->second.transaction.empty());
  auto reopened = Metadata::Read(authenticated_reopened, domain, fvk, activation);
  Require(same(reopened.Operations().Encode(), delivered.Operations().Encode()));
  Require(reopened.Observations() == delivered.Observations());
  Require(reopened.Archive() == delivered.Archive());
  Require(reopened.Delivery() == delivered.Delivery());
  Require(reopened.ParentSnapshotRevision() == delivered.ParentSnapshotRevision());
  Require(same(authenticated_reopened, delivered.Encode()));
  std::cout << "PASS account metadata exact reservations and encrypted reopen\n";

  // Same decoder used by full account restoration: checked domain/key binding,
  // complete framing and terminal EOF. A failed decode returns no prefix.
  auto wrong = domain; wrong.branch_id ^= 1;
  AccountReject([&] { (void)Metadata::Read(authenticated_reopened, wrong, fvk, activation); });
  wrong = domain; wrong.network_code ^= 1;
  AccountReject([&] { (void)Metadata::Read(authenticated_reopened, wrong, fvk, activation); });
  wrong = domain; wrong.genesis_wire[0] ^= 1;
  AccountReject([&] { (void)Metadata::Read(authenticated_reopened, wrong, fvk, activation); });
  auto other_fvk = fvk; other_fvk[0] ^= 1;
  AccountReject([&] { (void)Metadata::Read(authenticated_reopened, domain, other_fvk, activation); });
  AccountReject([&] { (void)Metadata::Read(authenticated_reopened, domain, fvk, activation + 1); });
  const std::vector<uint8_t> original(authenticated_reopened.Bytes().begin(),
                                      authenticated_reopened.Bytes().end());
  auto refuse = [&](std::vector<uint8_t> bad) {
    AccountReject([&] { (void)Metadata::Read(WalletStateBytes(bad), domain, fvk, activation); });
  };
  auto bad = original; bad.push_back(0); refuse(bad);
  bad = original; bad.pop_back(); refuse(bad);
  bad = original; bad[7] = '7'; refuse(bad);
  // First receiver counter's exhaustion flag is a strict Boolean.
  bad = original; bad[8 + 1 + 32 + 4 + 4 + 32 + 11] = 2; refuse(bad);
  // A nonzero delivery sequence must retain its nonzero digest.
  bad = original; std::fill(bad.end() - 32, bad.end(), 0); refuse(bad);
  // v6 cannot supply a retained-parent binding of zero.
  bad = original; bad[7] = '6'; bad.insert(bad.end(), 8, 0); refuse(bad);
  Require(same(authenticated_reopened, delivered.Encode()));
  std::cout << "PASS account metadata wrong owners and incomplete framing refuse\n";

  // Compatibility with authenticated v1-v4 ownership records. These layouts
  // have absent trailing metadata; absence never invents a delivery receipt.
  auto baseline = reserved.Encode();
  std::vector<uint8_t> legacy(baseline.Bytes().begin(), baseline.Bytes().end());
  for (uint8_t version : {'1', '2', '3', '4'}) {
    auto encoded = legacy; encoded[7] = version;
    if (version == '1') encoded.resize(encoded.size() - 84);
    if (version == '2') encoded.resize(encoded.size() - 80);
    if (version == '3') encoded.resize(encoded.size() - 40);
    auto old = Metadata::Read(WalletStateBytes(encoded), domain, fvk, activation);
    Require(same(old.Operations().Encode(), reserved.Operations().Encode()));
    Require(old.Observations().empty() && old.Archive().count == 0 &&
            old.Delivery().sequence == 0 && old.ParentSnapshotRevision() == 0);
  }
  std::cout << "PASS account metadata legacy ownership retained without scan reset\n";
}
