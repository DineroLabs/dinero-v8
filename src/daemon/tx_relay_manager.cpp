/**
 * Phase G.3: Mempool Relay - Implementation
 */

#include "daemon/tx_relay_manager.h"
#include "common/ilogger.h"
#include "primitives/transaction.h"
#include "primitives/hash_domains.h"  // Phase M.4.3-B: TxId type
#include "consensus/chainparams.h"    // Kill-switch and activation height
#include "mempool/tx_orphan_pool.h"   // Transaction orphan pool
#include <cstring>
#include <thread>
#include <type_traits>
#include <utility>

namespace dinero {

// Message type constants (Bitcoin-compatible)
static constexpr uint32_t MSG_TX = 1;

// ============================================================================
// Constructor
// ============================================================================

TxRelayManager::TxRelayManager(ILogger* logger)
    : logger_(logger)
    , send_message_callback_(nullptr)
    , validate_tx_callback_(nullptr)
    , retrieve_tx_callback_(nullptr) {

    if (logger_) {
        logger_->info("[TxRelayManager] Initialized (Phase G.3)");
    }
}

// ============================================================================
// Transaction Announcement (Outbound)
// ============================================================================

void TxRelayManager::AnnounceTx(const uint256& txid) {
    SendMessageCallback send;
    { std::lock_guard<std::mutex> lock(callback_mutex_); send = send_message_callback_; }

    if (!send) {
        if (logger_) {
            logger_->warning("[TxRelayManager] Cannot announce tx: send callback not set");
        }
        return;
    }

    // Mark as seen (prevent re-announcement)
    MarkTxAsSeen(txid);

    // Serialize inv message
    auto inv_payload = SerializeInv(txid);

    // Broadcast to all peers (empty peer_address = broadcast)
    send("", "inv", inv_payload);

    if (logger_) {
        logger_->info("[TxRelayManager] Announced tx: " + txid.GetHex().substr(0, 16) + "...");
    }
}

// ============================================================================
// Transaction Request Handling (Inbound)
// ============================================================================

void TxRelayManager::HandleInv(const std::string& peer_address, const uint256& txid) {
    SendMessageCallback send;
    { std::lock_guard<std::mutex> lock(callback_mutex_); send = send_message_callback_; }

    // Check if we already have this transaction
    if (IsTxSeen(txid)) {
        if (logger_) {
            logger_->debug("[TxRelayManager] Ignoring known tx inv from " +
                          peer_address + ": " + txid.GetHex().substr(0, 16) + "...");
        }
        return;
    }

    if (logger_) {
        logger_->info("[TxRelayManager] Received inv for unknown tx from " + peer_address +
                     ": " + txid.GetHex().substr(0, 16) + "...");
    }

    // Request the transaction
    if (!send) {
        if (logger_) {
            logger_->warning("[TxRelayManager] Cannot request tx: send callback not set");
        }
        return;
    }

    auto getdata_payload = SerializeGetData(txid, csn_mode_.load());
    send(peer_address, "getdata", getdata_payload);

    if (logger_) {
        logger_->info("[TxRelayManager] Requested tx from " + peer_address);
    }
}

void TxRelayManager::HandleGetData(const std::string& peer_address, const uint256& txid) {
    SendMessageCallback send;
    RetrieveTxCallback retrieve;
    { std::lock_guard<std::mutex> lock(callback_mutex_); send = send_message_callback_; retrieve = retrieve_tx_callback_; }

    if (logger_) {
        logger_->info("[TxRelayManager] Received getdata request from " + peer_address +
                     " for tx: " + txid.GetHex().substr(0, 16) + "...");
    }

    // Check if retrieve callback is set
    if (!retrieve) {
        if (logger_) {
            logger_->warning("[TxRelayManager] Cannot retrieve tx: retrieve callback not set");
        }
        return;
    }

    // Check if send callback is set
    if (!send) {
        if (logger_) {
            logger_->warning("[TxRelayManager] Cannot send tx: send callback not set");
        }
        return;
    }

    // Retrieve transaction from mempool
    Transaction tx;
    if (!retrieve(txid, tx)) {
        if (logger_) {
            logger_->warning("[TxRelayManager] Transaction not found in mempool: " +
                           txid.GetHex().substr(0, 16) + "...");
        }
        return;
    }

    // Serialize and send transaction to requesting peer
    auto tx_payload = SerializeTx(tx);
    if (tx_payload.empty()) {
        if (logger_) {
            logger_->error("[TxRelayManager] Failed to serialize tx: " +
                          txid.GetHex().substr(0, 16) + "...");
        }
        return;
    }

    send(peer_address, "tx", tx_payload);

    if (logger_) {
        logger_->info("[TxRelayManager] Sent tx to " + peer_address +
                     ": " + txid.GetHex().substr(0, 16) + "...");
    }
}

void TxRelayManager::HandleTx(const std::string& peer_address, const Transaction& tx) {
    SubmitTxCallback submit;
    ValidateTxCallback validate;
    { std::lock_guard<std::mutex> lock(callback_mutex_); submit = submit_tx_callback_; validate = validate_tx_callback_; }

    uint256 txid = tx.GetTxid().AsUint256();  // Network protocol boundary: TxId → uint256

    if (logger_) {
        logger_->info("[TxRelayManager] Received tx from " + peer_address +
                     ": " + txid.GetHex().substr(0, 16) + "...");
    }

    // Check if we already processed this transaction
    if (IsTxSeen(txid)) {
        if (logger_) {
            logger_->debug("[TxRelayManager] Ignoring duplicate tx: " +
                          txid.GetHex().substr(0, 16) + "...");
        }
        return;
    }

    // Check if already in orphan pool
    if (orphan_pool_ && orphan_pool_->hasOrphan(txid)) {
        if (logger_) {
            logger_->debug("[TxRelayManager] Already in orphan pool: " +
                          txid.GetHex().substr(0, 16) + "...");
        }
        return;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // CONFIDENTIAL TRANSACTION RELAY POLICY
    // ═══════════════════════════════════════════════════════════════════════════
    if (tx.HasConfidentialOutputs()) {
        if (dinero::Params().disable_confidential_transactions) {
            if (logger_) {
                logger_->warning("[TxRelayManager] Rejecting confidential tx from " + peer_address +
                               " (kill-switch engaged): " + txid.GetHex().substr(0, 16) + "...");
            }
            return;
        }

        if (dinero::Params().confidential_activation_height > 0) {
            if (logger_) {
                logger_->debug("[TxRelayManager] Relaying confidential tx from " + peer_address +
                              " (activation height: " +
                              std::to_string(dinero::Params().confidential_activation_height) + ")");
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // LAZY ORPHAN EXPIRY — run every 5 minutes during TX processing
    // ═══════════════════════════════════════════════════════════════════════════
    if (orphan_pool_) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_orphan_expiry_ > std::chrono::minutes(5)) {
            orphan_pool_->expireOldOrphans();
            last_orphan_expiry_ = now;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // STRUCTURED VALIDATION PATH (with orphan pool support)
    // ═══════════════════════════════════════════════════════════════════════════
    if (submit) {
        auto result = submit(tx, peer_address);

        if (result.accepted()) {
            MarkTxAsSeen(txid);

            if (logger_) {
                logger_->info("[TxRelayManager] Transaction accepted: " +
                             txid.GetHex().substr(0, 16) + "...");
            }

            // Resolve orphans: check if accepted TX is a parent of any orphans
            if (orphan_pool_) {
                auto resolved = orphan_pool_->getOrphansForParent(txid);
                for (const auto& orphan_tx : resolved) {
                    uint256 orphan_txid = orphan_tx.GetTxid().AsUint256();
                    auto orphan_result = submit(orphan_tx, "orphan-resolve");
                    if (orphan_result.accepted()) {
                        MarkTxAsSeen(orphan_txid);
                        AnnounceTx(orphan_txid);
                        if (logger_) {
                            logger_->info("[TxRelayManager] Resolved orphan " +
                                         orphan_txid.GetHex().substr(0, 16) + "...");
                        }
                    }
                    orphan_pool_->eraseOrphan(orphan_txid);
                }
            }

            // Announce accepted TX to peers
            AnnounceTx(txid);

        } else if (result.code == TxRejectCode::MISSING_INPUTS && orphan_pool_) {
            // Parent TX not yet known — add to orphan pool for later resolution
            bool added = orphan_pool_->addOrphan(tx, peer_address);
            if (added && logger_) {
                logger_->debug("[TxRelayManager] Added orphan " + txid.GetHex().substr(0, 16) +
                              "... (missing inputs, pool size: " +
                              std::to_string(orphan_pool_->size()) + ")");
            }
            // Do NOT mark as rejected — orphans are normal during propagation

        } else {
            if (logger_) {
                logger_->warning("[TxRelayManager] Transaction rejected: " +
                                txid.GetHex().substr(0, 16) + "... (" +
                                TxRejectCodeToString(result.code) + ": " + result.message + ")");
            }
        }
        return;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // LEGACY VALIDATION PATH (bool-only callback, no orphan pool)
    // ═══════════════════════════════════════════════════════════════════════════
    if (!validate) {
        if (logger_) {
            logger_->error("[TxRelayManager] Cannot validate tx: callback not set");
        }
        return;
    }

    bool accepted = validate(tx, peer_address);

    if (accepted) {
        MarkTxAsSeen(txid);

        if (logger_) {
            logger_->info("[TxRelayManager] Transaction accepted: " +
                         txid.GetHex().substr(0, 16) + "...");
        }
    } else {
        if (logger_) {
            logger_->warning("[TxRelayManager] Transaction rejected: " +
                            txid.GetHex().substr(0, 16) + "...");
        }
    }
}

// ============================================================================
// Proof Refresh (#6)
// ============================================================================

void TxRelayManager::RequestProofRefresh(const std::vector<uint256>& stale_txids, size_t batch_size) {
    SendMessageCallback send;
    { std::lock_guard<std::mutex> lock(callback_mutex_); send = send_message_callback_; }
    if (!csn_mode_.load() || !send) return;

    struct Request { uint256 txid; std::string target; std::vector<uint8_t> payload; };
    std::vector<Request> requests;
    auto batch = std::make_shared<const RefreshBatch>();
    size_t pending_count = 0;
    {
        std::lock_guard<std::mutex> lock(refresh_mutex_);
        const auto now = std::chrono::steady_clock::now();
        auto next = pending_refresh_;
        const std::unordered_set<uint256> stale_set(stale_txids.begin(), stale_txids.end());
        for (auto it = next.begin(); it != next.end();) {
            if (!stale_set.count(it->first) || now - it->second.started >= REFRESH_REQUEST_TIMEOUT)
                it = next.erase(it);
            else ++it;
        }
        size_t next_rr = refresh_rr_index_;
        auto next_time = last_refresh_batch_;
        if (!stale_txids.empty() && now - last_refresh_batch_ >= REFRESH_COOLDOWN) {
            std::vector<std::string> targets(bridge_capable_peers_.begin(), bridge_capable_peers_.end());
            if (targets.empty()) targets.emplace_back();
            for (const auto& txid : stale_txids) {
                if (requests.size() >= batch_size || next.size() >= MAX_PENDING_REFRESH) break;
                if (next.count(txid)) continue;
                requests.push_back({txid, targets[next_rr % targets.size()], SerializeGetData(txid, true)});
                ++next_rr;
                next.emplace(txid, PendingRefresh{now, batch});
            }
            next_time = now;
        }
        // All allocations/serialization complete before publishing reservations.
        static_assert(std::is_nothrow_swappable_v<decltype(pending_refresh_)>);
        pending_refresh_.swap(next);
        refresh_rr_index_ = next_rr;
        last_refresh_batch_ = next_time;
        pending_count = pending_refresh_.size();
    }
    size_t sent = 0;
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        {
            std::lock_guard<std::mutex> lock(refresh_mutex_);
            const auto it = pending_refresh_.find(request.txid);
            if (it == pending_refresh_.end() || it->second.batch != batch) continue;
        }
        // The owned reservation was current at the check. A concurrent tip may
        // change afterward; this request is not a proof-validity acknowledgment.
        try { send(request.target, "getdata", request.payload); ++sent; }
        catch (...) {
            std::lock_guard<std::mutex> lock(refresh_mutex_);
            // The attempted send is ambiguous. Retain its reservation, but
            // release unsent requests only if this batch still owns them.
            for (size_t j = i + 1; j < requests.size(); ++j) {
                const auto it = pending_refresh_.find(requests[j].txid);
                if (it != pending_refresh_.end() && it->second.batch == batch) pending_refresh_.erase(it);
            }
            throw;
        }
    }
    if (sent && logger_) logger_->info("[TxRelayManager] Requested proof refresh for " +
        std::to_string(sent) + " stale TXs, pending at preparation=" + std::to_string(pending_count));
}

struct TxRelayManager::PreparedTipUpdate::Impl {
    const std::thread::id thread = std::this_thread::get_id();
    TxRelayManager& relay;
    std::unique_lock<std::mutex> lock;
    decltype(pending_refresh_) discarded;
    bool clear, published = false;
    explicit Impl(TxRelayManager& owner)
        : relay(owner), lock(owner.refresh_mutex_), clear(owner.csn_mode_.load()) {}
    ~Impl() noexcept { if (thread != std::this_thread::get_id()) std::terminate(); }
};
TxRelayManager::PreparedTipUpdate::PreparedTipUpdate(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
TxRelayManager::PreparedTipUpdate::~PreparedTipUpdate() = default;
void TxRelayManager::PreparedTipUpdate::PublishAfterCommit() noexcept {
    if (impl_->thread != std::this_thread::get_id() || impl_->published) std::terminate();
    static_assert(std::is_nothrow_swappable_v<decltype(impl_->discarded)>);
    if (impl_->clear) impl_->discarded.swap(impl_->relay.pending_refresh_);
    impl_->published = true;
    impl_->lock.unlock();
}
std::unique_ptr<TxRelayManager::PreparedTipUpdate> TxRelayManager::PrepareTipChanged() {
    auto impl = std::make_unique<PreparedTipUpdate::Impl>(*this);
    return std::unique_ptr<PreparedTipUpdate>(new PreparedTipUpdate(std::move(impl)));
}
void TxRelayManager::OnTipChanged() {
    auto prepared = PrepareTipChanged();
    prepared->PublishAfterCommit();
}

void TxRelayManager::RecordBridgeResponse(const std::string& peer_addr) {
    std::lock_guard<std::mutex> lock(refresh_mutex_);
    bridge_capable_peers_.insert(peer_addr);
}

void TxRelayManager::CompleteRefresh(const uint256& txid) {
    std::lock_guard<std::mutex> lock(refresh_mutex_);
    pending_refresh_.erase(txid);
}

// ============================================================================
// State Queries
// ============================================================================

bool TxRelayManager::IsTxSeen(const uint256& txid) const {
    std::lock_guard<std::mutex> lock(seen_txs_mutex_);
    return seen_txs_.count(txid) > 0;
}

size_t TxRelayManager::GetSeenTxCount() const {
    std::lock_guard<std::mutex> lock(seen_txs_mutex_);
    return seen_txs_.size();
}

// ============================================================================
// Private Helpers
// ============================================================================

void TxRelayManager::MarkTxAsSeen(const uint256& txid) {
    std::lock_guard<std::mutex> lock(seen_txs_mutex_);
    seen_txs_.insert(txid);
}

std::vector<uint8_t> TxRelayManager::SerializeInv(const uint256& txid) const {
    std::vector<uint8_t> payload;

    // Count (1 inventory item)
    payload.push_back(1);

    // Type: MSG_UTREEXO_TX in CSN mode, MSG_TX otherwise
    uint32_t type = csn_mode_.load() ? 0x50000001 : MSG_TX;
    for (int i = 0; i < 4; i++) {
        payload.push_back((type >> (i * 8)) & 0xFF);
    }

    // Transaction ID (32 bytes)
    payload.insert(payload.end(), txid.begin(), txid.end());

    return payload;
}

std::vector<uint8_t> TxRelayManager::SerializeGetData(const uint256& txid, bool csn) const {
    // Phase #4: CSN mode sends MSG_UTREEXO_TX, full nodes send MSG_TX
    std::vector<uint8_t> payload;
    payload.push_back(1);  // Count = 1
    uint32_t type = csn ? 0x50000001 : MSG_TX;  // MSG_UTREEXO_TX or MSG_TX
    for (int i = 0; i < 4; i++) {
        payload.push_back((type >> (i * 8)) & 0xFF);
    }
    payload.insert(payload.end(), txid.begin(), txid.end());
    return payload;
}

std::vector<uint8_t> TxRelayManager::SerializeTx(const Transaction& tx) const {
    // Serialize transaction using Transaction::Serialize()
    try {
        std::vector<uint8_t> payload = tx.Serialize();

        if (logger_) {
            logger_->debug("[TxRelayManager] Serialized tx: " +
                          std::to_string(payload.size()) + " bytes");
        }

        return payload;
    } catch (const std::exception& e) {
        if (logger_) {
            logger_->error("[TxRelayManager] Transaction serialization failed: " +
                          std::string(e.what()));
        }
        return std::vector<uint8_t>();  // Return empty on error
    }
}

} // namespace dinero
