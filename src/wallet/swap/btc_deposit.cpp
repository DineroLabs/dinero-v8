#include "wallet/swap/btc_deposit.h"

#include "bech32/bech32.hpp"
#include "crypto/hash.h"
#include "wallet/swap/swap_crypto.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dinero::swap {
namespace {

std::array<uint8_t, 20> KeyHash(const std::array<uint8_t, 33>& pubkey) {
    const auto h = din::crypto::HASH160(pubkey.data(), pubkey.size());
    std::array<uint8_t, 20> out{};
    std::copy(h.begin(), h.end(), out.begin());
    return out;
}

// Virtual size of a transaction spending `inputs` P2WPKH coins to `outputs`,
// with every signature at its longest (72 bytes): an upper bound, so the fee
// rate paid is never below the one asked for.
uint64_t VsizeOf(size_t inputs, const std::vector<BtcTxOut>& outputs) {
    uint64_t base = 4 + 1 + inputs * 41 + 1 + 4;  // version, counts, outpoint + empty script + sequence, locktime
    for (const auto& o : outputs) base += 8 + 1 + o.script_pubkey.size();
    const uint64_t witness = 2 + inputs * (1 + 1 + 73 + 1 + 33);  // marker and flag; items: count, signature, key
    return (base * 4 + witness + 3) / 4;
}

uint64_t FeeFor(double feerate, uint64_t vsize, uint64_t max_fee_sat) {
    if (!(feerate > 0) || !std::isfinite(feerate)) throw std::invalid_argument("bitcoin fee rate must be positive");
    const double fee = std::ceil(feerate * static_cast<double>(vsize));
    if (fee > static_cast<double>(max_fee_sat)) {
        throw std::invalid_argument("the Bitcoin network fee now (" + std::to_string(static_cast<uint64_t>(fee)) +
                                    " sat) is above the limit of " + std::to_string(max_fee_sat) + " sat");
    }
    return static_cast<uint64_t>(fee);
}

std::vector<BtcDepositCoin> Usable(std::vector<BtcDepositCoin> coins) {
    coins.erase(std::remove_if(coins.begin(), coins.end(),
                               [](const BtcDepositCoin& c) { return c.value_sat < kBtcMinDepositCoinSat; }),
                coins.end());
    // Largest first; ties in a fixed order, so the same coins always give the same transaction.
    std::sort(coins.begin(), coins.end(), [](const BtcDepositCoin& a, const BtcDepositCoin& b) {
        if (a.value_sat != b.value_sat) return a.value_sat > b.value_sat;
        if (a.txid != b.txid) return a.txid < b.txid;
        return a.vout < b.vout;
    });
    return coins;
}

void RequireScript(const std::vector<uint8_t>& script, const char* what) {
    if (script.empty() || script.size() > 80) throw std::invalid_argument(std::string("bad ") + what + " script");
}

BtcDepositSpend Finish(BtcDepositSpend plan, const Bytes32& key, uint64_t fee, uint64_t vsize) {
    const auto pubkey = detail::CompressedOf(key);
    const auto hash = KeyHash(pubkey);
    // BIP143: a P2WPKH input is signed as if it were the old pay-to-pubkey-hash script.
    std::vector<uint8_t> script_code{0x76, 0xa9, 0x14};
    script_code.insert(script_code.end(), hash.begin(), hash.end());
    script_code.insert(script_code.end(), {0x88, 0xac});
    for (size_t i = 0; i < plan.tx.vin.size(); ++i) {
        const Bytes32 sighash = Bip143SighashAll(plan.tx, i, script_code, plan.used[i].value_sat);
        plan.tx.vin[i].witness = {detail::EcdsaSignAll(key, sighash), std::vector<uint8_t>(pubkey.begin(), pubkey.end())};
    }
    plan.raw = SerializeBtcTx(plan.tx, true);
    plan.txid = BtcTxid(plan.tx);
    plan.fee_sat = fee;
    plan.vsize = vsize;
    return plan;
}

void AddInputs(BtcDepositSpend& plan) {
    for (const BtcDepositCoin& coin : plan.used) {
        BtcTxIn in;
        in.prev_txid = coin.txid;
        in.prev_vout = coin.vout;
        in.sequence = 0xffffffff;  // final: the transaction does not signal replacement
        plan.tx.vin.push_back(in);
    }
}

}  // namespace

std::vector<uint8_t> BtcP2wpkhScript(const std::array<uint8_t, 33>& pubkey) {
    const auto hash = KeyHash(pubkey);
    std::vector<uint8_t> script{0x00, 0x14};
    script.insert(script.end(), hash.begin(), hash.end());
    return script;
}

std::string BtcP2wpkhAddress(const std::array<uint8_t, 33>& pubkey, const std::string& hrp) {
    const auto hash = KeyHash(pubkey);
    return bech32::Encode(hrp, 0, std::vector<uint8_t>(hash.begin(), hash.end()));
}

BtcDepositSpend BuildBtcLockFromDeposit(const std::vector<BtcDepositCoin>& coins, const Bytes32& deposit_secret_key,
                                        const std::vector<uint8_t>& htlc_script_pubkey, uint64_t amount_sat,
                                        const std::vector<uint8_t>& change_script, double feerate_sat_per_vb,
                                        uint64_t max_fee_sat) {
    RequireScript(htlc_script_pubkey, "lock");
    RequireScript(change_script, "change");
    if (amount_sat < kBtcDustSat) throw std::invalid_argument("lock amount below the dust limit");
    const auto usable = Usable(coins);

    BtcDepositSpend plan;
    plan.tx.version = 2;
    uint64_t total = 0;
    for (const BtcDepositCoin& coin : usable) {
        if (plan.used.size() == kBtcMaxDepositInputs) break;
        plan.used.push_back(coin);
        total += coin.value_sat;
        // Enough, with change? Enough without?
        const std::vector<BtcTxOut> with_change{{amount_sat, htlc_script_pubkey}, {0, change_script}};
        const std::vector<BtcTxOut> without_change{{amount_sat, htlc_script_pubkey}};
        const uint64_t vsize_change = VsizeOf(plan.used.size(), with_change);
        const uint64_t vsize_exact = VsizeOf(plan.used.size(), without_change);
        const uint64_t fee_change = FeeFor(feerate_sat_per_vb, vsize_change, max_fee_sat);
        const uint64_t fee_exact = FeeFor(feerate_sat_per_vb, vsize_exact, max_fee_sat);
        if (total >= amount_sat + fee_change + kBtcDustSat) {
            plan.change_sat = total - amount_sat - fee_change;
            plan.tx.vout = with_change;
            plan.tx.vout[1].value_sat = plan.change_sat;
            AddInputs(plan);
            return Finish(std::move(plan), deposit_secret_key, fee_change, vsize_change);
        }
        if (total >= amount_sat + fee_exact) {
            // What is left would be dust as an output: it goes to the fee, if that stays within the limit.
            const uint64_t fee = total - amount_sat;
            if (fee > max_fee_sat) continue;  // one more coin makes a proper change output
            plan.tx.vout = without_change;
            AddInputs(plan);
            return Finish(std::move(plan), deposit_secret_key, fee, vsize_exact);
        }
    }
    // Not enough: say by how much, counting the fee the coins present would need.
    const size_t n = std::max<size_t>(plan.used.size(), 1);
    const uint64_t need = amount_sat + FeeFor(feerate_sat_per_vb, VsizeOf(n, {{amount_sat, htlc_script_pubkey}}), max_fee_sat);
    throw std::runtime_error("deposit is short by " + std::to_string(need > total ? need - total : 1) + " sat (" +
                             std::to_string(total) + " of " + std::to_string(need) + " sat usable)");
}

BtcDepositSpend BuildBtcDepositSweep(const std::vector<BtcDepositCoin>& coins, const Bytes32& deposit_secret_key,
                                     const std::vector<uint8_t>& to_script, double feerate_sat_per_vb,
                                     uint64_t max_fee_sat) {
    RequireScript(to_script, "payout");
    auto usable = Usable(coins);
    if (usable.empty()) throw std::runtime_error("no deposit payment large enough to send back");
    if (usable.size() > kBtcMaxDepositInputs) usable.resize(kBtcMaxDepositInputs);

    BtcDepositSpend plan;
    plan.tx.version = 2;
    plan.used = usable;
    uint64_t total = 0;
    for (const BtcDepositCoin& coin : usable) total += coin.value_sat;
    plan.tx.vout = {{0, to_script}};
    const uint64_t vsize = VsizeOf(usable.size(), plan.tx.vout);
    const uint64_t fee = FeeFor(feerate_sat_per_vb, vsize, max_fee_sat);
    if (total < fee + kBtcDustSat) throw std::runtime_error("the deposit is too small to send back after the network fee");
    plan.tx.vout[0].value_sat = total - fee;
    AddInputs(plan);
    return Finish(std::move(plan), deposit_secret_key, fee, vsize);
}

}  // namespace dinero::swap
