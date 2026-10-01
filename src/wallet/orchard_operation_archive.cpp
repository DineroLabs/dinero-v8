#include "wallet/orchard_operation_archive.h"
#include <algorithm>
#include <climits>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <sqlite3.h>
#include <string_view>
namespace dinero::wallet {
namespace {
using namespace orchard;
using Observation = OrchardAccountState::OperationObservation;
[[noreturn]] void Fail() {
  throw std::runtime_error(
      "Orchard operation archive integrity or transaction failure");
}
void Check(bool value) {
  if (!value)
    Fail();
}
void Exec(sqlite3 *db, const char *sql) {
  Check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
}
struct Savepoint {
  sqlite3 *db;
  bool done = false;
  explicit Savepoint(sqlite3 *d) : db(d) {
    Check(db && !sqlite3_get_autocommit(db));
    Exec(db, "SAVEPOINT orchard_archive_move");
  }
  ~Savepoint() {
    if (!done) {
      const auto rollback = sqlite3_exec(db, "ROLLBACK TO orchard_archive_move",
                                         nullptr, nullptr, nullptr);
      const auto release = sqlite3_exec(db, "RELEASE orchard_archive_move",
                                        nullptr, nullptr, nullptr);
      // SQLite may already have rolled back the entire transaction on I/O
      // failure. Otherwise never permit a caller to commit partial movement.
      if ((rollback != SQLITE_OK || release != SQLITE_OK) &&
          !sqlite3_get_autocommit(db))
        std::terminate();
    }
  }
  void Release() {
    Exec(db, "RELEASE orchard_archive_move");
    done = true;
  }
};
WalletStateBytes Encode(const Hash &id, const Observation &observation,
                        const OrchardOperationQueue &queue, uint64_t sequence,
                        const Hash &previous) {
  auto bytes = queue.Encode();
  Check(bytes.Bytes().size() <= WalletSnapshotStore::kMaxStateBytes - 153);
  std::vector<uint8_t> out{'D', 'N', 'O', 'R', 'A', 'R', '0', '1'};
  struct Wipe {
    std::vector<uint8_t> &b;
    ~Wipe() {
      if (!b.empty())
        OPENSSL_cleanse(b.data(), b.size());
    }
  } wipe{out};
  const auto raw = [&](std::span<const uint8_t> b) {
    out.insert(out.end(), b.begin(), b.end());
  };
  raw(id);
  for (size_t i = 0; i < 8; ++i)
    out.push_back(uint8_t(sequence >> (i * 8)));
  raw(previous);
  out.push_back(static_cast<uint8_t>(observation.outcome));
  for (size_t i = 0; i < 4; ++i)
    out.push_back(uint8_t(observation.height >> (i * 8)));
  raw({observation.block_hash.begin(), 32});
  raw(observation.transaction_id);
  for (size_t i = 0; i < 4; ++i)
    out.push_back(uint8_t(bytes.Bytes().size() >> (i * 8)));
  raw(bytes.Bytes());
  return WalletStateBytes(out);
}
bool Equal(const WalletStateBytes &a, const WalletStateBytes &b) {
  return std::equal(a.Bytes().begin(), a.Bytes().end(), b.Bytes().begin(),
                    b.Bytes().end());
}
} // namespace
void OrchardOperationArchive::InitializeSchemaUnderTransaction(sqlite3 *db) {
  WalletSnapshotStore::InitializeSchemaUnderTransaction(db);
}
OrchardOperationArchive::OrchardOperationArchive(sqlite3* db,
    WalletStorageIdentity identity,SigningDomain domain,std::span<const uint8_t> seed)
    : OrchardArchiveReader(db,identity,domain,seed) {}
OrchardOperationArchive::Cursor
OrchardOperationArchive::Begin(const OrchardAccountState &account) const {
  WalletSnapshotStore store(db_, identity_, seed_.Bytes());
  const auto current = store.Read();
  Check(current && Equal(current->state, account.Encode()));
  const auto checkpoint = account.Archive();
  if (checkpoint.count) {
    const auto head = Read(checkpoint.head);
    Check(head.sequence == checkpoint.count);
  }
  return Cursor(checkpoint, checkpoint.count, checkpoint.head);
}
OrchardOperationArchive::Page
OrchardOperationArchive::List(Cursor cursor, size_t limit) const {
  Check(limit > 0 && limit <= 64);
  Page page{{}, cursor};
  while (page.next.remaining_ && page.entries.size() < limit) {
    const auto id = page.next.next_;
    const auto record = Read(id);
    Check(record.sequence == page.next.remaining_);
    page.entries.push_back(LocatedOperation(cursor.root_, record.sequence, id));
    page.next.next_ = record.previous;
    --page.next.remaining_;
  }
  Check((page.next.remaining_ == 0) == (page.next.next_ == Hash{}));
  return page;
}
void OrchardOperationArchive::CheckCurrent(
    uint64_t revision, const OrchardAccountState &account) const {
  Check(!sqlite3_get_autocommit(db_));
  WalletSnapshotStore store(db_, identity_, seed_.Bytes());
  const auto current = store.Read();
  Check(current && current->revision == revision &&
        Equal(current->state, account.Encode()));
  const auto &domain = account.Operations().domain_;
  Check(domain.network_code == domain_.network_code &&
        domain.genesis_wire == domain_.genesis_wire &&
        domain.branch_id == domain_.branch_id);
}
OrchardOperationArchive::Staged OrchardOperationArchive::StageCompleted(
    uint64_t revision, const OrchardAccountState &account, const Hash &id,
    const OrchardWalletRestoreLookups &lookups) {
  CheckCurrent(revision, account);
  account.VerifyOperationObservation(id, lookups);
  auto single = OrchardOperationQueue::Empty(domain_);
  single.entries_.emplace(id, account.Operations().Entries().at(id));
  uint64_t record_revision = 0, sequence = 0;
  Hash previous_id{};
  auto checkpoint = account.Archive();
  if (checkpoint.count) {
    auto head = Read(checkpoint.head);
    Check(head.sequence == checkpoint.count);
  }
  const bool exists = Contains(id);
  if (exists) {
    auto previous = Read(id);
    Check(single.ContinuesArchived(previous.operation));
    Check(previous.sequence <= checkpoint.count);
    record_revision = previous.revision;
    sequence = previous.sequence;
    previous_id = previous.previous;
  } else {
    Check(checkpoint.count < UINT64_MAX);
    sequence = checkpoint.count + 1;
    previous_id = checkpoint.head;
    checkpoint = {sequence, id};
  }
  auto next = account.RemoveObservedOperation(id).WithArchive(checkpoint);
  auto payload =
      Encode(id, account.Observations().at(id), single, sequence, previous_id);
  Savepoint save(db_);
  WalletSnapshotStore record(db_, RecordIdentity(id), seed_.Bytes());
  (void)record.StageReplace(record_revision, payload);
  WalletSnapshotStore store(db_, identity_, seed_.Bytes());
  const auto updated = store.StageReplace(revision, next.Encode());
  save.Release();
  return {updated, std::move(next)};
}
OrchardOperationArchive::Staged OrchardOperationArchive::StageReactivate(
    uint64_t revision, const OrchardAccountState &account,
    const LocatedOperation &located,
    const std::function<StatusOr<uint256>(uint32_t)> &selected_hash) {
  CheckCurrent(revision, account);
  Check(bool(selected_hash) && account.Archive() == located.root_);
  const auto &id = located.id_;
  auto record = Read(id);
  Check(record.sequence == located.sequence_);
  const auto &checkpoint = account.Scan().Checkpoint();
  const auto selected = selected_hash(checkpoint.height);
  if (!selected.ok())
    throw consensus::OrchardStateLookupError(selected.status());
  Check(*selected == checkpoint.block_hash);
  if (record.observation.height <= checkpoint.height) {
    const auto origin = selected_hash(record.observation.height);
    if (!origin.ok())
      throw consensus::OrchardStateLookupError(origin.status());
    Check(!origin->IsNull() && *origin != record.observation.block_hash);
  }
  auto next = account.RestoreArchivedOperation(id, record.operation);
  Savepoint save(db_);
  WalletSnapshotStore store(db_, identity_, seed_.Bytes());
  const auto updated = store.StageReplace(revision, next.Encode());
  save.Release();
  return {updated, std::move(next)};
}
} // namespace dinero::wallet
