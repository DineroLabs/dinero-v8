#pragma once
// Minimal Bitcoin transactions for the BTC side of a DIN <-> BTC swap
// (milestone 4 of docs/design/din-btc-atomic-swaps-v1-plan.md).
//
// Only what the swap needs: segwit serialization, txid, BIP143 SIGHASH_ALL for
// a P2WSH input, and the HTLC claim/refund spends. Every input is explicit, as
// on the Dinero side. Callers sign the returned sighash with ECDSA (DER) and
// append SIGHASH_ALL (0x01).

#include "wallet/swap/htlc.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dinero::swap {

struct BtcTxIn {
    std::array<uint8_t, 32> prev_txid{};  // wire byte order (reverse of the usual hex)
    uint32_t prev_vout{};
    uint32_t sequence{0xffffffff};
    std::vector<std::vector<uint8_t>> witness;
};

struct BtcTxOut {
    uint64_t value_sat{};
    std::vector<uint8_t> script_pubkey;
};

struct BtcTx {
    int32_t version{2};
    std::vector<BtcTxIn> vin;
    std::vector<BtcTxOut> vout;
    uint32_t locktime{0};
};

// Witness serialization (BIP144) when any input has witness data and
// with_witness is true; otherwise the legacy form (used for the txid).
std::vector<uint8_t> SerializeBtcTx(const BtcTx& tx, bool with_witness = true);
BtcTx ParseBtcTx(const std::vector<uint8_t>& bytes);  // throws std::invalid_argument
Bytes32 BtcTxid(const BtcTx& tx);                     // wire byte order

// BIP143 signature hash, SIGHASH_ALL, for input `index` spending a segwit v0
// output whose script code is `script_code` (the witness script for P2WSH).
Bytes32 Bip143SighashAll(const BtcTx& tx, size_t index, const std::vector<uint8_t>& script_code,
                         uint64_t amount_sat);

// The BTC HTLC output being spent.
struct BtcFunding {
    std::array<uint8_t, 32> txid{};  // wire byte order
    uint32_t vout{};
    uint64_t value_sat{};
};

// Unsigned spends: version 2, one input, one output, replaceable
// (nSequence 0xfffffffd). Claim: locktime 0. Refund: locktime = the refund lock.
// Throw std::invalid_argument when the fee is not below the funding value or
// the payout script is empty.
BtcTx BuildBtcClaimTx(const BtcFunding& funding, const std::vector<uint8_t>& payout_script,
                      uint64_t fee_sat);
BtcTx BuildBtcRefundTx(const BtcHtlcTerms& terms, const BtcFunding& funding,
                       const std::vector<uint8_t>& payout_script, uint64_t fee_sat);

// Message to sign for either path (the witness script is the script code).
Bytes32 BtcHtlcSighash(const BtcTx& tx, const BtcHtlcTerms& terms, const BtcFunding& funding);

// Witnesses. `signature` is DER + the SIGHASH_ALL byte. The claim checks the
// preimage (32 bytes, hashes to payment_hash) and throws otherwise.
void SetBtcClaimWitness(BtcTx& tx, const BtcHtlcTerms& terms, const std::vector<uint8_t>& signature,
                        const std::vector<uint8_t>& preimage);
void SetBtcRefundWitness(BtcTx& tx, const BtcHtlcTerms& terms,
                         const std::vector<uint8_t>& signature);

}  // namespace dinero::swap
