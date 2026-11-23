// Payments from the built-in Bitcoin wallet: which coins, what fee, what
// change, and that the signed bytes are exactly the plan the user was shown.
#include "wallet/swap/btc_wallet_send.h"

#include "wallet/swap/swap_crypto.h"

#include <gtest/gtest.h>

using namespace dinero::swap;

namespace {

struct Wallet {
    Bytes32 key{}, chain{};
    BtcWalletAccount account;
    explicit Wallet(uint8_t fill) {
        key.fill(fill);
        chain.fill(static_cast<uint8_t>(fill + 1));
        account = BtcWalletAccountFromSecret(key, chain);
    }
    std::function<Bytes32(uint32_t, uint32_t)> Keys() const {
        return [this](uint32_t branch, uint32_t index) { return BtcWalletSecretKey(key, chain, branch, index); };
    }
};

BtcWalletCoin Coin(uint8_t id, uint64_t sat, uint32_t branch = 0, uint32_t index = 0) {
    BtcWalletCoin c;
    c.txid.fill(id);
    c.vout = id % 3;
    c.value_sat = sat;
    c.branch = branch;
    c.index = index;
    return c;
}

const std::vector<uint8_t> kTo = [] { std::vector<uint8_t> s{0x00, 0x14}; s.insert(s.end(), 20, 0xaa); return s; }();

BtcSendRequest Request(const Wallet& w, uint64_t amount, double feerate = 2.0) {
    BtcSendRequest r;
    r.to_script = kTo;
    r.amount_sat = amount;
    r.feerate_sat_per_vb = feerate;
    r.max_fee_sat = 50'000;
    r.change_script = BtcWalletScript(w.account, 1, 0);
    return r;
}

uint64_t Sum(const std::vector<BtcWalletCoin>& coins) {
    uint64_t t = 0;
    for (const auto& c : coins) t += c.value_sat;
    return t;
}

}  // namespace

TEST(BtcWalletSend, TakesTheLargestCoinsAndReturnsTheChange) {
    Wallet w(0x21);
    const std::vector<BtcWalletCoin> coins{Coin(1, 5'000), Coin(2, 80'000, 0, 3), Coin(3, 30'000, 1, 2)};
    const BtcSendPlan p = PlanBtcSend(coins, Request(w, 20'000));
    ASSERT_EQ(p.used.size(), 1u);
    EXPECT_EQ(p.used[0].value_sat, 80'000u);
    EXPECT_EQ(p.amount_sat, 20'000u);
    ASSERT_EQ(p.tx.vout.size(), 2u);
    EXPECT_EQ(p.tx.vout[0].script_pubkey, kTo);
    EXPECT_EQ(p.tx.vout[0].value_sat, 20'000u);
    EXPECT_EQ(p.tx.vout[1].script_pubkey, BtcWalletScript(w.account, 1, 0));
    EXPECT_EQ(p.change_sat, 80'000u - 20'000u - p.fee_sat);
    EXPECT_EQ(p.fee_sat, 2 * p.vsize);  // 2 sat/vB on the size the fee was computed for
    EXPECT_EQ(p.dust_to_fee_sat, 0u);
    EXPECT_EQ(p.tx.vin[0].sequence, kBtcReplaceableSequence);  // can be re-sent with a higher fee
    // More than one coin only when one is not enough.
    const BtcSendPlan two = PlanBtcSend(coins, Request(w, 100'000));
    ASSERT_EQ(two.used.size(), 2u);
    EXPECT_EQ(Sum(two.used), 110'000u);
    EXPECT_EQ(two.amount_sat + two.fee_sat + two.change_sat, 110'000u);
    // The same coins in another order give the same payment.
    const std::vector<BtcWalletCoin> shuffled{coins[2], coins[0], coins[1]};
    EXPECT_EQ(SerializeBtcTx(PlanBtcSend(shuffled, Request(w, 100'000)).tx, false), SerializeBtcTx(two.tx, false));
}

TEST(BtcWalletSend, NothingIsEverLostBetweenInputsAndOutputs) {
    // Over many amounts and fee rates: inputs = amount + change + fee, exactly;
    // the fee is at least the rate asked for and never above the limit; no dust output.
    Wallet w(0x31);
    std::vector<BtcWalletCoin> coins;
    for (uint8_t i = 1; i <= 12; ++i) coins.push_back(Coin(i, 3'000ull * i + 17 * i * i, i % 2, i));
    for (double feerate : {1.0, 1.5, 7.0, 31.4}) {
        for (uint64_t amount = 546; amount < 200'000; amount += 1'237) {
            BtcSendPlan p;
            try {
                p = PlanBtcSend(coins, Request(w, amount, feerate));
            } catch (const std::runtime_error&) {
                continue;  // not enough: covered below
            }
            ASSERT_EQ(Sum(p.used), p.amount_sat + p.change_sat + p.fee_sat) << amount << " at " << feerate;
            ASSERT_EQ(p.amount_sat, amount);
            ASSERT_GE(static_cast<double>(p.fee_sat), feerate * static_cast<double>(p.vsize) - 1e-6);
            ASSERT_LE(p.fee_sat, 50'000u);
            for (const auto& o : p.tx.vout) ASSERT_GE(o.value_sat, kBtcWalletDustSat);
            ASSERT_EQ(p.change_sat == 0, p.tx.vout.size() == 1u);
            // And it signs to exactly that.
            const BtcSignedSend s = SignBtcSend(p, w.account, w.Keys());
            const BtcTx sent = ParseBtcTx(s.raw);
            uint64_t out_total = 0;
            for (const auto& o : sent.vout) out_total += o.value_sat;
            ASSERT_EQ(Sum(p.used) - out_total, p.fee_sat);
        }
    }
}

TEST(BtcWalletSend, ChangeTooSmallForAnOutputGoesToTheFeeAndIsShown) {
    Wallet w(0x41);
    const std::vector<BtcWalletCoin> coins{Coin(1, 20'000)};
    // 20,000 in; with the fee for one output the remainder is under the dust limit.
    BtcSendRequest r = Request(w, 19'500);
    const BtcSendPlan p = PlanBtcSend(coins, r);
    EXPECT_EQ(p.tx.vout.size(), 1u);
    EXPECT_EQ(p.change_sat, 0u);
    EXPECT_EQ(p.fee_sat, 500u);
    EXPECT_GT(p.dust_to_fee_sat, 0u);
    EXPECT_EQ(p.fee_sat - p.dust_to_fee_sat, 2 * p.vsize);
    // Never above the fee limit, though: then it is "not enough" (no second coin to make real change).
    r.max_fee_sat = 400;
    EXPECT_THROW(PlanBtcSend(coins, r), std::runtime_error);
}

TEST(BtcWalletSend, SendAllLeavesNothingBehind) {
    Wallet w(0x51);
    const std::vector<BtcWalletCoin> coins{Coin(1, 29'000), Coin(2, 2'817, 1, 0), Coin(3, 100)};  // 100 sat costs more than it holds
    BtcSendRequest r = Request(w, 0);
    r.send_all = true;
    const BtcSendPlan p = PlanBtcSend(coins, r);
    EXPECT_EQ(p.used.size(), 2u);
    EXPECT_EQ(p.tx.vout.size(), 1u);
    EXPECT_EQ(p.change_sat, 0u);
    EXPECT_EQ(p.amount_sat, 31'817u - p.fee_sat);
    EXPECT_EQ(p.fee_sat, 2 * p.vsize);
    // Nothing worth sending.
    EXPECT_THROW(PlanBtcSend({Coin(3, 100)}, r), std::runtime_error);
    EXPECT_THROW(PlanBtcSend({Coin(1, 700)}, r), std::runtime_error);  // the fee leaves dust
}

TEST(BtcWalletSend, SaysByHowMuchItIsShort) {
    Wallet w(0x61);
    try {
        PlanBtcSend({Coin(1, 10'000), Coin(2, 4'000)}, Request(w, 20'000));
        ADD_FAILURE() << "planned a payment of more than the wallet holds";
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        EXPECT_EQ(what.rfind("not enough bitcoin: short by ", 0), 0u) << what;
        EXPECT_NE(what.find("14000 spendable"), std::string::npos) << what;
    }
    EXPECT_THROW(PlanBtcSend({}, Request(w, 20'000)), std::runtime_error);
}

TEST(BtcWalletSend, RefusesWhatCannotBeAPayment) {
    Wallet w(0x71);
    const std::vector<BtcWalletCoin> coins{Coin(1, 100'000)};
    EXPECT_THROW(PlanBtcSend(coins, Request(w, 545)), std::invalid_argument);          // dust
    EXPECT_THROW(PlanBtcSend(coins, Request(w, 10'000, 0)), std::invalid_argument);    // no fee rate
    EXPECT_THROW(PlanBtcSend(coins, Request(w, 10'000, -1)), std::invalid_argument);
    BtcSendRequest r = Request(w, 10'000);
    r.to_script.clear();
    EXPECT_THROW(PlanBtcSend(coins, r), std::invalid_argument);
    r = Request(w, 10'000);
    r.change_script.clear();
    EXPECT_THROW(PlanBtcSend(coins, r), std::invalid_argument);
    r = Request(w, 10'000, 500.0);  // a fee rate that would cost more than the limit
    EXPECT_THROW(PlanBtcSend(coins, r), std::invalid_argument);
    EXPECT_THROW(PlanBtcSend({Coin(1, 100'000), Coin(1, 100'000)}, Request(w, 10'000)), std::invalid_argument);  // one coin twice
}

TEST(BtcWalletSend, TheSignedPaymentIsThePlanAndItsSignaturesVerify) {
    Wallet w(0x81);
    const std::vector<BtcWalletCoin> coins{Coin(1, 60'000, 0, 4), Coin(2, 50'000, 1, 9)};
    const BtcSendPlan p = PlanBtcSend(coins, Request(w, 100'000));
    ASSERT_EQ(p.used.size(), 2u);
    const BtcSignedSend s = SignBtcSend(p, w.account, w.Keys());
    const BtcTx sent = ParseBtcTx(s.raw);
    EXPECT_EQ(s.txid, BtcTxid(p.tx));  // segwit: the id does not depend on the signatures
    ASSERT_EQ(sent.vin.size(), 2u);
    for (size_t i = 0; i < sent.vin.size(); ++i) {
        ASSERT_EQ(sent.vin[i].witness.size(), 2u);
        const auto pubkey = BtcWalletPubkey(w.account, p.used[i].branch, p.used[i].index);
        EXPECT_EQ(sent.vin[i].witness[1], std::vector<uint8_t>(pubkey.begin(), pubkey.end()));
    }
    // The real size is within the size the fee was computed for.
    const uint64_t weight = SerializeBtcTx(sent, false).size() * 3 + s.raw.size();
    EXPECT_LE((weight + 3) / 4, p.vsize);
    EXPECT_GE((weight + 3) / 4 + 2 * p.used.size(), p.vsize);  // and not far below: at most a byte or two per signature
}

TEST(BtcWalletSend, SigningRefusesAPlanThatWasTamperedWith) {
    Wallet w(0x91), other(0x95);
    const std::vector<BtcWalletCoin> coins{Coin(1, 60'000, 0, 4)};
    const BtcSendPlan good = PlanBtcSend(coins, Request(w, 20'000));
    EXPECT_NO_THROW(SignBtcSend(good, w.account, w.Keys()));

    BtcSendPlan p = good;  // the destination's amount raised after the user saw the plan
    p.tx.vout[0].value_sat += 1'000;
    EXPECT_THROW(SignBtcSend(p, w.account, w.Keys()), std::runtime_error);
    p = good;  // the change redirected... the output no longer matches the shown change amount
    p.tx.vout[1].value_sat -= 5'000;
    EXPECT_THROW(SignBtcSend(p, w.account, w.Keys()), std::runtime_error);
    p = good;  // an extra output
    p.tx.vout.push_back({1'000, kTo});
    EXPECT_THROW(SignBtcSend(p, w.account, w.Keys()), std::runtime_error);
    p = good;  // a different coin than the one listed
    p.tx.vin[0].prev_vout += 1;
    EXPECT_THROW(SignBtcSend(p, w.account, w.Keys()), std::runtime_error);
    p = good;  // not replaceable any more
    p.tx.vin[0].sequence = 0xffffffff;
    EXPECT_THROW(SignBtcSend(p, w.account, w.Keys()), std::runtime_error);
    p = good;  // the fee shown is not the fee paid
    p.fee_sat += 1;
    EXPECT_THROW(SignBtcSend(p, w.account, w.Keys()), std::runtime_error);
    // Keys of another wallet, or the wrong address's key.
    EXPECT_THROW(SignBtcSend(good, w.account, other.Keys()), std::runtime_error);
    EXPECT_THROW(SignBtcSend(good, w.account, [&](uint32_t b, uint32_t i) { return w.Keys()(b, i + 1); }), std::runtime_error);
}

TEST(BtcWalletSend, AStuckPaymentIsReSentWithAHigherFeeFromItsChange) {
    Wallet w(0xa1);
    const std::vector<BtcWalletCoin> coins{Coin(1, 60'000, 0, 4)};
    const BtcSendPlan first = PlanBtcSend(coins, Request(w, 20'000, 1.0));
    const BtcTx sent = ParseBtcTx(SignBtcSend(first, w.account, w.Keys()).raw);

    const BtcSendPlan bumped = PlanBtcFeeBump(sent, first.used, 5.0, 50'000);
    EXPECT_EQ(bumped.amount_sat, 20'000u);                       // the destination gets the same
    EXPECT_EQ(bumped.tx.vout[0].script_pubkey, kTo);
    EXPECT_EQ(bumped.tx.vout[0].value_sat, 20'000u);
    EXPECT_EQ(bumped.fee_sat, 5 * bumped.vsize);
    EXPECT_EQ(bumped.change_sat, first.change_sat - (bumped.fee_sat - first.fee_sat));  // the change pays the difference
    EXPECT_EQ(bumped.amount_sat + bumped.fee_sat + bumped.change_sat, 60'000u);
    ASSERT_EQ(bumped.tx.vin.size(), 1u);
    EXPECT_EQ(bumped.tx.vin[0].prev_txid, sent.vin[0].prev_txid);  // the same coin: the two cannot both confirm
    EXPECT_NE(BtcTxid(bumped.tx), BtcTxid(sent));
    EXPECT_NO_THROW(SignBtcSend(bumped, w.account, w.Keys()));

    // A rate no higher than before still pays what a replacement must: the old fee plus 1 sat/vB.
    const BtcSendPlan minimal = PlanBtcFeeBump(sent, first.used, 1.0, 50'000);
    EXPECT_EQ(minimal.fee_sat, first.fee_sat + minimal.vsize);

    // What cannot be bumped says why.
    EXPECT_THROW(PlanBtcFeeBump(sent, first.used, 5.0, 300), std::invalid_argument);            // above the fee limit
    EXPECT_THROW(PlanBtcFeeBump(sent, first.used, 0, 50'000), std::invalid_argument);
    BtcSendRequest all = Request(w, 0);
    all.send_all = true;
    const BtcSendPlan everything = PlanBtcSend(coins, all);
    EXPECT_THROW(PlanBtcFeeBump(everything.tx, everything.used, 5.0, 50'000), std::runtime_error);  // no change
    const BtcSendPlan tight = PlanBtcSend(coins, Request(w, 59'000, 1.0));                       // change of a few hundred sat
    ASSERT_GT(tight.change_sat, 0u);
    EXPECT_THROW(PlanBtcFeeBump(tight.tx, tight.used, 20.0, 50'000), std::runtime_error);        // change too small
    std::vector<BtcWalletCoin> wrong = first.used;
    wrong[0].vout += 1;
    EXPECT_THROW(PlanBtcFeeBump(sent, wrong, 5.0, 50'000), std::invalid_argument);               // not this payment's coins
    BtcTx final_tx = sent;
    final_tx.vin[0].sequence = 0xffffffff;
    EXPECT_THROW(PlanBtcFeeBump(final_tx, first.used, 5.0, 50'000), std::invalid_argument);      // a swap lock, say
}
