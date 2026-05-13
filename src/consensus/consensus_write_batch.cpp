// ConsensusWriteBatch — phase 3a scaffold (skeleton implementation).
//
// See include/consensus/consensus_write_batch.h and
// docs/specs/atomic_consensus_persistence_phase3.md.
//
// Skeleton means:
//   - Construction / destruction with the D4 contract wired
//   - Stage* methods accepted as no-ops
//   - Commit() / Abort() state-machine transitions wired
//   - IsEnabled() reads the hidden config flag
// No live container is touched yet; subsequent commits add the
// actual rocksdb::WriteBatch + journal-row logic from §3.1 one
// container at a time.

#include "consensus/consensus_write_batch.h"

#include "consensus/interfaces/iconsensus_utxo_set.h"
#include "daemon/config.h"
#include "daemon/services/chainstate_service.h"
#include "storage/chain_db.h"

#include <rocksdb/write_batch.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>

namespace dinero {
namespace consensus {

ConsensusWriteBatch::ConsensusWriteBatch(ChainstateService& chainstate,
                                         uint32_t block_height,
                                         const uint256& block_hash)
    : chainstate_(chainstate)
    , block_height_(block_height)
    , block_hash_(block_hash)
    , state_(State::Open) {}

ConsensusWriteBatch::~ConsensusWriteBatch() {
    if (state_ == State::Open) {
        // D4 contract: a batch destructed without an explicit
        // Commit() or Abort() is a leak — and a leaked batch IS the
        // "third state" §1's law forbids. Loudly fail per build mode.
        const std::string reason =
            "consensus_write_batch_dropped (block_height=" +
            std::to_string(block_height_) +
            " block_hash=" + block_hash_.GetHex().substr(0, 16) + "…)";
#ifdef NDEBUG
        // Release / mainnet: enter consensus safe mode and refuse
        // block connect / template generation. Operator must call
        // safemode.exit { confirm: true } after inspecting the
        // chain state.
        std::cerr << "[ConsensusWriteBatch] FATAL (release): "
                  << reason << " — entering safe mode" << std::endl;
        chainstate_.EnterSafeMode(reason);
#else
        // Debug / regtest / test: hard abort. The whole point of
        // phase 3 is "no third state"; a test that leaks the batch
        // must fail loudly.
        std::cerr << "[ConsensusWriteBatch] FATAL (debug): "
                  << reason << " — abort()" << std::endl;
        std::abort();
#endif
    }
}

// ── Phase 3a staging surface ────────────────────────────────────────
//
// Each Stage* method appends to staged_utxo_ops_ in call order. No
// live container is touched here; Commit() replays the staged ops
// against the consensus UTXO set as a single block-scoped step.
// Order matters: ConnectBlock today applies all PASS-2 additions
// before all PASS-1 spends (deferred-spend pattern), and intra-block
// ephemeral outputs are added then spent within the same block.
// Replaying in stage order preserves both invariants.

void ConsensusWriteBatch::StageUTXOAddition(const OutPoint& outpoint,
                                            const UTXOEntry& entry) {
    staged_utxo_ops_.emplace_back(StagedUTXOAddition{outpoint, entry});
}

void ConsensusWriteBatch::StageUTXOSpend(const OutPoint& outpoint) {
    staged_utxo_ops_.emplace_back(StagedUTXOSpend{outpoint});
}

// ── Commit / Abort ──────────────────────────────────────────────────

Status ConsensusWriteBatch::CommitStagedLiveState() {
    // Phase 3b step 3 part 3 (split API): replay-only phase. Pushes
    // staged in-memory ops onto the live consensus UTXO set, but
    // does NOT write the journal row. Callers that have an outer
    // rocksdb::WriteBatch follow this with AttachJournalRowToBatch
    // + (after committing the outer batch) MarkCommittedAfterOuterBatch.
    // The legacy single-call Commit() runs this internally then
    // writes the journal row standalone.
    //
    // SCAFFOLD-ONLY error semantic: AddCoin/SpendCoin failures are
    // LOGGED but do NOT propagate as a non-Ok Status. The legacy
    // ConnectTip persist path still runs alongside this replay; the
    // scaffold's job is the call-site routing, not yet §1.3's
    // rollback rule. Phase 3b's working-copy pattern (stage to a
    // clone, swap on Commit) promotes this branch to fail-loud.
    auto* utxo_set = chainstate_.GetConsensusUTXOSet();
    if (utxo_set != nullptr) {
        for (const auto& op : staged_utxo_ops_) {
            if (const auto* add = std::get_if<StagedUTXOAddition>(&op)) {
                if (!utxo_set->AddCoin(add->outpoint, add->entry)) {
                    if (!utxo_set->HaveCoin(add->outpoint)) {
                        std::cerr << "[ConsensusWriteBatch] AddCoin failed at commit (scaffold; "
                                  << "phase 3b will fail-loud here): "
                                  << add->outpoint.ToString() << std::endl;
                    }
                }
            } else if (const auto* spend = std::get_if<StagedUTXOSpend>(&op)) {
                if (!utxo_set->SpendCoin(spend->outpoint)) {
                    std::cerr << "[ConsensusWriteBatch] SpendCoin failed at commit (scaffold; "
                              << "phase 3b will fail-loud here): "
                              << spend->outpoint.ToString() << std::endl;
                }
            }
        }
    }
    return Status::Ok;
}

void ConsensusWriteBatch::MarkCommittedAfterOuterBatch() noexcept {
    // Caller folded the journal row into an outer WriteBatch and
    // committed it externally. State machine transitions to
    // Committed so the destructor doesn't trip the D4 leak panic.
    // The journal row was already attached via AttachJournalRowToBatch;
    // no further write needed here.
    state_ = State::Committed;
}

Status ConsensusWriteBatch::Commit() {
    // Single-call shorthand: replay staged live state, then write
    // the journal row standalone via putUtreexoMeta. Used when no
    // outer rocksdb::WriteBatch is available to fold the row into.
    //
    // Callers with an outer batch should instead use the split form:
    //   CommitStagedLiveState();
    //   AttachJournalRowToBatch(&outer_wb);
    //   ... commit outer_wb ...
    //   MarkCommittedAfterOuterBatch();
    //
    // If AttachJournalRowToBatch was already called (journal row
    // is in someone else's batch), Commit() must NOT also write it
    // standalone — the row would land twice. The
    // journal_row_attached_ flag guards this case for safety, but
    // a correctly-ordered caller should not be calling Commit()
    // after attachment in the first place.
    const auto live_status = CommitStagedLiveState();
    if (live_status != Status::Ok) {
        return live_status;
    }

    if (consensus::ConsensusWriteBatch::IsEnabled(chainstate_) &&
        !journal_row_attached_) {
        char height_be_hex[9];
        std::snprintf(height_be_hex, sizeof(height_be_hex), "%08x", block_height_);
        const std::string journal_key =
            std::string("consensus_journal:") + height_be_hex + ":" +
            block_hash_.GetHex();
        const auto state_hash = chainstate_.ComputeShieldedReorgStateHash();
        const std::string journal_value = state_hash.GetHex();

        if (auto* cdb = chainstate_.GetChainDB()) {
            ChainWriteToken token = ChainWriteToken::CreateForTesting();
            const auto status =
                cdb->putUtreexoMeta(token, journal_key, journal_value);
            if (status != Status::Ok) {
                std::cerr << "[ConsensusWriteBatch] journal row standalone write failed: "
                          << journal_key << std::endl;
            }
        }
    }

    state_ = State::Committed;
    return Status::Ok;
}

void ConsensusWriteBatch::Abort() noexcept {
    // Skeleton: drop all staged ops (none yet) and mark aborted so
    // the destructor runs cleanly.
    state_ = State::Aborted;
}

// ── Phase 3b step 3 part 3 — atomic journal-row attachment ──────────

bool ConsensusWriteBatch::AttachJournalRowToBatch(void* rocksdb_write_batch) {
    if (rocksdb_write_batch == nullptr) {
        return false;
    }
    if (!ConsensusWriteBatch::IsEnabled(chainstate_)) {
        return false;
    }
    auto* cdb = chainstate_.GetChainDB();
    if (cdb == nullptr) {
        return false;
    }

    // Same key/value shape as Commit()'s standalone path. The
    // difference is that the write goes into the caller's outer
    // WriteBatch instead of a separate putUtreexoMeta call —
    // pairing the journal row with the UTXO/txindex writes that
    // share that batch.
    char height_be_hex[9];
    std::snprintf(height_be_hex, sizeof(height_be_hex), "%08x", block_height_);
    const std::string journal_key =
        std::string("consensus_journal:") + height_be_hex + ":" +
        block_hash_.GetHex();
    const auto state_hash = chainstate_.ComputeShieldedReorgStateHash();
    const std::string journal_value = state_hash.GetHex();

    auto* wb = static_cast<rocksdb::WriteBatch*>(rocksdb_write_batch);
    ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const auto status = cdb->putUtreexoMeta(token, journal_key, journal_value, wb);
    if (status == Status::Ok) {
        journal_row_attached_ = true;
        return true;
    }
    return false;
}

// ── Activation helper ───────────────────────────────────────────────

bool ConsensusWriteBatch::IsEnabled(const ChainstateService& /*chainstate*/) {
    // Single source of truth for "is the atomic-persist flag on?".
    // ChainstateService::ConnectTip / DisconnectTip will guard their
    // call sites with this in the next commit.
    return GetConfig().consensus_atomic_persist;
}

}  // namespace consensus
}  // namespace dinero
