#include "wallet/orchard_operation_queue.h"
#include <algorithm>
#include <openssl/crypto.h>
#include <set>
namespace dinero::wallet {
namespace {
using namespace orchard;
using namespace consensus;
[[noreturn]] void Fail() {
  throw std::runtime_error("Orchard pending wallet operation rejected");
}
void Check(bool v) {
  if (!v)
    Fail();
}
bool DomainEqual(const SigningDomain &a, const SigningDomain &b) {
  return a.network_code == b.network_code && a.genesis_wire == b.genesis_wire &&
         a.branch_id == b.branch_id;
}
Hash Bytes(const uint8_t (&v)[32]) {
  Hash h;
  std::copy(std::begin(v), std::end(v), h.begin());
  return h;
}
constexpr std::array<uint8_t, 8> magic{'D', 'N', 'O', 'R', 'O', 'P', '0', '1'};
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
      uint8_t b = n >> (8 * i);
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
    auto b = bytes.first(n);
    bytes = bytes.subspan(n);
    return b;
  }
  uint64_t Number(size_t n) {
    auto b = Raw(n);
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i)
      v |= uint64_t(b[i]) << (8 * i);
    return v;
  }
  Hash Hash32() {
    Hash h;
    auto b = Raw(32);
    std::copy(b.begin(), b.end(), h.begin());
    return h;
  }
  std::vector<uint8_t> Blob(size_t max) {
    auto n = Number(4);
    Check(n <= max);
    auto b = Raw(n);
    return {b.begin(), b.end()};
  }
};
} // namespace
OrchardOperationQueue OrchardOperationQueue::Empty(SigningDomain domain) {
  (void)SigningContext::Create(domain, 0, {}, {}, 0);
  return OrchardOperationQueue(domain);
}
void OrchardOperationQueue::CheckEntry(const Entry &e, bool verify) const {
  Check(e.inputs.size() <= 1024 && e.nullifiers.size() > 0 &&
        e.nullifiers.size() <= kMaxActionsV1);
  uint64_t amount = 0;
  for (const auto &input : e.inputs) {
    Check(input.amount_una <= kMaxMoneyUna &&
          amount <= kMaxMoneyUna - input.amount_una);
    amount += input.amount_una;
    Check(!input.script_pub_key.empty() &&
          input.script_pub_key.size() <= 10000);
  }
  Check(e.message != Hash{});
  if (e.phase == Phase::Reserved) {
    Check(e.transaction.empty());
    return;
  }
  Check(e.phase == Phase::Ready && !e.transaction.empty());
  auto tx = TransactionEnvelope::DecodeExact(e.transaction);
  Check(tx.Inputs().size() == e.inputs.size());
  std::vector<PreviousOutput> coins;
  for (size_t i = 0; i < e.inputs.size(); ++i) {
    const auto &in = e.inputs[i];
    const auto &wire = tx.Inputs()[i];
    Check(in.txid_wire == wire.txid_wire &&
          in.output_index == wire.output_index && in.sequence == wire.sequence);
    coins.push_back(
        {in.txid_wire, in.output_index, in.amount_una, in.script_pub_key});
  }
  Check(tx.SigningDigest(domain_, coins) == e.message);
  const auto &facts = tx.UnverifiedFacts();
  Check(e.nullifiers.size() == facts.action_count);
  for (size_t i = 0; i < e.nullifiers.size(); ++i)
    Check(e.nullifiers[i] == Bytes(facts.nullifiers[i]));
  if (verify)
    Check(tx.VerifyAuthorization(domain_, coins).Orchard().SigningDigest() ==
          e.message);
}
void OrchardOperationQueue::CheckUniqueReservations() const {
  Check(entries_.size() <= kMaxPending);
  std::set<std::pair<Hash, uint32_t>> inputs;
  std::set<Hash> nullifiers;
  for (const auto &[id, e] : entries_) {
    Check(id != Hash{});
    for (const auto &in : e.inputs)
      Check(inputs.emplace(in.txid_wire, in.output_index).second);
    for (const auto &nf : e.nullifiers)
      Check(nullifiers.insert(nf).second);
  }
}
OrchardOperationQueue
OrchardOperationQueue::Reserve(const Hash &id,
                               const WalletProvingIntent &intent) const {
  Check(id != Hash{} && DomainEqual(domain_, intent.Domain()) &&
        entries_.size() < kMaxPending && !entries_.contains(id));
  auto next = *this;
  Entry entry{Phase::Reserved,
              intent.Message(),
              intent.Inputs(),
              intent.Nullifiers(),
              {}};
  CheckEntry(entry, false);
  next.entries_.emplace(id, std::move(entry));
  next.CheckUniqueReservations();
  return next;
}
OrchardOperationQueue OrchardOperationQueue::SetReady(
    const Hash &id, const VerifiedOrchardAuthorizations &auth) const {
  const auto it = entries_.find(id);
  Check(it != entries_.end());
  const auto &old = it->second;
  // Idempotent retry may return exactly the already-frozen bytes, never a
  // new proof/txid for the same durable operation after it could be relayed.
  if (old.phase == Phase::Ready) {
    Check(old.transaction == auth.Orchard().CanonicalBytes());
    return *this;
  }
  Check(old.phase == Phase::Reserved &&
        old.message == auth.Orchard().Orchard().SigningDigest());
  auto next = *this;
  auto &e = next.entries_.at(id);
  e.phase = Phase::Ready;
  e.transaction = auth.Orchard().CanonicalBytes();
  next.CheckEntry(e, false);
  return next;
}
OrchardOperationQueue
OrchardOperationQueue::CancelReserved(const Hash &id) const {
  auto it = entries_.find(id);
  Check(it != entries_.end() && it->second.phase == Phase::Reserved);
  auto next = *this;
  next.entries_.erase(id);
  return next;
}
WalletStateBytes OrchardOperationQueue::Encode() const {
  Writer w;
  w.Raw(magic);
  w.Number(domain_.network_code, 1);
  w.Raw(domain_.genesis_wire);
  w.Number(domain_.branch_id, 4);
  w.Number(entries_.size(), 4);
  for (const auto &[id, e] : entries_) {
    w.Raw(id);
    w.Number(static_cast<uint8_t>(e.phase), 1);
    w.Raw(e.message);
    w.Number(e.inputs.size(), 4);
    for (const auto &in : e.inputs) {
      w.Raw(in.txid_wire);
      w.Number(in.output_index, 4);
      w.Number(in.sequence, 4);
      w.Number(in.amount_una, 8);
      w.Blob(in.script_pub_key);
    }
    w.Number(e.nullifiers.size(), 1);
    for (const auto &nf : e.nullifiers)
      w.Raw(nf);
    w.Blob(e.transaction);
  }
  return WalletStateBytes(w.bytes);
}
OrchardOperationQueue
OrchardOperationQueue::Restore(const WalletStateBytes &bytes,
                               SigningDomain domain) {
  auto queue = Empty(domain);
  Reader r{bytes.Bytes()};
  auto m = r.Raw(magic.size());
  Check(std::equal(m.begin(), m.end(), magic.begin()));
  Check(r.Number(1) == domain.network_code &&
        r.Hash32() == domain.genesis_wire && r.Number(4) == domain.branch_id);
  const auto count = r.Number(4);
  Check(count <= kMaxPending && count <= r.bytes.size() / 74);
  Hash previous{};
  for (size_t n = 0; n < count; ++n) {
    auto id = r.Hash32();
    Check(id > previous);
    previous = id;
    Entry e;
    auto phase = r.Number(1);
    Check(phase <= 1);
    e.phase = static_cast<Phase>(phase);
    e.message = r.Hash32();
    const auto inputs = r.Number(4);
    Check(inputs <= 1024 && inputs <= r.bytes.size() / 52);
    e.inputs.reserve(inputs);
    for (size_t i = 0; i < inputs; ++i) {
      ResolvedInput in;
      in.txid_wire = r.Hash32();
      in.output_index = r.Number(4);
      in.sequence = r.Number(4);
      in.amount_una = r.Number(8);
      in.script_pub_key = r.Blob(10000);
      e.inputs.push_back(std::move(in));
    }
    const auto nfs = r.Number(1);
    Check(nfs > 0 && nfs <= kMaxActionsV1);
    for (size_t i = 0; i < nfs; ++i)
      e.nullifiers.push_back(r.Hash32());
    e.transaction = r.Blob(kMaxTransactionBytes);
    queue.CheckEntry(e, false);
    queue.entries_.emplace(id, std::move(e));
  }
  Check(r.bytes.empty());
  queue.CheckUniqueReservations();
  // Only after bounded structural validation of the entire snapshot do any
  // expensive proof checks. Persisted coin values are authenticated wallet
  // data, not fresh chain eligibility; broadcasting still requires admission.
  for (const auto &[id, e] : queue.entries_)
    queue.CheckEntry(e, true);
  return queue;
}
} // namespace dinero::wallet
