#include "wallet/orchard_account_state.h"
#include "daemon/runtime_block_outbox.h"
#include "primitives/block.h"
#include "consensus/merkle_root.h"
#include <algorithm>
#include <openssl/crypto.h>
#include <openssl/sha.h>
namespace dinero::wallet {
namespace {
using namespace orchard;
using namespace consensus;
[[noreturn]] void Fail() {
  throw std::runtime_error("Orchard wallet account snapshot rejected");
}
void Check(bool v) {
  if (!v)
    Fail();
}
constexpr std::array<uint8_t, 8> magic{'D', 'N', 'O', 'R', 'A', 'C', '0', '5'};
Hash Identity(const FullViewingKeyBytes &fvk) {
  Hash h;
  SHA256(fvk.data(), fvk.size(), h.data());
  return h;
}
bool DomainEqual(const SigningDomain &a, const SigningDomain &b) {
  return a.network_code == b.network_code && a.genesis_wire == b.genesis_wire &&
         a.branch_id == b.branch_id;
}
struct Writer {
  std::vector<uint8_t> bytes;
  ~Writer() {
    if (!bytes.empty())
      OPENSSL_cleanse(bytes.data(), bytes.size());
  }
  void Raw(std::span<const uint8_t> b) {
    Check(b.size() <= WalletSnapshotStore::kMaxStateBytes - bytes.size());
    bytes.insert(bytes.end(), b.begin(), b.end());
  }
  void U32(uint32_t n) {
    for (size_t i = 0; i < 4; ++i) {
      uint8_t b = n >> (8 * i);
      Raw({&b, 1});
    }
  }
  void Blob(std::span<const uint8_t> b) {
    Check(b.size() <= UINT32_MAX);
    U32(b.size());
    Raw(b);
  }
};
struct Reader {
  std::span<const uint8_t> bytes;
  std::span<const uint8_t> Raw(size_t n) {
    Check(n <= bytes.size());
    auto b = bytes.first(n);
    bytes = bytes.subspan(n);
    return b;
  }
  uint32_t U32() {
    auto b = Raw(4);
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i)
      v |= uint32_t(b[i]) << (8 * i);
    return v;
  }
  Hash Hash32() {
    Hash h;
    auto b = Raw(32);
    std::copy(b.begin(), b.end(), h.begin());
    return h;
  }
  std::span<const uint8_t> Blob() {
    auto n = U32();
    Check(n <= WalletSnapshotStore::kMaxStateBytes);
    return Raw(n);
  }
};
using Observation = OrchardAccountState::OperationObservation;
using Outcome = OrchardAccountState::OperationOutcome;
Hash HashBytes(const uint256 &value) {
  Hash h;
  std::copy(value.begin(), value.end(), h.begin());
  return h;
}
uint256 HashValue(const Hash &value) {
  uint256 h;
  std::copy(value.begin(), value.end(), h.begin());
  return h;
}
class BlockObservations {
public:
  explicit BlockObservations(const OrchardBlockCandidate &block)
      : hash_(block.Header().GetHash()) {
    uint32_t ordinal = 0;
    for (const auto &tx : block.Transactions()) {
      const auto id = HashBytes(tx.GetTxid().AsUint256());
      Check(transactions_.emplace(id, ordinal).second);
      const auto input = [&](const Hash &previous, uint32_t index) {
        Check(
            inputs_
                .emplace(std::make_pair(previous, index), Spender{ordinal, id})
                .second);
      };
      if (tx.IsOrchard()) {
        for (const auto &in : tx.Orchard().Inputs())
          input(in.txid_wire, in.output_index);
        const auto &facts = tx.Orchard().UnverifiedFacts();
        for (uint32_t i = 0; i < facts.action_count; ++i) {
          Hash nf;
          std::copy(std::begin(facts.nullifiers[i]),
                    std::end(facts.nullifiers[i]), nf.begin());
          Check(nullifiers_.emplace(nf, Spender{ordinal, id}).second);
        }
      } else if (!tx.Historical().IsCoinbase()) {
        for (const auto &in : tx.Historical().vin)
          input(HashBytes(in.prevout.txid.AsUint256()), in.prevout.vout);
      }
      ++ordinal;
    }
  }
  explicit BlockObservations(const Block &block) : hash_(block.GetHash()) {
    bool mutated = false;
    Check(!block.vtx.empty() && ComputeMerkleRoot(block.vtx, &mutated) == block.header.merkle_root && !mutated);
    uint32_t ordinal = 0;
    for (const auto &tx : block.vtx) {
      const auto id = HashBytes(tx.GetTxid().AsUint256());
      Check(transactions_.emplace(id, ordinal).second);
      if (!tx.IsCoinbase()) for (const auto &in : tx.vin)
        Check(inputs_.emplace(std::make_pair(HashBytes(in.prevout.txid.AsUint256()), in.prevout.vout), Spender{ordinal,id}).second);
      ++ordinal;
    }
  }
  std::optional<Observation> Find(const OrchardOperationQueue::Entry &entry,
                                  uint32_t height) const {
    std::optional<Hash> own;
    if (entry.phase == OrchardOperationQueue::Phase::Ready)
      own = TransactionEnvelope::DecodeExact(entry.transaction).Txid();
    std::optional<Spender> conflict;
    const auto consider = [&](const auto &it, const auto &end) {
      if (it != end && (!own || it->second.id != *own) &&
          (!conflict || it->second.ordinal < conflict->ordinal))
        conflict = it->second;
    };
    for (const auto &in : entry.inputs)
      consider(inputs_.find({in.txid_wire, in.output_index}), inputs_.end());
    for (const auto &nf : entry.nullifiers)
      consider(nullifiers_.find(nf), nullifiers_.end());
    const bool included = own && transactions_.contains(*own);
    Check(!(included && conflict)); // Cannot be one validated selected block.
    if (included)
      return Observation{Outcome::Confirmed, height, hash_, *own};
    if (conflict)
      return Observation{Outcome::Conflicted, height, hash_, conflict->id};
    return std::nullopt;
  }

private:
  struct Spender {
    uint32_t ordinal;
    Hash id;
  };
  uint256 hash_;
  std::map<Hash, uint32_t> transactions_;
  std::map<std::pair<Hash, uint32_t>, Spender> inputs_;
  std::map<Hash, Spender> nullifiers_;
};
} // namespace
struct OrchardAccountState::Data {
  SigningDomain domain;
  FullViewingKeyBytes fvk;
  uint32_t activation;
  OrchardWalletScanState scan;
  OrchardOperationQueue operations;
  std::map<Hash, Observation> observations;
  ArchiveCheckpoint archive;
  DeliveryCheckpoint delivery;
  uint64_t parent_snapshot_revision = 0;
  std::array<DiversifierIndex, 2> next{};
  std::array<bool, 2> exhausted{};
  Data(SigningDomain d, const FullViewingKeyBytes &f, uint32_t a,
       const uint256 &parent)
      : domain(d), fvk(f), activation(a),
        scan(OrchardWalletScanState::Begin(d, f, a, parent)),
        operations(OrchardOperationQueue::Empty(d)) {}
  ~Data() { OPENSSL_cleanse(fvk.data(), fvk.size()); }
};
OrchardAccountState OrchardAccountState::Begin(SigningDomain domain,
                                               const FullViewingKeyBytes &fvk,
                                               uint32_t activation,
                                               const uint256 &parent) {
  return OrchardAccountState(
      std::make_shared<Data>(domain, fvk, activation, parent));
}
std::pair<OrchardAccountState, WalletReceiver>
OrchardAccountState::IssueReceiver(WalletScope scope) const {
  auto s = static_cast<uint8_t>(scope);
  Check(s < 2 && !data_->exhausted[s]);
  const auto receiver =
      WalletReceiver::FromViewingKey(data_->fvk, scope, data_->next[s]);
  auto next = std::make_shared<Data>(*data_);
  bool carry = true;
  for (auto &byte : next->next[s]) {
    if (!carry)
      break;
    carry = byte == 255;
    ++byte;
  }
  if (carry) {
    next->next[s].fill(255);
    next->exhausted[s] = true;
  }
  return {OrchardAccountState(std::move(next)), receiver};
}
OrchardAccountState OrchardAccountState::Advance(
    const OrchardBlockContext &context, const OrchardBlockCandidate &block,
    const PreparedOrchardState &prepared,
    std::span<const VerifiedOrchardAuthorizations> authorizations) const {
  Check(data_->delivery.sequence == 0);
  return AdvanceScan(context, block, prepared, authorizations);
}
OrchardAccountState OrchardAccountState::AdvanceScan(
    const OrchardBlockContext &context, const OrchardBlockCandidate &block,
    const PreparedOrchardState &prepared,
    std::span<const VerifiedOrchardAuthorizations> authorizations) const {
  auto next = std::make_shared<Data>(*data_);
  next->scan = data_->scan.Advance(context, block, prepared, authorizations);
  next->parent_snapshot_revision = 0;
  const BlockObservations observed(block);
  for (const auto &[id, entry] : next->operations.Entries()) {
    if (next->observations.contains(id))
      continue;
    if (auto observation = observed.Find(entry, context.height))
      next->observations.emplace(id, *observation);
  }
  return OrchardAccountState(std::move(next));
}
OrchardAccountState
OrchardAccountState::RewindScanFrom(const OrchardAccountState &parent) const {
  Check(data_->delivery.sequence == 0);
  return RewindScan(parent);
}
OrchardAccountState
OrchardAccountState::RewindScan(const OrchardAccountState &parent) const {
  Check(DomainEqual(data_->domain, parent.data_->domain) &&
        data_->activation == parent.data_->activation &&
        data_->fvk == parent.data_->fvk);
  Check(parent.Scan().Checkpoint().height < Scan().Checkpoint().height ||
        parent.Scan().Checkpoint() == Scan().Checkpoint());
  auto next = std::make_shared<Data>(*data_);
  next->scan = parent.data_->scan;
  next->parent_snapshot_revision = 0;
  const auto &checkpoint = next->scan.Checkpoint();
  std::erase_if(next->observations, [&](const auto &item) {
    return item.second.height > checkpoint.height;
  });
  return OrchardAccountState(std::move(next));
}
const OrchardAccountState::DeliveryCheckpoint &
OrchardAccountState::Delivery() const noexcept { return data_->delivery; }
uint64_t OrchardAccountState::ParentSnapshotRevision() const noexcept {
  return data_->parent_snapshot_revision;
}
OrchardAccountState OrchardAccountState::WithParentSnapshotRevision(uint64_t revision) const {
  Check(!revision || (data_->delivery.sequence && data_->scan.Checkpoint().height >= data_->activation));
  auto next = std::make_shared<Data>(*data_);
  next->parent_snapshot_revision = revision;
  return OrchardAccountState(std::move(next));
}
void OrchardAccountState::CheckDelivery(
    const RuntimeOutboxEvent &event, const OrchardBlockCandidate &block,
    bool connecting) const {
  const auto &current = data_->delivery;
  const auto &context = event.context;
  Check(current.sequence != UINT64_MAX &&
        event.cursor.sequence == current.sequence + 1 &&
        !event.cursor.digest.IsNull() && event.previous_digest == current.digest);
  Check(event.direction == (connecting ? RuntimeBlockDirection::Connect
                                      : RuntimeBlockDirection::Disconnect));
  Check(event.IsOrchardProfile() &&
        context.activation_height == data_->activation &&
        DomainEqual(context.domain, data_->domain) &&
        context.block_hash == block.Header().GetHash() &&
        context.parent_hash == block.Header().prev_block_hash &&
        event.body == block.WireBytes());
  const auto &scan = data_->scan.Checkpoint();
  if (connecting) {
    Check(scan.height != UINT32_MAX && context.height == scan.height + 1 &&
          context.parent_hash == scan.block_hash);
  } else {
    Check(context.height == scan.height && context.block_hash == scan.block_hash);
  }
}
OrchardAccountState OrchardAccountState::AdvanceDelivery(
    const RuntimeOutboxEvent &event, const OrchardBlockCandidate &block,
    const PreparedOrchardState &prepared,
    std::span<const VerifiedOrchardAuthorizations> authorizations) const {
  CheckDelivery(event, block, true);
  auto applied = AdvanceScan(event.context, block, prepared, authorizations);
  auto next = std::make_shared<Data>(*applied.data_);
  next->delivery = {event.cursor.sequence, event.cursor.digest};
  return OrchardAccountState(std::move(next));
}
OrchardAccountState OrchardAccountState::RewindDelivery(
    const RuntimeOutboxEvent &event, const OrchardBlockCandidate &block,
    const OrchardAccountState &parent) const {
  CheckDelivery(event, block, false);
  const auto &checkpoint = parent.Scan().Checkpoint();
  Check(checkpoint.height != UINT32_MAX &&
        checkpoint.height + 1 == event.context.height &&
        checkpoint.block_hash == event.context.parent_hash);
  auto applied = RewindScan(parent);
  auto next = std::make_shared<Data>(*applied.data_);
  next->delivery = {event.cursor.sequence, event.cursor.digest};
  return OrchardAccountState(std::move(next));
}
OrchardAccountState OrchardAccountState::ApplyHistoricalDelivery(
    const RuntimeOutboxEvent &event) const {
  const auto &c = event.context;
  const auto &receipt = data_->delivery;
  const auto &scan = data_->scan.Checkpoint();
  Check(receipt.sequence != UINT64_MAX && event.cursor.sequence == receipt.sequence + 1 &&
        !event.cursor.digest.IsNull() && event.previous_digest == receipt.digest);
  Check(!event.IsOrchardProfile() && c.activation_height == data_->activation &&
        DomainEqual(c.domain, data_->domain) && c.height > 0 && c.height <= INT32_MAX &&
        !c.block_hash.IsNull() && !c.parent_hash.IsNull() &&
        (event.direction == RuntimeBlockDirection::Connect || event.direction == RuntimeBlockDirection::Disconnect));
  const bool connecting = event.direction == RuntimeBlockDirection::Connect;
  Check(scan.height < data_->activation && data_->scan.Notes().empty() &&
        data_->scan.BalanceUna() == 0 && scan.pool_balance == 0 && scan.tree_size == 0);
  if (connecting) Check(c.height == scan.height + 1 && c.parent_hash == scan.block_hash);
  else Check(c.height == scan.height && c.block_hash == scan.block_hash);
  Check(!event.body.empty() && event.body.size() <= 16*1024*1024);
  const std::string wire(event.body.begin(), event.body.end());
  const auto body = Block::Deserialize(event.body);
  Check(body.has_value() && body->Serialize() == wire && body->GetHash() == c.block_hash &&
        body->header.prev_block_hash == c.parent_hash);
  const BlockObservations observed(*body); // Merkle/duplicates and real input IDs.
  auto next = std::make_shared<Data>(*data_);
  const auto height = connecting ? c.height : c.height - 1;
  const auto hash = connecting ? c.block_hash : c.parent_hash;
  next->parent_snapshot_revision = 0;
  next->scan = OrchardWalletScanState::AtHistoricalTip(data_->domain,data_->fvk,data_->activation,height,hash);
  if (connecting) {
    for (const auto &[id,entry] : next->operations.Entries()) {
      if (next->observations.contains(id)) continue;
      if (auto observation = observed.Find(entry,c.height)) {
        Check(observation->outcome == Outcome::Conflicted);
        next->observations.emplace(id,*observation);
      }
    }
  } else {
    std::erase_if(next->observations,[&](const auto &item){return item.second.height > height;});
  }
  next->delivery = {event.cursor.sequence,event.cursor.digest};
  return OrchardAccountState(std::move(next));
}
OrchardAccountState
OrchardAccountState::Reserve(const Hash &id,
                             const WalletProvingIntent &intent) const {
  auto next = std::make_shared<Data>(*data_);
  next->operations = data_->operations.Reserve(id, intent);
  return OrchardAccountState(std::move(next));
}
OrchardAccountState
OrchardAccountState::SetReady(const Hash &id,
                              const VerifiedOrchardAuthorizations &auth) const {
  auto next = std::make_shared<Data>(*data_);
  Check(!data_->observations.contains(id) ||
        data_->operations.Entries().at(id).phase ==
            OrchardOperationQueue::Phase::Ready);
  next->operations = data_->operations.SetReady(id, auth);
  return OrchardAccountState(std::move(next));
}
OrchardAccountState OrchardAccountState::CancelReserved(const Hash &id) const {
  auto next = std::make_shared<Data>(*data_);
  next->operations = data_->operations.CancelReserved(id);
  next->observations.erase(id);
  return OrchardAccountState(std::move(next));
}
const OrchardWalletScanState &OrchardAccountState::Scan() const noexcept {
  return data_->scan;
}
const OrchardOperationQueue &OrchardAccountState::Operations() const noexcept {
  return data_->operations;
}
const std::map<Hash, OrchardAccountState::OperationObservation> &
OrchardAccountState::Observations() const noexcept {
  return data_->observations;
}
WalletStateBytes OrchardAccountState::Encode() const {
  Writer w;
  auto version = magic;
  if (data_->parent_snapshot_revision) version[7] = '6';
  w.Raw(version);
  w.Raw({&data_->domain.network_code, 1});
  w.Raw(data_->domain.genesis_wire);
  w.U32(data_->domain.branch_id);
  w.U32(data_->activation);
  w.Raw(Identity(data_->fvk));
  for (size_t s = 0; s < 2; ++s) {
    w.Raw(data_->next[s]);
    uint8_t exhausted = data_->exhausted[s];
    w.Raw({&exhausted, 1});
  }
  auto scan = data_->scan.Encode(), operations = data_->operations.Encode();
  w.Blob(scan.Bytes());
  w.Blob(operations.Bytes());
  w.U32(data_->observations.size());
  for (const auto &[id, observation] : data_->observations) {
    w.Raw(id);
    uint8_t outcome = static_cast<uint8_t>(observation.outcome);
    w.Raw({&outcome, 1});
    w.U32(observation.height);
    w.Raw(HashBytes(observation.block_hash));
    w.Raw(observation.transaction_id);
  }
  w.U32(uint32_t(data_->archive.count));
  w.U32(uint32_t(data_->archive.count >> 32));
  w.Raw(data_->archive.head);
  w.U32(uint32_t(data_->delivery.sequence));
  w.U32(uint32_t(data_->delivery.sequence >> 32));
  w.Raw(HashBytes(data_->delivery.digest));
  if (data_->parent_snapshot_revision) {
    w.U32(uint32_t(data_->parent_snapshot_revision));
    w.U32(uint32_t(data_->parent_snapshot_revision >> 32));
  }
  return WalletStateBytes(w.bytes);
}
std::shared_ptr<OrchardAccountState::Data> OrchardAccountState::ReadMetadata(
    const WalletStateBytes &bytes, SigningDomain domain,
    const FullViewingKeyBytes &fvk, uint32_t activation, const uint256 &parent,
    std::span<const uint8_t> &scan) {
  auto state = std::make_shared<Data>(domain, fvk, activation, parent);
  Reader r{bytes.Bytes()};
  auto m = r.Raw(8);
  Check(std::equal(m.begin(), m.begin() + 7, magic.begin()) &&
        (m[7] >= '1' && m[7] <= '6'));
  const bool has_observations = m[7] >= '2';
  const bool has_archive = m[7] >= '3';
  const bool has_delivery = m[7] >= '4';
  Check(r.Raw(1)[0] == domain.network_code &&
        r.Hash32() == domain.genesis_wire && r.U32() == domain.branch_id &&
        r.U32() == activation && r.Hash32() == Identity(fvk));
  for (size_t s = 0; s < 2; ++s) {
    auto b = r.Raw(11);
    std::copy(b.begin(), b.end(), state->next[s].begin());
    auto exhausted = r.Raw(1)[0];
    Check(exhausted <= 1);
    state->exhausted[s] = exhausted;
    if (exhausted)
      Check(
          std::all_of(b.begin(), b.end(), [](uint8_t v) { return v == 255; }));
  }
  scan = r.Blob();
  auto operations = r.Blob();
  // Parse the bounded observation section before expensive proof restore.
  if (has_observations) {
    const auto count = r.U32();
    Check(count <= OrchardOperationQueue::kMaxPending &&
          count <= r.bytes.size() / 101);
    Hash previous{};
    for (uint32_t i = 0; i < count; ++i) {
      const auto id = r.Hash32();
      Check(id > previous);
      previous = id;
      const auto outcome = r.Raw(1)[0];
      Check(outcome == 1 || outcome == 2);
      const auto height = r.U32();
      const auto block = HashValue(r.Hash32());
      const auto txid = r.Hash32();
      Check(height > 0 && (height >= activation || (m[7] >= '5' && outcome == 2)) &&
            !block.IsNull() && txid != Hash{});
      state->observations.emplace(
          id, Observation{static_cast<Outcome>(outcome), height, block, txid});
    }
  }
  if (has_archive) {
    const uint64_t low = r.U32(), high = r.U32();
    state->archive.count = low | (high << 32);
    state->archive.head = r.Hash32();
    Check((state->archive.count == 0) == (state->archive.head == Hash{}));
  }
  if (has_delivery) {
    const uint64_t low = r.U32(), high = r.U32();
    state->delivery.sequence = low | (high << 32);
    state->delivery.digest = HashValue(r.Hash32());
    Check((state->delivery.sequence == 0) == state->delivery.digest.IsNull());
  }
  if (m[7] >= '6') {
    const uint64_t low = r.U32(), high = r.U32();
    state->parent_snapshot_revision = low | (high << 32);
    Check(state->parent_snapshot_revision && state->delivery.sequence);
  }
  Check(r.bytes.empty());
  state->operations =
      OrchardOperationQueue::Restore(WalletStateBytes(operations), domain);
  for (const auto &[id, observation] : state->observations) {
    const auto found = state->operations.Entries().find(id);
    Check(found != state->operations.Entries().end());
    if (observation.outcome == Outcome::Confirmed) {
      Check(
          found->second.phase == OrchardOperationQueue::Phase::Ready &&
          TransactionEnvelope::DecodeExact(found->second.transaction).Txid() ==
              observation.transaction_id);
    }
  }
  return state;
}
OrchardAccountState::DeliveryCheckpoint OrchardAccountState::ReadDeliveryMetadata(
    const WalletStateBytes& bytes,SigningDomain domain,const FullViewingKeyBytes& fvk,
    uint32_t activation,const uint256& origin) {
  std::span<const uint8_t> scan;
  return ReadMetadata(bytes,domain,fvk,activation,origin,scan)->delivery;
}
OrchardAccountState OrchardAccountState::Restore(
    const WalletStateBytes &bytes, SigningDomain domain,
    const FullViewingKeyBytes &fvk, uint32_t activation,
    const storage::OrchardStoredState &checkpoint,
    const OrchardWalletRestoreLookups &lookups) {
  std::span<const uint8_t> scan;
  auto state =
      ReadMetadata(bytes, domain, fvk, activation, checkpoint.block_hash, scan);
  state->scan = OrchardWalletScanState::Restore(
      WalletStateBytes(scan), domain, fvk, activation, checkpoint, lookups);
  Check(!state->parent_snapshot_revision || checkpoint.height >= activation);
  const OrchardAccountState candidate(state);
  for (const auto &[id, observation] : state->observations)
    candidate.VerifyOperationObservation(id, lookups);
  return OrchardAccountState(std::move(state));
}
const OrchardAccountState::ArchiveCheckpoint &
OrchardAccountState::Archive() const noexcept {
  return data_->archive;
}
OrchardAccountState
OrchardAccountState::WithArchive(ArchiveCheckpoint checkpoint) const {
  Check((checkpoint.count == 0) == (checkpoint.head == Hash{}));
  auto next = std::make_shared<Data>(*data_);
  next->archive = checkpoint;
  return OrchardAccountState(std::move(next));
}
void OrchardAccountState::VerifyOperationObservation(
    const Hash &id, const OrchardWalletRestoreLookups &lookups) const {
  const auto &observation = data_->observations.at(id);
  const auto &checkpoint = data_->scan.Checkpoint();
  Check(observation.height <= checkpoint.height);
  if (observation.height < data_->activation) {
    Check(observation.outcome == Outcome::Conflicted && bool(lookups.selected_historical_block));
    const auto block = lookups.selected_historical_block(observation.height,observation.block_hash);
    Check(bool(block) && block->GetHash() == observation.block_hash);
    if (observation.height == checkpoint.height) Check(observation.block_hash == checkpoint.block_hash);
    Check(BlockObservations(*block).Find(data_->operations.Entries().at(id),observation.height) == observation);
    return;
  }
  Check(bool(lookups.selected_block));
  if (observation.height == checkpoint.height)
    Check(observation.block_hash == checkpoint.block_hash);
  const auto block =
      lookups.selected_block(observation.height, observation.block_hash);
  Check(bool(block) && block->Header().GetHash() == observation.block_hash);
  std::string error;
  Check(block->CheckSizeLimits(error) &&
        block->CheckIdentityCommitments(true, error));
  Check(BlockObservations(*block).Find(data_->operations.Entries().at(id),
                                       observation.height) == observation);
}
OrchardAccountState
OrchardAccountState::RemoveObservedOperation(const Hash &id) const {
  Check(data_->observations.contains(id) &&
        data_->operations.Entries().contains(id));
  auto next = std::make_shared<Data>(*data_);
  next->operations.entries_.erase(id);
  next->observations.erase(id);
  return OrchardAccountState(std::move(next));
}
OrchardAccountState OrchardAccountState::RestoreArchivedOperation(
    const Hash &id, const OrchardOperationQueue &single) const {
  Check(single.entries_.size() == 1 && single.entries_.contains(id) &&
        DomainEqual(single.domain_, data_->domain) &&
        !data_->observations.contains(id));
  const auto &entry = single.entries_.at(id);
  if (auto it = data_->operations.entries_.find(id);
      it != data_->operations.entries_.end()) {
    // Reconciliation retry never replaces or re-proves a transaction.
    auto existing = OrchardOperationQueue::Empty(data_->domain);
    existing.entries_.emplace(id, it->second);
    Check(existing.ContinuesArchived(single));
    return *this;
  }
  Check(data_->operations.entries_.size() < OrchardOperationQueue::kMaxPending);
  auto next = std::make_shared<Data>(*data_);
  next->operations.entries_.emplace(id, entry);
  next->operations.CheckUniqueReservations();
  return OrchardAccountState(std::move(next));
}
OrchardAccountState OrchardAccountState::ObserveReactivatedOperation(
    const Hash& id,uint32_t fork_height,const OrchardWalletRestoreLookups& lookups,
    const std::function<StatusOr<uint256>(uint32_t)>& selected_hash) const {
  const auto& checkpoint=data_->scan.Checkpoint();
  Check(bool(selected_hash)&&fork_height<=checkpoint.height&&
        data_->operations.Entries().contains(id)&&!data_->observations.contains(id));
  const auto selected=[&](uint32_t height){
    const auto hash=selected_hash(height);
    if(!hash.ok())throw OrchardStateLookupError(hash.status());
    Check(!hash->IsNull());return *hash;
  };
  auto previous=selected(fork_height);auto next=std::make_shared<Data>(*data_);
  const auto& entry=next->operations.Entries().at(id);
  for(uint64_t height=uint64_t(fork_height)+1;height<=checkpoint.height;++height){
    const auto hash=selected(uint32_t(height));
    const auto observed=[&]{
      if(height<data_->activation){
        Check(bool(lookups.selected_historical_block));
        const auto block=lookups.selected_historical_block(uint32_t(height),hash);
        Check(bool(block)&&block->GetHash()==hash&&block->header.prev_block_hash==previous);
        return BlockObservations(*block);
      }
      Check(bool(lookups.selected_block));
      const auto block=lookups.selected_block(uint32_t(height),hash);std::string error;
      Check(bool(block)&&block->Header().GetHash()==hash&&block->Header().prev_block_hash==previous&&
            block->CheckSizeLimits(error)&&block->CheckIdentityCommitments(true,error));
      return BlockObservations(*block);
    }();
    if(!next->observations.contains(id))
      if(auto observation=observed.Find(entry,uint32_t(height)))next->observations.emplace(id,*observation);
    previous=hash;
  }
  Check(previous==checkpoint.block_hash);
  return OrchardAccountState(std::move(next));
}
OrchardAccountState OrchardAccountState::RestoreForRescan(
    const WalletStateBytes &bytes, SigningDomain domain,
    const FullViewingKeyBytes &fvk, uint32_t activation,
    const uint256 &parent) {
  std::span<const uint8_t> ignored;
  auto state = ReadMetadata(bytes, domain, fvk, activation, parent, ignored);
  state->delivery = {};
  state->parent_snapshot_revision = 0;
  state->observations
      .clear(); // Rescan rebuilds chain observations, never signed bytes.
  return OrchardAccountState(std::move(state));
}
} // namespace dinero::wallet
