#pragma once
// Hash-time-locked contracts for DIN <-> BTC atomic swaps.
// Design: docs/design/din-btc-atomic-swaps.md (§4 Dinero output, §5 Bitcoin output).
//
// Every input is explicit: the payment hash, both keys, the refund lock (a Unix
// timestamp, never a height), the exact funding output being spent, the payout
// script and the fee. Builders return unsigned transactions; callers sign the
// returned sighash (BIP341 SIGHASH_DEFAULT, script path, the named leaf), which
// commits to every input and output but not to witness arguments such as the
// preimage — so a claim can be signed first and the preimage inserted later.

#include "primitives/amount.h"
#include "primitives/hash_domains.h"
#include "primitives/transaction.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace dinero::swap {

using Bytes32 = std::array<uint8_t, 32>;

// Smallest nLockTime value interpreted as a Unix timestamp (BIP65/BIP113).
inline constexpr uint32_t kTimestampLockThreshold = 500'000'000;

// --- Dinero side: Taproot HTLC --------------------------------------------

struct DinHtlcTerms {
    Bytes32 payment_hash{};          // SHA256(preimage)
    Bytes32 claim_pubkey{};          // x-only; spends with the preimage
    Bytes32 refund_pubkey{};         // x-only; spends after the refund lock
    uint32_t refund_locktime_unix{};  // >= kTimestampLockThreshold
};

struct DinHtlcOutput {
    std::vector<uint8_t> claim_script;   // OP_SIZE 32 OP_EQUALVERIFY OP_SHA256 <h> OP_EQUALVERIFY <claim> OP_CHECKSIG
    std::vector<uint8_t> refund_script;  // <T> OP_CHECKLOCKTIMEVERIFY OP_DROP <refund> OP_CHECKSIG
    Bytes32 internal_key{};              // BIP341 NUMS point: no key-path spend
    Bytes32 output_key{};
    std::vector<uint8_t> script_pubkey;  // OP_1 <output_key>
    std::vector<uint8_t> claim_control_block;
    std::vector<uint8_t> refund_control_block;
};

// Throws std::invalid_argument for a height-type or zero lock, or keys that are
// not valid x-only points.
DinHtlcOutput BuildDinHtlc(const DinHtlcTerms& terms);

// The HTLC output being spent, exactly as it exists on chain.
struct FundingOutput {
    TxId txid;
    uint32_t vout{};
    AmountUna value;
    std::vector<uint8_t> script_pubkey;
};

// Where the coins go. The payout is funding.value - fee in a single output.
struct Payout {
    std::vector<uint8_t> script_pubkey;
    AmountUna fee;
};

// Unsigned spends. Both are version 2, one input, one output, replaceable
// (nSequence 0xfffffffd). Claim: nLockTime 0. Refund: nLockTime = the refund
// lock, which the CLTV leaf requires. Throw std::invalid_argument if the funding
// script does not match the HTLC or the fee is not below the funding value.
Transaction BuildDinClaimTx(const DinHtlcOutput& htlc, const FundingOutput& funding,
                            const Payout& payout);
Transaction BuildDinRefundTx(const DinHtlcTerms& terms, const DinHtlcOutput& htlc,
                             const FundingOutput& funding, const Payout& payout);

// Message to sign with BIP340 (SIGHASH_DEFAULT, script path) for each leaf.
Bytes32 DinClaimSighash(const Transaction& tx, const FundingOutput& funding,
                        const DinHtlcOutput& htlc);
Bytes32 DinRefundSighash(const Transaction& tx, const FundingOutput& funding,
                         const DinHtlcOutput& htlc);

// Complete the witness. The claim checks the preimage (32 bytes, hashes to
// payment_hash) and throws std::invalid_argument otherwise. Signatures are the
// 64-byte SIGHASH_DEFAULT form.
void SetDinClaimWitness(Transaction& tx, const DinHtlcTerms& terms,
                        const DinHtlcOutput& htlc,
                        const std::array<uint8_t, 64>& signature,
                        const std::vector<uint8_t>& preimage);
void SetDinRefundWitness(Transaction& tx, const DinHtlcOutput& htlc,
                         const std::array<uint8_t, 64>& signature);

// --- Bitcoin side: P2WSH HTLC ---------------------------------------------

struct BtcHtlcTerms {
    Bytes32 payment_hash{};
    std::array<uint8_t, 33> claim_pubkey{};   // compressed; spends with the preimage
    std::array<uint8_t, 33> refund_pubkey{};  // compressed; spends after the lock
    uint32_t refund_locktime_unix{};          // >= kTimestampLockThreshold
};

// OP_IF OP_SIZE 32 OP_EQUALVERIFY OP_SHA256 <h> OP_EQUALVERIFY <claim>
// OP_ELSE <T> OP_CHECKLOCKTIMEVERIFY OP_DROP <refund> OP_ENDIF OP_CHECKSIG
std::vector<uint8_t> BuildBtcHtlcWitnessScript(const BtcHtlcTerms& terms);
std::vector<uint8_t> BtcP2wshScriptPubKey(const std::vector<uint8_t>& witness_script);

// The preimage from a Bitcoin HTLC spend: any 32-byte witness item that hashes
// to payment_hash. Present exactly when the spend took the claim branch.
std::optional<Bytes32> ExtractPreimageFromBtcClaim(
    const std::vector<std::vector<uint8_t>>& witness, const Bytes32& payment_hash);

}  // namespace dinero::swap
