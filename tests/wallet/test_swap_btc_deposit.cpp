// Building a swap's lock from a deposit address: the address itself, which
// coins are spent, change and dust, fees, and signatures that verify.

#include "wallet/swap/btc_deposit.h"
#include "wallet/swap/btc_raw_tx.h"
#include "wallet/swap/swap_crypto.h"

#include <gtest/gtest.h>

using namespace dinero::swap;

namespace {

Bytes32 Key(uint8_t last) {
    Bytes32 k{};
    k[31] = last;
    return k;
}
BtcDepositCoin Coin(uint8_t salt, uint64_t value, uint32_t vout = 0) {
    BtcDepositCoin c;
    c.txid.fill(salt);
    c.vout = vout;
    c.value_sat = value;
    return c;
}
const std::vector<uint8_t> kHtlc = [] {
    std::vector<uint8_t> s{0x00, 0x20};
    s.insert(s.end(), 32, 0xaa);
    return s;
}();
const std::vector<uint8_t> kChange{0x00, 0x14, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};

// The real virtual size of a signed transaction.
uint64_t Vsize(const BtcTx& tx) {
    const uint64_t base = SerializeBtcTx(tx, false).size(), total = SerializeBtcTx(tx, true).size();
    return (base * 3 + total + 3) / 4;
}

// Every input is signed by `key` for the coin it spends.
void ExpectSigned(const BtcDepositSpend& plan, const Bytes32& key) {
    const auto pubkey = detail::CompressedOf(key);
    const auto spk = BtcP2wpkhScript(pubkey);
    std::vector<uint8_t> script_code{0x76, 0xa9, 0x14};
    script_code.insert(script_code.end(), spk.begin() + 2, spk.end());
    script_code.insert(script_code.end(), {0x88, 0xac});
    ASSERT_EQ(plan.tx.vin.size(), plan.used.size());
    for (size_t i = 0; i < plan.tx.vin.size(); ++i) {
        const auto& in = plan.tx.vin[i];
        EXPECT_EQ(in.prev_txid, plan.used[i].txid);
        EXPECT_EQ(in.prev_vout, plan.used[i].vout);
        EXPECT_EQ(in.sequence, 0xffffffffu);
        ASSERT_EQ(in.witness.size(), 2u);
        EXPECT_EQ(in.witness[1], std::vector<uint8_t>(pubkey.begin(), pubkey.end()));
        EXPECT_TRUE(detail::EcdsaVerifyAll(pubkey, Bip143SighashAll(plan.tx, i, script_code, plan.used[i].value_sat), in.witness[0]));
    }
    // What goes on the wire is this transaction, and its id does not depend on the signatures.
    const auto parsed = ParseBtcRawTx(plan.raw);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->txid, plan.txid);
    EXPECT_EQ(plan.txid, BtcTxid(plan.tx));
    EXPECT_LE(Vsize(plan.tx), plan.vsize);       // the fee was computed for at least the real size
    // ...and not for much more: a signature may come out a byte or two shorter than the longest assumed.
    EXPECT_GE(Vsize(plan.tx) + 1 + plan.tx.vin.size() / 2, plan.vsize);
    uint64_t in_total = 0, out_total = 0;
    for (const auto& c : plan.used) in_total += c.value_sat;
    for (const auto& o : plan.tx.vout) out_total += o.value_sat;
    EXPECT_EQ(in_total - out_total, plan.fee_sat);
}

std::string Refusal(const std::function<void()>& build) {
    try {
        build();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

}  // namespace

// BIP173's example: the key 1 (the curve's generator) and its P2WPKH address.
TEST(BtcDeposit, AddressOfAKey) {
    const auto pubkey = detail::CompressedOf(Key(1));
    EXPECT_EQ(BtcP2wpkhAddress(pubkey, "bc"), "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4");
    EXPECT_EQ(BtcP2wpkhAddress(pubkey, "tb"), "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx");
    const auto script = BtcP2wpkhScript(pubkey);
    ASSERT_EQ(script.size(), 22u);
    EXPECT_EQ(script[0], 0x00);
    EXPECT_EQ(script[1], 0x14);
    EXPECT_EQ(script[2], 0x75);  // 751e76e8199196d454941c45d1b3a323f1433bd6
    EXPECT_EQ(script[21], 0xd6);
    EXPECT_NE(BtcP2wpkhScript(detail::CompressedOf(Key(2))), script);
}

TEST(BtcDeposit, LockWithChange) {
    const Bytes32 key = Key(7);
    const auto plan = BuildBtcLockFromDeposit({Coin(1, 300'000)}, key, kHtlc, 250'000, kChange, 2.0, 5'000);
    ASSERT_EQ(plan.tx.vout.size(), 2u);
    EXPECT_EQ(plan.tx.vout[0].script_pubkey, kHtlc);
    EXPECT_EQ(plan.tx.vout[0].value_sat, 250'000u);  // output 0, exactly the swap amount
    EXPECT_EQ(plan.tx.vout[1].script_pubkey, kChange);
    EXPECT_EQ(plan.fee_sat, 2 * plan.vsize);
    EXPECT_EQ(plan.change_sat, 300'000u - 250'000u - plan.fee_sat);
    EXPECT_EQ(plan.tx.vout[1].value_sat, plan.change_sat);
    EXPECT_EQ(plan.tx.locktime, 0u);
    EXPECT_EQ(plan.tx.version, 2);
    ExpectSigned(plan, key);
    // A fractional rate is rounded up, never down.
    const auto slow = BuildBtcLockFromDeposit({Coin(1, 300'000)}, key, kHtlc, 250'000, kChange, 1.001, 5'000);
    EXPECT_EQ(slow.fee_sat, slow.vsize + 1);  // 1.001 x 153 = 153.2: one whole sat more, not one less
}

TEST(BtcDeposit, OnlyAsManyCoinsAsNeededLargestFirst) {
    const Bytes32 key = Key(8);
    const std::vector<BtcDepositCoin> coins{Coin(1, 60'000), Coin(2, 200'000), Coin(3, 90'000), Coin(4, 1'999), Coin(5, 500)};
    auto plan = BuildBtcLockFromDeposit(coins, key, kHtlc, 150'000, kChange, 1.0, 5'000);
    ASSERT_EQ(plan.used.size(), 1u);  // the 200,000 coin alone covers it
    EXPECT_EQ(plan.used[0].value_sat, 200'000u);
    ExpectSigned(plan, key);

    plan = BuildBtcLockFromDeposit(coins, key, kHtlc, 280'000, kChange, 1.0, 5'000);
    ASSERT_EQ(plan.used.size(), 2u);  // 200,000 + 90,000
    EXPECT_EQ(plan.used[1].value_sat, 90'000u);
    ExpectSigned(plan, key);

    // The same coins in any order give the same transaction.
    std::vector<BtcDepositCoin> shuffled{coins[3], coins[2], coins[4], coins[0], coins[1]};
    EXPECT_EQ(BuildBtcLockFromDeposit(shuffled, key, kHtlc, 280'000, kChange, 1.0, 5'000).txid, plan.txid);

    // Coins under the minimum are never spent, however many there are: dust cannot bloat the lock.
    std::vector<BtcDepositCoin> dusty{Coin(9, 260'000)};
    for (uint8_t i = 0; i < 200; ++i) dusty.push_back(Coin(i, kBtcMinDepositCoinSat - 1, 100 + i));
    plan = BuildBtcLockFromDeposit(dusty, key, kHtlc, 250'000, kChange, 1.0, 5'000);
    EXPECT_EQ(plan.used.size(), 1u);
    const std::string short_of = Refusal([&] { BuildBtcLockFromDeposit(dusty, key, kHtlc, 300'000, kChange, 1.0, 5'000); });
    EXPECT_EQ(short_of.rfind("deposit is short by ", 0), 0u) << short_of;
    EXPECT_NE(short_of.find("260000 of "), std::string::npos) << short_of;  // the dust is not counted
}

TEST(BtcDeposit, LeftoverTooSmallForAnOutputGoesToTheFee) {
    const Bytes32 key = Key(9);
    // 250,000 + a fee of about 122 + 300 left: no 300-sat output is made.
    const auto plan = BuildBtcLockFromDeposit({Coin(1, 250'422)}, key, kHtlc, 250'000, kChange, 1.0, 5'000);
    ASSERT_EQ(plan.tx.vout.size(), 1u);
    EXPECT_EQ(plan.change_sat, 0u);
    EXPECT_EQ(plan.fee_sat, 422u);
    ExpectSigned(plan, key);
    // Exactly at the dust limit there is a change output.
    const auto with = BuildBtcLockFromDeposit({Coin(1, 251'000)}, key, kHtlc, 250'000, kChange, 1.0, 5'000);
    ASSERT_EQ(with.tx.vout.size(), 2u);
    EXPECT_GE(with.change_sat, kBtcDustSat);
    // The leftover never pushes the fee past its limit: with a tight limit and no second coin it is refused...
    EXPECT_NE(Refusal([&] { BuildBtcLockFromDeposit({Coin(1, 250'500)}, key, kHtlc, 250'000, kChange, 1.0, 400); }), "");
    // ...and with a second coin there is proper change instead.
    const auto two = BuildBtcLockFromDeposit({Coin(1, 250'500), Coin(2, 50'000)}, key, kHtlc, 250'000, kChange, 1.0, 400);
    EXPECT_EQ(two.used.size(), 2u);
    EXPECT_LE(two.fee_sat, 400u);
    ExpectSigned(two, key);
}

TEST(BtcDeposit, RefusesWhatCannotBeBuilt) {
    const Bytes32 key = Key(10);
    const std::vector<BtcDepositCoin> rich{Coin(1, 1'000'000)};
    EXPECT_EQ(Refusal([&] { BuildBtcLockFromDeposit({}, key, kHtlc, 250'000, kChange, 1.0, 5'000); }).rfind("deposit is short by ", 0), 0u);
    EXPECT_EQ(Refusal([&] { BuildBtcLockFromDeposit({Coin(1, 250'000)}, key, kHtlc, 250'000, kChange, 1.0, 5'000); }).rfind("deposit is short by ", 0), 0u);
    // A fee rate that needs more than the limit: said so, nothing built.
    const std::string fee = Refusal([&] { BuildBtcLockFromDeposit(rich, key, kHtlc, 250'000, kChange, 500.0, 5'000); });
    EXPECT_NE(fee.find("above the limit of 5000 sat"), std::string::npos) << fee;
    for (double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        EXPECT_NE(Refusal([&] { BuildBtcLockFromDeposit(rich, key, kHtlc, 250'000, kChange, bad, 5'000); }), "") << bad;
    }
    EXPECT_NE(Refusal([&] { BuildBtcLockFromDeposit(rich, key, {}, 250'000, kChange, 1.0, 5'000); }), "");
    EXPECT_NE(Refusal([&] { BuildBtcLockFromDeposit(rich, key, kHtlc, 250'000, {}, 1.0, 5'000); }), "");
    EXPECT_NE(Refusal([&] { BuildBtcLockFromDeposit(rich, key, kHtlc, 100, kChange, 1.0, 5'000); }), "");
    // More coins than one transaction may spend.
    std::vector<BtcDepositCoin> many;
    for (uint8_t i = 0; i < 30; ++i) many.push_back(Coin(i, 10'000));
    EXPECT_EQ(Refusal([&] { BuildBtcLockFromDeposit(many, key, kHtlc, 250'000, kChange, 1.0, 50'000); }).rfind("deposit is short by ", 0), 0u);
    const auto fits = BuildBtcLockFromDeposit(many, key, kHtlc, 150'000, kChange, 1.0, 50'000);
    EXPECT_LE(fits.used.size(), kBtcMaxDepositInputs);
    ExpectSigned(fits, key);
}

TEST(BtcDeposit, SweepSendsEverythingBack) {
    const Bytes32 key = Key(11);
    const std::vector<BtcDepositCoin> coins{Coin(1, 60'000), Coin(2, 200'000), Coin(3, 1'000)};
    const auto plan = BuildBtcDepositSweep(coins, key, kChange, 3.0, 5'000);
    ASSERT_EQ(plan.tx.vout.size(), 1u);
    EXPECT_EQ(plan.used.size(), 2u);  // the 1,000-sat coin is not worth an input
    EXPECT_EQ(plan.tx.vout[0].script_pubkey, kChange);
    EXPECT_EQ(plan.tx.vout[0].value_sat, 260'000u - plan.fee_sat);
    EXPECT_EQ(plan.fee_sat, 3 * plan.vsize);
    ExpectSigned(plan, key);
    EXPECT_EQ(Refusal([&] { BuildBtcDepositSweep({Coin(1, 1'000)}, key, kChange, 1.0, 5'000); }),
              "no deposit payment large enough to send back");
    EXPECT_EQ(Refusal([&] { BuildBtcDepositSweep({Coin(1, 2'100)}, key, kChange, 15.0, 5'000); }),
              "the deposit is too small to send back after the network fee");
    EXPECT_NE(Refusal([&] { BuildBtcDepositSweep(coins, key, {}, 1.0, 5'000); }), "");
}
