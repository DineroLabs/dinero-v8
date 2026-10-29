#include "wallet/swap/zec_tx.h"

#include "crypto/ripemd160.h"
#include "crypto/sha256.h"
#include "dinero_zcash_swap.h"
#include "wallet/swap/swap_crypto.h"

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

size_t CompactSizeLen(size_t n) { return n < 0xfd ? 1 : n <= 0xffff ? 3 : n <= 0xffffffffULL ? 5 : 9; }

// Serialized sizes, as ZIP 317 counts them.
size_t InputSize(size_t script_sig_len) { return 32 + 4 + CompactSizeLen(script_sig_len) + script_sig_len + 4; }
size_t OutputSize(const std::vector<uint8_t>& spk) { return 8 + CompactSizeLen(spk.size()) + spk.size(); }

// <sig, at most 72 DER bytes + sighash byte> <33-byte pubkey>
constexpr size_t kSignedP2pkhScriptSigLen = 1 + 73 + 1 + 33;

uint64_t SumCoins(const std::vector<ZecCoin>& coins) {
    uint64_t total = 0;
    for (const auto& c : coins) {
        if (c.value_zat > 21'000'000ULL * 100'000'000ULL - total) throw std::invalid_argument("zcash tx: coin values overflow");
        total += c.value_zat;
    }
    return total;
}

ZecTx SpendAll(const std::vector<ZecCoin>& coins, uint32_t consensus_branch_id, uint32_t expiry_height) {
    if (coins.empty()) throw std::invalid_argument("zcash tx: no deposit coins");
    ZecTx tx;
    tx.consensus_branch_id = consensus_branch_id;
    tx.expiry_height = expiry_height;
    for (const auto& c : coins) tx.vin.push_back({c.txid, c.vout, kFinalSequence, {}});
    return tx;
}

uint64_t SignedFee(const ZecTx& tx, uint32_t multiplier) {
    size_t in = 0, out = 0;
    for (size_t i = 0; i < tx.vin.size(); ++i) in += InputSize(kSignedP2pkhScriptSigLen);
    for (const auto& o : tx.vout) out += OutputSize(o.script_pubkey);
    return multiplier * Zip317ConventionalFee(in, out);
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

std::optional<Bytes32> ExtractPreimageFromZecClaim(const std::vector<uint8_t>& script_sig,
                                                   const Bytes32& payment_hash) {
    for (size_t pos = 0; pos < script_sig.size();) {
        const uint8_t op = script_sig[pos++];
        size_t len = 0;
        if (op <= 75) {
            len = op;
        } else if (op >= 0x4c && op <= 0x4e) {  // OP_PUSHDATA1/2/4
            const size_t width = op == 0x4c ? 1 : op == 0x4d ? 2 : 4;
            if (script_sig.size() - pos < width) return std::nullopt;
            for (size_t i = 0; i < width; ++i) len |= size_t(script_sig[pos + i]) << (8 * i);
            pos += width;
        } else if (op == 0x4f || (op >= 0x51 && op <= 0x60)) {
            continue;  // OP_1NEGATE, OP_1..OP_16: pushes with no data
        } else {
            return std::nullopt;  // not push-only
        }
        if (script_sig.size() - pos < len) return std::nullopt;
        if (len == 32) {
            Bytes32 item{}, hash{};
            std::copy(script_sig.begin() + pos, script_sig.begin() + pos + 32, item.begin());
            crypto::CSHA256().Write(item.data(), item.size()).Finalize(hash.data());
            if (hash == payment_hash) return item;
        }
        pos += len;
    }
    return std::nullopt;
}

uint64_t Zip317ConventionalFee(size_t tx_in_total_size, size_t tx_out_total_size) {
    const size_t actions = std::max<size_t>({2, (tx_in_total_size + 149) / 150, (tx_out_total_size + 33) / 34});
    return kZip317MarginalFee * actions;
}

uint64_t Zip317ConventionalFee(const ZecTx& tx) {
    size_t in = 0, out = 0;
    for (const auto& i : tx.vin) in += InputSize(i.script_sig.size());
    for (const auto& o : tx.vout) out += OutputSize(o.script_pubkey);
    return Zip317ConventionalFee(in, out);
}

ZecFundingPlan BuildZecFundingTx(const std::vector<ZecCoin>& deposit, const std::vector<uint8_t>& htlc_script_pubkey,
                                 uint64_t htlc_amount_zat, const std::vector<uint8_t>& change_script,
                                 uint32_t fee_multiplier, uint32_t consensus_branch_id, uint32_t expiry_height) {
    if (htlc_script_pubkey.empty() || change_script.empty() || htlc_amount_zat == 0 || fee_multiplier == 0) {
        throw std::invalid_argument("zcash tx: bad funding parameters");
    }
    const uint64_t total = SumCoins(deposit);
    ZecFundingPlan plan;
    plan.tx = SpendAll(deposit, consensus_branch_id, expiry_height);
    plan.tx.vout.push_back({htlc_amount_zat, htlc_script_pubkey});
    plan.tx.vout.push_back({0, change_script});
    plan.fee_zat = SignedFee(plan.tx, fee_multiplier);  // with the change output
    if (total < htlc_amount_zat || total - htlc_amount_zat < plan.fee_zat) {
        throw std::invalid_argument("zcash tx: deposit is short by " +
                                    std::to_string(htlc_amount_zat + plan.fee_zat - std::min(total, htlc_amount_zat + plan.fee_zat)) +
                                    " zat");
    }
    const uint64_t change = total - htlc_amount_zat - plan.fee_zat;
    if (change < kZip317MarginalFee) {
        plan.tx.vout.pop_back();  // not worth an output: it goes to the fee
        plan.fee_zat += change;
    } else {
        plan.tx.vout.back().value_zat = change;
    }
    return plan;
}

ZecTx BuildZecSweepTx(const std::vector<ZecCoin>& deposit, const std::vector<uint8_t>& payout_script,
                      uint32_t consensus_branch_id, uint32_t expiry_height) {
    if (payout_script.empty()) throw std::invalid_argument("zcash tx: empty payout script");
    const uint64_t total = SumCoins(deposit);
    ZecTx tx = SpendAll(deposit, consensus_branch_id, expiry_height);
    tx.vout.push_back({0, payout_script});
    const uint64_t fee = SignedFee(tx, 1);
    if (total <= fee) throw std::invalid_argument("zcash tx: deposit does not cover the fee");
    tx.vout[0].value_zat = total - fee;
    return tx;
}

void SignZecP2pkhInputs(ZecTx& tx, const std::vector<ZecCoin>& coins, const Bytes32& secret_key) {
    if (coins.size() != tx.vin.size()) throw std::invalid_argument("zcash tx: one coin per input");
    const auto pubkey = detail::CompressedOf(secret_key);
    const auto spk = ZecScriptPubKey({ZecNetwork::Main, ZecAddressKind::P2pkh, Hash160({pubkey.begin(), pubkey.end()})});
    std::vector<ZecTxOut> prevouts;
    for (size_t i = 0; i < coins.size(); ++i) {
        if (tx.vin[i].prev_txid != coins[i].txid || tx.vin[i].prev_vout != coins[i].vout) {
            throw std::invalid_argument("zcash tx: coins are not the inputs, in order");
        }
        prevouts.push_back({coins[i].value_zat, spk});
    }
    for (auto& in : tx.vin) in.script_sig.clear();
    const auto unsigned_bytes = SerializeZecTx(tx);  // scriptSigs are not in the sighash
    std::vector<std::vector<uint8_t>> sigs;
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        sigs.push_back(detail::EcdsaSignAll(secret_key, Zip244SighashAll(unsigned_bytes, i, prevouts)));
    }
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        std::vector<uint8_t> script_sig;
        PushData(script_sig, sigs[i]);
        PushData(script_sig, {pubkey.begin(), pubkey.end()});
        tx.vin[i].script_sig = std::move(script_sig);
    }
}

ZecTAddress ZecP2pkhAddressOf(const std::array<uint8_t, 33>& pubkey, ZecNetwork network) {
    return {network, ZecAddressKind::P2pkh, Hash160({pubkey.begin(), pubkey.end()})};
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
