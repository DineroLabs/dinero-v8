#pragma once

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║                    CONSENSUS LAYER - BIP143 SIGHASH                        ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║                                                                           ║
// ║  BIP143: Transaction Signature Verification for SegWit v0                 ║
// ║  Reference: https://github.com/bitcoin/bips/blob/master/bip-0143.mediawiki║
// ║                                                                           ║
// ║  This module computes signature hashes for transaction validation.        ║
// ║  Used by BOTH consensus (verification) and wallet (signing).              ║
// ║                                                                           ║
// ║  INVARIANTS:                                                              ║
// ║    - Uses ONLY primitives types (Transaction, TxInput, TxOutput)          ║
// ║    - NO wallet dependencies                                               ║
// ║    - Pure computation (no I/O, no state)                                  ║
// ║                                                                           ║
// ║  Direction of trust: Wallet → Consensus                                   ║
// ║    - Wallet calls this for sighash computation                            ║
// ║    - Consensus uses this for signature verification                       ║
// ║                                                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#include "primitives/transaction.h"
#include <vector>
#include <cstdint>

namespace dinero {
namespace consensus {

/**
 * BIP143 Sighash computation for SegWit v0 transactions
 *
 * This is the canonical implementation used by both validation and signing.
 * All sighash computation goes through this module.
 */
class SighashBIP143 {
public:
    // SIGHASH types (from BIP143)
    static constexpr uint32_t SIGHASH_ALL = 0x01;
    static constexpr uint32_t SIGHASH_NONE = 0x02;
    static constexpr uint32_t SIGHASH_SINGLE = 0x03;
    static constexpr uint32_t SIGHASH_ANYONECANPAY = 0x80;

    /**
     * Compute BIP143 sighash for a single input
     *
     * This is the hash that gets signed/verified.
     *
     * @param tx Transaction being signed/verified
     * @param input_index Index of the input being signed
     * @param scriptCode The script being executed (P2WPKH: OP_DUP OP_HASH160 <pkh> OP_EQUALVERIFY OP_CHECKSIG)
     * @param input_value Value of the input being spent (una)
     * @param sighash_type SIGHASH flags (default: SIGHASH_ALL)
     * @return 32-byte signature hash, or empty on error
     */
    static std::vector<uint8_t> ComputeSighash(
        const Transaction& tx,
        size_t input_index,
        const std::vector<uint8_t>& scriptCode,
        uint64_t input_value,
        uint32_t sighash_type = SIGHASH_ALL
    );

private:
    // BIP143 preimage components
    static std::vector<uint8_t> GetPrevoutsHash(const Transaction& tx);
    static std::vector<uint8_t> GetSequenceHash(const Transaction& tx);
    static std::vector<uint8_t> GetOutputsHash(const Transaction& tx);

    // Serialization helpers
    static void WriteUint32LE(std::vector<uint8_t>& out, uint32_t value);
    static void WriteUint64LE(std::vector<uint8_t>& out, uint64_t value);
};

} // namespace consensus
} // namespace dinero
