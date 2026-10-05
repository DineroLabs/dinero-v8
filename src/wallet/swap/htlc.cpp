#include "wallet/swap/htlc.h"

#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <algorithm>
#include <stdexcept>

namespace dinero::swap {
namespace {

namespace op = dinero::consensus;

constexpr uint8_t kTapscriptLeafVersion = 0xc0;
constexpr uint32_t kReplaceableSequence = 0xfffffffd;

// BIP341's NUMS point H (no known discrete log): an output whose internal key is
// H can only be spent through a script leaf.
constexpr Bytes32 kNumsInternalKey{
    0x50, 0x92, 0x9b, 0x74, 0xc1, 0xa0, 0x49, 0x54, 0xb7, 0x8b, 0x4b, 0x60, 0x35, 0xe9, 0x7a, 0x5e,
    0x07, 0x8a, 0x5a, 0x0f, 0x28, 0xec, 0x96, 0xd5, 0x47, 0xbf, 0xee, 0x9a, 0xce, 0x80, 0x3a, 0xc0};

Bytes32 Sha256(const uint8_t* data, size_t size) {
    Bytes32 out{};
    crypto::CSHA256().Write(data, size).Finalize(out.data());
    return out;
}

void Push(std::vector<uint8_t>& script, const uint8_t* data, size_t size) {
    // Every push here is 1..75 bytes, so a single length byte is the minimal form.
    script.push_back(static_cast<uint8_t>(size));
    script.insert(script.end(), data, data + size);
}

// Minimal CScriptNum encoding of a non-negative lock time (BIP65 takes up to 5 bytes).
void PushLockTime(std::vector<uint8_t>& script, uint32_t value) {
    std::vector<uint8_t> number;
    for (uint64_t v = value; v != 0; v >>= 8) number.push_back(static_cast<uint8_t>(v & 0xff));
    if (number.back() & 0x80) number.push_back(0x00);  // keep it positive
    Push(script, number.data(), number.size());
}

void RequireTimestampLock(uint32_t lock_time) {
    if (lock_time < kTimestampLockThreshold) {
        throw std::invalid_argument(
            "swap refund lock must be a Unix timestamp (>= 500000000), never a block height");
    }
}

secp256k1_xonly_pubkey ParseXOnly(const Bytes32& key, const char* what) {
    secp256k1_xonly_pubkey parsed;
    if (secp256k1_xonly_pubkey_parse(crypto::GetSecp256k1ContextVerify(), &parsed, key.data()) != 1) {
        throw std::invalid_argument(std::string(what) + " is not a valid x-only public key");
    }
    return parsed;
}

std::vector<uint8_t> ToVector(const Bytes32& bytes) { return {bytes.begin(), bytes.end()}; }

void RequireFundingMatches(const DinHtlcOutput& htlc, const FundingOutput& funding,
                           const Payout& payout) {
    if (funding.script_pubkey != htlc.script_pubkey) {
        throw std::invalid_argument("funding output is not this HTLC");
    }
    if (payout.script_pubkey.empty()) {
        throw std::invalid_argument("payout script is empty");
    }
    if (payout.fee.GetUna() >= funding.value.GetUna()) {
        throw std::invalid_argument("fee must be below the funding value");
    }
}

Transaction BuildSpend(const FundingOutput& funding, const Payout& payout, uint32_t lock_time) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = lock_time;
    TxInput input;
    input.prevout = TxOutPoint(funding.txid, funding.vout);
    input.sequence = kReplaceableSequence;  // non-final (CLTV) and replaceable (fee ladder)
    tx.vin.push_back(std::move(input));
    tx.vout.emplace_back(AmountUna::Una(funding.value.GetUna() - payout.fee.GetUna()),
                         payout.script_pubkey);
    return tx;
}

Bytes32 LeafSighash(const Transaction& tx, const FundingOutput& funding,
                    const std::vector<uint8_t>& leaf_script) {
    if (tx.vin.size() != 1) throw std::invalid_argument("swap spends have exactly one input");
    const uint64_t amount = funding.value.GetUna();
    // Dinero's sighash also takes per-input confidential flags and commitments;
    // a swap HTLC is a transparent output, so: not confidential, no commitment.
    op::ScriptExecutionContext context(&tx, 0, amount, op::SCRIPT_VERIFY_STANDARD, {amount},
                                       {funding.script_pubkey}, /*confidential_flags=*/{0},
                                       /*input_commitments=*/{{}});
    const auto message =
        op::SignatureHashTaproot(context, /*SIGHASH_DEFAULT*/ 0,
                                 op::TapLeafHash(kTapscriptLeafVersion, leaf_script));
    if (message.size() != 32) throw std::runtime_error("taproot sighash failed");
    Bytes32 out{};
    std::copy(message.begin(), message.end(), out.begin());
    return out;
}

}  // namespace

DinHtlcOutput BuildDinHtlc(const DinHtlcTerms& terms) {
    RequireTimestampLock(terms.refund_locktime_unix);
    ParseXOnly(terms.claim_pubkey, "claim key");
    ParseXOnly(terms.refund_pubkey, "refund key");

    DinHtlcOutput out;
    out.claim_script = {op::OP_SIZE, 0x01, 0x20, op::OP_EQUALVERIFY, op::OP_SHA256};
    Push(out.claim_script, terms.payment_hash.data(), 32);
    out.claim_script.push_back(op::OP_EQUALVERIFY);
    Push(out.claim_script, terms.claim_pubkey.data(), 32);
    out.claim_script.push_back(op::OP_CHECKSIG);

    PushLockTime(out.refund_script, terms.refund_locktime_unix);
    out.refund_script.push_back(op::OP_CHECKLOCKTIMEVERIFY);
    out.refund_script.push_back(op::OP_DROP);
    Push(out.refund_script, terms.refund_pubkey.data(), 32);
    out.refund_script.push_back(op::OP_CHECKSIG);

    // Same tagged hashes the consensus verifier uses.
    const auto claim_leaf = op::TapLeafHash(kTapscriptLeafVersion, out.claim_script);
    const auto refund_leaf = op::TapLeafHash(kTapscriptLeafVersion, out.refund_script);
    const auto merkle_root = op::TapBranchHash(claim_leaf, refund_leaf);

    out.internal_key = kNumsInternalKey;
    const auto tweak = op::TapTweakHash(ToVector(kNumsInternalKey), merkle_root);
    auto* secp = crypto::GetSecp256k1ContextVerify();
    secp256k1_xonly_pubkey internal = ParseXOnly(kNumsInternalKey, "NUMS key");
    secp256k1_pubkey tweaked;
    secp256k1_xonly_pubkey output;
    int parity = 0;
    if (secp256k1_xonly_pubkey_tweak_add(secp, &tweaked, &internal, tweak.data()) != 1 ||
        secp256k1_xonly_pubkey_from_pubkey(secp, &output, &parity, &tweaked) != 1 ||
        secp256k1_xonly_pubkey_serialize(secp, out.output_key.data(), &output) != 1) {
        throw std::runtime_error("taproot output key computation failed");
    }

    out.script_pubkey = {op::OP_1, 0x20};
    out.script_pubkey.insert(out.script_pubkey.end(), out.output_key.begin(), out.output_key.end());

    const auto control_block = [&](const std::vector<uint8_t>& sibling) {
        std::vector<uint8_t> cb{static_cast<uint8_t>(kTapscriptLeafVersion | parity)};
        cb.insert(cb.end(), kNumsInternalKey.begin(), kNumsInternalKey.end());
        cb.insert(cb.end(), sibling.begin(), sibling.end());
        return cb;
    };
    out.claim_control_block = control_block(refund_leaf);
    out.refund_control_block = control_block(claim_leaf);
    return out;
}

Transaction BuildDinClaimTx(const DinHtlcOutput& htlc, const FundingOutput& funding,
                            const Payout& payout) {
    RequireFundingMatches(htlc, funding, payout);
    return BuildSpend(funding, payout, /*lock_time=*/0);
}

Transaction BuildDinRefundTx(const DinHtlcTerms& terms, const DinHtlcOutput& htlc,
                             const FundingOutput& funding, const Payout& payout) {
    RequireTimestampLock(terms.refund_locktime_unix);
    RequireFundingMatches(htlc, funding, payout);
    return BuildSpend(funding, payout, terms.refund_locktime_unix);
}

Bytes32 DinClaimSighash(const Transaction& tx, const FundingOutput& funding,
                        const DinHtlcOutput& htlc) {
    return LeafSighash(tx, funding, htlc.claim_script);
}

Bytes32 DinRefundSighash(const Transaction& tx, const FundingOutput& funding,
                         const DinHtlcOutput& htlc) {
    return LeafSighash(tx, funding, htlc.refund_script);
}

void SetDinClaimWitness(Transaction& tx, const DinHtlcTerms& terms, const DinHtlcOutput& htlc,
                        const std::array<uint8_t, 64>& signature,
                        const std::vector<uint8_t>& preimage) {
    if (preimage.size() != 32 || Sha256(preimage.data(), preimage.size()) != terms.payment_hash) {
        throw std::invalid_argument("preimage does not match the payment hash");
    }
    if (tx.vin.size() != 1) throw std::invalid_argument("swap spends have exactly one input");
    tx.vin[0].witness = {std::vector<uint8_t>(signature.begin(), signature.end()), preimage,
                         htlc.claim_script, htlc.claim_control_block};
}

void SetDinRefundWitness(Transaction& tx, const DinHtlcOutput& htlc,
                         const std::array<uint8_t, 64>& signature) {
    if (tx.vin.size() != 1) throw std::invalid_argument("swap spends have exactly one input");
    tx.vin[0].witness = {std::vector<uint8_t>(signature.begin(), signature.end()),
                         htlc.refund_script, htlc.refund_control_block};
}

std::vector<uint8_t> BuildBtcHtlcWitnessScript(const BtcHtlcTerms& terms) {
    RequireTimestampLock(terms.refund_locktime_unix);
    for (const auto* key : {&terms.claim_pubkey, &terms.refund_pubkey}) {
        if ((*key)[0] != 0x02 && (*key)[0] != 0x03) {
            throw std::invalid_argument("bitcoin HTLC keys must be compressed public keys");
        }
    }
    // Bitcoin opcodes: OP_IF 0x63, OP_SIZE 0x82, OP_EQUALVERIFY 0x88, OP_SHA256 0xa8,
    // OP_ELSE 0x67, OP_CHECKLOCKTIMEVERIFY 0xb1, OP_DROP 0x75, OP_ENDIF 0x68, OP_CHECKSIG 0xac.
    std::vector<uint8_t> script{0x63, 0x82, 0x01, 0x20, 0x88, 0xa8};
    Push(script, terms.payment_hash.data(), 32);
    script.push_back(0x88);
    Push(script, terms.claim_pubkey.data(), 33);
    script.push_back(0x67);
    PushLockTime(script, terms.refund_locktime_unix);
    script.push_back(0xb1);
    script.push_back(0x75);
    Push(script, terms.refund_pubkey.data(), 33);
    script.push_back(0x68);
    script.push_back(0xac);
    return script;
}

std::vector<uint8_t> BtcP2wshScriptPubKey(const std::vector<uint8_t>& witness_script) {
    const auto hash = Sha256(witness_script.data(), witness_script.size());
    std::vector<uint8_t> spk{0x00, 0x20};
    spk.insert(spk.end(), hash.begin(), hash.end());
    return spk;
}

std::optional<Bytes32> ExtractPreimageFromBtcClaim(
    const std::vector<std::vector<uint8_t>>& witness, const Bytes32& payment_hash) {
    // By content, not shape: every valid claim must carry the secret as a
    // witness item (the script hashes it), whatever the OP_IF selector bytes are
    // (MINIMALIF is only relay policy for P2WSH). A spend with no item hashing to
    // payment_hash took the refund branch.
    for (const auto& item : witness) {
        if (item.size() != 32 || Sha256(item.data(), 32) != payment_hash) continue;
        Bytes32 preimage{};
        std::copy(item.begin(), item.end(), preimage.begin());
        return preimage;
    }
    return std::nullopt;
}

}  // namespace dinero::swap
