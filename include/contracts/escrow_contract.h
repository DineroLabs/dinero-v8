#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace dinero {
namespace contracts {

/**
 * EscrowContract - Bitcoin-style P2SH escrow smart contract
 *
 * Contract Logic:
 * ═══════════════════════════════════════════════════════════════
 * IF (release path):
 *     2 <PK_Buyer> <PK_Seller> <PK_Mediator> 3 OP_CHECKMULTISIG
 *     → Requires 2-of-3 signatures (normal release or dispute)
 *
 * ELSE (refund path):
 *     <REFUND_TIME> OP_CHECKLOCKTIMEVERIFY OP_DROP
 *     <PK_Buyer> OP_CHECKSIG
 *     → After timeout, buyer can reclaim funds alone
 * ═══════════════════════════════════════════════════════════════
 *
 * Features:
 * - Trustless: No central authority needed
 * - Time-locked: Automatic refund protection
 * - Dispute resolution: Optional mediator
 * - On-chain: Enforced by consensus rules
 */

struct EscrowKeys {
    std::string buyer_pubkey;      // Buyer's public key (hex)
    std::string seller_pubkey;     // Seller's public key (hex)
    std::string mediator_pubkey;   // Mediator's public key (hex)
};

enum class EscrowType {
    TwoOfTwo,       // Simple buyer+seller (no mediator)
    TwoOfThreeAuto, // Buyer+Seller+DaemonMediator (time-based auto-resolution)
    TwoOfThreeManual // Buyer+Seller+CustomMediator (human mediator)
};

struct EscrowContract {
    std::string contract_id;        // Unique contract identifier
    EscrowType type;                // Contract type (2of2, 2of3_auto, 2of3_manual)
    EscrowKeys keys;                // All public keys
    double amount;                  // DIN amount locked
    uint32_t refund_time;          // Block height or UNIX timestamp for refund
    uint32_t seller_window_blocks; // Blocks until daemon favors seller (for auto type)
    std::string redeem_script;      // Full redeem script (hex)
    std::string script_hash;        // Hash of redeem script
    std::string p2sh_address;       // P2SH escrow address (din1q...)
    std::string lock_txid;          // Transaction ID that locked funds
    uint32_t lock_vout;             // Output index (vout) in lock transaction
    uint64_t created_at;            // Creation timestamp
    uint32_t created_height;        // Block height when created
    std::string status;             // "pending", "locked", "released", "refunded", "expired"
    int confirmations;              // Number of confirmations on lock tx
};

/**
 * EscrowContractBuilder - Creates Bitcoin-style escrow smart contracts
 *
 * Usage:
 * 1. Build redeem script with 2-of-3 multisig + timelock refund
 * 2. Hash script to create P2SH address
 * 3. Buyer sends funds to P2SH address
 * 4. Contract enforces rules automatically on-chain
 */
class EscrowContractBuilder {
public:
    /**
     * Build complete escrow contract
     *
     * @param keys Buyer, seller, and mediator public keys
     * @param amount DIN amount to lock
     * @param refund_blocks Number of blocks until refund (e.g., 2880 ≈ 6 days)
     * @param type Contract type (TwoOfTwo, TwoOfThreeAuto, TwoOfThreeManual)
     * @param seller_window_blocks Blocks until daemon favors seller (for auto type)
     * @param current_height Current blockchain height
     * @return Complete escrow contract with P2SH address
     */
    static EscrowContract buildContract(
        const EscrowKeys& keys,
        double amount,
        uint32_t refund_blocks,
        EscrowType type = EscrowType::TwoOfThreeManual,
        uint32_t seller_window_blocks = 6,
        uint32_t current_height = 0
    );

    /**
     * Generate redeem script for 2-of-3 multisig with timelock refund
     *
     * Script structure:
     * OP_IF
     *     2 <PK_B> <PK_S> <PK_M> 3 OP_CHECKMULTISIG
     * OP_ELSE
     *     <REFUND_TIME> OP_CHECKLOCKTIMEVERIFY OP_DROP <PK_B> OP_CHECKSIG
     * OP_ENDIF
     *
     * @param keys Public keys for all parties
     * @param refund_time Block height or timestamp for refund
     * @return Redeem script (hex)
     */
    static std::string buildRedeemScript(
        const EscrowKeys& keys,
        uint32_t refund_time
    );

    /**
     * Hash redeem script to create script hash for P2SH
     *
     * @param redeem_script Redeem script (hex)
     * @return Script hash (hex)
     */
    static std::string hashRedeemScript(const std::string& redeem_script);

    /**
     * Create P2SH address from script hash
     *
     * @param script_hash Script hash (hex)
     * @return P2SH address (din1q...)
     */
    static std::string createP2SHAddress(const std::string& script_hash);

    /**
     * Create transaction to lock funds in escrow
     *
     * @param contract Escrow contract
     * @param from_address Buyer's address
     * @return Transaction hex (ready to broadcast)
     */
    static std::string createLockTransaction(
        const EscrowContract& contract,
        const std::string& from_address
    );

    /**
     * Create release transaction (2-of-3 multisig path)
     *
     * @param contract Escrow contract
     * @param to_address Seller's destination address
     * @param sig_buyer Buyer's signature (hex)
     * @param sig_seller Seller's signature (hex)
     * @return Transaction hex (ready to broadcast)
     */
    static std::string createReleaseTransaction(
        const EscrowContract& contract,
        const std::string& to_address,
        const std::string& sig_buyer,
        const std::string& sig_seller
    );

    /**
     * Create refund transaction (timelock path)
     *
     * @param contract Escrow contract
     * @param refund_address Buyer's refund address
     * @param sig_buyer Buyer's signature (hex)
     * @return Transaction hex (ready to broadcast)
     */
    static std::string createRefundTransaction(
        const EscrowContract& contract,
        const std::string& refund_address,
        const std::string& sig_buyer
    );

    /**
     * Verify contract is valid and can be spent
     *
     * @param contract Escrow contract
     * @return true if contract is valid
     */
    static bool verifyContract(const EscrowContract& contract);

    /**
     * Convert Dinero address to scriptPubKey
     *
     * @param address Dinero bech32 address (din1q...)
     * @return scriptPubKey bytes (empty on error)
     */
    static std::vector<uint8_t> addressToScriptPubKey(const std::string& address);

private:
    // Helper: Encode integer as Bitcoin Script format
    static std::vector<uint8_t> encodeScriptInt(uint32_t value);

    // Helper: Encode public key for script
    static std::vector<uint8_t> encodePubKey(const std::string& pubkey_hex);

    // Helper: Build multisig script segment
    static std::vector<uint8_t> buildMultisigScript(const EscrowKeys& keys);

    // Helper: Build timelock refund script segment
    static std::vector<uint8_t> buildTimelockScript(
        const std::string& buyer_pubkey,
        uint32_t refund_time
    );
};

} // namespace contracts
} // namespace dinero
