#include "wallet/orchard_scan_state.h"
#include <algorithm>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <set>
namespace dinero::wallet {
namespace {
using namespace orchard;
using namespace consensus;
[[noreturn]] void Fail() {
  throw std::runtime_error("Orchard wallet scan source/state mismatch");
}
void Check(bool ok) {
  if (!ok)
    Fail();
}
Hash HashBytes(const uint256 &value) {
  Hash out;
  std::copy(value.begin(), value.end(), out.begin());
  return out;
}
Hash HashBytes(const uint8_t (&value)[32]) {
  Hash out;
  std::copy(std::begin(value), std::end(value), out.begin());
  return out;
}
bool DomainEqual(const SigningDomain &a, const SigningDomain &b) {
  return a.network_code == b.network_code && a.genesis_wire == b.genesis_wire &&
         a.branch_id == b.branch_id;
}
OrchardFrontier Frontier(const storage::OrchardStoredState &state) {
  return OrchardFrontier::Decode(
      {reinterpret_cast<const uint8_t *>(state.frontier.data()),
       state.frontier.size()});
}
constexpr size_t kMaxWalletNotes = 4096;
} // namespace
struct OrchardWalletScanState::Data {
  orchard::SigningDomain domain;
  orchard::FullViewingKeyBytes fvk;
  uint32_t activation;
  storage::OrchardStoredState checkpoint;
  std::vector<ScannedOrchardNote> notes;
  uint64_t balance = 0;
  ~Data() { OPENSSL_cleanse(fvk.data(), fvk.size()); }
};
OrchardWalletScanState
OrchardWalletScanState::Begin(SigningDomain domain,
                              const FullViewingKeyBytes &fvk,
                              uint32_t activation, const uint256 &parent) {
  Check(activation > 0 && activation != UINT32_MAX && !parent.IsNull());
  (void)SigningContext::Create(domain, 0, {}, {}, 0);
  (void)WalletReceiver::FromViewingKey(fvk, WalletScope::External, {});
  auto state = std::make_shared<Data>();
  state->domain = domain;
  state->fvk = fvk;
  state->activation = activation;
  const auto frontier = OrchardFrontier::Empty();
  uint256 root;
  std::copy(frontier.Root().begin(), frontier.Root().end(), root.begin());
  state->checkpoint = {
      activation - 1,
      parent,
      root,
      0,
      0,
      std::string(frontier.Bytes().begin(), frontier.Bytes().end())};
  return OrchardWalletScanState(std::move(state));
}
const storage::OrchardStoredState &
OrchardWalletScanState::Checkpoint() const noexcept {
  return data_->checkpoint;
}
const std::vector<ScannedOrchardNote> &
OrchardWalletScanState::Notes() const noexcept {
  return data_->notes;
}
uint64_t OrchardWalletScanState::BalanceUna() const noexcept {
  return data_->balance;
}
OrchardWalletScanState OrchardWalletScanState::Advance(
    const OrchardBlockContext &context, const OrchardBlockCandidate &block,
    const PreparedOrchardState &prepared,
    std::span<const VerifiedOrchardAuthorizations> authorizations) const {
  Check(data_->checkpoint.height < UINT32_MAX &&
        context.height == data_->checkpoint.height + 1 &&
        context.parent_hash == data_->checkpoint.block_hash);
  Check(context.activation_height == data_->activation &&
        DomainEqual(context.domain, data_->domain));
  Check(block.Header().GetHash() == context.block_hash &&
        block.Header().prev_block_hash == context.parent_hash &&
        block.Header().IsReservedValid());
  std::string error;
  Check(block.CheckSizeLimits(error) &&
        block.CheckIdentityCommitments(true, error));
  Check(prepared.Next().height == context.height &&
        prepared.Next().block_hash == context.block_hash);
  if (context.height == data_->activation)
    Check(!prepared.Parent());
  else
    Check(prepared.Parent() && *prepared.Parent() == data_->checkpoint);
  size_t count = 0;
  std::set<TxId> ids;
  for (const auto &tx : block.Transactions()) {
    Check(ids.insert(tx.GetTxid()).second);
    if (tx.IsOrchard()) {
      Check(count < authorizations.size() &&
            tx.Orchard().CanonicalBytes() ==
                authorizations[count].Orchard().CanonicalBytes());
      ++count;
    } else
      Check(!Transaction::IsShieldedVersion(tx.Historical().version) &&
            tx.Historical().shielded_bundle_bytes.empty());
  }
  Check(count == authorizations.size());
  std::set<Hash> spent;
  size_t nf_index = 0;
  for (const auto &auth : authorizations) {
    Check(auth.Transparent().CandidateHeight() == context.height &&
          auth.Transparent().Snapshot().ViewHeight() ==
              data_->checkpoint.height);
    Check(auth.Transparent().Snapshot().SigningDigest(data_->domain) ==
          auth.Orchard().Orchard().SigningDigest());
    const auto &facts = auth.Orchard().Orchard().Facts();
    Check(facts.action_count > 0 && facts.action_count <= kMaxActionsV1);
    for (uint32_t i = 0; i < facts.action_count; ++i) {
      Check(nf_index < prepared.Nullifiers().size());
      auto nf = HashBytes(facts.nullifiers[i]);
      Check(nf == HashBytes(prepared.Nullifiers()[nf_index++]) &&
            spent.insert(nf).second);
    }
  }
  Check(nf_index == prepared.Nullifiers().size());
  auto next = std::make_shared<Data>(*data_);
  std::erase_if(next->notes, [&](const auto &owned) {
    return spent.contains(HashBytes(owned.note->Facts().nullifier));
  });
  auto frontier =
      std::make_unique<OrchardFrontier>(Frontier(data_->checkpoint));
  for (const auto &auth : authorizations) {
    const auto &verified = auth.Orchard().Orchard();
    const auto &facts = verified.Facts();
    std::vector<Hash> commitments;
    for (uint32_t i = 0; i < facts.action_count; ++i)
      commitments.push_back(HashBytes(facts.commitments[i]));
    auto after =
        std::make_unique<OrchardFrontier>(frontier->Append(commitments));
    for (auto &owned : next->notes)
      owned.witness = std::make_shared<WalletWitness>(
          owned.witness->Append(commitments, frontier->Root(), after->Root()));
    std::shared_ptr<const VerifiedOrchardAuthorizations> origin;
    for (uint32_t i = 0; i < facts.action_count; ++i) {
      bool matched = false;
      for (auto scope : {WalletScope::External, WalletScope::Internal}) {
        auto received = WalletNote::Receive(verified, next->fvk, scope, i);
        if (!received)
          continue;
        Check(!matched);
        matched = true;
        if (received->Facts().amount == 0)
          continue;
        Check(next->notes.size() < kMaxWalletNotes);
        if (!origin)
          origin = std::make_shared<VerifiedOrchardAuthorizations>(auth);
        auto witness = std::make_shared<WalletWitness>(
            WalletWitness::ForAppendedLeaf(*frontier, commitments, i));
        next->notes.push_back(
            {std::make_shared<WalletNote>(std::move(*received)),
             std::move(witness), origin, i, scope, context.height,
             context.block_hash});
      }
    }
    frontier = std::move(after);
  }
  Check(frontier->Bytes() == Frontier(prepared.Next()).Bytes() &&
        frontier->Size() == prepared.Next().tree_size &&
        frontier->Root() == HashBytes(prepared.Next().anchor));
  std::set<Hash> notes;
  std::set<uint32_t> positions;
  next->balance = 0;
  for (const auto &owned : next->notes) {
    const auto &nf = owned.note->Facts();
    const auto &witness = owned.witness->Facts();
    Check(notes.insert(HashBytes(nf.nullifier)).second &&
          positions.insert(witness.position).second);
    Check(HashBytes(witness.root) == frontier->Root() &&
          witness.leaf_count == frontier->Size() &&
          HashBytes(witness.commitment) == HashBytes(nf.commitment));
    Check(nf.amount <= kMaxMoneyUna &&
          next->balance <= kMaxMoneyUna - nf.amount);
    next->balance += nf.amount;
  }
  Check(next->balance <= prepared.Next().pool_balance);
  next->checkpoint = prepared.Next();
  return OrchardWalletScanState(std::move(next));
}

namespace {
// The snapshot is wallet-private and must only reach durable storage through
// WalletSnapshotStore. Bounds are enforced before allocation or callbacks.
constexpr std::array<uint8_t, 8> kScanMagic{'D', 'N', 'O', 'R',
                                            'W', 'S', '0', '1'};
struct Writer {
  std::vector<uint8_t> bytes;
  ~Writer() {
    if (!bytes.empty())
      OPENSSL_cleanse(bytes.data(), bytes.size());
  }
  void Raw(std::span<const uint8_t> v) {
    Check(v.size() <= WalletSnapshotStore::kMaxStateBytes - bytes.size());
    bytes.insert(bytes.end(), v.begin(), v.end());
  }
  void Number(uint64_t n, size_t width) {
    for (size_t i = 0; i < width; ++i) {
      const uint8_t b = n >> (8 * i);
      Raw({&b, 1});
    }
  }
  void Blob(std::span<const uint8_t> v) {
    Check(v.size() <= UINT32_MAX);
    Number(v.size(), 4);
    Raw(v);
  }
};
struct Reader {
  std::span<const uint8_t> bytes;
  std::span<const uint8_t> Raw(size_t n) {
    Check(n <= bytes.size());
    auto v = bytes.first(n);
    bytes = bytes.subspan(n);
    return v;
  }
  uint64_t Number(size_t width) {
    uint64_t n = 0;
    auto v = Raw(width);
    for (size_t i = 0; i < width; ++i)
      n |= uint64_t(v[i]) << (8 * i);
    return n;
  }
  std::span<const uint8_t> Blob(size_t max) {
    auto n = Number(4);
    Check(n <= max);
    return Raw(n);
  }
  uint256 Uint256() {
    uint256 v;
    auto b = Raw(32);
    std::copy(b.begin(), b.end(), v.begin());
    return v;
  }
  Hash Hash32() {
    Hash v;
    auto b = Raw(32);
    std::copy(b.begin(), b.end(), v.begin());
    return v;
  }
};
Hash ViewingIdentity(const FullViewingKeyBytes &fvk) {
  Hash h;
  SHA256(fvk.data(), fvk.size(), h.data());
  return h;
}
void WriteCheckpoint(Writer &w, const storage::OrchardStoredState &s) {
  w.Number(s.height, 4);
  w.Raw(HashBytes(s.block_hash));
  w.Raw(HashBytes(s.anchor));
  w.Number(s.pool_balance, 8);
  w.Number(s.tree_size, 8);
  w.Blob({reinterpret_cast<const uint8_t *>(s.frontier.data()),
          s.frontier.size()});
}
storage::OrchardStoredState ReadCheckpoint(Reader &r) {
  storage::OrchardStoredState s;
  s.height = r.Number(4);
  s.block_hash = r.Uint256();
  s.anchor = r.Uint256();
  s.pool_balance = r.Number(8);
  s.tree_size = r.Number(8);
  auto frontier = r.Blob(storage::ORCHARD_STORED_FRONTIER_LIMIT);
  s.frontier.assign(reinterpret_cast<const char *>(frontier.data()),
                    frontier.size());
  return s;
}
} // namespace
WalletStateBytes OrchardWalletScanState::Encode() const {
  Writer w;
  w.Raw(kScanMagic);
  w.Number(data_->domain.network_code, 1);
  w.Raw(data_->domain.genesis_wire);
  w.Number(data_->domain.branch_id, 4);
  w.Number(data_->activation, 4);
  w.Raw(ViewingIdentity(data_->fvk));
  WriteCheckpoint(w, data_->checkpoint);
  w.Number(data_->notes.size(), 4);
  for (const auto &owned : data_->notes) {
    w.Number(owned.created_height, 4);
    w.Raw(HashBytes(owned.created_block));
    w.Raw(owned.origin->Orchard().Txid());
    w.Number(owned.action_index, 4);
    w.Number(static_cast<uint8_t>(owned.scope), 1);
    w.Blob(owned.witness->Encode());
  }
  return WalletStateBytes(w.bytes);
}
OrchardWalletScanState OrchardWalletScanState::Restore(
    const WalletStateBytes &encoded, SigningDomain domain,
    const FullViewingKeyBytes &fvk, uint32_t activation,
    const storage::OrchardStoredState &selected,
    const OrchardWalletRestoreLookups &lookups) {
  Check(activation > 0 && activation != UINT32_MAX &&
        selected.height >= activation - 1 && selected.height < UINT32_MAX &&
        !selected.block_hash.IsNull());
  (void)SigningContext::Create(domain, 0, {}, {}, 0);
  (void)WalletReceiver::FromViewingKey(fvk, WalletScope::External, {});
  Reader r{encoded.Bytes()};
  auto magic = r.Raw(kScanMagic.size());
  Check(std::equal(magic.begin(), magic.end(), kScanMagic.begin()));
  Check(r.Number(1) == domain.network_code &&
        r.Hash32() == domain.genesis_wire && r.Number(4) == domain.branch_id);
  Check(r.Number(4) == activation && r.Hash32() == ViewingIdentity(fvk));
  auto checkpoint = ReadCheckpoint(r);
  Check(checkpoint == selected && checkpoint.pool_balance <= kMaxMoneyUna);
  auto frontier = Frontier(checkpoint);
  Check(frontier.Size() == checkpoint.tree_size &&
        frontier.Root() == HashBytes(checkpoint.anchor));
  if (checkpoint.height == activation - 1)
    Check(checkpoint.pool_balance == 0 && frontier.Size() == 0);
  const auto count = r.Number(4);
  Check(count <= kMaxWalletNotes && count <= r.bytes.size() / 77);
  struct Record {
    uint32_t height;
    uint256 block;
    Hash txid;
    uint32_t index;
    WalletScope scope;
    std::span<const uint8_t> witness;
  };
  std::vector<Record> records;
  records.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    Record item;
    item.height = r.Number(4);
    item.block = r.Uint256();
    item.txid = r.Hash32();
    item.index = r.Number(4);
    const auto scope = r.Number(1);
    Check(scope <= static_cast<uint8_t>(WalletScope::Internal));
    item.scope = static_cast<WalletScope>(scope);
    item.witness = r.Blob(4096);
    Check(item.height >= activation && item.height <= checkpoint.height &&
          !item.block.IsNull() && item.index < kMaxActionsV1);
    records.push_back(item);
  }
  Check(r.bytes.empty());
  if (count)
    Check(bool(lookups.origin) && bool(lookups.spent_nullifier));
  auto state = std::make_shared<Data>();
  state->domain = domain;
  state->fvk = fvk;
  state->activation = activation;
  state->checkpoint = checkpoint;
  std::set<Hash> nullifiers;
  std::set<uint32_t> positions;
  for (const auto &item : records) {
    auto origin = lookups.origin(item.height, item.block, item.txid);
    Check(bool(origin));
    Check(origin->Orchard().Txid() == item.txid &&
          origin->Transparent().CandidateHeight() == item.height &&
          origin->Transparent().Snapshot().ViewHeight() == item.height - 1 &&
          origin->Transparent().Snapshot().SigningDigest(domain) ==
              origin->Orchard().Orchard().SigningDigest());
    auto received = WalletNote::Receive(origin->Orchard().Orchard(), fvk,
                                        item.scope, item.index);
    Check(bool(received));
    const auto &facts = received->Facts();
    Check(facts.amount > 0 && facts.amount <= kMaxMoneyUna);
    auto witness =
        WalletWitness::Decode(item.witness, HashBytes(facts.commitment),
                              frontier.Root(), frontier.Size());
    Check(nullifiers.insert(HashBytes(facts.nullifier)).second &&
          positions.insert(witness.Facts().position).second);
    uint256 nf;
    std::copy(std::begin(facts.nullifier), std::end(facts.nullifier),
              nf.begin());
    auto spent = lookups.spent_nullifier(nf);
    if (!spent.ok())
      throw OrchardStateLookupError(spent.status());
    Check(!spent.value());
    Check(state->balance <= kMaxMoneyUna - facts.amount);
    state->balance += facts.amount;
    state->notes.push_back({std::make_shared<WalletNote>(std::move(*received)),
                            std::make_shared<WalletWitness>(std::move(witness)),
                            std::move(origin), item.index, item.scope,
                            item.height, item.block});
  }
  Check(state->balance <= checkpoint.pool_balance);
  auto restored = OrchardWalletScanState(std::move(state));
  auto canonical = restored.Encode();
  Check(std::equal(canonical.Bytes().begin(), canonical.Bytes().end(),
                   encoded.Bytes().begin(), encoded.Bytes().end()));
  return restored;
}
} // namespace dinero::wallet
