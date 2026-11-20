// The built-in Bitcoin wallet's memory and its balance reading, against a
// reader the test controls (no network).
#include "wallet/swap/btc_wallet.h"

#include "wallet/swap/swap_crypto.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>

using namespace dinero::swap;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    std::string path;
    explicit TempDir(const std::string& name) : path((fs::temp_directory_path() / ("btcwallet_" + name)).string()) {
        fs::remove_all(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

BtcWalletAccount Account(uint8_t fill) {
    Bytes32 key{}, chain{};
    key.fill(fill);
    chain.fill(static_cast<uint8_t>(fill + 1));
    return BtcWalletAccountFromSecret(key, chain);
}

// What the chain holds, by script; and every question asked.
struct Chain {
    std::map<std::vector<uint8_t>, BtcScriptActivity> at;
    size_t asked{0};
    bool reachable{true};
    void Pay(const BtcWalletAccount& a, uint32_t branch, uint32_t index, uint64_t sat) {
        at[BtcWalletScript(a, branch, index)] = {sat, true};
    }
    void Spend(const BtcWalletAccount& a, uint32_t branch, uint32_t index) { at[BtcWalletScript(a, branch, index)].sat = 0; }
    BtcWallet::ScriptReader Reader() {
        return [this](const std::vector<uint8_t>& script) {
            ++asked;
            if (!reachable) throw std::runtime_error("cannot reach any Bitcoin server");
            const auto it = at.find(script);
            return it == at.end() ? BtcScriptActivity{} : it->second;
        };
    }
};

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

}  // namespace

TEST(BtcWallet, IsOffUntilTurnedOnAndThenRemembersItsAccount) {
    TempDir d("on");
    const auto account = Account(0x11);
    EXPECT_FALSE(BtcWallet::Enabled(d.path));
    EXPECT_THROW(BtcWallet(d.path, "bcrt"), std::runtime_error);
    BtcWallet::Enable(d.path, account);
    EXPECT_TRUE(BtcWallet::Enabled(d.path));
    EXPECT_EQ(BtcWallet(d.path, "bcrt").account(), account);
    // Again with the same seed: nothing changes, the counters included.
    const std::string first = BtcWallet(d.path, "bcrt").NewReceiveAddress();
    const std::string before = ReadFile(BtcWallet::FilePath(d.path));
    BtcWallet::Enable(d.path, account);
    EXPECT_EQ(ReadFile(BtcWallet::FilePath(d.path)), before);
    EXPECT_EQ(BtcWallet(d.path, "bcrt").CurrentReceiveAddress(), first);
    // Another seed is refused: its addresses are not this wallet's.
    EXPECT_THROW(BtcWallet::Enable(d.path, Account(0x22)), std::runtime_error);
    EXPECT_EQ(BtcWallet(d.path, "bcrt").account(), account);
    // The file holds no private key: the account's secret is 32 bytes of 0x11.
    EXPECT_EQ(before.find(std::string(16, '1')), std::string::npos);
}

TEST(BtcWallet, ReceiveAddressesAreHandedOutOnceAndSurviveARestart) {
    TempDir d("recv");
    const auto account = Account(0x31);
    BtcWallet::Enable(d.path, account);
    std::string a0, a1;
    {
        BtcWallet w(d.path, "bcrt");
        EXPECT_EQ(w.CurrentReceiveAddress(), BtcWalletAddress(account, 0, 0, "bcrt"));  // the first is handed out on demand
        a0 = w.CurrentReceiveAddress();
        a1 = w.NewReceiveAddress();
        EXPECT_EQ(a1, BtcWalletAddress(account, 0, 1, "bcrt"));
        EXPECT_NE(a0, a1);
        EXPECT_EQ(w.CurrentReceiveAddress(), a1);
    }
    BtcWallet again(d.path, "bcrt");
    EXPECT_EQ(again.receive_handed_out(), 2u);
    EXPECT_EQ(again.CurrentReceiveAddress(), a1);
    EXPECT_EQ(again.NewReceiveAddress(), BtcWalletAddress(account, 0, 2, "bcrt"));
}

TEST(BtcWallet, StopsHandingOutAddressesARestoreWouldNotFind) {
    TempDir d("gap");
    const auto account = Account(0x41);
    BtcWallet::Enable(d.path, account);
    BtcWallet w(d.path, "bcrt");
    for (uint32_t i = 0; i < kBtcWalletGapLimit; ++i) EXPECT_EQ(w.NewReceiveAddress(), BtcWalletAddress(account, 0, i, "bcrt"));
    try {
        w.NewReceiveAddress();
        ADD_FAILURE() << "handed out a 21st unused address";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("have not been paid yet"), std::string::npos) << e.what();
    }
    EXPECT_EQ(w.receive_handed_out(), kBtcWalletGapLimit);
    // One of them is paid: the window moves with it.
    Chain chain;
    chain.Pay(account, 0, 4, 1000);
    w.ReadBalance(chain.Reader());
    for (uint32_t i = kBtcWalletGapLimit; i < 5 + kBtcWalletGapLimit; ++i) {
        EXPECT_EQ(w.NewReceiveAddress(), BtcWalletAddress(account, 0, i, "bcrt"));
    }
    EXPECT_THROW(w.NewReceiveAddress(), std::runtime_error);
}

TEST(BtcWallet, BalanceIsTheSumOfBothBranches) {
    TempDir d("bal");
    const auto account = Account(0x51);
    BtcWallet::Enable(d.path, account);
    BtcWallet w(d.path, "bcrt");
    Chain chain;
    auto b = w.ReadBalance(chain.Reader());
    EXPECT_EQ(b.sat, 0u);
    EXPECT_TRUE(b.rows.empty());
    EXPECT_EQ(b.addresses_read, 2 * kBtcWalletGapLimit);  // the gap on each branch, nothing more

    chain.Pay(account, 0, 0, 29'000);
    chain.Pay(account, 0, 3, 500);
    chain.Pay(account, 1, 0, 2'817);
    chain.Pay(account, 0, 2, 7'000);
    chain.Spend(account, 0, 2);  // paid once, spent since: no balance, but it counts as used
    b = w.ReadBalance(chain.Reader());
    EXPECT_EQ(b.sat, 32'317u);
    ASSERT_EQ(b.rows.size(), 3u);
    EXPECT_EQ(b.rows[0].address, BtcWalletAddress(account, 0, 0, "bcrt"));
    EXPECT_EQ(b.rows[0].sat, 29'000u);
    EXPECT_EQ(b.rows[1].index, 3u);
    EXPECT_EQ(b.rows[2].branch, 1u);
    EXPECT_EQ(b.rows[2].address, BtcWalletAddress(account, 1, 0, "bcrt"));
    // The next address is past everything that was ever paid, also the spent one.
    EXPECT_EQ(w.NewReceiveAddress(), BtcWalletAddress(account, 0, 4, "bcrt"));
}

TEST(BtcWallet, ARestoredWalletFindsItsCoinsWithoutAnyRecord) {
    // The old node handed out 31 addresses over time; coins sit at receive 18
    // and 30, and at change 7. The new node knows only the seed.
    TempDir d("restore");
    const auto account = Account(0x61);
    Chain chain;
    chain.Pay(account, 0, 5, 0);  // paid long ago and spent
    chain.at[BtcWalletScript(account, 0, 5)] = {0, true};
    chain.Pay(account, 0, 18, 40'000);
    chain.Pay(account, 0, 30, 6'000);   // within 20 of index 18
    chain.Pay(account, 1, 7, 900);
    chain.Pay(account, 0, 60, 123);     // beyond the gap of anything used: not this wallet's by the standard rule
    BtcWallet::Enable(d.path, account);
    BtcWallet w(d.path, "bcrt");
    const auto b = w.ReadBalance(chain.Reader());
    EXPECT_EQ(b.sat, 46'900u);
    EXPECT_EQ(b.rows.size(), 3u);
    EXPECT_EQ(b.addresses_read, (31 + kBtcWalletGapLimit) + (8 + kBtcWalletGapLimit));
    // What it learned is kept: the next address is 31, also after a restart.
    EXPECT_EQ(BtcWallet(d.path, "bcrt").NewReceiveAddress(), BtcWalletAddress(account, 0, 31, "bcrt"));
}

TEST(BtcWallet, AnAddressThatWasPaidAndSpentStillCountsAsUsed) {
    // Receive 10 was paid and spent long ago; the coins are at receive 25.
    // Only the spent address carries the search past the first 20.
    TempDir d("spent");
    const auto account = Account(0x91);
    Chain chain;
    chain.at[BtcWalletScript(account, 0, 10)] = {0, true};
    chain.Pay(account, 0, 25, 77'000);
    BtcWallet::Enable(d.path, account);
    BtcWallet w(d.path, "bcrt");
    EXPECT_EQ(w.ReadBalance(chain.Reader()).sat, 77'000u);
    // And an address that was paid is never handed out again, empty or not.
    TempDir d2("spent2");
    Chain only_spent;
    only_spent.at[BtcWalletScript(account, 0, 9)] = {0, true};
    BtcWallet::Enable(d2.path, account);
    BtcWallet w2(d2.path, "bcrt");
    EXPECT_EQ(w2.ReadBalance(only_spent.Reader()).sat, 0u);
    EXPECT_EQ(w2.NewReceiveAddress(), BtcWalletAddress(account, 0, 10, "bcrt"));
}

TEST(BtcWallet, AnUnreachableServerIsAnErrorNotAnEmptyWallet) {
    TempDir d("down");
    const auto account = Account(0x71);
    BtcWallet::Enable(d.path, account);
    BtcWallet w(d.path, "bcrt");
    Chain chain;
    chain.Pay(account, 0, 0, 5'000);
    chain.reachable = false;
    const std::string before = ReadFile(BtcWallet::FilePath(d.path));
    EXPECT_THROW(w.ReadBalance(chain.Reader()), std::runtime_error);
    EXPECT_EQ(ReadFile(BtcWallet::FilePath(d.path)), before);  // nothing learned, nothing written
    chain.reachable = true;
    EXPECT_EQ(w.ReadBalance(chain.Reader()).sat, 5'000u);
}

TEST(BtcWallet, ADamagedFileIsRefusedAndRebuiltFromTheSeed) {
    TempDir d("damage");
    const auto account = Account(0x81);
    BtcWallet::Enable(d.path, account);
    { BtcWallet w(d.path, "bcrt"); w.NewReceiveAddress(); w.NewReceiveAddress(); }
    const std::string path = BtcWallet::FilePath(d.path);
    std::string text = ReadFile(path);
    // One hex digit of the account changed: other addresses, silently, without the check.
    const size_t at = text.find("account ") + 20;
    text[at] = text[at] == '0' ? '1' : '0';
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << text; }
    EXPECT_THROW(BtcWallet(d.path, "bcrt"), std::runtime_error);
    // A counter changed.
    text = ReadFile(path);
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << "garbage"; }
    EXPECT_THROW(BtcWallet(d.path, "bcrt"), std::runtime_error);
    // Turning it on again (the account comes from the seed) rebuilds it; the chain says what was used.
    BtcWallet::Enable(d.path, account);
    BtcWallet w(d.path, "bcrt");
    EXPECT_EQ(w.account(), account);
    Chain chain;
    chain.Pay(account, 0, 1, 800);
    EXPECT_EQ(w.ReadBalance(chain.Reader()).sat, 800u);
    EXPECT_EQ(w.NewReceiveAddress(), BtcWalletAddress(account, 0, 2, "bcrt"));
}

TEST(BtcWallet, RemembersWhatItSentSoCoinsAreNotSpentTwice) {
    TempDir d("sent");
    const auto account = Account(0xa1);
    BtcWallet::Enable(d.path, account);
    Chain chain;
    BtcScriptCoin c1, c2;
    c1.txid.fill(0x01); c1.vout = 0; c1.value_sat = 60'000;
    c2.txid.fill(0x02); c2.vout = 1; c2.value_sat = 9'000;
    chain.at[BtcWalletScript(account, 0, 0)] = {69'000, true, {c1, c2}};
    {
        BtcWallet w(d.path, "bcrt");
        const auto b = w.ReadBalance(chain.Reader());
        ASSERT_EQ(b.coins.size(), 2u);
        EXPECT_EQ(b.coins[0].branch, 0u);
        EXPECT_EQ(b.coins[0].index, 0u);
        EXPECT_EQ(w.WithoutSpent(b.coins).size(), 2u);
        EXPECT_EQ(w.change_index(), 0u);
        const auto change_before = w.ChangeScript();

        BtcWallet::Sent sent;
        sent.txid_hex = std::string(64, 'a');
        sent.raw = {0x02, 0x00, 0x00, 0x00, 0xff};
        sent.amount_sat = 20'000; sent.fee_sat = 300; sent.change_sat = 39'700;
        sent.spent = {c1};
        w.RecordSent(sent, true);
        // The servers have not heard of the payment yet and still list c1: it is not offered again.
        const auto left = w.WithoutSpent(b.coins);
        ASSERT_EQ(left.size(), 1u);
        EXPECT_EQ(left[0].txid, c2.txid);
        // Its change address is taken: the next payment's change goes elsewhere.
        EXPECT_EQ(w.change_index(), 1u);
        EXPECT_NE(w.ChangeScript(), change_before);
    }
    // All of it survives a restart.
    BtcWallet again(d.path, "bcrt");
    const auto sent = again.SentPayments();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].txid_hex, std::string(64, 'a'));
    EXPECT_EQ(sent[0].raw, (std::vector<uint8_t>{0x02, 0x00, 0x00, 0x00, 0xff}));
    EXPECT_EQ(sent[0].amount_sat, 20'000u);
    EXPECT_EQ(sent[0].fee_sat, 300u);
    EXPECT_EQ(sent[0].change_sat, 39'700u);
    ASSERT_EQ(sent[0].spent.size(), 1u);
    EXPECT_EQ(sent[0].spent[0].txid, c1.txid);
    EXPECT_EQ(again.change_index(), 1u);
    // A send-all (no change) leaves the change address alone; a torn line is skipped, not fatal.
    BtcWallet::Sent all;
    all.txid_hex = std::string(64, 'b');
    all.raw = {0x01};
    all.spent = {c2};
    again.RecordSent(all, false);
    EXPECT_EQ(again.change_index(), 1u);
    { std::ofstream out(d.path + "/btc-wallet.sent", std::ios::app); out << "deadbeef 1 2"; }
    EXPECT_EQ(BtcWallet(d.path, "bcrt").SentPayments().size(), 2u);
    EXPECT_TRUE(BtcWallet(d.path, "bcrt").WithoutSpent(BtcWallet(d.path, "bcrt").ReadBalance(chain.Reader()).coins).empty());
}
