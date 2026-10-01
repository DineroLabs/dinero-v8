#include "wallet/orchard_account_metadata.h"
#include <algorithm>
#include <openssl/sha.h>
namespace dinero::wallet {
namespace {
using namespace orchard;
using Observation = OrchardAccountMetadata::OperationObservation;
using Outcome = OrchardAccountMetadata::OperationOutcome;
[[noreturn]] void Fail() {
  throw std::runtime_error("Orchard wallet account snapshot rejected");
}
void Check(bool v) { if (!v) Fail(); }
constexpr std::array<uint8_t,8> magic{'D','N','O','R','A','C','0','5'};
Hash Identity(const FullViewingKeyBytes &fvk) {
  Hash h;
  SHA256(fvk.data(), fvk.size(), h.data());
  return h;
}
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
uint256 HashValue(const Hash &value) {
  uint256 h;
  std::copy(value.begin(), value.end(), h.begin());
  return h;
}
} // namespace
OrchardAccountMetadata OrchardAccountMetadata::Read(
    const WalletStateBytes &bytes, SigningDomain domain,
    const FullViewingKeyBytes &fvk, uint32_t activation) {
  std::span<const uint8_t> ignored;
  return Read(bytes, domain, fvk, activation, ignored);
}
OrchardAccountMetadata OrchardAccountMetadata::Read(
    const WalletStateBytes &bytes, SigningDomain domain,
    const FullViewingKeyBytes &fvk, uint32_t activation,
    std::span<const uint8_t> &scan) {
  OrchardAccountMetadata result(domain);
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
    std::copy(b.begin(), b.end(), result.next[s].begin());
    auto exhausted = r.Raw(1)[0];
    Check(exhausted <= 1);
    result.exhausted[s] = exhausted;
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
      result.observations.emplace(
          id, Observation{static_cast<Outcome>(outcome), height, block, txid});
    }
  }
  if (has_archive) {
    const uint64_t low = r.U32(), high = r.U32();
    result.archive.count = low | (high << 32);
    result.archive.head = r.Hash32();
    Check((result.archive.count == 0) == (result.archive.head == Hash{}));
  }
  if (has_delivery) {
    const uint64_t low = r.U32(), high = r.U32();
    result.delivery.sequence = low | (high << 32);
    result.delivery.digest = HashValue(r.Hash32());
    Check((result.delivery.sequence == 0) == result.delivery.digest.IsNull());
  }
  if (m[7] >= '6') {
    const uint64_t low = r.U32(), high = r.U32();
    result.parent_snapshot_revision = low | (high << 32);
    Check(result.parent_snapshot_revision && result.delivery.sequence);
  }
  Check(r.bytes.empty());
  result.operations =
      OrchardOperationQueue::Restore(WalletStateBytes(operations), domain);
  for (const auto &[id, observation] : result.observations) {
    const auto found = result.operations.Entries().find(id);
    Check(found != result.operations.Entries().end());
    if (observation.outcome == Outcome::Confirmed) {
      Check(
          found->second.phase == OrchardOperationQueue::Phase::Ready &&
          TransactionEnvelope::DecodeExact(found->second.transaction).Txid() ==
              observation.transaction_id);
    }
  }
  return result;
}
} // namespace dinero::wallet
