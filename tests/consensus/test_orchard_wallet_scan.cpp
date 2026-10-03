#include "orchard_block_test_fixture.h"
#include "wallet/orchard_scan_state.h"
#include <filesystem>
#include <sqlite3.h>
#include <unistd.h>
using dinero::wallet::OrchardWalletScanState;
template <class F> static void ScanReject(F fn) {
  bool failed = false;
  try {
    fn();
  } catch (const std::exception &) {
    failed = true;
  }
  Require(failed);
}
static orchard::FullViewingKeyBytes Viewing(const std::string &name) {
  const auto bytes = Load(name);
  Require(bytes.size() == 96);
  orchard::FullViewingKeyBytes fvk;
  std::copy(bytes.begin(), bytes.end(), fvk.begin());
  return fvk;
}
int main(int argc, char **argv) {
  try {
    Require(argc == 2);
    const std::string base = argv[1];
    const auto domain = Fixture(base).domain;
    const auto sender = Viewing(base + "/combined-sender.fvk"),
               recipient = Viewing(base + "/combined-recipient.fvk");
    auto a = OrchardWalletScanState::Begin(domain, sender, 20001, H(1));
    auto b = OrchardWalletScanState::Begin(domain, recipient, 20001, H(1));
    Require(a.BalanceUna() == 0 && a.Notes().empty());
    const std::vector<VerifiedOrchardAuthorizations> shields{
        Authorized(base, false, 20000)},
        spends{Authorized(base, true, 20001)};
    OrchardBlockContext c{20001, H(2), H(1), 20001, domain};
    const auto block = Candidate(c, shields);
    c.block_hash = block.Header().GetHash();
    OrchardStateLookups view{
        [](const uint256 &) -> StatusOr<bool> { return true; },
        [](const uint256 &) -> StatusOr<bool> { return false; }};
    const auto funded =
        PrepareOrchardStateTransition(c, std::nullopt, shields, view);
    auto fundedA = a.Advance(c, block, funded, shields);
    auto fundedB = b.Advance(c, block, funded, shields);
    Require(fundedA.BalanceUna() == 5000 && fundedA.Notes().size() == 1 &&
            fundedB.BalanceUna() == 0);
    Require(a.BalanceUna() == 0 && a.Checkpoint().height == 20000);
    const auto &owned = fundedA.Notes()[0];
    Require(owned.note->Facts().amount == 5000 &&
            owned.created_block == c.block_hash);
    Require(std::equal(std::begin(owned.note->Facts().nullifier),
                       std::end(owned.note->Facts().nullifier),
                       Load(base + "/combined-spend.note-nullifier").begin()));
    Require(owned.origin->Orchard().CanonicalBytes() ==
            shields[0].Orchard().CanonicalBytes());
    ScanReject([&] { (void)a.Advance(c, block, funded, {}); });
    auto wrong = c;
    wrong.parent_hash = H(99);
    ScanReject([&] { (void)a.Advance(wrong, block, funded, shields); });
    wrong = c;
    wrong.domain.network_code = 1;
    ScanReject([&] { (void)a.Advance(wrong, block, funded, shields); });
    auto next = c;
    next.height++;
    next.parent_hash = c.block_hash;
    const auto spendBlock = Candidate(next, spends);
    next.block_hash = spendBlock.Header().GetHash();
    const auto paid =
        PrepareOrchardStateTransition(next, funded.Next(), spends, view);
    auto paidA = fundedA.Advance(next, spendBlock, paid, spends);
    auto paidB = fundedB.Advance(next, spendBlock, paid, spends);
    Require(paidA.BalanceUna() == 0 && paidA.Notes().empty());
    Require(paidB.BalanceUna() == 4500 && paidB.Notes().size() == 1);
    Require(fundedA.BalanceUna() == 5000 &&
            fundedA.Notes().size() == 1); // prior state retained for undo
    Require(paidB.Notes()[0].witness->Facts().leaf_count == 4);
    auto emptyContext = next;
    emptyContext.height++;
    emptyContext.parent_hash = next.block_hash;
    const auto emptyBlock = Candidate(emptyContext, {});
    emptyContext.block_hash = emptyBlock.Header().GetHash();
    const auto emptyTransition =
        PrepareOrchardStateTransition(emptyContext, paid.Next(), {}, view);
    auto emptyB = paidB.Advance(emptyContext, emptyBlock, emptyTransition, {});
    Require(emptyB.BalanceUna() == 4500 &&
            emptyB.Checkpoint() == emptyTransition.Next());
    Require(emptyB.Notes()[0].witness->Encode() ==
            paidB.Notes()[0].witness->Encode());
    // Restore the retained parent, then advance a different selected branch.
    const auto replacement = Candidate(next, spends, {}, 17);
    auto alternate = next;
    alternate.block_hash = replacement.Header().GetHash();
    const auto alternativeState =
        PrepareOrchardStateTransition(alternate, funded.Next(), spends, view);
    const auto reorgB =
        fundedB.Advance(alternate, replacement, alternativeState, spends);
    Require(reorgB.BalanceUna() == 4500 &&
            reorgB.Checkpoint().block_hash != paidB.Checkpoint().block_hash);
    Require(reorgB.Notes()[0].witness->Encode() ==
            paidB.Notes()[0].witness->Encode());
    // Restore re-decrypts actual origin ciphertext and authenticates the
    // persisted witness against the selected chain's exact checkpoint.
    dinero::wallet::OrchardWalletRestoreLookups lookups{
        [&](uint32_t height, const uint256 &hash, const orchard::Hash &txid) {
          Require(height == next.height && hash == next.block_hash &&
                  txid == spends[0].Orchard().Txid());
          return std::make_shared<const VerifiedOrchardAuthorizations>(
              spends[0]);
        },
        [](const uint256 &) -> StatusOr<bool> { return false; }};
    auto encoded = paidB.Encode();
    auto restore = [&](const orchard::WalletStateBytes &bytes) {
      return OrchardWalletScanState::Restore(bytes, domain, recipient, 20001,
                                             paidB.Checkpoint(), lookups);
    };
    const auto restored = restore(encoded);
    Require(restored.BalanceUna() == 4500 && restored.Notes().size() == 1 &&
            restored.Checkpoint() == paidB.Checkpoint());
    Require(restored.Notes()[0].witness->Encode() ==
            paidB.Notes()[0].witness->Encode());
    const auto resumed =
        restored.Advance(emptyContext, emptyBlock, emptyTransition, {});
    Require(resumed.Checkpoint() == emptyB.Checkpoint() &&
            resumed.BalanceUna() == 4500);
    ScanReject([&] {
      (void)OrchardWalletScanState::Restore(encoded, domain, sender, 20001,
                                            paidB.Checkpoint(), lookups);
    });
    ScanReject([&] {
      (void)OrchardWalletScanState::Restore(encoded, domain, recipient, 20001,
                                            reorgB.Checkpoint(), lookups);
    });
    auto noOrigin = lookups;
    noOrigin.origin = {};
    ScanReject([&] {
      (void)OrchardWalletScanState::Restore(encoded, domain, recipient, 20001,
                                            paidB.Checkpoint(), noOrigin);
    });
    auto alreadySpent = lookups;
    alreadySpent.spent_nullifier = [](const uint256 &) -> StatusOr<bool> {
      return true;
    };
    ScanReject([&] {
      (void)OrchardWalletScanState::Restore(encoded, domain, recipient, 20001,
                                            paidB.Checkpoint(), alreadySpent);
    });
    alreadySpent.spent_nullifier = [](const uint256 &) -> StatusOr<bool> {
      return Status::Internal;
    };
    bool readError = false;
    try {
      (void)OrchardWalletScanState::Restore(encoded, domain, recipient, 20001,
                                            paidB.Checkpoint(), alreadySpent);
    } catch (const OrchardStateLookupError &e) {
      readError = e.SourceStatus() == Status::Internal;
    }
    Require(readError);
    for (size_t n = 0; n < encoded.Bytes().size(); ++n)
      ScanReject([&] {
        (void)restore(orchard::WalletStateBytes(encoded.Bytes().first(n)));
      });
    auto extra =
        std::vector<uint8_t>(encoded.Bytes().begin(), encoded.Bytes().end());
    extra.push_back(0);
    ScanReject([&] { (void)restore(orchard::WalletStateBytes(extra)); });
    extra.assign(encoded.Bytes().begin(), encoded.Bytes().end());
    extra[0] ^= 1;
    ScanReject([&] { (void)restore(orchard::WalletStateBytes(extra)); });
    extra.assign(encoded.Bytes().begin(), encoded.Bytes().end());
    extra.back() ^= 1;
    ScanReject([&] { (void)restore(orchard::WalletStateBytes(extra)); });
    // Close/reopen the encrypted SQLite store, then use the typed restored
    // view.
    const auto path =
        std::filesystem::temp_directory_path() /
        ("dinero-orchard-scan-" + std::to_string(getpid()) + ".sqlite");
    Require(!std::filesystem::exists(path));
    sqlite3 *db = nullptr;
    Require(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    auto sql = [&](const char *s) {
      Require(sqlite3_exec(db, s, nullptr, nullptr, nullptr) == SQLITE_OK);
    };
    const std::array<uint8_t, 32> seed{71};
    orchard::WalletStorageIdentity identity{orchard::WalletNetwork::Regtest,
                                            domain.genesis_wire,
                                            orchard::Hash{42}, 0};
    sql("PRAGMA synchronous=FULL; BEGIN IMMEDIATE;");
    orchard::WalletSnapshotStore::InitializeSchemaUnderTransaction(db);
    {
      orchard::WalletSnapshotStore store(db, identity, seed);
      Require(store.StageReplace(0, encoded) == 1);
      sql("COMMIT;");
    }
    Require(sqlite3_close(db) == SQLITE_OK);
    Require(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    {
      orchard::WalletSnapshotStore store(db, identity, seed);
      auto saved = store.Read();
      Require(saved && saved->revision == 1);
      Require(restore(saved->state).BalanceUna() == 4500);
    }
    Require(sqlite3_close(db) == SQLITE_OK);
    Require(std::filesystem::remove(path));
    auto initial = a.Encode();
    const auto initialRestore = OrchardWalletScanState::Restore(
        initial, domain, sender, 20001, a.Checkpoint(), {});
    Require(initialRestore.BalanceUna() == 0 && initialRestore.Notes().empty());
    std::cout << "Orchard wallet scanner: exact selected-body coverage, "
                 "receipt, spend removal, witness/checkpoint advance, "
                 "immutable undo and replacement branch passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
