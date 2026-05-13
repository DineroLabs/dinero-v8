#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace dinero {

/**
 * Attestor Receipt — a Schnorr signature from one attestor over
 * (trade_id || outcome_hash).
 *
 * outcome_hash = SHA256("release") or SHA256("refund")
 */
struct AttestorReceipt {
    std::vector<uint8_t> attestor_pubkey;   // 32-byte x-only
    std::vector<uint8_t> signature;         // 64-byte Schnorr signature

    std::vector<uint8_t> Serialize() const;
    static AttestorReceipt Deserialize(const std::vector<uint8_t>& data);
};

/**
 * Receipt Bundle — collection of attestor receipts for an escrow trade.
 *
 * To release funds, the seller collects enough attestor receipts
 * to meet the threshold, then uses them alongside their own signature
 * in the release leaf script-path spend.
 */
struct ReceiptBundle {
    std::array<uint8_t, 32> trade_id;        // Finalized trade ID
    std::vector<AttestorReceipt> receipts;

    /**
     * Check if we have at least k valid receipts.
     * Does NOT verify signatures (use VerifyReceipts for that).
     */
    bool HasThreshold(uint8_t k) const {
        return receipts.size() >= k;
    }

    /**
     * Verify all receipt signatures against trade_id + outcome_hash.
     * @param outcome_hash 32-byte hash of outcome (e.g., SHA256("release"))
     * @return true if all signatures verify
     */
    bool VerifyReceipts(const std::array<uint8_t, 32>& outcome_hash) const;

    std::vector<uint8_t> Serialize() const;
    static ReceiptBundle Deserialize(const std::vector<uint8_t>& data);
};

} // namespace dinero
