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
    if (!send_message_callback_) {
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
    send_message_callback_("", "inv", inv_payload);

    if (logger_) {
        logger_->info("[TxRelayManager] Announced tx: " + txid.GetHex().substr(0, 16) + "...");
    }
}

// ============================================================================
// Transaction Request Handling (Inbound)
// ============================================================================

void TxRelayManager::HandleInv(const std::string& peer_address, const uint256& txid) {
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
    if (!send_message_callback_) {
        if (logger_) {
            logger_->warning("[TxRelayManager] Cannot request tx: send callback not set");
        }
        return;
    }

    auto getdata_payload = SerializeGetData(txid);
    send_message_callback_(peer_address, "getdata", getdata_payload);

    if (logger_) {
        logger_->info("[TxRelayManager] Requested tx from " + peer_address);
    }
}

void TxRelayManager::HandleGetData(const std::string& peer_address, const uint256& txid) {
    if (logger_) {
        logger_->info("[TxRelayManager] Received getdata request from " + peer_address +
                     " for tx: " + txid.GetHex().substr(0, 16) + "...");
    }

    // Check if retrieve callback is set
    if (!retrieve_tx_callback_) {
        if (logger_) {
            logger_->warning("[TxRelayManager] Cannot retrieve tx: retrieve callback not set");
        }
        return;
    }

    // Check if send callback is set
    if (!send_message_callback_) {
        if (logger_) {
            logger_->warning("[TxRelayManager] Cannot send tx: send callback not set");
        }
        return;
    }

    // Retrieve transaction from mempool
    Transaction tx;
    if (!retrieve_tx_callback_(txid, tx)) {
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

    send_message_callback_(peer_address, "tx", tx_payload);

    if (logger_) {
        logger_->info("[TxRelayManager] Sent tx to " + peer_address +
                     ": " + txid.GetHex().substr(0, 16) + "...");
    }
}

void TxRelayManager::HandleTx(const std::string& peer_address, const Transaction& tx) {
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
    if (submit_tx_callback_) {
        auto result = submit_tx_callback_(tx, peer_address);

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
                    auto orphan_result = submit_tx_callback_(orphan_tx, "orphan-resolve");
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
    if (!validate_tx_callback_) {
        if (logger_) {
            logger_->error("[TxRelayManager] Cannot validate tx: callback not set");
        }
        return;
    }

    bool accepted = validate_tx_callback_(tx, peer_address);

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
    if (!csn_mode_ || !send_message_callback_) return;

    std::lock_guard<std::mutex> lock(refresh_mutex_);

    auto now = std::chrono::steady_clock::now();

    // Drop in-flight refreshes that are no longer stale or timed out.
    std::unordered_set<uint256> stale_set;
    stale_set.reserve(stale_txids.size());
    for (const auto& txid : stale_txids) {
        stale_set.insert(txid);
    }
    for (auto it = pending_refresh_.begin(); it != pending_refresh_.end();) {
        const bool no_longer_stale = stale_set.count(it->first) == 0;
        const bool timed_out = now - it->second >= REFRESH_REQUEST_TIMEOUT;
        if (no_longer_stale || timed_out) {
            it = pending_refresh_.erase(it);
        } else {
            ++it;
        }
    }

    if (stale_txids.empty()) return;
    if (now - last_refresh_batch_ < REFRESH_COOLDOWN) return;

    // Build target list: rotate across known bridge peers to avoid
    // concentrating refresh load on a single proof server.
    std::vector<std::string> targets;
    targets.reserve(bridge_capable_peers_.size());
    for (const auto& peer : bridge_capable_peers_) {
        targets.push_back(peer);
    }
    if (targets.empty()) {
        // Fallback: broadcast until we discover bridge-capable responders.
        targets.push_back("");
    }

    size_t sent = 0;
    for (const auto& txid : stale_txids) {
        if (sent >= batch_size) break;
        if (pending_refresh_.size() >= MAX_PENDING_REFRESH) break;
        if (pending_refresh_.count(txid) > 0) continue;

        const std::string& target = targets[refresh_rr_index_ % targets.size()];
        refresh_rr_index_++;

        auto payload = SerializeGetData(txid);
        send_message_callback_(target, "getdata", payload);
        pending_refresh_[txid] = now;
        sent++;
    }

    last_refresh_batch_ = now;

    if (sent > 0 && logger_) {
        logger_->info("[TxRelayManager] Requested proof refresh for " +
                     std::to_string(sent) + " stale TXs" +
                     (targets.size() == 1 && targets[0].empty()
                         ? " (broadcast)"
                         : " across " + std::to_string(targets.size()) + " bridge peer(s)") +
                     ", pending=" + std::to_string(pending_refresh_.size()));
    }
}

void TxRelayManager::OnTipChanged() {
    if (!csn_mode_) return;

    std::lock_guard<std::mutex> lock(refresh_mutex_);
    if (!pending_refresh_.empty() && logger_) {
        logger_->debug("[TxRelayManager] Tip change: cleared " +
                       std::to_string(pending_refresh_.size()) +
                       " pending proof-refresh requests");
    }
    pending_refresh_.clear();
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
    uint32_t type = csn_mode_ ? 0x50000001 : MSG_TX;
    for (int i = 0; i < 4; i++) {
        payload.push_back((type >> (i * 8)) & 0xFF);
    }

    // Transaction ID (32 bytes)
    payload.insert(payload.end(), txid.begin(), txid.end());

    return payload;
}

std::vector<uint8_t> TxRelayManager::SerializeGetData(const uint256& txid) const {
    // Phase #4: CSN mode sends MSG_UTREEXO_TX, full nodes send MSG_TX
    std::vector<uint8_t> payload;
    payload.push_back(1);  // Count = 1
    uint32_t type = csn_mode_ ? 0x50000001 : MSG_TX;  // MSG_UTREEXO_TX or MSG_TX
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
