#include "wallet/orchard_archive_reader.h"
#include <algorithm>
#include <openssl/sha.h>
#include <set>
#include <sqlite3.h>
#include <string_view>
namespace dinero::wallet {
namespace {
using namespace orchard;
using Observation = OrchardAccountMetadata::OperationObservation;
[[noreturn]] void Fail() {
  throw std::runtime_error("Orchard operation archive integrity or transaction failure");
}
void Check(bool value) { if (!value) Fail(); }
WalletStateBytes CheckedSeed(std::span<const uint8_t> seed) {
  Check(seed.size() >= 32 && seed.size() <= 252);
  return WalletStateBytes(seed);
}
Hash H(std::span<const uint8_t> bytes) {
  Check(bytes.size() == 32);
  Hash h;
  std::copy(bytes.begin(), bytes.end(), h.begin());
  return h;
}
OrchardArchiveReader::Record Decode(uint64_t revision, const Hash &id,
                                       const WalletStateBytes &bytes,
                                       SigningDomain domain) {
  auto remaining = bytes.Bytes();
  const auto take = [&](size_t size) {
    Check(size <= remaining.size());
    auto out = remaining.first(size);
    remaining = remaining.subspan(size);
    return out;
  };
  const auto u32 = [&]() {
    auto data = take(4);
    uint32_t out = 0;
    for (size_t i = 0; i < 4; ++i)
      out |= uint32_t(data[i]) << (i * 8);
    return out;
  };
  constexpr std::string_view magic = "DNORAR01";
  const auto prefix = take(8);
  Check(std::equal(prefix.begin(), prefix.end(), magic.begin()));
  Check(H(take(32)) == id);
  const auto serial = take(8);
  uint64_t sequence = 0;
  for (size_t i = 0; i < 8; ++i)
    sequence |= uint64_t(serial[i]) << (i * 8);
  const auto previous = H(take(32));
  Check(sequence > 0 && ((sequence == 1) == (previous == Hash{})));
  const auto outcome = take(1)[0];
  Check(outcome == 1 || outcome == 2);
  Observation observation;
  observation.outcome =
      static_cast<OrchardAccountMetadata::OperationOutcome>(outcome);
  observation.height = u32();
  const auto block = take(32);
  std::copy(block.begin(), block.end(), observation.block_hash.begin());
  observation.transaction_id = H(take(32));
  Check(observation.height > 0 && !observation.block_hash.IsNull() &&
        observation.transaction_id != Hash{});
  const auto size = u32();
  Check(size == remaining.size());
  auto queue =
      OrchardOperationQueue::Restore(WalletStateBytes(take(size)), domain);
  Check(queue.Entries().size() == 1 && queue.Entries().contains(id));
  if (outcome == 1) {
    const auto &entry = queue.Entries().at(id);
    Check(entry.phase == OrchardOperationQueue::Phase::Ready &&
          TransactionEnvelope::DecodeExact(entry.transaction).Txid() ==
              observation.transaction_id);
  }
  return {revision, sequence, previous, std::move(queue), observation};
}
} // namespace
OrchardArchiveReader::OrchardArchiveReader(sqlite3 *db,
                                                 WalletStorageIdentity identity,
                                                 SigningDomain domain,
                                                 std::span<const uint8_t> seed)
    : db_(db), identity_(identity), domain_(domain), seed_(CheckedSeed(seed)) {
  Check(domain.network_code == uint8_t(identity.network) &&
        domain.genesis_wire == identity.genesis);
  (void)SigningContext::Create(domain, 0, {}, {}, 0);
  WalletSnapshotStore validate(db_, identity_, seed_.Bytes());
}
WalletStorageIdentity
OrchardArchiveReader::RecordIdentity(const Hash &id) const {
  Check(id != Hash{});
  // Domain-separated fixed-width identity. Reuses the existing authenticated
  // per-record snapshot encryption without changing account key/nonce rules.
  constexpr std::string_view tag = "DIN/orchard/operation-archive-record/v1";
  std::vector<uint8_t> preimage(tag.begin(), tag.end());
  preimage.push_back(0);
  preimage.insert(preimage.end(), identity_.wallet_id.begin(),
                  identity_.wallet_id.end());
  preimage.insert(preimage.end(), id.begin(), id.end());
  auto identity = identity_;
  SHA256(preimage.data(), preimage.size(), identity.wallet_id.data());
  Check(identity.wallet_id != identity_.wallet_id);
  return identity;
}
bool OrchardArchiveReader::Contains(const Hash &id) const {
  WalletSnapshotStore store(db_, RecordIdentity(id), seed_.Bytes());
  return bool(store.Read());
}
OrchardArchiveReader::Record
OrchardArchiveReader::Read(const Hash &id) const {
  WalletSnapshotStore record(db_, RecordIdentity(id), seed_.Bytes());
  const auto loaded = record.Read();
  Check(bool(loaded));
  return Decode(loaded->revision, id, loaded->state, domain_);
}
OrchardArchiveReader::Record OrchardArchiveReader::ReadRetained(
    const Hash& id,uint64_t revision) const {
  Check(db_&&!sqlite3_get_autocommit(db_));
  WalletSnapshotStore store(db_,RecordIdentity(id),seed_.Bytes());
  const auto saved=store.ReadRetained(revision);
  return Decode(saved.revision,id,saved.state,domain_);
}
OrchardArchiveReader::Captured OrchardArchiveReader::CaptureCurrent(
    const FullViewingKeyBytes& fvk,uint32_t activation) const {
  Check(db_ && !sqlite3_get_autocommit(db_));
  WalletSnapshotStore store(db_,identity_,seed_.Bytes());
  auto current=store.Read();Check(bool(current));
  auto metadata=OrchardAccountMetadata::Read(current->state,domain_,fvk,activation);
  Check(metadata.ParentSnapshotRevision()<current->revision);
  auto remaining=metadata.Archive().count;
  Check(remaining<=kMaxCapturedRecords);
  Captured result{current->revision,std::move(metadata),{}};
  result.archive.reserve(static_cast<size_t>(remaining));
  auto id=result.metadata.Archive().head;
  std::set<Hash> visited;
  while(remaining) {
    Check(id!=Hash{} && visited.insert(id).second);
    auto record=Read(id);Check(record.sequence==remaining);
    const auto previous=record.previous;
    result.archive.push_back({id,RecordIdentity(id),std::move(record)});
    id=previous;--remaining;
  }
  Check(id==Hash{});
  return result;
}
} // namespace dinero::wallet
