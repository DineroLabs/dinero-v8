#include "wallet/swap/btc_tx.h"

#include "crypto/sha256.h"

#include <stdexcept>

namespace dinero::swap {
namespace {

constexpr uint32_t kReplaceableSequence = 0xfffffffd;

Bytes32 Sha256d(const std::vector<uint8_t>& data) {
    Bytes32 once{}, twice{};
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(once.data());
    crypto::CSHA256().Write(once.data(), once.size()).Finalize(twice.data());
    return twice;
}

void PutLE(std::vector<uint8_t>& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void PutCompact(std::vector<uint8_t>& out, uint64_t n) {
    if (n < 0xfd) {
        out.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xffff) {
        out.push_back(0xfd);
        PutLE(out, n, 2);
    } else if (n <= 0xffffffff) {
        out.push_back(0xfe);
        PutLE(out, n, 4);
    } else {
        out.push_back(0xff);
        PutLE(out, n, 8);
    }
}

void PutBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& b) {
    PutCompact(out, b.size());
    out.insert(out.end(), b.begin(), b.end());
}

struct Reader {
    const std::vector<uint8_t>& in;
    size_t pos = 0;
    void Need(size_t n) const {
        if (pos + n > in.size()) throw std::invalid_argument("bitcoin tx: truncated");
    }
    uint64_t LE(int bytes) {
        Need(bytes);
        uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= uint64_t(in[pos++]) << (8 * i);
        return v;
    }
    uint64_t Compact() {
        const uint64_t first = LE(1);
        if (first < 0xfd) return first;
        return LE(first == 0xfd ? 2 : first == 0xfe ? 4 : 8);
    }
    std::vector<uint8_t> Bytes(size_t n) {
        Need(n);
        std::vector<uint8_t> out(in.begin() + pos, in.begin() + pos + n);
        pos += n;
        return out;
    }
};

bool HasWitness(const BtcTx& tx) {
    for (const auto& in : tx.vin) {
        if (!in.witness.empty()) return true;
    }
    return false;
}

BtcTx BuildSpend(const BtcFunding& funding, const std::vector<uint8_t>& payout_script,
                 uint64_t fee_sat, uint32_t locktime) {
    if (payout_script.empty()) throw std::invalid_argument("bitcoin spend: empty payout script");
    if (fee_sat >= funding.value_sat) throw std::invalid_argument("bitcoin spend: fee must be below the funding value");
    BtcTx tx;
    tx.version = 2;
    tx.locktime = locktime;
    BtcTxIn in;
    in.prev_txid = funding.txid;
    in.prev_vout = funding.vout;
    in.sequence = kReplaceableSequence;  // non-final (CLTV) and replaceable (fee ladder)
    tx.vin.push_back(std::move(in));
    tx.vout.push_back({funding.value_sat - fee_sat, payout_script});
    return tx;
}

}  // namespace

std::vector<uint8_t> SerializeBtcTx(const BtcTx& tx, bool with_witness) {
    const bool segwit = with_witness && HasWitness(tx);
    std::vector<uint8_t> out;
    PutLE(out, static_cast<uint32_t>(tx.version), 4);
    if (segwit) {
        out.push_back(0x00);  // marker
        out.push_back(0x01);  // flag
    }
    PutCompact(out, tx.vin.size());
    for (const auto& in : tx.vin) {
        out.insert(out.end(), in.prev_txid.begin(), in.prev_txid.end());
        PutLE(out, in.prev_vout, 4);
        out.push_back(0x00);  // empty scriptSig (segwit inputs only)
        PutLE(out, in.sequence, 4);
    }
    PutCompact(out, tx.vout.size());
    for (const auto& o : tx.vout) {
        PutLE(out, o.value_sat, 8);
        PutBytes(out, o.script_pubkey);
    }
    if (segwit) {
        for (const auto& in : tx.vin) {
            PutCompact(out, in.witness.size());
            for (const auto& item : in.witness) PutBytes(out, item);
        }
    }
    PutLE(out, tx.locktime, 4);
    return out;
}

BtcTx ParseBtcTx(const std::vector<uint8_t>& bytes) {
    Reader r{bytes};
    BtcTx tx;
    tx.version = static_cast<int32_t>(r.LE(4));
    bool segwit = false;
    r.Need(2);
    if (bytes[r.pos] == 0x00 && bytes[r.pos + 1] == 0x01) {
        segwit = true;
        r.pos += 2;
    }
    const uint64_t nin = r.Compact();
    if (nin == 0 || nin > 10'000) throw std::invalid_argument("bitcoin tx: bad input count");
    for (uint64_t i = 0; i < nin; ++i) {
        BtcTxIn in;
        const auto txid = r.Bytes(32);
        std::copy(txid.begin(), txid.end(), in.prev_txid.begin());
        in.prev_vout = static_cast<uint32_t>(r.LE(4));
        if (r.Compact() != 0) throw std::invalid_argument("bitcoin tx: non-empty scriptSig not supported");
        in.sequence = static_cast<uint32_t>(r.LE(4));
        tx.vin.push_back(std::move(in));
    }
    const uint64_t nout = r.Compact();
    if (nout > 10'000) throw std::invalid_argument("bitcoin tx: bad output count");
    for (uint64_t i = 0; i < nout; ++i) {
        BtcTxOut o;
        o.value_sat = r.LE(8);
        o.script_pubkey = r.Bytes(r.Compact());
        tx.vout.push_back(std::move(o));
    }
    if (segwit) {
        for (auto& in : tx.vin) {
            const uint64_t items = r.Compact();
            for (uint64_t i = 0; i < items; ++i) in.witness.push_back(r.Bytes(r.Compact()));
        }
    }
    tx.locktime = static_cast<uint32_t>(r.LE(4));
    if (r.pos != bytes.size()) throw std::invalid_argument("bitcoin tx: trailing bytes");
    return tx;
}

Bytes32 BtcTxid(const BtcTx& tx) { return Sha256d(SerializeBtcTx(tx, /*with_witness=*/false)); }

Bytes32 Bip143SighashAll(const BtcTx& tx, size_t index, const std::vector<uint8_t>& script_code,
                         uint64_t amount_sat) {
    if (index >= tx.vin.size()) throw std::invalid_argument("bip143: input index out of range");
    std::vector<uint8_t> prevouts, sequences, outputs;
    for (const auto& in : tx.vin) {
        prevouts.insert(prevouts.end(), in.prev_txid.begin(), in.prev_txid.end());
        PutLE(prevouts, in.prev_vout, 4);
        PutLE(sequences, in.sequence, 4);
    }
    for (const auto& o : tx.vout) {
        PutLE(outputs, o.value_sat, 8);
        PutBytes(outputs, o.script_pubkey);
    }
    const auto& in = tx.vin[index];
    std::vector<uint8_t> pre;
    PutLE(pre, static_cast<uint32_t>(tx.version), 4);
    const auto hp = Sha256d(prevouts), hs = Sha256d(sequences), ho = Sha256d(outputs);
    pre.insert(pre.end(), hp.begin(), hp.end());
    pre.insert(pre.end(), hs.begin(), hs.end());
    pre.insert(pre.end(), in.prev_txid.begin(), in.prev_txid.end());
    PutLE(pre, in.prev_vout, 4);
    PutBytes(pre, script_code);
    PutLE(pre, amount_sat, 8);
    PutLE(pre, in.sequence, 4);
    pre.insert(pre.end(), ho.begin(), ho.end());
    PutLE(pre, tx.locktime, 4);
    PutLE(pre, 1, 4);  // SIGHASH_ALL
    return Sha256d(pre);
}

BtcTx BuildBtcClaimTx(const BtcFunding& funding, const std::vector<uint8_t>& payout_script,
                      uint64_t fee_sat) {
    return BuildSpend(funding, payout_script, fee_sat, /*locktime=*/0);
}

BtcTx BuildBtcRefundTx(const BtcHtlcTerms& terms, const BtcFunding& funding,
                       const std::vector<uint8_t>& payout_script, uint64_t fee_sat) {
    if (terms.refund_locktime_unix < kTimestampLockThreshold) {
        throw std::invalid_argument("bitcoin refund lock must be a Unix timestamp");
    }
    return BuildSpend(funding, payout_script, fee_sat, terms.refund_locktime_unix);
}

Bytes32 BtcHtlcSighash(const BtcTx& tx, const BtcHtlcTerms& terms, const BtcFunding& funding) {
    if (tx.vin.size() != 1) throw std::invalid_argument("swap spends have exactly one input");
    return Bip143SighashAll(tx, 0, BuildBtcHtlcWitnessScript(terms), funding.value_sat);
}

void SetBtcClaimWitness(BtcTx& tx, const BtcHtlcTerms& terms, const std::vector<uint8_t>& signature,
                        const std::vector<uint8_t>& preimage) {
    Bytes32 h{};
    crypto::CSHA256().Write(preimage.data(), preimage.size()).Finalize(h.data());
    if (preimage.size() != 32 || h != terms.payment_hash) {
        throw std::invalid_argument("preimage does not match the payment hash");
    }
    if (tx.vin.size() != 1) throw std::invalid_argument("swap spends have exactly one input");
    tx.vin[0].witness = {signature, preimage, {0x01}, BuildBtcHtlcWitnessScript(terms)};
}

void SetBtcRefundWitness(BtcTx& tx, const BtcHtlcTerms& terms,
                         const std::vector<uint8_t>& signature) {
    if (tx.vin.size() != 1) throw std::invalid_argument("swap spends have exactly one input");
    tx.vin[0].witness = {signature, {}, BuildBtcHtlcWitnessScript(terms)};
}

}  // namespace dinero::swap
