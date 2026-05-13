/**
 * Stub implementations for daemon Mempool dependencies.
 *
 * The proof-staleness test only exercises addUnchecked, onBlockConnected,
 * onBlockDisconnected, refreshProof, getStaleCount, getStaleTxIds, getStats,
 * and size().  Everything else in mempool.cpp is dead code for this test,
 * but the linker still needs the symbols.  Provide no-op stubs here.
 */

#include "privacy/silent_scanner_manager.h"
#include "mempool/fee_estimator.h"
#include "mempool/coins_view_mempool.h"
#include "mempool/mempool_persistence.h"
// "daemon/validation_confidential.h" was removed from the codebase;
// no symbols from it are referenced below, so the include is gone.
#include "mining/ct_selection_policy.h"
#include "primitives/transaction.h"
#include "policy/rbf_policy.h"

namespace dinero {
class NetworkManager {
public:
    void relayTransaction(const Transaction&);
};
}

// ── ScannerManager ──────────────────────────────────────────────────────────
namespace din::sp {
ScannerManager::ScannerManager() {}
ScannerManager::~ScannerManager() {}
void ScannerManager::scanMempoolTransaction(const std::string&) {}
}

// ── FeeEstimator ────────────────────────────────────────────────────────────
namespace dinero {
FeeEstimator::FeeEstimator() {}
void FeeEstimator::recordTxEntry(const uint256&, double, uint32_t) {}
void FeeEstimator::recordTxConfirmation(const uint256&, uint32_t) {}
}

// ── Legacy network relay shim ───────────────────────────────────────────────
namespace dinero {
void NetworkManager::relayTransaction(const Transaction&) {}
}

// ── CoinsViewMemPool ────────────────────────────────────────────────────────
namespace dinero {
CoinsViewMemPool::CoinsViewMemPool(const consensus::ChainStateView* base)
    : base_(base) {}
void CoinsViewMemPool::clear() {}
void CoinsViewMemPool::addCoin(const OutPoint&, const consensus::UTXOEntry&) {}
void CoinsViewMemPool::spendCoin(const OutPoint&) {}
StatusOr<consensus::UTXOEntry> CoinsViewMemPool::getCoin(const OutPoint&) const {
    return StatusOr<consensus::UTXOEntry>(Status::NotFound);
}
bool CoinsViewMemPool::hasCommitment(const std::vector<uint8_t>&) const {
    return false;
}
}

// ── CTSelectionPolicy ───────────────────────────────────────────────────────
namespace dinero::mining {
CTSelectionPolicy::CTSelectionPolicy(const CTSelectionConfig& config)
    : config_(config) {}

bool CTSelectionPolicy::HasConfidentialOutputs(const Transaction& tx) const {
    return tx.HasConfidentialOutputs();
}

CTWeightInfo CTSelectionPolicy::GetWeightInfo(const Transaction& tx) const {
    CTWeightInfo info{};
    info.base_weight = 0;
    info.proof_weight = 0;
    info.total_weight = 0;
    info.proof_bytes = 0;
    info.confidential_outputs = tx.HasConfidentialOutputs() ? 1 : 0;
    return info;
}
}

// ── MempoolPersistence ──────────────────────────────────────────────────────
namespace dinero {
bool MempoolPersistence::save(const std::vector<MempoolEntry>&, const std::string&) {
    return false;
}
std::vector<MempoolPersistence::PersistedEntry> MempoolPersistence::load(const std::string&) {
    return {};
}
}

// ── ConfidentialValidator stub retired ──────────────────────────────────────
// The ConfidentialValidator class was removed from the codebase when
// confidential-transaction validation moved into the shielded pipeline
// (consensus/shielded/*). No production code references it; the stub
// definition has been deleted along with its header include.

// ── TransactionSerializer ───────────────────────────────────────────────────
namespace dinero {
bool TransactionSerializer::Deserialize(Transaction&, const std::vector<uint8_t>&) {
    return false;
}
}

// ── RBFPolicy ───────────────────────────────────────────────────────────────
namespace dinero::policy {
RBFPolicy::RBFPolicy(const Config&) {}
bool RBFPolicy::isRBFSignaled(const Transaction&) const { return false; }
RBFConflictSet RBFPolicy::buildConflictSet(
    const Transaction&, const std::vector<MempoolEntry>&, const mining::CTSelectionConfig&) {
    return {};
}
RBFValidationResult RBFPolicy::validateReplacement(
    const Transaction&, uint64_t, const RBFConflictSet&,
    const mining::CTSelectionConfig&, const std::unordered_set<uint256>&,
    const std::vector<MempoolEntry>&, std::string&) const {
    return RBFValidationResult::NOT_SIGNALED;
}
}

// ── Bulletproofs FFI ────────────────────────────────────────────────────────
extern "C" {
int bp_init(void) { return 0; }
int bp_verify_batch(const uint8_t**, const uint8_t**, const size_t*, size_t) { return -1; }
}
