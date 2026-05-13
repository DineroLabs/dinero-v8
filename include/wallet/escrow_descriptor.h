#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dinero {

/**
 * Escrow Descriptor — identifies an escrow trade and its parameters.
 *
 * Dual trade_id scheme:
 * - pending_trade_id: deterministic from nonce + buyer + seller + amount
 *   (before funding tx is known)
 * - finalized_trade_id: deterministic from funding txid:vout
 *   (after on-chain funding)
 */
struct EscrowDescriptor {
    std::vector<uint8_t> buyer_pubkey;                  // 32-byte x-only
    std::vector<uint8_t> seller_pubkey;                 // 32-byte x-only
    std::vector<std::vector<uint8_t>> attestor_pubkeys; // 32-byte x-only each
    uint8_t attestor_threshold;                          // k-of-n
    uint32_t timeout_blocks;                             // CSV for buyer refund
    uint64_t escrow_amount;                              // Amount in una

    /**
     * Compute pending trade ID (before funding).
     * pending_trade_id = TaggedHash("dinero/trade/pending/v1",
     *     nonce || buyer || seller || amount_le)
     */
    static std::array<uint8_t, 32> ComputePendingTradeId(
        const std::vector<uint8_t>& nonce,
        const std::vector<uint8_t>& buyer_pubkey,
        const std::vector<uint8_t>& seller_pubkey,
        uint64_t amount
    );

    /**
     * Compute finalized trade ID (after funding tx confirmed).
     * finalized_trade_id = TaggedHash("dinero/trade/final/v1",
     *     funding_txid || funding_vout_le)
     */
    std::array<uint8_t, 32> ComputeFinalizedTradeId(
        const std::array<uint8_t, 32>& funding_txid,
        uint32_t funding_vout
    ) const;

    std::vector<uint8_t> Serialize() const;
    static EscrowDescriptor Deserialize(const std::vector<uint8_t>& data);
};

} // namespace dinero
