#include "orchard_block_test_fixture.h"
#include "wallet/orchard_operation_archive.h"
#include <filesystem>
#include <spawn.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
using dinero::wallet::OrchardAccountState;
using dinero::wallet::OrchardOperationArchive;
using dinero::wallet::OrchardWalletRestoreLookups;
static const std::array<uint8_t, 64> seed{7};
template <class F> static void Fails(F f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception &) {
    failed = true;
  }
  Require(failed);
}
static void Sql(sqlite3 *db, const char *q) {
  Require(sqlite3_exec(db, q, nullptr, nullptr, nullptr) == SQLITE_OK);
}
static WalletStorageIdentity Identity(const SigningDomain &domain) {
  return {WalletNetwork::Regtest, domain.genesis_wire, Hash{44}, 0};
}
struct Funded {
  OrchardAccountState account;
  OrchardBlockCandidate block;
  PreparedOrchardState prepared;
  VerifiedOrchardAuthorizations auth;
  std::optional<OrchardAccountState> reserved;
};
static Funded Fund(const char *fixtures, const OrchardAccountState &parent,
                   Hash id, uint8_t tag) {
  Fixture f(fixtures);
  f.view.height = parent.Scan().Checkpoint().height;
  const auto a = f.view.coins.at(Point(f.inputs[0])),
             b = f.view.coins.at(Point(f.inputs[1]));
  f.view.coins.clear();
  f.inputs[0].txid_wire[0] = tag;
  f.inputs[1].txid_wire[0] = tag + 1;
  f.view.coins.emplace(Point(f.inputs[0]), a);
  f.view.coins.emplace(Point(f.inputs[1]), b);
  f.outputs[0].script_pub_key = a.scriptPubKey;
  f.outputs[1].script_pub_key = b.scriptPubKey;
  f.outputs[1].amount_una = 51000;
  auto keys = WalletKeys::FromSeed(seed, 0);
  std::vector<WalletPayment> payments{
      {5000, keys.Receiver(WalletScope::External, {})}};
  auto plan = WalletBundlePlan::PrepareShield(keys, payments);
  std::vector<ResolvedInput> inputs;
  for (const auto &in : f.inputs) {
    const auto &coin = f.view.coins.at(Point(in));
    inputs.push_back({in.txid_wire, in.output_index, in.sequence,
                      coin.value.GetUna(), coin.scriptPubKey});
  }
  const auto signing =
      SigningContext::Create(f.domain, f.lock, inputs, f.outputs, f.fee);
  auto reserved = parent.Reserve(id, plan.Intent(signing));
  auto proof = std::move(plan).Prove(signing);
  f.bundle = proof.Bytes();
  f.Sign();
  auto auth = VerifyOrchardAuthorizations(f.Snapshot(), f.domain,
                                          f.view.height + 1, {});
  auto ready = reserved.SetReady(id, auth);
  OrchardBlockContext c{f.view.height + 1, H(2),
                        parent.Scan().Checkpoint().block_hash, 20001, f.domain};
  auto block = Candidate(c, std::span(&auth, 1));
  c.block_hash = block.Header().GetHash();
  OrchardStateLookups lookups{
      [](const uint256 &) -> StatusOr<bool> { return true; },
      [](const uint256 &) -> StatusOr<bool> { return false; }};
  std::optional<storage::OrchardStoredState> previous;
  if (c.height > 20001)
    previous = parent.Scan().Checkpoint();
  auto prepared =
      PrepareOrchardStateTransition(c, previous, std::span(&auth, 1), lookups);
  return {ready.Advance(c, block, prepared, std::span(&auth, 1)), block,
          prepared, auth, reserved};
}
static OrchardWalletRestoreLookups Lookups(const Funded &funded) {
  return {[&](uint32_t height, const uint256 &hash, const Hash &txid) {
            Require(height == funded.prepared.Next().height &&
                    hash == funded.block.Header().GetHash() &&
                    txid == funded.auth.Orchard().Txid());
            return std::make_shared<const VerifiedOrchardAuthorizations>(
                funded.auth);
          },
          [](const uint256 &) -> StatusOr<bool> { return false; },
          [&](uint32_t height, const uint256 &hash) {
            Require(height == funded.prepared.Next().height &&
                    hash == funded.block.Header().GetHash());
            return std::make_shared<const OrchardBlockCandidate>(funded.block);
          }};
}
static Funded RestoreFixture(const char *fixtures, const Bytes &body,
                             const WalletStateBytes &bytes) {
  Fixture f(fixtures);
  auto block = OrchardBlockCandidate::DecodeExact(body);
  const auto &tx = block.Transactions()[1].Orchard();
  const auto a = f.view.coins.at(Point(f.inputs[0])),
             b = f.view.coins.at(Point(f.inputs[1]));
  f.view.coins.clear();
  f.view.coins.emplace(Point(tx.Inputs()[0]), a);
  f.view.coins.emplace(Point(tx.Inputs()[1]), b);
  auto auth = VerifyOrchardAuthorizations(
      OrchardCoinSnapshot::ResolveUnderChainstateLock(tx, f.view), f.domain,
      20001, {});
  OrchardBlockContext c{20001, block.Header().GetHash(), H(1), 20001, f.domain};
  OrchardStateLookups state{
      [](const uint256 &) -> StatusOr<bool> { return true; },
      [](const uint256 &) -> StatusOr<bool> { return false; }};
  auto prepared =
      PrepareOrchardStateTransition(c, {}, std::span(&auth, 1), state);
  OrchardWalletRestoreLookups lookups{
      [&](uint32_t h, const uint256 &bh, const Hash &id) {
        Require(h == 20001 && bh == c.block_hash && id == tx.Txid());
        return std::make_shared<const VerifiedOrchardAuthorizations>(auth);
      },
      [](const uint256 &) -> StatusOr<bool> { return false; },
      [&](uint32_t h, const uint256 &bh) {
        Require(h == 20001 && bh == c.block_hash);
        return std::make_shared<const OrchardBlockCandidate>(block);
      }};
  auto keys = WalletKeys::FromSeed(seed, 0);
  return {OrchardAccountState::Restore(bytes, f.domain,
                                       keys.ExportFullViewingKey(), 20001,
                                       prepared.Next(), lookups),
          block, prepared, auth};
}
int main(int argc, char **argv) {
  try {
    if (argc == 6 && std::string_view(argv[1]) == "--crash") {
      Fixture f(argv[5]);
      sqlite3 *db = nullptr;
      Require(sqlite3_open(argv[2], &db) == SQLITE_OK);
      Sql(db, "PRAGMA synchronous=FULL;");
      WalletSnapshotStore store(db, Identity(f.domain), seed);
      auto loaded = store.Read();
      Require(loaded && loaded->revision == 1);
      auto funded = RestoreFixture(argv[5], Load(argv[3]), loaded->state);
      OrchardOperationArchive archive(db, Identity(f.domain), f.domain, seed);
      Sql(db, "BEGIN IMMEDIATE;");
      auto next =
          archive.StageCompleted(1, funded.account, Hash{1}, Lookups(funded));
      Require(next.revision == 2);
      Sql(db, "UPDATE locks SET count=0;");
      if (std::string_view(argv[4]) == "post")
        Sql(db, "COMMIT;");
      std::_Exit(73);
    }
    Require(argc == 2);
    Fixture fixture(argv[1]);
    const auto domain = fixture.domain;
    auto keys = WalletKeys::FromSeed(seed, 0);
    auto initial = OrchardAccountState::Begin(
        domain, keys.ExportFullViewingKey(), 20001, H(1));
    auto first = Fund(argv[1], initial, Hash{1}, 21);
    auto pattern =
        (std::filesystem::temp_directory_path() / "orchard-archive-XXXXXX")
            .string();
    std::vector<char> directory(pattern.begin(), pattern.end());
    directory.push_back(0);
    Require(mkdtemp(directory.data()));
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{directory.data()};
    sqlite3 *db = nullptr;
    const auto path = (cleanup.p / "wallet.sqlite").string();
    Require(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    Sql(db, "PRAGMA synchronous=FULL;BEGIN IMMEDIATE;");
    OrchardOperationArchive::InitializeSchemaUnderTransaction(db);
    {
      WalletSnapshotStore store(db, Identity(domain), seed);
      Require(store.StageReplace(0, first.account.Encode()) == 1);
    }
    Sql(db, "COMMIT;");
    auto current = first.account;
    uint64_t revision = 1;
    {
      OrchardOperationArchive archive(db, Identity(domain), domain, seed);
      Require(archive.Begin(current).Remaining() == 0 &&
              !archive.Contains(Hash{1}));
      Fails([&] {
        (void)archive.StageCompleted(1, current, Hash{1}, Lookups(first));
      });
      Sql(db, "BEGIN IMMEDIATE;");
      Fails([&] {
        (void)archive.StageCompleted(2, current, Hash{1}, Lookups(first));
      });
      auto missing = Lookups(first);
      missing.selected_block = {};
      Fails(
          [&] { (void)archive.StageCompleted(1, current, Hash{1}, missing); });
      auto next = archive.StageCompleted(1, current, Hash{1}, Lookups(first));
      Require(next.account.Operations().Entries().empty() &&
              next.account.Archive().count == 1);
      Sql(db, "ROLLBACK;");
      Require(!archive.Contains(Hash{1}) &&
              archive.Begin(current).Remaining() == 0);
      // Fail the account update after the new archive record was written. Catch
      // and commit the outer transaction: the savepoint must leave neither
      // half.
      Sql(db, "CREATE TRIGGER fail_account BEFORE UPDATE ON "
              "orchard_wallet_snapshots WHEN "
              "OLD.wallet_id=x'"
              "2c00000000000000000000000000000000000000000000000000000000000000"
              "' BEGIN SELECT RAISE(ABORT,'test update failure'); END;");
      Sql(db, "BEGIN IMMEDIATE;");
      Fails([&] {
        (void)archive.StageCompleted(1, current, Hash{1}, Lookups(first));
      });
      Sql(db, "COMMIT;");
      Require(!archive.Contains(Hash{1}) &&
              archive.Begin(current).Remaining() == 0);
      Sql(db, "DROP TRIGGER fail_account;");
      Sql(db, "BEGIN IMMEDIATE;");
      next = archive.StageCompleted(1, current, Hash{1}, Lookups(first));
      Sql(db, "COMMIT;");
      current = std::move(next.account);
      revision = next.revision;
      Require(
          archive.Read(Hash{1}).operation.Entries().at(Hash{1}).transaction ==
          first.auth.Orchard().CanonicalBytes());
      auto page = archive.List(archive.Begin(current), 1);
      Require(page.entries.size() == 1 && page.next.Remaining() == 0 &&
              page.entries[0].Id() == Hash{1});
      Fails([&] { (void)archive.List(archive.Begin(current), 65); });
      Sql(db, "BEGIN IMMEDIATE;");
      Fails([&] {
        (void)archive.StageReactivate(revision, current, page.entries[0],
                                      [&](uint32_t) -> StatusOr<uint256> {
                                        return first.block.Header().GetHash();
                                      });
      });
      Sql(db, "ROLLBACK;");
      auto rewound = current.RewindScanFrom(initial);
      Require(rewound.Archive() == current.Archive());
      {
        WalletSnapshotStore store(db, Identity(domain), seed);
        Sql(db, "BEGIN IMMEDIATE;");
        revision = store.StageReplace(revision, rewound.Encode());
        Sql(db, "COMMIT;");
      }
      current = rewound;
      page = archive.List(archive.Begin(current), 1);
      Sql(db, "BEGIN IMMEDIATE;");
      Fails([&] {
        (void)archive.StageReactivate(
            revision, current, page.entries[0],
            [](uint32_t) -> StatusOr<uint256> { return Status::Internal; });
      });
      Sql(db, "ROLLBACK;");
      Sql(db, "BEGIN IMMEDIATE;");
      auto resurrected = archive.StageReactivate(
          revision, current, page.entries[0],
          [](uint32_t) -> StatusOr<uint256> { return H(1); });
      Sql(db, "COMMIT;");
      current = std::move(resurrected.account);
      revision = resurrected.revision;
      Require(current.Operations().Entries().at(Hash{1}).transaction ==
                  first.auth.Orchard().CanonicalBytes() &&
              archive.Contains(Hash{1}));
      OrchardBlockContext c{20001, first.block.Header().GetHash(), H(1), 20001,
                            domain};
      auto reconfirmed = current.Advance(c, first.block, first.prepared,
                                         std::span(&first.auth, 1));
      {
        WalletSnapshotStore store(db, Identity(domain), seed);
        Sql(db, "BEGIN IMMEDIATE;");
        revision = store.StageReplace(revision, reconfirmed.Encode());
        Sql(db, "COMMIT;");
      }
      current = reconfirmed;
      Sql(db, "BEGIN IMMEDIATE;");
      auto rearchived =
          archive.StageCompleted(revision, current, Hash{1}, Lookups(first));
      Sql(db, "COMMIT;");
      current = std::move(rearchived.account);
      revision = rearchived.revision;
      Require(current.Archive().count == 1 &&
              archive.Read(Hash{1}).revision == 2);
      auto second = Fund(argv[1], current, Hash{2}, 31);
      {
        WalletSnapshotStore store(db, Identity(domain), seed);
        Sql(db, "BEGIN IMMEDIATE;");
        revision = store.StageReplace(revision, second.account.Encode());
        Sql(db, "COMMIT;");
      }
      current = second.account;
      Sql(db, "BEGIN IMMEDIATE;");
      auto secondArchive =
          archive.StageCompleted(revision, current, Hash{2}, Lookups(second));
      Sql(db, "COMMIT;");
      current = std::move(secondArchive.account);
      revision = secondArchive.revision;
      Require(current.Archive().count == 2 &&
              current.Operations().Entries().empty());
      auto p1 = archive.List(archive.Begin(current), 1);
      auto p2 = archive.List(p1.next, 1);
      Require(p1.entries[0].Id() == Hash{2} && p2.entries[0].Id() == Hash{1} &&
              p2.next.Remaining() == 0);
      Require(archive.Read(Hash{2}).previous == Hash{1});
      // Deleting an encrypted predecessor is an integrity failure, never a
      // shortened page or an apparently empty history. Roll back the damage.
      Sql(db, "BEGIN IMMEDIATE;");
      Sql(db,
          "DELETE FROM orchard_wallet_snapshots WHERE revision=2 AND "
          "wallet_id!=x'"
          "2c00000000000000000000000000000000000000000000000000000000000000';");
      Fails([&] { (void)archive.List(p1.next, 1); });
      Sql(db, "ROLLBACK;");
      Require(archive.List(p1.next, 1).entries.size() == 1);
    }
    Require(sqlite3_close(db) == SQLITE_OK);
    // A pre-proof conflict can later be disconnected. Completing the SAME
    // reserved intent must not make its retained archive impossible to update.
    const auto reservedPath = (cleanup.p / "reserved.sqlite").string();
    Require(sqlite3_open(reservedPath.c_str(), &db) == SQLITE_OK);
    Sql(db, "PRAGMA synchronous=FULL;BEGIN IMMEDIATE;");
    OrchardOperationArchive::InitializeSchemaUnderTransaction(db);
    {
      OrchardBlockContext c{20001, first.block.Header().GetHash(), H(1), 20001,
                            domain};
      auto conflict = first.reserved->Advance(c, first.block, first.prepared,
                                              std::span(&first.auth, 1));
      Require(conflict.Observations().at(Hash{1}).outcome ==
              OrchardAccountState::OperationOutcome::Conflicted);
      WalletSnapshotStore store(db, Identity(domain), seed);
      uint64_t rev = store.StageReplace(0, conflict.Encode());
      OrchardOperationArchive archive(db, Identity(domain), domain, seed);
      auto completed =
          archive.StageCompleted(rev, conflict, Hash{1}, Lookups(first));
      Sql(db, "COMMIT;");
      auto account = completed.account.RewindScanFrom(initial);
      Sql(db, "BEGIN IMMEDIATE;");
      rev = store.StageReplace(completed.revision, account.Encode());
      Sql(db, "COMMIT;");
      const auto located = archive.List(archive.Begin(account), 1).entries[0];
      Sql(db, "BEGIN IMMEDIATE;");
      auto active = archive.StageReactivate(
          rev, account, located,
          [](uint32_t) -> StatusOr<uint256> { return H(1); });
      Sql(db, "COMMIT;");
      account = active.account.SetReady(Hash{1}, first.auth);
      Sql(db, "BEGIN IMMEDIATE;");
      rev = store.StageReplace(active.revision, account.Encode());
      Sql(db, "COMMIT;");
      // Reconciliation retry preserves the newly completed bytes.
      Sql(db, "BEGIN IMMEDIATE;");
      auto again = archive.StageReactivate(
          rev, account, located,
          [](uint32_t) -> StatusOr<uint256> { return H(1); });
      account = again.account.Advance(c, first.block, first.prepared,
                                      std::span(&first.auth, 1));
      rev = store.StageReplace(again.revision, account.Encode());
      auto confirmed =
          archive.StageCompleted(rev, account, Hash{1}, Lookups(first));
      Sql(db, "COMMIT;");
      const auto record = archive.Read(Hash{1});
      Require(record.revision == 2 && record.sequence == 1 &&
              confirmed.account.Archive().count == 1 &&
              record.operation.Entries().at(Hash{1}).transaction ==
                  first.auth.Orchard().CanonicalBytes());
    }
    Require(sqlite3_close(db) == SQLITE_OK);
    // Fresh-process exits bracket the actual archive+account+companion commit.
    const auto crashPath = (cleanup.p / "crash.sqlite").string(),
               bodyPath = (cleanup.p / "funding.block").string();
    {
      std::ofstream out(bodyPath, std::ios::binary);
      const auto &body = first.block.WireBytes();
      out.write(reinterpret_cast<const char *>(body.data()), body.size());
      Require(bool(out));
    }
    Require(sqlite3_open(crashPath.c_str(), &db) == SQLITE_OK);
    Sql(db, "PRAGMA synchronous=FULL;BEGIN IMMEDIATE;CREATE TABLE locks(count "
            "INTEGER);INSERT INTO locks VALUES(2);");
    OrchardOperationArchive::InitializeSchemaUnderTransaction(db);
    {
      WalletSnapshotStore store(db, Identity(domain), seed);
      Require(store.StageReplace(0, first.account.Encode()) == 1);
    }
    Sql(db, "COMMIT;");
    Require(sqlite3_close(db) == SQLITE_OK);
    for (const char *mode : {"pre", "post"}) {
      auto exe = std::filesystem::absolute(argv[0]).string();
      std::vector<char *> args{exe.data(),
                               const_cast<char *>("--crash"),
                               const_cast<char *>(crashPath.c_str()),
                               const_cast<char *>(bodyPath.c_str()),
                               const_cast<char *>(mode),
                               argv[1],
                               nullptr};
      pid_t pid;
      Require(posix_spawn(&pid, exe.c_str(), nullptr, nullptr, args.data(),
                          environ) == 0);
      int code;
      Require(waitpid(pid, &code, 0) == pid && WIFEXITED(code) &&
              WEXITSTATUS(code) == 73);
      Require(sqlite3_open(crashPath.c_str(), &db) == SQLITE_OK);
      Sql(db, "PRAGMA synchronous=FULL;");
      {
        WalletSnapshotStore store(db, Identity(domain), seed);
        auto loaded = store.Read();
        const bool committed = std::string_view(mode) == "post";
        Require(loaded && loaded->revision == (committed ? 2 : 1));
        auto restored =
            RestoreFixture(argv[1], first.block.WireBytes(), loaded->state);
        OrchardOperationArchive archive(db, Identity(domain), domain, seed);
        Require(archive.Contains(Hash{1}) == committed &&
                restored.account.Operations().Entries().empty() == committed &&
                restored.account.Archive().count == uint64_t(committed));
        if (committed)
          Require(archive.Read(Hash{1})
                      .operation.Entries()
                      .at(Hash{1})
                      .transaction == first.auth.Orchard().CanonicalBytes());
        sqlite3_stmt *st = nullptr;
        Require(sqlite3_prepare_v2(db, "SELECT count FROM locks", -1, &st,
                                   nullptr) == SQLITE_OK);
        Require(sqlite3_step(st) == SQLITE_ROW &&
                sqlite3_column_int(st, 0) == (committed ? 0 : 2));
        sqlite3_finalize(st);
      }
      Require(sqlite3_close(db) == SQLITE_OK);
    }
    std::cout
        << "PASS: encrypted atomic archive, bounded authenticated history, "
           "reorg reactivation, rollback and fresh-process commit boundaries\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
