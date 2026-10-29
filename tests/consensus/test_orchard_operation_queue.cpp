#include "orchard_test_fixture.h"
#include "wallet/orchard_operation_queue.h"
#include <filesystem>
#include <spawn.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
using dinero::wallet::OrchardOperationQueue;
template <class F> void QueueReject(F fn) {
  bool failed = false;
  try {
    fn();
  } catch (const std::exception &) {
    failed = true;
  }
  Require(failed);
}
int main(int argc, char **argv) {
  try {
    if (argc == 5 && std::string_view(argv[1]) == "--crash") {
      Fixture f(argv[4]);
      sqlite3 *db = nullptr;
      Require(sqlite3_open(argv[2], &db) == SQLITE_OK);
      auto sql = [&](const char *q) {
        Require(sqlite3_exec(db, q, nullptr, nullptr, nullptr) == SQLITE_OK);
      };
      WalletStorageIdentity identity{WalletNetwork::Regtest,
                                     f.domain.genesis_wire, Hash{43}, 0};
      auto candidateIdentity = identity;
      candidateIdentity.account = 1;
      const std::array<uint8_t, 64> seed{7};
      WalletSnapshotStore store(db, identity, seed),
          candidate(db, candidateIdentity, seed);
      auto ready = candidate.Read();
      Require(bool(ready));
      (void)OrchardOperationQueue::Restore(ready->state, f.domain);
      sql("PRAGMA synchronous=FULL;BEGIN IMMEDIATE;");
      Require(store.StageReplace(1, ready->state) == 2);
      sql("UPDATE locks SET count=3;");
      if (std::string_view(argv[3]) == "post")
        sql("COMMIT;");
      std::_Exit(73);
    }
    Require(argc == 2);
    Fixture f(argv[1]);
    f.outputs[0].script_pub_key =
        f.view.coins.at(Point(f.inputs[0])).scriptPubKey;
    f.outputs[1].script_pub_key =
        f.view.coins.at(Point(f.inputs[1])).scriptPubKey;
    f.outputs[1].amount_una = 51000;
    auto keys = WalletKeys::FromSeed(std::array<uint8_t, 64>{7}, 0),
         recipient = WalletKeys::FromSeed(std::array<uint8_t, 64>{9}, 0);
    std::vector<WalletPayment> payments{
        {5000, recipient.Receiver(WalletScope::External, {})}};
    auto plan = WalletBundlePlan::PrepareShield(keys, payments);
    std::vector<ResolvedInput> resolved;
    for (const auto &input : f.inputs) {
      const auto &coin = f.view.coins.at(Point(input));
      resolved.push_back({input.txid_wire, input.output_index, input.sequence,
                          coin.value.GetUna(), coin.scriptPubKey});
    }
    const auto context =
        SigningContext::Create(f.domain, f.lock, resolved, f.outputs, f.fee);
    const auto intent = plan.Intent(context);
    const Hash id{1}, second{2};
    auto queue = OrchardOperationQueue::Empty(f.domain);
    auto reserved = queue.Reserve(id, intent);
    Require(queue.Entries().empty() &&
            reserved.Entries().at(id).phase ==
                OrchardOperationQueue::Phase::Reserved);
    QueueReject([&] { (void)reserved.Reserve(id, intent); });
    QueueReject([&] { (void)reserved.Reserve(second, intent); });
    auto other = WalletBundlePlan::PrepareShield(
        keys, payments); // fresh padding NFs, same transparent inputs
    QueueReject([&] { (void)reserved.Reserve(second, other.Intent(context)); });
    auto cancelled = reserved.CancelReserved(id);
    Require(cancelled.Entries().empty());
    Require(cancelled.Reserve(second, intent).Entries().size() == 1);
    auto restart = OrchardOperationQueue::Restore(reserved.Encode(), f.domain);
    Require(restart.Entries().at(id).message == intent.Message());
    QueueReject([&] { (void)restart.Reserve(second, intent); });
    auto complete = std::move(plan).Prove(context);
    f.bundle = complete.Bytes();
    f.Sign();
    const auto auth = VerifyOrchardAuthorizations(f.Snapshot(), f.domain,
                                                  f.view.height + 1, {});
    const auto ready = restart.SetReady(id, auth);
    Require(ready.Entries().at(id).transaction == f.Build().CanonicalBytes());
    Require(ready.SetReady(id, auth).Entries().at(id).transaction ==
            ready.Entries().at(id).transaction);
    QueueReject([&] { (void)ready.CancelReserved(id); });
    QueueReject([&] { (void)ready.Reserve(second, intent); });
    auto wrong = queue.Reserve(id, other.Intent(context));
    QueueReject([&] { (void)wrong.SetReady(id, auth); });
    const auto encoded = ready.Encode();
    auto restored = OrchardOperationQueue::Restore(encoded, f.domain);
    Require(restored.Entries().at(id).transaction ==
            ready.Entries().at(id).transaction);
    auto wrongDomain = f.domain;
    wrongDomain.network_code = 1;
    QueueReject(
        [&] { (void)OrchardOperationQueue::Restore(encoded, wrongDomain); });
    // Parse truncation before expensive proof verification. Test all short
    // prefixes and representative cuts through the large proof payload.
    for (size_t n = 0; n < std::min<size_t>(encoded.Bytes().size(), 512); ++n)
      QueueReject([&] {
        (void)OrchardOperationQueue::Restore(
            WalletStateBytes(encoded.Bytes().first(n)), f.domain);
      });
    for (size_t n = 512; n < encoded.Bytes().size(); n += 503)
      QueueReject([&] {
        (void)OrchardOperationQueue::Restore(
            WalletStateBytes(encoded.Bytes().first(n)), f.domain);
      });
    auto changed =
        std::vector<uint8_t>(encoded.Bytes().begin(), encoded.Bytes().end());
    changed.push_back(0);
    QueueReject([&] {
      (void)OrchardOperationQueue::Restore(WalletStateBytes(changed), f.domain);
    });
    changed.assign(encoded.Bytes().begin(), encoded.Bytes().end());
    changed.back() ^= 1;
    QueueReject([&] {
      (void)OrchardOperationQueue::Restore(WalletStateBytes(changed), f.domain);
    });
    // Both phases survive real encrypted storage. Companion wallet locks
    // commit in the same SQLite transaction before the host could broadcast.
    auto pattern =
        (std::filesystem::temp_directory_path() / "orchard-operation-XXXXXX")
            .string();
    std::vector<char> dir(pattern.begin(), pattern.end());
    dir.push_back(0);
    Require(mkdtemp(dir.data()));
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{dir.data()};
    const auto path = (cleanup.p / "wallet.sqlite").string();
    sqlite3 *db = nullptr;
    Require(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    auto sql = [&](const char *s) {
      Require(sqlite3_exec(db, s, nullptr, nullptr, nullptr) == SQLITE_OK);
    };
    const std::array<uint8_t, 64> seed{7};
    WalletStorageIdentity identity{WalletNetwork::Regtest,
                                   f.domain.genesis_wire, Hash{43}, 0};
    sql("PRAGMA synchronous=FULL;BEGIN IMMEDIATE;CREATE TABLE locks(count "
        "INTEGER);INSERT INTO locks VALUES(2);");
    WalletSnapshotStore::InitializeSchemaUnderTransaction(db);
    {
      WalletSnapshotStore store(db, identity, seed);
      Require(store.StageReplace(0, reserved.Encode()) == 1);
      sql("COMMIT;");
    }
    Require(sqlite3_close(db) == SQLITE_OK);
    Require(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    {
      WalletSnapshotStore store(db, identity, seed);
      auto loaded = store.Read();
      Require(loaded && loaded->revision == 1);
      auto resumed = OrchardOperationQueue::Restore(loaded->state, f.domain);
      Require(resumed.Entries().at(id).phase ==
              OrchardOperationQueue::Phase::Reserved);
      sql("BEGIN IMMEDIATE;");
      Require(store.StageReplace(1, ready.Encode()) == 2);
      sql("ROLLBACK;");
      Require(store.Read()->revision == 1);
      sql("BEGIN IMMEDIATE;");
      Require(store.StageReplace(1, ready.Encode()) == 2);
      sql("COMMIT;");
    }
    Require(sqlite3_close(db) == SQLITE_OK);
    Require(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    {
      WalletSnapshotStore store(db, identity, seed);
      auto loaded = store.Read();
      Require(loaded && loaded->revision == 2);
      auto resumed = OrchardOperationQueue::Restore(loaded->state, f.domain);
      Require(resumed.Entries().at(id).transaction ==
              auth.Orchard().CanonicalBytes());
    }
    Require(sqlite3_close(db) == SQLITE_OK);
    // Fresh process exits bracket the actual Ready publication transaction.
    const auto crashPath = (cleanup.p / "crash.sqlite").string();
    Require(sqlite3_open(crashPath.c_str(), &db) == SQLITE_OK);
    sql("PRAGMA synchronous=FULL;BEGIN IMMEDIATE;CREATE TABLE locks(count "
        "INTEGER);INSERT INTO locks VALUES(2);");
    WalletSnapshotStore::InitializeSchemaUnderTransaction(db);
    {
      auto candidateIdentity = identity;
      candidateIdentity.account = 1;
      WalletSnapshotStore store(db, identity, seed),
          candidate(db, candidateIdentity, seed);
      Require(store.StageReplace(0, reserved.Encode()) == 1);
      Require(candidate.StageReplace(0, ready.Encode()) == 1);
      sql("COMMIT;");
    }
    Require(sqlite3_close(db) == SQLITE_OK);
    for (bool commit : {false, true}) {
      const auto exe = std::filesystem::absolute(argv[0]).string();
      std::string mode = commit ? "post" : "pre";
      std::vector<char *> args{const_cast<char *>(exe.c_str()),
                               const_cast<char *>("--crash"),
                               const_cast<char *>(crashPath.c_str()),
                               mode.data(),
                               argv[1],
                               nullptr};
      pid_t pid;
      Require(posix_spawn(&pid, exe.c_str(), nullptr, nullptr, args.data(),
                          environ) == 0);
      int result = 0;
      Require(waitpid(pid, &result, 0) == pid);
      Require(WIFEXITED(result) && WEXITSTATUS(result) == 73);
      Require(sqlite3_open(crashPath.c_str(), &db) == SQLITE_OK);
      {
        WalletSnapshotStore store(db, identity, seed);
        auto loaded = store.Read();
        Require(loaded && loaded->revision == (commit ? 2 : 1));
        auto resumed = OrchardOperationQueue::Restore(loaded->state, f.domain);
        Require(resumed.Entries().at(id).phase ==
                (commit ? OrchardOperationQueue::Phase::Ready
                        : OrchardOperationQueue::Phase::Reserved));
        if (commit)
          Require(resumed.Entries().at(id).transaction ==
                  auth.Orchard().CanonicalBytes());
        QueueReject([&] { (void)resumed.Reserve(second, intent); });
      }
      sqlite3_stmt *st = nullptr;
      Require(sqlite3_prepare_v2(db, "SELECT count FROM locks", -1, &st,
                                 nullptr) == SQLITE_OK);
      Require(sqlite3_step(st) == SQLITE_ROW);
      Require(sqlite3_column_int(st, 0) == (commit ? 3 : 2));
      sqlite3_finalize(st);
      Require(sqlite3_close(db) == SQLITE_OK);
    }
    std::cout << "Orchard operation reservations, immutable proof intent, "
                 "signed-byte freeze, cancellation bounds, encrypted "
                 "restart/rollback and pre/post-commit process exit passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
