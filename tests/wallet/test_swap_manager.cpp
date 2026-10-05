// Swap manager: seed-derived keys, offer/accept between two wallets, index
// never reused (even after a restored backup), locked wallet refuses and
// pauses, cancel only before funds are locked, payout address parsing.
#include "wallet/swap/swap_manager.h"

#include "bech32/bech32.hpp"
#include "crypto/sha256.h"
#include "wallet/swap/swap_crypto.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace {

using namespace dinero;
using namespace dinero::swap;
namespace fs = std::filesystem;

constexpr uint32_t kNow = 1'800'000'000;

// A stand-in for BIP32: key = SHA256(seed || path). Locked -> nullopt.
struct FakeWallet {
    uint8_t seed;
    bool locked{false};
    std::vector<std::vector<uint32_t>> asked;
    KeyDeriver Deriver() {
        return [this](const std::vector<uint32_t>& path) -> std::optional<Bytes32> {
            asked.push_back(path);
            if (locked) return std::nullopt;
            crypto::CSHA256 h;
            h.Write(&seed, 1);
            for (uint32_t c : path) {
                const uint8_t b[4] = {uint8_t(c >> 24), uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
                h.Write(b, 4);
            }
            Bytes32 k{};
            h.Finalize(k.data());
            return k;
        };
    }
};

std::string P2trAddress(const std::string& hrp, uint8_t fill) {
    return bech32::Encode(hrp, 1, std::vector<uint8_t>(32, fill), bech32::Encoding::BECH32M);
}

struct TempDir {
    std::string path;
    explicit TempDir(const std::string& name) : path((fs::temp_directory_path() / ("swapmgr_" + name)).string()) {
        fs::remove_all(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

SwapManagerConfig Config(const std::string& dir) {
    SwapManagerConfig c;
    c.dir = dir;
    c.network = SwapNetwork::Regtest;
    c.runner.din_hrp = "rdin";
    c.runner.btc_hrp = "bcrt";
    return c;
}

// A Dinero node that only reports its height (the scan start of a new swap).
const DinRpc kNoDin = [](const std::string& m, const Json::Value&) {
    return m == "getblockcount" ? std::optional<Json::Value>(Json::Value(1000)) : std::optional<Json::Value>{};
};
// A Bitcoin node that only reports its height (the scan start of a new swap).
const BtcRpc kNoBtc = [](const std::string& m, const Json::Value&) {
    return m == "getblockcount" ? std::optional<Json::Value>(Json::Value(500)) : std::optional<Json::Value>{};
};

OfferRequest Request() {
    OfferRequest r;
    r.din_amount_una = 10 * 100'000'000ULL;
    r.btc_amount_sat = 1'000'000;
    r.din_refund_address = P2trAddress("rdin", 0x21);
    r.btc_claim_address = P2trAddress("bcrt", 0x22);
    return r;
}

TEST(SwapManager, KeysAreSeedDerivedPerIndexAndNetwork) {
    FakeWallet w{0x01};
    const auto a = SwapKeysForIndex(w.Deriver(), SwapNetwork::Regtest, 0);
    const auto a2 = SwapKeysForIndex(w.Deriver(), SwapNetwork::Regtest, 0);
    const auto b = SwapKeysForIndex(w.Deriver(), SwapNetwork::Regtest, 1);
    const auto m = SwapKeysForIndex(w.Deriver(), SwapNetwork::Mainnet, 0);
    EXPECT_EQ(a.keys.din_secret_key, a2.keys.din_secret_key);

    EXPECT_NE(a.keys.din_secret_key, b.keys.din_secret_key);
    EXPECT_NE(a.keys.btc_secret_key, b.keys.btc_secret_key);
    EXPECT_NE(a.keys.din_secret_key, a.keys.btc_secret_key);

    EXPECT_NE(a.keys.din_secret_key, m.keys.din_secret_key) << "networks are separated";
    EXPECT_NE(SwapStoreKeyFromSeed(w.Deriver(), SwapNetwork::Regtest), a.keys.din_secret_key);
    for (const auto& path : w.asked) EXPECT_EQ(path.at(0), kSwapKeyPurpose);
    w.locked = true;
    EXPECT_THROW(SwapKeysForIndex(w.Deriver(), SwapNetwork::Regtest, 0), std::runtime_error);
}

TEST(SwapManager, OfferAcceptBetweenTwoWallets) {
    TempDir da("alice"), db("bob");
    FakeWallet wa{0xa1}, wb{0xb0};
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);

    const std::string offer = alice.MakeOffer(Request(), kNow);
    ASSERT_EQ(alice.List().size(), 1u);
    EXPECT_TRUE(alice.List()[0].pending_accept);

    const auto b = bob.Accept(offer, P2trAddress("rdin", 0x31), P2trAddress("bcrt", 0x32), kNow + 60);
    ASSERT_TRUE(b.accept_text.has_value());
    const auto a = alice.Accept(*b.accept_text, "", "", kNow + 120);
    EXPECT_FALSE(a.accept_text.has_value());
    EXPECT_EQ(a.id, b.id);

    const auto sa = alice.Status(a.id), sb = bob.Status(b.id);
    EXPECT_EQ(sa.role, Role::DinSeller);
    EXPECT_EQ(sb.role, Role::BtcSeller);
    EXPECT_EQ(sa.state, SwapState::Accepted);
    EXPECT_FALSE(sa.pending_accept);
    EXPECT_EQ(sa.din_amount_una, Request().din_amount_una);
    EXPECT_EQ(sa.t_din_unix, kNow + 96 * 3600);

    // Alice's secret matches the offer's payment hash; nothing secret is
    // readable on disk.
    const auto session = EncryptedFileSwapStore::Load(da.path + "/swap-" + std::to_string(sa.index) + ".swap",
                                                      SwapStoreKeyFromSeed(wa.Deriver(), SwapNetwork::Regtest));
    ASSERT_TRUE(session.record.secret.has_value());
    Bytes32 h{};
    crypto::CSHA256().Write(session.record.secret->data(), 32).Finalize(h.data());
    EXPECT_EQ(h, session.record.offer.payment_hash);
    const Bytes32 secret = *session.record.secret;
    for (const auto& e : fs::directory_iterator(da.path)) {
        std::ifstream in(e.path());
        std::stringstream t;
        t << in.rdbuf();
        EXPECT_EQ(t.str().find(detail::ToHex(std::vector<uint8_t>(secret.begin(), secret.end()))),
                  std::string::npos)
            << e.path();
    }
    // The same accept twice is refused (the swap already started).
    EXPECT_THROW(alice.Accept(*b.accept_text, "", "", kNow + 130), std::runtime_error);
}

TEST(SwapManager, AnIndexIsNeverReusedEvenAfterARestoredBackup) {
    TempDir d("idx");
    FakeWallet w{0x05};
    {
        SwapManager m(Config(d.path), w.Deriver(), kNoDin, kNoBtc);
        m.MakeOffer(Request(), kNow);
        m.MakeOffer(Request(), kNow + 1);
    }
    fs::remove(d.path + "/next_index");  // an older backup without the counter
    {
        SwapManager m(Config(d.path), w.Deriver(), kNoDin, kNoBtc);
        m.MakeOffer(Request(), kNow + 2);
        std::set<uint32_t> idx;
        for (const auto& s : m.List()) idx.insert(s.index);
        EXPECT_EQ(idx, (std::set<uint32_t>{0, 1, 2}));
    }
    std::ofstream(d.path + "/swap-7.swap") << "dinswap1e00";  // a swap file from elsewhere
    {
        SwapManager m(Config(d.path), w.Deriver(), kNoDin, kNoBtc);
        const auto offer = m.MakeOffer(Request(), kNow + 3);
        bool found8 = false;
        for (const auto& e : fs::directory_iterator(d.path)) found8 |= e.path().filename() == "offer-8.offer";
        EXPECT_TRUE(found8) << "next index is above every file on disk";
    }
}

TEST(SwapManager, LockedWalletRefusesToStartAndPausesTicks) {
    TempDir d("locked");
    FakeWallet w{0x07};
    SwapManager m(Config(d.path), w.Deriver(), kNoDin, kNoBtc);
    m.MakeOffer(Request(), kNow);
    w.locked = true;
    EXPECT_THROW(m.MakeOffer(Request(), kNow), std::runtime_error);
    // A daemon started while the wallet is locked has no keys: paused.
    SwapManager restarted(Config(d.path), w.Deriver(), kNoDin, kNoBtc);
    EXPECT_FALSE(restarted.TickAll(kNow));
    w.locked = false;
    EXPECT_TRUE(restarted.TickAll(kNow));
}

TEST(SwapManager, StatusOfAPausedSwapWorksWhileTheWalletIsLocked) {
    // After a restart with the wallet still locked, the user must see
    // "paused", not "unknown swap": the id is public and kept beside the file.
    TempDir da("pa"), db("pb");
    FakeWallet wa{0x1a}, wb{0x1b};
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    std::string id;
    {
        SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
        const auto b = bob.Accept(alice.MakeOffer(Request(), kNow), P2trAddress("rdin", 1), P2trAddress("bcrt", 2), kNow);
        id = b.id;
    }
    wb.locked = true;
    SwapManager restarted(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    const auto s = restarted.Status(id);
    EXPECT_TRUE(s.wallet_locked);
    EXPECT_EQ(s.id, id);
}

TEST(SwapManager, CancelOnlyBeforeAnythingIsLocked) {
    TempDir da("ca"), db("cb");
    FakeWallet wa{0x0a}, wb{0x0b};
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    const auto offer1 = alice.MakeOffer(Request(), kNow);
    const auto id1 = alice.List().at(0).id;
    alice.Cancel(id1);  // pending offer: nothing locked
    EXPECT_TRUE(alice.List().empty());

    const auto offer2 = alice.MakeOffer(Request(), kNow + 1);
    const auto b = bob.Accept(offer2, P2trAddress("rdin", 0x31), P2trAddress("bcrt", 0x32), kNow + 60);
    bob.Cancel(b.id);  // Bob, Accepted: nothing locked yet
    EXPECT_EQ(bob.Status(b.id).state, SwapState::Aborted);

    // Alice past Accepted (her DIN may be locked): refused.
    const auto a = alice.Accept(*b.accept_text, "", "", kNow + 120);
    {
        const auto key = SwapStoreKeyFromSeed(wa.Deriver(), SwapNetwork::Regtest);
        const auto idx = alice.Status(a.id).index;
        const std::string path = da.path + "/swap-" + std::to_string(idx) + ".swap";
        auto s = EncryptedFileSwapStore::Load(path, key);
        s.record.state = SwapState::DinLockBroadcast;
        EncryptedFileSwapStore(path, key).Save(s);
    }
    SwapManager alice2(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    EXPECT_THROW(alice2.Cancel(a.id), std::runtime_error);
}

TEST(SwapManager, AcceptRefusesWrongNetworkOrExpiredOffers) {
    TempDir da("na"), db("nb");
    FakeWallet wa{0x0c}, wb{0x0d};
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    auto main_cfg = Config(db.path);
    main_cfg.network = SwapNetwork::Mainnet;
    SwapManager bob_main(main_cfg, wb.Deriver(), kNoDin, kNoBtc);
    const auto offer = alice.MakeOffer(Request(), kNow);
    EXPECT_THROW(bob_main.Accept(offer, P2trAddress("rdin", 1), P2trAddress("bcrt", 2), kNow), std::invalid_argument);
    TempDir dc("nc");
    SwapManager bob(Config(dc.path), wb.Deriver(), kNoDin, kNoBtc);
    EXPECT_THROW(bob.Accept(offer, P2trAddress("rdin", 1), P2trAddress("bcrt", 2), kNow + 2 * 3600),
                 std::invalid_argument);
}

TEST(SwapManager, BetaCapsRefuseOversizedSwapsBothWays) {
    TempDir da("cap_a"), db("cap_b"), dc("cap_c");
    FakeWallet wa{0x2a}, wb{0x2b}, wc{0x2c};
    auto capped = Config(da.path);
    capped.max_btc_sat = 500'000;
    capped.max_din_una = 5 * 100'000'000ULL;
    SwapManager alice(capped, wa.Deriver(), kNoDin, kNoBtc);
    auto big = Request();  // 10 DIN / 0.01 BTC
    EXPECT_THROW(alice.MakeOffer(big, kNow), std::invalid_argument);
    auto small = Request();
    small.din_amount_una = 5 * 100'000'000ULL;
    small.btc_amount_sat = 500'000;
    EXPECT_NO_THROW(alice.MakeOffer(small, kNow));

    // A capped Bob refuses an oversized offer made by an uncapped Alice.
    SwapManager free_alice(Config(dc.path), wc.Deriver(), kNoDin, kNoBtc);
    const auto offer = free_alice.MakeOffer(big, kNow);
    auto bob_cfg = Config(db.path);
    bob_cfg.max_btc_sat = 500'000;
    SwapManager bob(bob_cfg, wb.Deriver(), kNoDin, kNoBtc);
    EXPECT_THROW(bob.Accept(offer, P2trAddress("rdin", 1), P2trAddress("bcrt", 2), kNow), std::invalid_argument);
    EXPECT_TRUE(bob.List().empty());
}

TEST(SwapManager, MainnetBetaNeedsOptInAndCapsCannotBeRaisedPastTheCeiling) {
    // Mainnet: refused without the explicit beta opt-in.
    EXPECT_TRUE(BetaPolicy(SwapNetwork::Mainnet, false, 0, 0).refusal.has_value());
    const auto d = BetaPolicy(SwapNetwork::Mainnet, true, 0, 0);
    ASSERT_FALSE(d.refusal.has_value());
    EXPECT_EQ(d.max_btc_sat, kBetaDefaultMaxBtcSat);
    EXPECT_EQ(d.max_din_una, kBetaDefaultMaxDinUna);
    // A configured limit may be lower, never above the compiled-in ceiling.
    EXPECT_EQ(BetaPolicy(SwapNetwork::Mainnet, true, 50'000, 0).max_btc_sat, 50'000u);
    EXPECT_EQ(BetaPolicy(SwapNetwork::Mainnet, true, 50'000'000, 0).max_btc_sat, kBetaHardMaxBtcSat);
    // Test networks: no opt-in, no caps.
    const auto r = BetaPolicy(SwapNetwork::Regtest, false, 0, 0);
    EXPECT_FALSE(r.refusal.has_value());
    EXPECT_EQ(r.max_btc_sat, 0u);
    EXPECT_EQ(r.max_din_una, 0u);
}

TEST(SwapManager, TheSecretIsFreshEvenWhenAnIndexRepeatsAfterARestore) {
    // Restoring the seed into an empty swap directory restarts at index 0: the
    // keys repeat, but the secret must not (a revealed old secret would let the
    // next taker claim Alice's DIN without paying).
    FakeWallet w{0x3a};
    Bytes32 h1{}, h2{};
    {
        TempDir d("fresh1");
        SwapManager m(Config(d.path), w.Deriver(), kNoDin, kNoBtc);
        h1 = DecodeOffer(m.MakeOffer(Request(), kNow)).payment_hash;
    }
    {
        TempDir d("fresh1");  // same path, emptied: a restore from seed
        SwapManager m(Config(d.path), w.Deriver(), kNoDin, kNoBtc);
        const auto o = DecodeOffer(m.MakeOffer(Request(), kNow));
        EXPECT_EQ(m.List().at(0).index, 0u) << "the index repeats";
        h2 = o.payment_hash;
    }
    EXPECT_NE(h1, h2);
}

TEST(SwapManager, PendingOffersAreNotReadableOnDisk) {
    TempDir d("sealed");
    FakeWallet w{0x3b};
    SwapManager m(Config(d.path), w.Deriver(), kNoDin, kNoBtc);
    const auto offer = m.MakeOffer(Request(), kNow);
    for (const auto& e : fs::directory_iterator(d.path)) {
        std::ifstream in(e.path());
        std::stringstream t;
        t << in.rdbuf();
        EXPECT_EQ(t.str().find("dinswap1o"), std::string::npos) << e.path() << " holds the offer in clear text";
    }
    w.locked = true;
    SwapManager locked(Config(d.path), w.Deriver(), kNoDin, kNoBtc);
    ASSERT_EQ(locked.List().size(), 1u);
    EXPECT_TRUE(locked.List()[0].wallet_locked);
    EXPECT_EQ(locked.List()[0].id, SwapId(DecodeOffer(offer)));
}

TEST(SwapManager, MainnetNeedsThreeBtcConfirmationsAndSwapsBigEnoughForFees) {
    TempDir dm("main3"), dr("min");
    FakeWallet w{0x4a};
    auto mainnet = Config(dm.path);
    mainnet.network = SwapNetwork::Mainnet;
    mainnet.runner.din_hrp = "din";
    mainnet.runner.btc_hrp = "bc";
    SwapManager m(mainnet, w.Deriver(), kNoDin, kNoBtc);
    auto r = Request();
    r.din_refund_address = P2trAddress("din", 0x21);
    r.btc_claim_address = P2trAddress("bc", 0x22);
    r.n_btc_confirmations = 1;
    EXPECT_THROW(m.MakeOffer(r, kNow), std::invalid_argument) << "mainnet: N_btc >= 3";
    r.n_btc_confirmations = 3;
    EXPECT_NO_THROW(m.MakeOffer(r, kNow));

    // Fees must stay small next to the amounts, on every network.
    SwapManager rt(Config(dr.path), w.Deriver(), kNoDin, kNoBtc);
    auto tiny = Request();
    tiny.btc_amount_sat = 15'000;  // < 20 x the 1000 sat fee
    EXPECT_THROW(rt.MakeOffer(tiny, kNow), std::invalid_argument);
    tiny = Request();
    tiny.din_amount_una = 50'000'000;  // < 100 x the 1,000,000 una urgent DIN fee
    EXPECT_THROW(rt.MakeOffer(tiny, kNow), std::invalid_argument);
}

TEST(SwapManager, FeeSettingsMustBePositiveAndBounded) {
    EXPECT_FALSE(FeeConfigProblem(100'000, 1'000'000, 1'000).has_value());
    EXPECT_TRUE(FeeConfigProblem(-1, 1'000'000, 1'000).has_value());
    EXPECT_TRUE(FeeConfigProblem(100'000, 0, 1'000).has_value());
    EXPECT_TRUE(FeeConfigProblem(100'000, 1'000'000, -5).has_value());
    EXPECT_TRUE(FeeConfigProblem(100'000, 50'000, 1'000).has_value()) << "urgent below the base fee";
    EXPECT_TRUE(FeeConfigProblem(100'000, 100'000'000'000LL, 1'000).has_value()) << "absurd";
}

TEST(SwapManager, LiveSwapsKeepRunningWhenTheWalletRelocks) {
    // A relock (unlock timeout) must not pause swaps already in flight: their
    // keys are held in memory. Only a restart while locked pauses them.
    TempDir da("rl_a"), db("rl_b");
    FakeWallet wa{0x5a}, wb{0x5b};
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    bob.Accept(alice.MakeOffer(Request(), kNow), P2trAddress("rdin", 1), P2trAddress("bcrt", 2), kNow);
    wb.locked = true;
    EXPECT_TRUE(bob.TickAll(kNow + 60));
    SwapManager restarted(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    EXPECT_FALSE(restarted.TickAll(kNow + 120));
}

TEST(SwapManager, MainnetBobNeedsAWatchtower) {
    TempDir da("mt_a"), db("mt_b");
    FakeWallet wa{0x5c}, wb{0x5d};
    auto cfg = [](const std::string& dir) {
        auto c = Config(dir);
        c.network = SwapNetwork::Mainnet;
        c.runner.din_hrp = "din";
        c.runner.btc_hrp = "bc";
        c.require_tower_for_bob = true;
        return c;
    };
    SwapManager alice(cfg(da.path), wa.Deriver(), kNoDin, kNoBtc);
    auto r = Request();
    r.din_refund_address = P2trAddress("din", 0x21);
    r.btc_claim_address = P2trAddress("bc", 0x22);
    const auto offer = alice.MakeOffer(r, kNow);
    SwapManager bob(cfg(db.path), wb.Deriver(), kNoDin, kNoBtc);
    EXPECT_THROW(bob.Accept(offer, P2trAddress("din", 1), P2trAddress("bc", 2), kNow), std::runtime_error);
    bob.SetTowerSink([](const std::string&) {});
    bob.SetTowerAck([](const std::string&, const std::string&) { return true; });
    EXPECT_NO_THROW(bob.Accept(offer, P2trAddress("din", 1), P2trAddress("bc", 2), kNow));
}

TEST(SwapManager, CancelWhileBobsFundingIsPreparedButUnsent) {
    TempDir da("pu_a"), db("pu_b");
    FakeWallet wa{0x5e}, wb{0x5f};
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    std::string id;
    uint32_t index = 0;
    {
        SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
        id = bob.Accept(alice.MakeOffer(Request(), kNow), P2trAddress("rdin", 1), P2trAddress("bcrt", 2), kNow).id;
        index = bob.Status(id).index;
    }
    const auto key = SwapStoreKeyFromSeed(wb.Deriver(), SwapNetwork::Regtest);
    const std::string path = db.path + "/swap-" + std::to_string(index) + ".swap";
    auto s = EncryptedFileSwapStore::Load(path, key);
    s.record.state = SwapState::BtcLockBroadcast;
    s.btc_funding_raw = "0200";  // signed, never sent (the tower did not confirm)
    EncryptedFileSwapStore(path, key).Save(s);
    // Bitcoin Core: the prepared input is still unspent, so nothing was sent.
    const BtcRpc unsent = [](const std::string& m, const Json::Value&) -> std::optional<Json::Value> {
        if (m == "getblockcount") return Json::Value(500);
        if (m == "decoderawtransaction") {
            Json::Value tx(Json::objectValue), in(Json::objectValue);
            in["txid"] = std::string(64, 'a');
            in["vout"] = 0;
            tx["vin"].append(in);
            return tx;
        }
        if (m == "gettxout") return Json::Value(Json::objectValue);
        if (m == "lockunspent") return Json::Value(true);
        return std::nullopt;
    };
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, unsent);
    EXPECT_NO_THROW(bob.Cancel(id));
    EXPECT_EQ(bob.Status(id).state, SwapState::Aborted);

    s.record.state = SwapState::BtcLockBroadcast;
    s.btc_funding_raw.clear();  // sent: BTC may be locked
    EncryptedFileSwapStore(path, key).Save(s);
    SwapManager bob2(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    EXPECT_THROW(bob2.Cancel(id), std::runtime_error);
}

TEST(SwapManager, CancelRefusesAPreparedFundingThatMayHaveBeenSent) {
    // The broadcast reply was lost: the raw is still in the session, but the
    // BTC may be in the HTLC. Cancel asks Bitcoin Core first: only when every
    // prepared input is still unspent (mempool included) was nothing sent;
    // then the inputs are unlocked and the swap aborted.
    TempDir da("ps_a"), db("ps_b");
    FakeWallet wa{0x6e}, wb{0x6f};
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    std::string id;
    uint32_t index = 0;
    {
        SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
        id = bob.Accept(alice.MakeOffer(Request(), kNow), P2trAddress("rdin", 1), P2trAddress("bcrt", 2), kNow).id;
        index = bob.Status(id).index;
    }
    const auto key = SwapStoreKeyFromSeed(wb.Deriver(), SwapNetwork::Regtest);
    const std::string path = db.path + "/swap-" + std::to_string(index) + ".swap";
    auto s = EncryptedFileSwapStore::Load(path, key);
    s.record.state = SwapState::BtcLockBroadcast;
    s.btc_funding_raw = "0200";
    EncryptedFileSwapStore(path, key).Save(s);

    enum class Core { Unspent, Spent, Down };
    auto core = [](Core c, int* unlocked) {
        return BtcRpc([c, unlocked](const std::string& m, const Json::Value&) -> std::optional<Json::Value> {
            if (m == "getblockcount") return Json::Value(500);
            if (c == Core::Down) return std::nullopt;
            if (m == "decoderawtransaction") {
                Json::Value tx(Json::objectValue), in(Json::objectValue);
                in["txid"] = std::string(64, 'a');
                in["vout"] = 1;
                tx["vin"].append(in);
                return tx;
            }
            if (m == "gettxout") return c == Core::Unspent ? Json::Value(Json::objectValue) : Json::Value();
            if (m == "lockunspent") { ++*unlocked; return Json::Value(true); }
            return std::nullopt;
        });
    };
    int unlocked = 0;
    for (const Core c : {Core::Spent, Core::Down}) {
        SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, core(c, &unlocked));
        EXPECT_THROW(bob.Cancel(id), std::runtime_error) << "may have been sent";
        EXPECT_EQ(bob.Status(id).state, SwapState::BtcLockBroadcast);
    }
    EXPECT_EQ(unlocked, 0);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, core(Core::Unspent, &unlocked));
    EXPECT_NO_THROW(bob.Cancel(id));
    EXPECT_EQ(bob.Status(id).state, SwapState::Aborted);
    EXPECT_EQ(unlocked, 1) << "the prepared inputs are released";
}

TEST(SwapManager, BobsSwapIsNotFinishedUntilHisDinIsSwept) {
    TempDir da("sw_a"), db("sw_b");
    FakeWallet wa{0x6a}, wb{0x6b};
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    std::string id;
    uint32_t index = 0;
    {
        SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
        id = bob.Accept(alice.MakeOffer(Request(), kNow), P2trAddress("rdin", 1), P2trAddress("bcrt", 2), kNow).id;
        index = bob.Status(id).index;
    }
    const auto key = SwapStoreKeyFromSeed(wb.Deriver(), SwapNetwork::Regtest);
    const std::string path = db.path + "/swap-" + std::to_string(index) + ".swap";
    auto s = EncryptedFileSwapStore::Load(path, key);
    EXPECT_NE(s.din_sweep_pubkey, Bytes32{}) << "Bob's claims pay a sweep output";
    s.record.state = SwapState::Done;
    EncryptedFileSwapStore(path, key).Save(s);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    bob.TickAll(kNow + 60);
    EXPECT_TRUE(bob.Status(id).sweep_pending) << "done on chain, DIN still at the swap key";
    s.din_swept = true;
    EncryptedFileSwapStore(path, key).Save(s);
    SwapManager bob2(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    bob2.TickAll(kNow + 120);
    EXPECT_FALSE(bob2.Status(id).sweep_pending);
}

TEST(SwapManager, PayoutScriptFromAddress) {
    const auto tr = PayoutScriptFromAddress(P2trAddress("bcrt", 0x44), "bcrt");
    EXPECT_EQ(tr.size(), 34u);
    EXPECT_EQ(tr[0], 0x51);
    const auto wpkh = PayoutScriptFromAddress(bech32::Encode("bcrt", 0, std::vector<uint8_t>(20, 1)), "bcrt");
    EXPECT_EQ(wpkh.size(), 22u);
    EXPECT_EQ(wpkh[0], 0x00);
    EXPECT_THROW(PayoutScriptFromAddress(P2trAddress("bcrt", 0x44), "rdin"), std::invalid_argument);  // wrong chain
    EXPECT_THROW(PayoutScriptFromAddress(bech32::Encode("bcrt", 1, std::vector<uint8_t>(32, 1)), "bcrt"),
                 std::invalid_argument);  // v1 must be bech32m
    EXPECT_THROW(PayoutScriptFromAddress(bech32::Encode("bcrt", 0, std::vector<uint8_t>(25, 1)), "bcrt"),
                 std::invalid_argument);  // bad program length
}

}  // namespace
