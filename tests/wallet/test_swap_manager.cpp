// Swap manager: seed-derived keys, offer/accept between two wallets, index
// never reused (even after a restored backup), locked wallet refuses and
// pauses, cancel only before funds are locked, offers limited to the spendable
// balance, payout address parsing.
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
        return [this](const std::vector<std::vector<uint32_t>>& paths, bool require_funding_identity) -> std::optional<DerivedKeyBatch> {
            if (locked) return std::nullopt;
            const uint8_t captured_seed = seed;
            DerivedKeyBatch result;
            if (require_funding_identity) result.wallet_id.fill(captured_seed);
            result.keys.resize(paths.size());
            for (size_t i = 0; i < paths.size(); ++i) {
                const auto& path = paths[i];
                asked.push_back(path);
                crypto::CSHA256 h;
                h.Write(&captured_seed, 1);
                for (uint32_t c : path) {
                    const uint8_t b[4] = {uint8_t(c >> 24), uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
                    h.Write(b, 4);
                }
                h.Finalize(result.keys[i].data());
            }
            return result;
        };
    }
};

// One key of the wallet at a hardened path (the deriver answers in batches).
Bytes32 KeyAt(const KeyDeriver& derive, std::vector<uint32_t> path) {
    const auto batch = derive({std::move(path)}, false);
    if (!batch || batch->keys.size() != 1) throw std::runtime_error("no key");
    return batch->keys[0];
}

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

// What getblockchaininfo says about transaction time locks.
std::optional<Json::Value> ChainInfo(bool locks_active) {
    Json::Value i(Json::objectValue);
    i["contextual_locks_active"] = locks_active;
    return i;
}

// A Dinero node that reports its height (the scan start of a new swap), that
// it enforces time locks, and a wallet balance of `spendable_din` (DIN, as
// wallet.getbalance reports it).
DinRpc DinWithBalance(double spendable_din, bool locks_active = true) {
    return [spendable_din, locks_active](const std::string& m, const Json::Value&) -> std::optional<Json::Value> {
        if (m == "getblockcount") return Json::Value(1000);
        if (m == "getblockchaininfo") return ChainInfo(locks_active);
        if (m == "wallet.getbalance") {
            Json::Value b(Json::objectValue);
            b["spendable"] = spendable_din;
            return b;
        }
        return std::nullopt;
    };
}
const DinRpc kNoDin = DinWithBalance(1'000'000);
// A Bitcoin node that only reports its height (the scan start of a new swap).
const BtcRpc kNoBtc = [](const std::string& m, const Json::Value&) {
    return m == "getblockcount" ? std::optional<Json::Value>(Json::Value(500)) : std::optional<Json::Value>{};
};

OfferRequest Request() {
    OfferRequest r;
    r.din_amount_una = 10 * 100'000'000ULL;
    r.btc_amount_sat = 1'000'000;
    r.din_funding_fee_rate_hint = 2;
    r.din_funding_maximum_fee_una = 5'000'000;
    r.din_refund_address = P2trAddress("rdin", 0x21);
    r.btc_claim_address = P2trAddress("bcrt", 0x22);
    return r;
}


TEST(SwapManager, FundingSubmissionSummaryRetainsUnknownOutcomeAfterReopen) {
    TempDir da("funding_status_alice"), db("funding_status_bob");
    FakeWallet wa{0xc1}, wb{0xc2};
    std::string id;
    {
        SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
        SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
        const auto offer = alice.MakeOffer(Request(), kNow);
        const auto accepted = bob.Accept(offer, P2trAddress("rdin", 0x31), P2trAddress("bcrt", 0x32), kNow+1);
        ASSERT_TRUE(accepted.accept_text);
        id = alice.Accept(*accepted.accept_text, "", "", kNow+2).id;
    }
    const auto key = SwapStoreKeyFromSeed(wa.Deriver(), SwapNetwork::Regtest);
    const auto path = da.path + "/swap-0.swap";
    auto session = EncryptedFileSwapStore::Load(path, key);
    session.record.state = SwapState::DinLockBroadcast;
    const auto body_before_status = [&] {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    // The state alone (also in legacy sessions) proves no submission outcome.
    EncryptedFileSwapStore(path, key).Save(session);
    {
        const auto bytes = body_before_status();
        SwapManager reopened(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
        EXPECT_FALSE(reopened.Status(id).din_funding_submission_acknowledged.has_value());
        EXPECT_EQ(body_before_status(), bytes);
    }
    Transaction tx;
    tx.version = 2;
    tx.vin.resize(1);
    tx.vin[0].prevout = TxOutPoint(TxId(uint256::FromHexUnsafe(std::string(64, '1'))), 0);
    tx.vout.emplace_back(AmountUna::Una(session.record.offer.din_amount_una),
        BuildDinHtlc(MakeDinTerms(session.record.offer, session.record.accept)).script_pubkey);
    session.din_funding_raw = detail::ToHex(tx.Serialize(TxSerializationMode::WithWitness));
    session.din_funding_txid = tx.GetTxid().AsUint256().GetHex();
    for (bool acknowledged : {false, true}) {
        session.din_funding_submitted = acknowledged;
        EncryptedFileSwapStore(path, key).Save(session);
        const auto bytes = body_before_status();
        SwapManager reopened(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
        const auto summary = reopened.Status(id);
        ASSERT_TRUE(summary.din_funding_submission_acknowledged.has_value());
        EXPECT_EQ(*summary.din_funding_submission_acknowledged, acknowledged);
        EXPECT_EQ(summary.state, SwapState::DinLockBroadcast);
        EXPECT_EQ(body_before_status(), bytes);
    }
    wa.locked = true;
    SwapManager locked(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    const auto summary = locked.Status(id);
    EXPECT_TRUE(summary.wallet_locked);
    EXPECT_FALSE(summary.din_funding_submission_acknowledged.has_value());
}

TEST(SwapManager, FundingPayerAndFeeTermsSurviveOfferAcceptanceAndReopen) {
    TempDir da("funding_binding_alice"),db("funding_binding_bob");FakeWallet wa{0xa1},wb{0xb2};
    auto request=Request();request.din_funding_fee_rate_hint=11;request.din_funding_maximum_fee_una=7654321;
    std::string offer;
    {SwapManager alice(Config(da.path),wa.Deriver(),kNoDin,kNoBtc);offer=alice.MakeOffer(request,kNow);}
    const auto key=SwapStoreKeyFromSeed(wa.Deriver(),SwapNetwork::Regtest);
    const auto original=OpenSealedFile(da.path+"/offer-0.offer",key);
    EXPECT_NE(original.find("din_funding_fee_rate_hint=11\n"),std::string::npos);
    EXPECT_NE(original.find("din_funding_maximum_fee_una=7654321\n"),std::string::npos);
    SwapManager bob(Config(db.path),wb.Deriver(),kNoDin,kNoBtc);
    const auto accepted=bob.Accept(offer,P2trAddress("rdin",0x31),P2trAddress("bcrt",0x32),kNow+1);
    ASSERT_TRUE(accepted.accept_text);
    {SwapManager alice(Config(da.path),wa.Deriver(),kNoDin,kNoBtc);alice.Accept(*accepted.accept_text,"","",kNow+2);}
    const auto session=EncryptedFileSwapStore::Load(da.path+"/swap-0.swap",key);
    ASSERT_TRUE(session.din_funding_binding);Bytes32 expected{};expected.fill(wa.seed);
    EXPECT_EQ(session.din_funding_binding->wallet_id,expected);
    EXPECT_EQ(session.din_funding_binding->fee_rate_hint,request.din_funding_fee_rate_hint);
    EXPECT_EQ(session.din_funding_binding->maximum_fee_una,request.din_funding_maximum_fee_una);
    EXPECT_EQ(OfferId(session.record.offer),OfferId(DecodeOffer(offer)));
    EXPECT_FALSE(EncryptedFileSwapStore::Load(db.path+"/swap-0.swap",
        SwapStoreKeyFromSeed(wb.Deriver(),SwapNetwork::Regtest)).din_funding_binding);
}

TEST(SwapManager, FundingRefusesMissingBudgetAndMissingOrChangedDatabaseOwner) {
    TempDir da("funding_owner_refusal"),db("funding_owner_bob");FakeWallet wa{0xa3},wb{0xb4};
    SwapManager alice(Config(da.path),wa.Deriver(),kNoDin,kNoBtc);
    for(int mode=0;mode<2;++mode){auto request=Request();if(mode==0)request.din_funding_fee_rate_hint=0;else request.din_funding_maximum_fee_una=0;
        EXPECT_THROW(alice.MakeOffer(request,kNow),std::invalid_argument);EXPECT_FALSE(fs::exists(da.path+"/next_index"));}
    const auto offer=alice.MakeOffer(Request(),kNow);
    SwapManager bob(Config(db.path),wb.Deriver(),kNoDin,kNoBtc);
    const auto accepted=bob.Accept(offer,P2trAddress("rdin",0x31),P2trAddress("bcrt",0x32),kNow+1);ASSERT_TRUE(accepted.accept_text);
    const auto slurp=[](const std::string& path){std::ifstream in(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
    const auto original=slurp(da.path+"/offer-0.offer");
    for(int mode=0;mode<2;++mode){KeyDeriver changed=[&](const auto& paths,bool require_identity)->std::optional<DerivedKeyBatch>{auto batch=wa.Deriver()(paths,require_identity);batch->wallet_id.fill(mode==0?0:0x77);return batch;};
        SwapManager other(Config(da.path),changed,kNoDin,kNoBtc);
        EXPECT_THROW(other.Accept(*accepted.accept_text,"","",kNow+2),std::runtime_error);
        EXPECT_EQ(slurp(da.path+"/offer-0.offer"),original);EXPECT_FALSE(fs::exists(da.path+"/swap-0.swap"));}
    TempDir missing("funding_missing_owner");KeyDeriver absent=[&](const auto& paths,bool require_identity)->std::optional<DerivedKeyBatch>{auto batch=wa.Deriver()(paths,require_identity);batch->wallet_id={};return batch;};
    SwapManager unbound(Config(missing.path),absent,kNoDin,kNoBtc);
    EXPECT_THROW(unbound.MakeOffer(Request(),kNow),std::invalid_argument);EXPECT_FALSE(fs::exists(missing.path+"/offer-0.offer"));
    EXPECT_NO_THROW(alice.Accept(*accepted.accept_text,"","",kNow+2));
}

TEST(SwapManager, LegacyPartialAndDuplicateFundingBindingPreservePendingOffer) {
    TempDir da("funding_legacy"),db("funding_legacy_bob");FakeWallet wa{0xa5},wb{0xb6};
    SwapManager alice(Config(da.path),wa.Deriver(),kNoDin,kNoBtc);const auto offer=alice.MakeOffer(Request(),kNow);
    SwapManager bob(Config(db.path),wb.Deriver(),kNoDin,kNoBtc);const auto accepted=bob.Accept(offer,P2trAddress("rdin",0x31),P2trAddress("bcrt",0x32),kNow+1);ASSERT_TRUE(accepted.accept_text);
    const auto key=SwapStoreKeyFromSeed(wa.Deriver(),SwapNetwork::Regtest);const auto path=da.path+"/offer-0.offer";
    const auto original=OpenSealedFile(path,key);std::string legacy;
    {std::istringstream in(original);for(std::string line;std::getline(in,line);)if(line.rfind("din_funding_",0)!=0)legacy+=line+"\n";}
    for(const auto& malformed:{legacy,legacy+"din_funding_fee_rate_hint=2\n",original+"din_funding_fee_rate_hint=2\n"}){
        SealToFile(path,key,malformed);
        EXPECT_THROW(alice.Accept(*accepted.accept_text,"","",kNow+2),std::exception);
        EXPECT_EQ(OpenSealedFile(path,key),malformed);EXPECT_FALSE(fs::exists(da.path+"/swap-0.swap"));}
    SealToFile(path,key,original);EXPECT_NO_THROW(alice.Accept(*accepted.accept_text,"","",kNow+2));
}


TEST(SwapManager, LegacyClaimRefundKeysDoNotRequestFundingIdentity) {
    TempDir da("legacy_key_scope"),db("legacy_key_scope_bob");FakeWallet wa{0xa7},wb{0xb8};
    const auto key=SwapStoreKeyFromSeed(wa.Deriver(),SwapNetwork::Regtest);
    {SwapManager alice(Config(da.path),wa.Deriver(),kNoDin,kNoBtc),bob(Config(db.path),wb.Deriver(),kNoDin,kNoBtc);
     const auto offer=alice.MakeOffer(Request(),kNow);const auto accepted=bob.Accept(offer,P2trAddress("rdin",0x31),P2trAddress("bcrt",0x32),kNow+1);
     ASSERT_TRUE(accepted.accept_text);alice.Accept(*accepted.accept_text,"","",kNow+2);}
    const auto path=da.path+"/swap-0.swap";auto legacy=EncryptedFileSwapStore::Load(path,key);
    legacy.din_funding_binding.reset();legacy.record.state=SwapState::DinLocked;legacy.din_funding_txid=std::string(64,'a');
    EncryptedFileSwapStore(path,key).Save(legacy);
    size_t funding_requests=0,key_requests=0;
    KeyDeriver recovered=[&](const auto& paths,bool funding)->std::optional<DerivedKeyBatch>{
        if(funding){++funding_requests;throw std::runtime_error("legacy database has no funding identity");}
        ++key_requests;return wa.Deriver()(paths,false);
    };
    SwapManager reopened(Config(da.path),recovered,kNoDin,kNoBtc);
    EXPECT_TRUE(reopened.TickAll(kNow+3));EXPECT_GT(key_requests,0u);EXPECT_EQ(funding_requests,0u);
    const auto material=SwapKeysForIndex(recovered,SwapNetwork::Regtest,0);
    const auto original=SwapKeysForIndex(wa.Deriver(),SwapNetwork::Regtest,0);
    EXPECT_EQ(material.keys.din_secret_key,original.keys.din_secret_key);
    EXPECT_EQ(material.keys.btc_secret_key,original.keys.btc_secret_key);
    FundingOutput coin;coin.txid=TxId(uint256::FromHexUnsafe(legacy.din_funding_txid));coin.vout=0;
    coin.value=AmountUna::Una(legacy.record.offer.din_amount_una);
    coin.script_pubkey=BuildDinHtlc(MakeDinTerms(legacy.record.offer,legacy.record.accept)).script_pubkey;
    EXPECT_FALSE(SignedDinRefund(legacy,material.keys,coin,Config(da.path).runner.din_fee_una).empty());
    EXPECT_THROW(reopened.MakeOffer(Request(),kNow+4),std::runtime_error);EXPECT_EQ(funding_requests,1u);
    EXPECT_EQ(EncodeSession(EncryptedFileSwapStore::Load(path,key)),EncodeSession(legacy));
}

TEST(SwapManager, DerivationBatchShapeAndSingleCapture) {
    FakeWallet wallet{0x41};
    size_t calls = 0;
    std::vector<std::vector<uint32_t>> captured;
    KeyDeriver derive = [&](const auto& paths,bool require_identity) -> std::optional<DerivedKeyBatch> {
        ++calls; captured = paths; return wallet.Deriver()(paths,require_identity);
    };
    const auto material = SwapKeysForIndex(derive, SwapNetwork::Regtest, 7);
    EXPECT_EQ(calls, 1u);
    ASSERT_EQ(captured.size(), 4u);
    EXPECT_EQ(captured[0], (std::vector<uint32_t>{kSwapKeyPurpose, uint32_t(SwapNetwork::Regtest), 0}));
    EXPECT_EQ(material.store_key, SwapStoreKeyFromSeed(wallet.Deriver(), SwapNetwork::Regtest));
    for (size_t size : {0u, 3u, 5u}) {
        KeyDeriver malformed = [size](const auto&,bool) -> std::optional<DerivedKeyBatch> {
            DerivedKeyBatch batch; batch.keys.resize(size); return batch;
        };
        EXPECT_THROW(SwapKeysForIndex(malformed, SwapNetwork::Regtest, 7), std::invalid_argument);
    }
}

TEST(SwapManager, DerivedBatchMustMatchEncryptedOwner) {
    TempDir dir("batch_owner"); FakeWallet wallet{0x52};
    bool foreign_batch = true;
    KeyDeriver derive = [&](const auto& paths,bool require_identity) -> std::optional<DerivedKeyBatch> {
        // Serialized input selection, not a concurrent switch or removed lock.
        FakeWallet selected{uint8_t(paths.size() == 4 && foreign_batch ? 0x53 : wallet.seed)};
        return selected.Deriver()(paths,require_identity);
    };
    SwapManager manager(Config(dir.path), derive, kNoDin, kNoBtc);
    EXPECT_THROW(manager.MakeOffer(Request(), kNow), std::runtime_error);
    EXPECT_FALSE(fs::exists(dir.path + "/offer-0.offer"));
    EXPECT_FALSE(fs::exists(dir.path + "/offer-0.offer.id"));
    // Existing index allocation is retained, never reset/reused on refusal.
    std::ifstream counter(dir.path + "/next_index"); std::string next; counter >> next;
    EXPECT_EQ(next, "1");
    foreign_batch = false;
    const auto offer = DecodeOffer(manager.MakeOffer(Request(), kNow));
    const auto expected = SwapKeysForIndex(wallet.Deriver(), SwapNetwork::Regtest, 1);
    EXPECT_EQ(offer.din_refund_pubkey, detail::XOnlyOf(expected.keys.din_secret_key));
    EXPECT_EQ(offer.btc_claim_pubkey, detail::CompressedOf(expected.keys.btc_secret_key));
    EXPECT_TRUE(fs::exists(dir.path + "/offer-1.offer"));
    EXPECT_NO_THROW(manager.Cancel(SwapId(offer)));
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

TEST(SwapManager, ALockedWalletIsRefusedBeforeAnIndexIsUsed) {
    // After a relock the store key is still cached: new offers and accepts
    // must still be refused up front, without burning a swap index.
    TempDir da("li_a"), db("li_b");
    FakeWallet wa{0x7a}, wb{0x7b};
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    const auto offer = alice.MakeOffer(Request(), kNow);  // both store keys now cached
    bob.TickAll(kNow);
    auto next_index = [](const std::string& dir) {
        std::ifstream in(dir + "/next_index");
        uint32_t n = 0;
        in >> n;
        return n;
    };
    const uint32_t a0 = next_index(da.path), b0 = next_index(db.path);
    wa.locked = wb.locked = true;
    EXPECT_THROW(alice.MakeOffer(Request(), kNow), std::runtime_error);
    EXPECT_THROW(bob.Accept(offer, P2trAddress("rdin", 1), P2trAddress("bcrt", 2), kNow), std::runtime_error);
    EXPECT_EQ(next_index(da.path), a0) << "no index used by a refused offer";
    EXPECT_EQ(next_index(db.path), b0) << "no index used by a refused accept";
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

// An offer is refused unless the wallet can pay it: spendable DIN must cover
// its amount plus the funding fee allowance, on top of what open offers and
// unfunded swaps already promise. Request() offers 10 DIN.
TEST(SwapManager, OfferBeyondTheSpendableBalanceIsRefused) {
    TempDir d("balance_short");
    FakeWallet w{0x51};
    SwapManager m(Config(d.path), w.Deriver(), DinWithBalance(5), kNoBtc);
    try {
        m.MakeOffer(Request(), kNow);
        FAIL() << "a 10 DIN offer from a 5 DIN wallet was accepted";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("offer exceeds available balance"), std::string::npos) << e.what();
    }
    EXPECT_TRUE(m.List().empty()) << "nothing is written for a refused offer";
    // Exactly the amount is not enough: the funding fee needs room too.
    TempDir d2("balance_exact");
    SwapManager exact(Config(d2.path), w.Deriver(), DinWithBalance(10), kNoBtc);
    EXPECT_THROW(exact.MakeOffer(Request(), kNow), std::invalid_argument);
    TempDir d3("balance_enough");
    SwapManager enough(Config(d3.path), w.Deriver(), DinWithBalance(10.01), kNoBtc);
    EXPECT_NO_THROW(enough.MakeOffer(Request(), kNow));
}

TEST(SwapManager, OpenOffersAndUnfundedSwapsReserveTheirDin) {
    TempDir da("reserve_alice"), db("reserve_bob");
    FakeWallet wa{0x52}, wb{0x53};
    SwapManager alice(Config(da.path), wa.Deriver(), DinWithBalance(15), kNoBtc);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);

    const std::string offer = alice.MakeOffer(Request(), kNow);
    EXPECT_THROW(alice.MakeOffer(Request(), kNow + 1), std::invalid_argument)
        << "two 10 DIN offers against 15 DIN";

    // Still reserved once the offer turns into a swap that has not funded yet.
    const auto b = bob.Accept(offer, P2trAddress("rdin", 0x31), P2trAddress("bcrt", 0x32), kNow + 60);
    const auto a = alice.Accept(*b.accept_text, "", "", kNow + 120);
    ASSERT_EQ(alice.Status(a.id).state, SwapState::Accepted);
    EXPECT_THROW(alice.MakeOffer(Request(), kNow + 130), std::invalid_argument);

    // Cancelling releases it.
    alice.Cancel(a.id);
    EXPECT_NO_THROW(alice.MakeOffer(Request(), kNow + 140));
}

TEST(SwapManager, AnUnreadableBalanceRefusesTheOffer) {
    TempDir d("balance_unreadable");
    FakeWallet w{0x54};
    const DinRpc no_wallet = [](const std::string& m, const Json::Value&) -> std::optional<Json::Value> {
        if (m == "getblockchaininfo") return ChainInfo(true);
        return m == "getblockcount" ? std::optional<Json::Value>(Json::Value(1000)) : std::optional<Json::Value>{};
    };
    SwapManager m(Config(d.path), w.Deriver(), no_wallet, kNoBtc);
    EXPECT_THROW(m.MakeOffer(Request(), kNow), std::runtime_error);
    EXPECT_TRUE(m.List().empty());
}

// Dinero testnet does not enforce transaction time locks: Alice's refund
// would confirm at once, so neither side may start a swap there.
TEST(SwapManager, NoSwapOnADineroChainWithoutTimeLocks) {
    TempDir da("nolocks_a"), db("nolocks_b"), dc("nolocks_c");
    FakeWallet wa{0x61}, wb{0x62};
    const DinRpc no_locks = DinWithBalance(1'000'000, false);
    SwapManager alice_unsafe(Config(da.path), wa.Deriver(), no_locks, kNoBtc);
    EXPECT_THROW(alice_unsafe.MakeOffer(Request(), kNow), std::runtime_error);
    EXPECT_TRUE(alice_unsafe.List().empty());

    SwapManager alice(Config(dc.path), wa.Deriver(), kNoDin, kNoBtc);
    const auto offer = alice.MakeOffer(Request(), kNow);
    SwapManager bob_unsafe(Config(db.path), wb.Deriver(), no_locks, kNoBtc);
    EXPECT_THROW(bob_unsafe.Accept(offer, P2trAddress("rdin", 1), P2trAddress("bcrt", 2), kNow), std::runtime_error);
    EXPECT_TRUE(bob_unsafe.List().empty());
    // A node that does not say counts as one that does not enforce them.
    const DinRpc silent = [](const std::string& m, const Json::Value&) -> std::optional<Json::Value> {
        if (m == "getblockchaininfo") return Json::Value(Json::objectValue);
        return kNoDin(m, Json::Value());
    };
    SwapManager bob_silent(Config(db.path), wb.Deriver(), silent, kNoBtc);
    EXPECT_THROW(bob_silent.Accept(offer, P2trAddress("rdin", 1), P2trAddress("bcrt", 2), kNow), std::runtime_error);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    EXPECT_NO_THROW(bob.Accept(offer, P2trAddress("rdin", 1), P2trAddress("bcrt", 2), kNow));
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

// Exchanges and older wallets still hand out 1…/3… addresses: the BTC side
// takes them, on the node's own Bitcoin network only. DIN payouts stay bech32.
TEST(SwapManager, BtcPayoutScriptAcceptsLegacyAddresses) {
    const std::string pkh = "77bff20c60e522dfaa3350c39b030a5d004e839a";
    const std::string sh = "b472a266d0bd89c13706a4132ccfb16f7c3b9fcb";
    EXPECT_EQ(detail::ToHex(BtcPayoutScriptFromAddress("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2", "bc")), "76a914" + pkh + "88ac");
    EXPECT_EQ(detail::ToHex(BtcPayoutScriptFromAddress("3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy", "bc")), "a914" + sh + "87");
    for (const char* hrp : {"tb", "bcrt"}) {
        EXPECT_EQ(detail::ToHex(BtcPayoutScriptFromAddress("mrS8eVKXguwufwvsVe9GtgGb7fif9UQeAu", hrp)), "76a914" + pkh + "88ac");
        EXPECT_EQ(detail::ToHex(BtcPayoutScriptFromAddress("2N9hLwkSqr1cPQAPxbrGVUjxyjD11G2e1he", hrp)), "a914" + sh + "87");
    }
    // The all-zero hash keeps its leading zero bytes.
    EXPECT_EQ(detail::ToHex(BtcPayoutScriptFromAddress("1111111111111111111114oLvT2", "bc")),
              "76a914" + std::string(40, '0') + "88ac");
    // Segwit addresses are unchanged.
    EXPECT_EQ(BtcPayoutScriptFromAddress(P2trAddress("bcrt", 0x44), "bcrt"),
              PayoutScriptFromAddress(P2trAddress("bcrt", 0x44), "bcrt"));

    // Another network's address, a typo, or anything else is refused.
    EXPECT_THROW(BtcPayoutScriptFromAddress("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2", "bcrt"), std::invalid_argument);
    EXPECT_THROW(BtcPayoutScriptFromAddress("mrS8eVKXguwufwvsVe9GtgGb7fif9UQeAu", "bc"), std::invalid_argument);
    EXPECT_THROW(BtcPayoutScriptFromAddress("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN3", "bc"), std::invalid_argument);
    EXPECT_THROW(BtcPayoutScriptFromAddress("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN0", "bc"), std::invalid_argument);
    EXPECT_THROW(BtcPayoutScriptFromAddress("", "bc"), std::invalid_argument);
    EXPECT_THROW(BtcPayoutScriptFromAddress(std::string(200, '2'), "bc"), std::invalid_argument);
    EXPECT_THROW(BtcPayoutScriptFromAddress(P2trAddress("bc", 0x44), "bcrt"), std::invalid_argument);
    // A Dinero payout never takes a Bitcoin legacy address.
    EXPECT_THROW(PayoutScriptFromAddress("mrS8eVKXguwufwvsVe9GtgGb7fif9UQeAu", "rdin"), std::invalid_argument);
}

// Both entry points take a legacy Bitcoin address: the seller's claim address
// in the offer and the buyer's refund address in the accept.
TEST(SwapManager, OfferAndAcceptTakeLegacyBitcoinAddresses) {
    TempDir da("legacy_btc_alice"), db("legacy_btc_bob");
    FakeWallet wa{0xd1}, wb{0xd2};
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    OfferRequest r = Request();
    r.btc_claim_address = "mrS8eVKXguwufwvsVe9GtgGb7fif9UQeAu";
    std::string offer;
    ASSERT_NO_THROW(offer = alice.MakeOffer(r, kNow));
    EXPECT_NO_THROW(bob.Accept(offer, P2trAddress("rdin", 0x31), "2N9hLwkSqr1cPQAPxbrGVUjxyjD11G2e1he", kNow + 1));
    r.btc_claim_address = "1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2";  // mainnet address on a regtest node
    EXPECT_THROW(alice.MakeOffer(r, kNow), std::invalid_argument);
}

// Switching the key source is serialized here: no concurrent threads, IPC,
// network node or synchronization-removal control is involved. A manager's
// cached live session must not be rewritten under another wallet's store key.
TEST(SwapManager, ForeignWalletCannotReencryptCachedSessionDuringCancel) {
    TempDir da("owner_cancel_alice"), db("owner_cancel_bob");
    FakeWallet wa{0x61}, wb{0x62};
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    const auto accepted = bob.Accept(alice.MakeOffer(Request(), kNow),
        P2trAddress("rdin", 0x31), P2trAddress("bcrt", 0x32), kNow);
    ASSERT_TRUE(accepted.accept_text.has_value());
    const auto before_status = bob.Status(accepted.id);
    ASSERT_EQ(before_status.state, SwapState::Accepted);
    const auto owner_key = SwapStoreKeyFromSeed(wb.Deriver(), SwapNetwork::Regtest);
    const auto swap_path = db.path + "/swap-" + std::to_string(before_status.index) + ".swap";
    const auto snapshot = [](const std::string& directory) {
        std::map<std::string, std::string> files;
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (!entry.is_regular_file()) continue;
            std::ifstream in(entry.path(), std::ios::binary);
            EXPECT_TRUE(in.good());
            std::ostringstream contents;
            contents << in.rdbuf();
            files.emplace(entry.path().lexically_relative(directory).string(), contents.str());
        }
        return files;
    };
    const auto before = snapshot(db.path);
    ASSERT_FALSE(before.empty());

    // Equivalent library input to a later call deriving from a different
    // selected seed. It must refuse before replacing any existing file.
    wb.seed = 0x63;
    EXPECT_THROW(bob.Cancel(accepted.id), std::runtime_error);
    ASSERT_EQ(snapshot(db.path), before);
    const auto preserved = EncryptedFileSwapStore::Load(swap_path, owner_key);
    EXPECT_EQ(preserved.record.state, SwapState::Accepted);
    EXPECT_EQ(SwapId(preserved.record.offer), accepted.id);

    // Refusal must preserve the original owner and permit its ordinary retry.
    wb.seed = 0x62;
    ASSERT_NO_THROW(bob.Cancel(accepted.id));
    const auto cancelled = EncryptedFileSwapStore::Load(swap_path, owner_key);
    EXPECT_EQ(cancelled.record.state, SwapState::Aborted);
    EXPECT_EQ(SwapId(cancelled.record.offer), accepted.id);
}

// The sidecar is deliberately public and cannot authorize encrypted deletion.
// Every mutation below is serialized; no network, IPC or race control.
static std::map<std::string, std::string> PendingOfferOwnerFiles(const std::string& directory) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        EXPECT_TRUE(in.good());
        std::ostringstream data; data << in.rdbuf();
        files.emplace(entry.path().lexically_relative(directory).string(), data.str());
    }
    return files;
}

TEST(SwapManager, PendingOfferCancelRequiresAuthenticatedOwner) {
    TempDir d("pending_owner"); FakeWallet wallet{0x71};
    SwapManager manager(Config(d.path), wallet.Deriver(), kNoDin, kNoBtc);
    const auto text = manager.MakeOffer(Request(), kNow);
    const auto id = SwapId(DecodeOffer(text));
    const auto before = PendingOfferOwnerFiles(d.path);
    ASSERT_TRUE(before.count("offer-0.offer"));
    ASSERT_TRUE(before.count("offer-0.offer.id"));
    wallet.seed = 0x72;
    EXPECT_THROW(manager.Cancel(id), std::runtime_error);
    ASSERT_EQ(PendingOfferOwnerFiles(d.path), before);
    SwapManager foreign(Config(d.path), wallet.Deriver(), kNoDin, kNoBtc);
    EXPECT_THROW(foreign.Cancel(id), std::runtime_error);
    ASSERT_EQ(PendingOfferOwnerFiles(d.path), before);
    wallet.seed = 0x71; wallet.locked = true;
    SwapManager unopened(Config(d.path), wallet.Deriver(), kNoDin, kNoBtc);
    EXPECT_THROW(unopened.Cancel(id), std::runtime_error);
    ASSERT_EQ(PendingOfferOwnerFiles(d.path), before);
    // Established authority retains the existing cached-key relock behavior.
    ASSERT_NO_THROW(manager.Cancel(id));
    auto expected = before;
    expected.erase("offer-0.offer"); expected.erase("offer-0.offer.id");
    EXPECT_EQ(PendingOfferOwnerFiles(d.path), expected);
}

TEST(SwapManager, PendingOfferCancelRefusesSidecarAndCipherDamage) {
    TempDir d("pending_binding"); FakeWallet wallet{0x73};
    SwapManager manager(Config(d.path), wallet.Deriver(), kNoDin, kNoBtc);
    const auto text = manager.MakeOffer(Request(), kNow);
    const auto id = SwapId(DecodeOffer(text));
    const auto original = PendingOfferOwnerFiles(d.path);
    const std::string other_id = "0011223344556677";
    ASSERT_NE(id, other_id);
    const auto write = [&](const std::string& name, const std::string& data) {
        std::ofstream out(d.path + "/" + name, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()); out.write(data.data(), data.size()); out.close(); ASSERT_FALSE(out.fail());
    };
    write("offer-0.offer.id", other_id + "\n");
    const auto foreign_sidecar = PendingOfferOwnerFiles(d.path);
    EXPECT_THROW(manager.Cancel(other_id), std::runtime_error);
    ASSERT_EQ(PendingOfferOwnerFiles(d.path), foreign_sidecar);
    write("offer-0.offer.id", original.at("offer-0.offer.id"));
    auto damaged = original.at("offer-0.offer"); ASSERT_FALSE(damaged.empty());
    damaged.back() ^= 1;
    write("offer-0.offer", damaged);
    const auto corrupt = PendingOfferOwnerFiles(d.path);
    EXPECT_THROW(manager.Cancel(id), std::exception);
    ASSERT_EQ(PendingOfferOwnerFiles(d.path), corrupt);
    write("offer-0.offer", original.at("offer-0.offer"));
    ASSERT_EQ(PendingOfferOwnerFiles(d.path), original);
    ASSERT_NO_THROW(manager.Cancel(id));
    auto expected = original;
    expected.erase("offer-0.offer"); expected.erase("offer-0.offer.id");
    EXPECT_EQ(PendingOfferOwnerFiles(d.path), expected);
}

}  // namespace

// --- Zcash swaps (docs/design/din-zec-atomic-swaps.md) ---------------------------

namespace {

// A stand-in for swap_manager_zec.cpp: records what the manager asks for.
struct FakeZec {
    ZecDepositStatus status;              // what every deposit shows
    bool upgrade_ahead{false};            // require_no_upgrade refuses
    std::vector<std::pair<Bytes32, std::vector<uint8_t>>> sweeps;
    std::set<std::string> deposit_keys;   // hex of each deposit key asked about
    std::vector<bool> use_tower_seen;     // RunnerConfig::use_tower of each Zcash session started
    RpcSwapChainIo* last_io{nullptr};     // the chain IO of the newest Zcash session
    ZecSupport Support() {
        ZecSupport z;
        z.make_io = [this](const SwapSession& s, const RunnerConfig& rc, const Bytes32&) {
            use_tower_seen.push_back(rc.use_tower);
            auto io = std::make_unique<RpcSwapChainIo>(kNoDin, kNoBtc, s, rc);
            last_io = io.get();  // owned by the manager while the swap is live
            return io;
        };
        z.deposit_address = [this](const Bytes32& key) {
            deposit_keys.insert(ToHex(std::vector<uint8_t>(key.begin(), key.end())));
            return "tmDeposit" + ToHex(std::vector<uint8_t>(key.begin(), key.begin() + 4));
        };
        z.deposit_status = [this](const Bytes32&) { return status; };
        z.sweep_deposit = [this](const Bytes32& key, const std::vector<uint8_t>& to) {
            sweeps.emplace_back(key, to);
            return std::string(64, 'e');
        };
        z.payout_script = [](const std::string& address) {
            if (address.rfind("tm", 0) != 0) throw std::invalid_argument("not a regtest t-address: " + address);
            return std::vector<uint8_t>(address.begin(), address.end());
        };
        z.require_no_upgrade = [this](const SwapOffer&, uint32_t) {
            if (upgrade_ahead) throw std::runtime_error("Zcash network upgrade NU7 falls inside this swap");
        };
        z.tip_height = [] { return uint32_t{2000}; };
        return z;
    }
    static std::string ToHex(const std::vector<uint8_t>& b) { return detail::ToHex(b); }
};

OfferRequest ZecRequest() {
    OfferRequest r = Request();
    r.chain = CounterChain::Zcash;
    r.btc_amount_sat = 2'500'000;  // 0.025 ZEC
    r.btc_claim_address = "tmAliceClaim";
    return r;
}

}  // namespace

TEST(SwapManager, ZecKeyPathsLeaveBitcoinKeysUnchanged) {
    FakeWallet w{0x07};
    const auto d = w.Deriver();
    auto at = [&](uint32_t branch, uint32_t i) { return KeyAt(d, {kSwapKeyPurpose, uint32_t(SwapNetwork::Regtest), branch, i}); };
    const auto btc = SwapKeysForIndex(d, SwapNetwork::Regtest, 3);
    EXPECT_EQ(btc.keys.btc_secret_key, at(2, 3));  // BTC stays at 2'
    EXPECT_EQ(SwapKeysForIndex(d, SwapNetwork::Regtest, 3, CounterChain::Bitcoin).keys.btc_secret_key, at(2, 3));
    const auto zec = SwapKeysForIndex(d, SwapNetwork::Regtest, 3, CounterChain::Zcash);
    EXPECT_EQ(zec.keys.btc_secret_key, at(5, 3));  // ZEC HTLC key at 5'
    EXPECT_EQ(zec.keys.din_secret_key, btc.keys.din_secret_key);
    EXPECT_EQ(ZecDepositKey(d, SwapNetwork::Regtest, 3), at(6, 3));  // deposit at 6'
}

TEST(SwapManager, ZecNeedsZcashSupportAndIsRefusedOnMainnet) {
    TempDir dir("zec_support");
    FakeWallet w{0xa1};
    SwapManager plain(Config(dir.path), w.Deriver(), kNoDin, kNoBtc);
    EXPECT_FALSE(plain.SupportsZec());
    try {
        plain.MakeOffer(ZecRequest(), kNow);
        FAIL() << "a Zcash offer without Zcash support";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("built without Zcash"), std::string::npos) << e.what();
    }
    FakeZec z;
    auto mc = Config(dir.path);
    mc.network = SwapNetwork::Mainnet;
    SwapManager main(mc, w.Deriver(), kNoDin, kNoBtc);
    main.SetZecSupport(z.Support());
    EXPECT_FALSE(main.SupportsZec());
    EXPECT_THROW(main.MakeOffer(ZecRequest(), kNow), std::runtime_error);
}

TEST(SwapManager, ZecOfferIsV2WithFortyConfirmationsAndTheUpgradeGuard) {
    TempDir dir("zec_offer");
    FakeWallet w{0xa1};
    FakeZec z;
    SwapManager alice(Config(dir.path), w.Deriver(), kNoDin, kNoBtc);
    alice.SetZecSupport(z.Support());
    EXPECT_TRUE(alice.SupportsZec());
    auto r = ZecRequest();
    r.n_btc_confirmations = 3;  // raised to the Zcash minimum
    const auto text = alice.MakeOffer(r, kNow);
    EXPECT_EQ(text.rfind(kOffer2Prefix, 0), 0u);
    const auto o = DecodeOffer(text);
    EXPECT_EQ(o.chain, CounterChain::Zcash);
    EXPECT_EQ(o.n_btc_confirmations, kMinZecConfirmations);
    EXPECT_EQ(o.btc_claim_pubkey, detail::CompressedOf(KeyAt(w.Deriver(), {kSwapKeyPurpose, 2, 5, 0})));  // the 5' key
    ASSERT_EQ(alice.List().size(), 1u);
    EXPECT_EQ(alice.List()[0].chain, CounterChain::Zcash);

    r.btc_claim_address = "bcrt1notzcash";
    EXPECT_THROW(alice.MakeOffer(r, kNow), std::invalid_argument);
    r = ZecRequest();
    r.btc_amount_sat = 399'999;  // below 20 x the 20,000-zat spend fee
    EXPECT_THROW(alice.MakeOffer(r, kNow), std::invalid_argument);
    z.upgrade_ahead = true;
    EXPECT_THROW(alice.MakeOffer(ZecRequest(), kNow), std::runtime_error);
}

TEST(SwapManager, BobAcceptsAZecOfferOnlyFromAConfirmedDeposit) {
    TempDir da("zec_alice"), db("zec_bob");
    FakeWallet wa{0xa1}, wb{0xb0};
    FakeZec za, zb;
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    alice.SetZecSupport(za.Support());
    bob.SetZecSupport(zb.Support());
    const std::string offer = alice.MakeOffer(ZecRequest(), kNow);

    // Accepting before any deposit: refused, nothing created.
    EXPECT_THROW(bob.Accept(offer, P2trAddress("rdin", 0x31), "", kNow + 60), std::runtime_error);
    EXPECT_TRUE(bob.List().empty());

    const auto d = bob.PrepareDeposit(offer, "tmBobRefund", kNow + 60);
    EXPECT_EQ(d.amount_due_zat, 2'500'000u + 20'000u);
    EXPECT_EQ(d.address.rfind("tmDeposit", 0), 0u);
    EXPECT_EQ(bob.PrepareDeposit(offer, "tmBobRefund", kNow + 61).index, d.index);  // the same deposit again
    ASSERT_EQ(bob.List().size(), 1u);
    EXPECT_TRUE(bob.List()[0].pending_deposit);
    EXPECT_EQ(bob.List()[0].deposit_address, d.address);

    zb.status = {2'520'000, 2'519'999, 10};  // one zatoshi short at 10 confirmations
    try {
        bob.Accept(offer, P2trAddress("rdin", 0x31), "", kNow + 120);
        FAIL() << "accepted a short deposit";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("deposit is short by 0.00000001 ZEC"), std::string::npos) << e.what();
    }
    zb.status = {2'520'000, 2'520'000, 10};
    const auto b = bob.Accept(offer, P2trAddress("rdin", 0x31), "", kNow + 120);
    ASSERT_TRUE(b.accept_text.has_value());
    const auto list = bob.List();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_FALSE(list[0].pending_deposit);  // the deposit became the swap, same index
    EXPECT_EQ(list[0].index, d.index);
    EXPECT_EQ(list[0].chain, CounterChain::Zcash);
    // Bob's ZEC refund key is his 5' key for that index.
    EXPECT_EQ(DecodeAccept(*b.accept_text).btc_refund_pubkey,
              detail::CompressedOf(KeyAt(wb.Deriver(), {kSwapKeyPurpose, 2, 5, d.index})));
    const auto a = alice.Accept(*b.accept_text, "", "", kNow + 180);
    EXPECT_EQ(a.id, b.id);
}

TEST(SwapManager, CancellingAZecDepositSweepsItBack) {
    TempDir da("zec_cancel_a"), db("zec_cancel_b");
    FakeWallet wa{0xa1}, wb{0xb0};
    FakeZec za, zb;
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    alice.SetZecSupport(za.Support());
    bob.SetZecSupport(zb.Support());
    const auto d = bob.PrepareDeposit(alice.MakeOffer(ZecRequest(), kNow), "tmBobRefund", kNow + 60);

    bob.Cancel(d.id);  // nothing paid yet: nothing to sweep
    EXPECT_TRUE(zb.sweeps.empty());
    EXPECT_TRUE(bob.List().empty());

    const auto d2 = bob.PrepareDeposit(alice.MakeOffer(ZecRequest(), kNow), "tmBobRefund", kNow + 60);
    zb.status = {1'000'000, 0, 2};
    bob.Cancel(d2.id);
    ASSERT_EQ(zb.sweeps.size(), 1u);
    EXPECT_EQ(zb.sweeps[0].first, ZecDepositKey(wb.Deriver(), SwapNetwork::Regtest, d2.index));
    EXPECT_EQ(std::string(zb.sweeps[0].second.begin(), zb.sweeps[0].second.end()), "tmBobRefund");
    EXPECT_TRUE(bob.List().empty());

    // After accepting (nothing locked yet): cancel aborts and sweeps the deposit too.
    const std::string offer3 = alice.MakeOffer(ZecRequest(), kNow);
    const auto d3 = bob.PrepareDeposit(offer3, "tmBobRefund", kNow + 60);
    zb.status = {2'520'000, 2'520'000, 12};
    bob.Accept(offer3, P2trAddress("rdin", 0x31), "", kNow + 120);
    bob.Cancel(d3.id);
    EXPECT_EQ(zb.sweeps.size(), 2u);
    EXPECT_EQ(bob.Status(d3.id).state, SwapState::Aborted);
}

TEST(SwapManager, ZecSwapsNeverUseTheBitcoinOnlyTower) {
    TempDir da("zec_tower_a"), db("zec_tower_b");
    FakeWallet wa{0xa1}, wb{0xb0};
    FakeZec za, zb;
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    alice.SetZecSupport(za.Support());
    bob.SetZecSupport(zb.Support());
    std::vector<std::string> delivered;
    bob.SetTowerSink([&](const std::string& p) { delivered.push_back(p); });  // a tower inbox is configured
    bob.SetTowerAck([](const std::string&, const std::string&) { return true; });
    const std::string offer = alice.MakeOffer(ZecRequest(), kNow);
    bob.PrepareDeposit(offer, "tmBobRefund", kNow + 60);
    zb.status = {2'520'000, 2'520'000, 12};
    bob.Accept(offer, P2trAddress("rdin", 0x31), "", kNow + 120);
    ASSERT_FALSE(zb.use_tower_seen.empty());
    EXPECT_FALSE(zb.use_tower_seen.back());  // dinero-swap-tower would refuse a Zcash package
}

// A Zcash swap's DIN lock is paid by the wallet's funding owner, like a
// Bitcoin swap's: the Zcash chain IO must be handed that owner.
TEST(SwapManager, AZecSellerFundsDinThroughTheWalletFundingOwner) {
    TempDir da("zec_funding_owner_a"), db("zec_funding_owner_b");
    FakeWallet wa{0xa4}, wb{0xb4};
    FakeZec za, zb;
    size_t funding_calls = 0;
    DinFundingOwner owner = [&](const DinFundingRequest&, DinFundingOperation, const std::vector<uint8_t>&) -> PreparedDinFunding {
        ++funding_calls;
        throw std::runtime_error("test wallet: no coins");
    };
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc, owner);
    alice.SetZecSupport(za.Support());
    SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    bob.SetZecSupport(zb.Support());
    const std::string offer = alice.MakeOffer(ZecRequest(), kNow);
    bob.PrepareDeposit(offer, "tmBobRefund", kNow + 60);
    zb.status = {2'520'000, 2'520'000, 12};
    const auto accepted = bob.Accept(offer, P2trAddress("rdin", 0x31), "", kNow + 120);
    ASSERT_TRUE(accepted.accept_text.has_value());
    alice.Accept(*accepted.accept_text, "", "", kNow + 180);
    ASSERT_NE(za.last_io, nullptr);
    // The runner pays the DIN lock through this call on the session's chain IO.
    try {
        za.last_io->PrepareDinFunding(DinFundingRequest{});
        ADD_FAILURE() << "the test owner always throws";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "test wallet: no coins");  // not "DIN funding owner unavailable"
    }
    EXPECT_EQ(funding_calls, 1u);
}

// The deposit address a buyer pays must belong to the wallet that owns this
// manager's swaps: a key batch from another wallet is refused, not shown.
TEST(SwapManager, AZecDepositKeyFromAnotherWalletIsRefused) {
    TempDir db("zec_deposit_binding_b"), da("zec_deposit_binding_a");
    FakeWallet wa{0xa5}, wb{0xb5}, other{0xc5};
    FakeZec za, zb;
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    alice.SetZecSupport(za.Support());
    const std::string offer = alice.MakeOffer(ZecRequest(), kNow);
    // The wallet is switched between the store-key check and the deposit key.
    KeyDeriver switching = [&](const std::vector<std::vector<uint32_t>>& paths, bool funding) {
        bool deposit = false;
        for (const auto& path : paths) deposit = deposit || (path.size() == 4 && path[2] == 6);
        return deposit ? other.Deriver()(paths, funding) : wb.Deriver()(paths, funding);
    };
    SwapManager bob(Config(db.path), switching, kNoDin, kNoBtc);
    bob.SetZecSupport(zb.Support());
    EXPECT_THROW(bob.PrepareDeposit(offer, "tmBobRefund", kNow + 60), std::runtime_error);
    EXPECT_TRUE(zb.deposit_keys.empty());  // no address of the other wallet was ever produced
}

TEST(SwapManager, AZecSwapWithoutZcashSupportIsReportedAsUnwatched) {
    TempDir da("zec_unwatched_a"), db("zec_unwatched_b");
    FakeWallet wa{0xa1}, wb{0xb0};
    FakeZec za, zb;
    SwapManager alice(Config(da.path), wa.Deriver(), kNoDin, kNoBtc);
    alice.SetZecSupport(za.Support());
    const std::string offer = alice.MakeOffer(ZecRequest(), kNow);
    {
        SwapManager bob(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
        bob.SetZecSupport(zb.Support());
        const auto d = bob.PrepareDeposit(offer, "tmBobRefund", kNow + 60);
        EXPECT_EQ(bob.List()[0].deposit_offer, offer);  // a wallet can resume the deposit after a restart
        zb.status = {2'520'000, 2'520'000, 12};
        bob.Accept(offer, P2trAddress("rdin", 0x31), "", kNow + 120);
    }
    // The same swaps directory under a dinerod without Zcash support (rebuilt
    // with the option off, or swap.zec_rpc removed): it cannot watch the swap.
    SwapManager bare(Config(db.path), wb.Deriver(), kNoDin, kNoBtc);
    EXPECT_TRUE(bare.TickAll(kNow + 180));
    const auto list = bare.List();
    ASSERT_EQ(list.size(), 1u);
    ASSERT_FALSE(list[0].last_events.empty());
    EXPECT_NE(list[0].last_events[0].find("NOT being watched"), std::string::npos);
}

