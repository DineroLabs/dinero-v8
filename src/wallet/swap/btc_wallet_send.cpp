#include "wallet/swap/btc_wallet_send.h"

#include "crypto/hash.h"
#include "wallet/swap/swap_crypto.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dinero::swap {
namespace {

constexpr uint64_t kInputVsize = 68;  // one P2WPKH input with its longest signature, rounded up

// Virtual size spending `inputs` P2WPKH coins to `outputs`, every signature at
// its longest (72 bytes and the hash-type byte): the real size is never above
// it, so the fee rate paid is never below the one asked for.
uint64_t VsizeOf(size_t inputs, const std::vector<BtcTxOut>& outputs) {
    uint64_t base = 4 + 1 + inputs * 41 + 1 + 4;
    for (const auto& o : outputs) base += 8 + 1 + o.script_pubkey.size();
    const uint64_t witness = 2 + inputs * (1 + 1 + 73 + 1 + 33);
    return (base * 4 + witness + 3) / 4;
}

uint64_t FeeFor(double feerate, uint64_t vsize, uint64_t max_fee_sat) {
    const double fee = std::ceil(feerate * static_cast<double>(vsize));
    if (fee > static_cast<double>(max_fee_sat)) {
        throw std::invalid_argument("the Bitcoin network fee now (" + std::to_string(static_cast<uint64_t>(fee)) +
                                    " sat) is above the limit of " + std::to_string(max_fee_sat) + " sat");
    }
    return static_cast<uint64_t>(fee);
}

void RequireScript(const std::vector<uint8_t>& script, const char* what) {
    if (script.empty() || script.size() > 80) throw std::invalid_argument(std::string("bad ") + what + " script");
}

void AddInputs(BtcSendPlan& plan) {
    for (const BtcWalletCoin& coin : plan.used) {
        BtcTxIn in;
        in.prev_txid = coin.txid;
        in.prev_vout = coin.vout;
        in.sequence = kBtcReplaceableSequence;
        plan.tx.vin.push_back(in);
    }
}

}  // namespace

BtcSendPlan PlanBtcSend(const std::vector<BtcWalletCoin>& all_coins, const BtcSendRequest& r) {
    RequireScript(r.to_script, "destination");
    if (!(r.feerate_sat_per_vb > 0) || !std::isfinite(r.feerate_sat_per_vb)) {
        throw std::invalid_argument("bitcoin fee rate must be positive");
    }
    if (!r.send_all) {
        RequireScript(r.change_script, "change");
        if (r.amount_sat < kBtcWalletDustSat) {
            throw std::invalid_argument("the smallest amount that can be sent is " + std::to_string(kBtcWalletDustSat) + " sat");
        }
    }
    // A coin that costs more to spend than it holds only makes the payment dearer.
    const uint64_t cost_of_one = static_cast<uint64_t>(std::ceil(r.feerate_sat_per_vb * kInputVsize));
    std::vector<BtcWalletCoin> coins;
    for (const BtcWalletCoin& c : all_coins) {
        if (c.value_sat > cost_of_one) coins.push_back(c);
    }
    // Largest first; ties in a fixed order, so the same coins always give the same payment.
    std::sort(coins.begin(), coins.end(), [](const BtcWalletCoin& a, const BtcWalletCoin& b) {
        if (a.value_sat != b.value_sat) return a.value_sat > b.value_sat;
        if (a.txid != b.txid) return a.txid < b.txid;
        return a.vout < b.vout;
    });
    for (size_t i = 1; i < coins.size(); ++i) {
        if (coins[i].txid == coins[i - 1].txid && coins[i].vout == coins[i - 1].vout) {
            throw std::invalid_argument("the same coin was given twice");
        }
    }

    BtcSendPlan plan;
    plan.tx.version = 2;
    uint64_t total = 0;

    if (r.send_all) {
        if (coins.size() > kBtcWalletMaxInputs) coins.resize(kBtcWalletMaxInputs);
        if (coins.empty()) throw std::runtime_error("not enough bitcoin: nothing spendable at this fee rate");
        plan.used = coins;
        for (const auto& c : coins) total += c.value_sat;
        plan.tx.vout = {{0, r.to_script}};
        plan.vsize = VsizeOf(coins.size(), plan.tx.vout);
        plan.fee_sat = FeeFor(r.feerate_sat_per_vb, plan.vsize, r.max_fee_sat);
        if (total < plan.fee_sat + kBtcWalletDustSat) {
            throw std::runtime_error("not enough bitcoin: what is left after the network fee (" +
                                     std::to_string(plan.fee_sat) + " sat) is too small to send");
        }
        plan.amount_sat = total - plan.fee_sat;
        plan.tx.vout[0].value_sat = plan.amount_sat;
        AddInputs(plan);
        return plan;
    }

    for (const BtcWalletCoin& coin : coins) {
        if (plan.used.size() == kBtcWalletMaxInputs) break;
        plan.used.push_back(coin);
        total += coin.value_sat;
        const std::vector<BtcTxOut> with_change{{r.amount_sat, r.to_script}, {0, r.change_script}};
        const std::vector<BtcTxOut> without_change{{r.amount_sat, r.to_script}};
        const uint64_t vsize_change = VsizeOf(plan.used.size(), with_change);
        const uint64_t vsize_exact = VsizeOf(plan.used.size(), without_change);
        const uint64_t fee_change = FeeFor(r.feerate_sat_per_vb, vsize_change, r.max_fee_sat);
        const uint64_t fee_exact = FeeFor(r.feerate_sat_per_vb, vsize_exact, r.max_fee_sat);
        if (total >= r.amount_sat + fee_change + kBtcWalletDustSat) {
            plan.amount_sat = r.amount_sat;
            plan.fee_sat = fee_change;
            plan.change_sat = total - r.amount_sat - fee_change;
            plan.vsize = vsize_change;
            plan.tx.vout = with_change;
            plan.tx.vout[1].value_sat = plan.change_sat;
            AddInputs(plan);
            return plan;
        }
        if (total >= r.amount_sat + fee_exact) {
            // What is left would be dust as an output: it goes to the fee, if the limit allows.
            const uint64_t fee = total - r.amount_sat;
            if (fee > r.max_fee_sat) continue;  // one more coin makes a proper change output
            plan.amount_sat = r.amount_sat;
            plan.fee_sat = fee;
            plan.dust_to_fee_sat = fee - fee_exact;
            plan.vsize = vsize_exact;
            plan.tx.vout = without_change;
            AddInputs(plan);
            return plan;
        }
    }
    const size_t n = std::max<size_t>(plan.used.size(), 1);
    const uint64_t need = r.amount_sat + FeeFor(r.feerate_sat_per_vb, VsizeOf(n, {{r.amount_sat, r.to_script}}), r.max_fee_sat);
    throw std::runtime_error("not enough bitcoin: short by " + std::to_string(need > total ? need - total : 1) +
                             " sat (" + std::to_string(total) + " spendable, " + std::to_string(need) +
                             " needed with the network fee)");
}

BtcSignedSend SignBtcSend(const BtcSendPlan& plan, const BtcWalletAccount& account,
                          const std::function<Bytes32(uint32_t, uint32_t)>& key_of) {
    if (plan.used.empty() || plan.used.size() != plan.tx.vin.size()) throw std::runtime_error("not a payment plan");
    BtcTx tx = plan.tx;
    uint64_t in_total = 0;
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        const BtcWalletCoin& coin = plan.used[i];
        if (tx.vin[i].prev_txid != coin.txid || tx.vin[i].prev_vout != coin.vout) {
            throw std::runtime_error("the payment plan's inputs do not match its coins");
        }
        Bytes32 key = key_of(coin.branch, coin.index);
        const auto pubkey = detail::CompressedOf(key);
        // The key must be the one the wallet's public half says owns this coin.
        if (pubkey != BtcWalletPubkey(account, coin.branch, coin.index)) {
            key.fill(0);
            throw std::runtime_error("a signing key does not belong to this Bitcoin wallet");
        }
        const auto hash = din::crypto::HASH160(pubkey.data(), pubkey.size());
        // BIP143: a P2WPKH input is signed as if it were the old pay-to-pubkey-hash script.
        std::vector<uint8_t> script_code{0x76, 0xa9, 0x14};
        script_code.insert(script_code.end(), hash.begin(), hash.end());
        script_code.insert(script_code.end(), {0x88, 0xac});
        const Bytes32 sighash = Bip143SighashAll(tx, i, script_code, coin.value_sat);
        std::vector<uint8_t> signature = detail::EcdsaSignAll(key, sighash);
        key.fill(0);
        if (!detail::EcdsaVerifyAll(pubkey, sighash, signature)) throw std::runtime_error("a signature did not verify");
        tx.vin[i].witness = {std::move(signature), std::vector<uint8_t>(pubkey.begin(), pubkey.end())};
        in_total += coin.value_sat;
    }
    BtcSignedSend out;
    out.raw = SerializeBtcTx(tx, true);
    out.txid = BtcTxid(tx);

    // What is sent is what was shown: read the signed bytes back and compare.
    const BtcTx sent = ParseBtcTx(out.raw);
    const auto differs = [](const char* what) { return std::runtime_error(std::string("the signed payment differs from its plan: ") + what); };
    if (sent.version != plan.tx.version || sent.locktime != plan.tx.locktime) throw differs("version or lock time");
    if (sent.vin.size() != plan.tx.vin.size() || sent.vout.size() != plan.tx.vout.size()) throw differs("number of inputs or outputs");
    for (size_t i = 0; i < sent.vin.size(); ++i) {
        if (sent.vin[i].prev_txid != plan.used[i].txid || sent.vin[i].prev_vout != plan.used[i].vout ||
            sent.vin[i].sequence != kBtcReplaceableSequence) {
            throw differs("an input");
        }
    }
    uint64_t out_total = 0;
    for (size_t i = 0; i < sent.vout.size(); ++i) {
        if (sent.vout[i].value_sat != plan.tx.vout[i].value_sat || sent.vout[i].script_pubkey != plan.tx.vout[i].script_pubkey) {
            throw differs("an output");
        }
        out_total += sent.vout[i].value_sat;
    }
    if (sent.vout[0].value_sat != plan.amount_sat) throw differs("the amount");
    if (plan.change_sat ? (sent.vout.size() != 2 || sent.vout[1].value_sat != plan.change_sat) : sent.vout.size() != 1) {
        throw differs("the change");
    }
    if (in_total < out_total || in_total - out_total != plan.fee_sat) throw differs("the fee");
    // The fee was computed for plan.vsize; the real size must not be above it.
    const uint64_t weight = SerializeBtcTx(tx, false).size() * 3 + out.raw.size();
    if ((weight + 3) / 4 > plan.vsize) throw differs("the size");
    return out;
}

}  // namespace dinero::swap
