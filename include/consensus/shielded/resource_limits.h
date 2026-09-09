#pragma once

#include "consensus/limits.h"
#include "consensus/shielded/shielded_serialization.h"
#include "primitives/transaction.h"
#include <cstdint>
#include <string>

namespace dinero::consensus::shielded {

// Recipient-authority resource profile. Activated by the same dormant height
// as spend proof 0x06; never change historical block validity. See the resource
// profile spec for measured shapes and the separately bounded block work.
constexpr size_t kAuthMaxTxBytes = 512'000;
constexpr size_t kAuthMaxTxWeight = 4 * kAuthMaxTxBytes;
constexpr size_t kAuthMaxSpends = 4;
constexpr size_t kAuthMaxOutputs = 2;
// The largest supported transaction uses six proofs. Eight allows limited
// packing without multiplying its measured verification cost across a block.
constexpr size_t kAuthMaxBlockProofs = 8;
// Explicit consensus bound for this profile. Do not infer it from the miner's
// 1 MB template preference: the block wire decoder accepts up to 4 MB.
constexpr size_t kAuthMaxBlockShieldedBytes = 1'000'000;
constexpr size_t kAuthMaxPackageBytes = 600'000;
constexpr size_t kLegacyMaxPackageBytes = 101 * 1024;
constexpr size_t kMaxPackageTransactions = 25;
inline size_t PackageByteLimit(bool auth_active, bool contains_shielded) {
    return auth_active && contains_shielded ? kAuthMaxPackageBytes : kLegacyMaxPackageBytes;
}

inline bool AuthResourcesActive(uint64_t height, uint32_t activation) {
    return activation != UINT32_MAX && height >= activation;
}
inline bool HasShieldedResources(const Transaction& tx) {
    return Transaction::IsShieldedVersion(tx.version) && !tx.shielded_bundle_bytes.empty();
}
// Pre-deserialization allocation ceiling. A version claim only buys bounded
// parsing; a canonical bundle and the contextual gate are still mandatory.
inline size_t WireTxByteLimit(const std::vector<uint8_t>& raw) {
    if (raw.size() < 4) return MAX_TX_SIZE;
    const uint32_t version = uint32_t(raw[0]) | (uint32_t(raw[1]) << 8) |
        (uint32_t(raw[2]) << 16) | (uint32_t(raw[3]) << 24);
    return version == static_cast<uint32_t>(Transaction::TX_VERSION_SHIELDED_V2)
        ? kAuthMaxTxBytes : MAX_TX_SIZE;
}
inline size_t TxByteLimit(const Transaction& tx, bool auth_active) {
    return auth_active && tx.version == Transaction::TX_VERSION_SHIELDED_V2 &&
            HasShieldedResources(tx)
        ? kAuthMaxTxBytes : MAX_TX_SIZE;
}
inline size_t TxWeightLimit(const Transaction& tx, bool auth_active) {
    return auth_active && tx.version == Transaction::TX_VERSION_SHIELDED_V2 &&
            HasShieldedResources(tx)
        ? kAuthMaxTxWeight : MAX_TX_WEIGHT;
}
inline bool CheckTxResourceEnvelope(const Transaction& tx, bool auth_active, std::string& error) {
    if (tx.GetSize() > TxByteLimit(tx, auth_active)) {
        error = "transaction-size-limit-exceeded";
        return false;
    }
    if (tx.GetWeight() == 0 || tx.GetWeight() > TxWeightLimit(tx, auth_active)) {
        error = "transaction-weight-limit-exceeded";
        return false;
    }
    return true;
}
inline bool CheckAuthBundleCounts(size_t spends, size_t outputs) {
    return spends <= kAuthMaxSpends && outputs <= kAuthMaxOutputs;
}
// Cheap and contextual: runs before any proof verification or state mutation.
// Auth height is supplied by the caller, never inferred from a proof/version.
inline bool CheckAuthTransactionResources(const Transaction& tx, uint64_t height,
                                         uint32_t activation, size_t& proofs,
                                         std::string& error) {
    proofs = 0;
    if (!AuthResourcesActive(height, activation) || !HasShieldedResources(tx)) return true;
    // Auth bundles must commit to their bytes through txid. Legacy v5 carries
    // the bundle only in witness serialization, so granting it the larger
    // resource profile would preserve its mempool-identity ambiguity.
    if (tx.version != Transaction::TX_VERSION_SHIELDED_V2) {
        error = "shielded-auth-requires-tx-v6";
        return false;
    }
    if (!CheckTxResourceEnvelope(tx, true, error)) return false;
    ShieldedBundle bundle;
    if (DeserializeShieldedBundle(tx.shielded_bundle_bytes, &bundle) != BundleDecodeError::Ok) {
        error = "shielded-bundle-malformed"; return false;
    }
    if (!CheckAuthBundleCounts(bundle.spends.size(), bundle.outputs.size())) {
        error = "shielded-bundle-resource-limit"; return false;
    }
    proofs = bundle.spends.size() + bundle.outputs.size();
    return true;
}
struct AuthBlockResourceUsage {
    size_t proofs = 0;
    size_t shielded_bytes = 0;
};
// Transactional accumulator shared by template selection and block validation:
// false leaves usage unchanged, so a rejected package cannot partially consume
// or (more dangerously) escape the aggregate block budget.
inline bool AccumulateAuthBlockResources(const Transaction& tx, uint64_t height,
                                         uint32_t activation,
                                         AuthBlockResourceUsage& usage,
                                         std::string& error) {
    size_t proofs = 0;
    if (!CheckAuthTransactionResources(tx, height, activation, proofs, error)) {
        return false;
    }
    if (!AuthResourcesActive(height, activation)) return true;
    if (usage.proofs > kAuthMaxBlockProofs ||
        proofs > kAuthMaxBlockProofs - usage.proofs) {
        error = "shielded-block-proof-limit";
        return false;
    }
    size_t bytes = 0;
    if (HasShieldedResources(tx)) bytes = tx.GetSize();
    if (usage.shielded_bytes > kAuthMaxBlockShieldedBytes ||
        bytes > kAuthMaxBlockShieldedBytes - usage.shielded_bytes) {
        error = "shielded-block-byte-limit";
        return false;
    }
    usage.proofs += proofs;
    usage.shielded_bytes += bytes;
    return true;
}
template <typename Transactions>
inline bool CheckAuthBlockResources(const Transactions& transactions, uint64_t height,
                                   uint32_t activation, std::string& error) {
    if (!AuthResourcesActive(height, activation)) return true;
    AuthBlockResourceUsage usage;
    for (const auto& tx : transactions) {
        if (!AccumulateAuthBlockResources(tx, height, activation, usage, error))
            return false;
    }
    return true;
}
static_assert(kAuthMaxTxBytes < MAX_BLOCK_SIZE);
static_assert(kAuthMaxTxWeight < MAX_BLOCK_WEIGHT);
static_assert(kAuthMaxPackageBytes >= kAuthMaxTxBytes);
} // namespace dinero::consensus::shielded
