#include "wallet/orchard_account_state.h"
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
constexpr std::array<uint8_t, 8> magic{'D', 'N', 'O', 'R', 'A', 'C', '0', '1'};
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
} // namespace
struct OrchardAccountState::Data {
  SigningDomain domain;
  FullViewingKeyBytes fvk;
  uint32_t activation;
  OrchardWalletScanState scan;
  OrchardOperationQueue operations;
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
  auto next = std::make_shared<Data>(*data_);
  next->scan = data_->scan.Advance(context, block, prepared, authorizations);
  return OrchardAccountState(std::move(next));
}
OrchardAccountState
OrchardAccountState::RewindScanFrom(const OrchardAccountState &parent) const {
  Check(DomainEqual(data_->domain, parent.data_->domain) &&
        data_->activation == parent.data_->activation &&
        data_->fvk == parent.data_->fvk);
  Check(parent.Scan().Checkpoint().height <= Scan().Checkpoint().height);
  auto next = std::make_shared<Data>(*data_);
  next->scan = parent.data_->scan;
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
  next->operations = data_->operations.SetReady(id, auth);
  return OrchardAccountState(std::move(next));
}
OrchardAccountState OrchardAccountState::CancelReserved(const Hash &id) const {
  auto next = std::make_shared<Data>(*data_);
  next->operations = data_->operations.CancelReserved(id);
  return OrchardAccountState(std::move(next));
}
const OrchardWalletScanState &OrchardAccountState::Scan() const noexcept {
  return data_->scan;
}
const OrchardOperationQueue &OrchardAccountState::Operations() const noexcept {
  return data_->operations;
}
WalletStateBytes OrchardAccountState::Encode() const {
  Writer w;
  w.Raw(magic);
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
  return WalletStateBytes(w.bytes);
}
std::shared_ptr<OrchardAccountState::Data> OrchardAccountState::ReadMetadata(
    const WalletStateBytes &bytes, SigningDomain domain,
    const FullViewingKeyBytes &fvk, uint32_t activation, const uint256 &parent,
    std::span<const uint8_t> &scan) {
  auto state = std::make_shared<Data>(domain, fvk, activation, parent);
  Reader r{bytes.Bytes()};
  auto m = r.Raw(8);
  Check(std::equal(m.begin(), m.end(), magic.begin()));
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
  Check(r.bytes.empty());
  state->operations =
      OrchardOperationQueue::Restore(WalletStateBytes(operations), domain);
  return state;
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
  return OrchardAccountState(std::move(state));
}
OrchardAccountState OrchardAccountState::RestoreForRescan(
    const WalletStateBytes &bytes, SigningDomain domain,
    const FullViewingKeyBytes &fvk, uint32_t activation,
    const uint256 &parent) {
  std::span<const uint8_t> ignored;
  return OrchardAccountState(
      ReadMetadata(bytes, domain, fvk, activation, parent, ignored));
}
} // namespace dinero::wallet
