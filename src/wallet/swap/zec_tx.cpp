#include "wallet/swap/zec_tx.h"

#include "crypto/ripemd160.h"
#include "crypto/sha256.h"
#include "dinero_zcash_swap.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace dinero::swap {
namespace {

constexpr uint32_t kFinalSequence = 0xffffffff;
constexpr uint32_t kLockTimeSequence = 0xfffffffe;  // non-final, so CLTV applies; no RBF on Zcash

[[noreturn]] void Fail(const char* what, int32_t code) {
    throw std::invalid_argument(std::string("zcash tx: ") + what + " (code " + std::to_string(code) + ")");
}

void Check(int32_t code, const char* what) {
    if (code != DZS_OK) Fail(what, code);
}

std::vector<dzs_txout> ToFfi(const std::vector<ZecTxOut>& outs) {
    std::vector<dzs_txout> ffi;
    ffi.reserve(outs.size());
    for (const auto& o : outs) ffi.push_back({o.value_zat, o.script_pubkey.data(), o.script_pubkey.size()});
    return ffi;
}

// Minimal push (MINIMALDATA): direct for 1..75 bytes, OP_PUSHDATA1 up to 255.
void PushData(std::vector<uint8_t>& script, const std::vector<uint8_t>& data) {
    if (data.empty() || data.size() > 255) throw std::invalid_argument("zcash tx: push of unsupported size");
    if (data.size() > 75) script.push_back(0x4c);
    script.push_back(static_cast<uint8_t>(data.size()));
    script.insert(script.end(), data.begin(), data.end());
}

std::array<uint8_t, 20> Hash160(const std::vector<uint8_t>& data) {
    Bytes32 sha{};
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(sha.data());
    return RIPEMD160(sha.data(), sha.size());
}

ZecTx BuildSpend(const ZecFunding& funding, const std::vector<uint8_t>& payout_script, uint64_t fee_zat,
                 uint32_t consensus_branch_id, uint32_t expiry_height, uint32_t locktime, uint32_t sequence) {
    if (payout_script.empty()) throw std::invalid_argument("zcash tx: empty payout script");
    if (fee_zat >= funding.value_zat) throw std::invalid_argument("zcash tx: fee is not below the HTLC value");
    ZecTx tx;
    tx.consensus_branch_id = consensus_branch_id;
    tx.locktime = locktime;
    tx.expiry_height = expiry_height;
    tx.vin.push_back({funding.txid, funding.vout, sequence, {}});
    tx.vout.push_back({funding.value_zat - fee_zat, payout_script});
    return tx;
}

void RequireOneInput(const ZecTx& tx) {
    if (tx.vin.size() != 1) throw std::invalid_argument("swap spends have exactly one input");
}

}  // namespace

std::vector<uint8_t> SerializeZecTx(const ZecTx& tx) {
    std::vector<dzs_txin> vin;
    vin.reserve(tx.vin.size());
    for (const auto& i : tx.vin) {
        dzs_txin in{};
        std::copy(i.prev_txid.begin(), i.prev_txid.end(), in.prev_txid);
        in.prev_index = i.prev_vout;
        in.sequence = i.sequence;
        in.script_sig = i.script_sig.data();
        in.script_sig_len = i.script_sig.size();
        vin.push_back(in);
    }
    const auto vout = ToFfi(tx.vout);
    size_t len = 0;
    std::vector<uint8_t> out(256 + 64 * (tx.vin.size() + tx.vout.size()));
    for (;;) {
        const int32_t rc = dzs_tx_build(tx.consensus_branch_id, tx.locktime, tx.expiry_height, vin.data(),
                                        vin.size(), vout.data(), vout.size(), out.data(), out.size(), &len);
        if (rc == DZS_ERR_BUFFER && len > out.size()) {
            out.resize(len);
            continue;
        }
        Check(rc, "cannot build the transaction");
        out.resize(len);
        return out;
    }
}

Bytes32 ZecTxid(const std::vector<uint8_t>& tx_bytes) {
    Bytes32 id{};
    Check(dzs_txid(tx_bytes.data(), tx_bytes.size(), id.data()), "not a v5 transaction");
    return id;
}

Bytes32 ZecTxid(const ZecTx& tx) { return ZecTxid(SerializeZecTx(tx)); }

Bytes32 Zip244SighashAll(const std::vector<uint8_t>& tx_bytes, size_t index,
                         const std::vector<ZecTxOut>& prevouts) {
    const auto ffi = ToFfi(prevouts);
    Bytes32 hash{};
    Check(dzs_sighash_all(tx_bytes.data(), tx_bytes.size(), index, ffi.data(), ffi.size(), hash.data()),
          "cannot compute the signature hash");
    return hash;
}

std::vector<uint8_t> BuildZecHtlcRedeemScript(const ZecHtlcTerms& terms) {
    return BuildBtcHtlcWitnessScript(terms);
}

std::vector<uint8_t> ZecP2shScriptPubKey(const std::vector<uint8_t>& redeem_script) {
    return ZecScriptPubKey({ZecNetwork::Main, ZecAddressKind::P2sh, Hash160(redeem_script)});
}

ZecTx BuildZecClaimTx(const ZecFunding& funding, const std::vector<uint8_t>& payout_script, uint64_t fee_zat,
                      uint32_t consensus_branch_id, uint32_t expiry_height) {
    return BuildSpend(funding, payout_script, fee_zat, consensus_branch_id, expiry_height, 0, kFinalSequence);
}

ZecTx BuildZecRefundTx(const ZecHtlcTerms& terms, const ZecFunding& funding,
                       const std::vector<uint8_t>& payout_script, uint64_t fee_zat,
                       uint32_t consensus_branch_id, uint32_t expiry_height) {
    BuildZecHtlcRedeemScript(terms);  // validates the lock time and keys
    return BuildSpend(funding, payout_script, fee_zat, consensus_branch_id, expiry_height,
                      terms.refund_locktime_unix, kLockTimeSequence);
}

Bytes32 ZecHtlcSighash(const ZecTx& tx, const ZecHtlcTerms& terms, const ZecFunding& funding) {
    RequireOneInput(tx);
    if (tx.vin[0].prev_txid != funding.txid || tx.vin[0].prev_vout != funding.vout) {
        throw std::invalid_argument("zcash tx: input does not spend the HTLC funding output");
    }
    const ZecTxOut htlc{funding.value_zat, ZecP2shScriptPubKey(BuildZecHtlcRedeemScript(terms))};
    return Zip244SighashAll(SerializeZecTx(tx), 0, {htlc});
}

void SetZecClaimScriptSig(ZecTx& tx, const ZecHtlcTerms& terms, const std::vector<uint8_t>& signature,
                          const std::vector<uint8_t>& preimage) {
    RequireOneInput(tx);
    Bytes32 hash{};
    crypto::CSHA256().Write(preimage.data(), preimage.size()).Finalize(hash.data());
    if (preimage.size() != 32 || hash != terms.payment_hash) {
        throw std::invalid_argument("zcash tx: preimage does not match the payment hash");
    }
    std::vector<uint8_t> script_sig;
    PushData(script_sig, signature);
    PushData(script_sig, preimage);
    script_sig.push_back(0x51);  // OP_TRUE: the claim branch
    PushData(script_sig, BuildZecHtlcRedeemScript(terms));
    tx.vin[0].script_sig = std::move(script_sig);
}

void SetZecRefundScriptSig(ZecTx& tx, const ZecHtlcTerms& terms, const std::vector<uint8_t>& signature) {
    RequireOneInput(tx);
    std::vector<uint8_t> script_sig;
    PushData(script_sig, signature);
    script_sig.push_back(0x00);  // OP_FALSE: the refund branch
    PushData(script_sig, BuildZecHtlcRedeemScript(terms));
    tx.vin[0].script_sig = std::move(script_sig);
}

std::string EncodeZecTAddress(const ZecTAddress& address) {
    std::string out(64, '\0');
    size_t len = 0;
    Check(dzs_taddr_encode(static_cast<uint8_t>(address.network), static_cast<uint8_t>(address.kind),
                           address.hash.data(), out.data(), out.size(), &len),
          "cannot encode the address");
    out.resize(len);
    return out;
}

ZecTAddress DecodeZecTAddress(const std::string& text) {
    uint8_t network = 0, kind = 0;
    ZecTAddress out;
    Check(dzs_taddr_decode(text.data(), text.size(), &network, &kind, out.hash.data()),
          "not a transparent Zcash address");
    out.network = static_cast<ZecNetwork>(network);
    out.kind = static_cast<ZecAddressKind>(kind);
    return out;
}

std::vector<uint8_t> ZecScriptPubKey(const ZecTAddress& address) {
    const std::vector<uint8_t> hash(address.hash.begin(), address.hash.end());
    std::vector<uint8_t> spk;
    if (address.kind == ZecAddressKind::P2sh) {
        spk.push_back(0xa9);  // OP_HASH160
        PushData(spk, hash);
        spk.push_back(0x87);  // OP_EQUAL
    } else {
        spk = {0x76, 0xa9};   // OP_DUP OP_HASH160
        PushData(spk, hash);
        spk.push_back(0x88);  // OP_EQUALVERIFY
        spk.push_back(0xac);  // OP_CHECKSIG
    }
    return spk;
}

}  // namespace dinero::swap
