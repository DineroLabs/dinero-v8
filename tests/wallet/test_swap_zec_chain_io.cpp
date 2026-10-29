// The Zcash counter chain for the swap runner (zec_chain_io): the hooks the
// runner calls, against a canned Zebra (milestone 5 of
// docs/design/din-zec-atomic-swaps.md).

#include "wallet/swap/zec_chain_io.h"

#include "crypto/sha256.h"
#include "dinero_zcash_swap.h"
#include "wallet/swap/swap_crypto.h"
#include "wallet/swap/tower.h"

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace {

using namespace dinero;
using namespace dinero::swap;

constexpr uint32_t kNow = 1'800'000'000;
constexpr uint32_t kHour = 3600;
constexpr uint32_t kNu61 = 0x4dec4df0;

Bytes32 Key(uint8_t scalar) {
    Bytes32 s{};
    s.back() = scalar;
    return s;
}

Bytes32 XOnly(uint8_t scalar) { return detail::XOnlyOf(Key(scalar)); }

// A canned Zebra: tip, branch, upgrades, deposit coins, known transactions.
struct FakeZebra {
    uint32_t tip = 1000;
    std::string next_branch = "4dec4df0";
    uint32_t upgrade_height = 0;  // 0: none pending
    std::string deposit_address;
    Json::Value utxos{Json::arrayValue};
    std::set<std::string> known_txids;
    bool reachable = true;
    std::vector<std::string> sent;

    ZecRpc Rpc() {
        return [this](const std::string& m, const Json::Value& p) -> std::optional<Json::Value> {
            if (!reachable) return std::nullopt;
            Json::Value r;
            if (m == "getblockchaininfo") {
                r["chain"] = "test";
                r["blocks"] = tip;
                r["consensus"]["nextblock"] = next_branch;
                r["upgrades"] = Json::Value(Json::objectValue);
                if (upgrade_height) {
                    r["upgrades"]["5437f330"]["name"] = "NU6.2";
                    r["upgrades"]["5437f330"]["activationheight"] = upgrade_height;
                }
                return r;
            }
            if (m == "getblockhash") return Json::Value("h" + std::to_string(p[0].asUInt()));
            if (m == "getblockheader") {
                r["time"] = Json::UInt(kNow - (tip - std::stoul(p[0].asString().substr(1))) * 75);
                return r;
            }
            if (m == "getblock") {
                r["tx"] = Json::Value(Json::arrayValue);
                return r;
            }
            if (m == "getaddressutxos") {
                r["utxos"] = utxos;
                r["height"] = tip;
                return r;
            }
            if (m == "getrawtransaction") {
                if (!known_txids.count(p[0].asString())) return std::nullopt;
                return Json::Value("00");
            }
            if (m == "getrawmempool") return Json::Value(Json::arrayValue);
            return std::nullopt;
        };
    }

    void AddCoin(uint8_t id, uint64_t zat, uint32_t confirmations) {
        Json::Value u;
        u["address"] = deposit_address;
        u["txid"] = std::string(62, '0') + (id < 16 ? "0" : "") + [&] {
            char b[3];
            std::snprintf(b, sizeof b, "%x", id);
            return std::string(b);
        }();
        u["outputIndex"] = 0;
        u["satoshis"] = Json::UInt64(zat);
        u["height"] = tip - confirmations + 1;
        utxos.append(u);
    }
};

struct ZecSwap {
    SwapOffer offer;
    SwapAccept accept;
    std::vector<uint8_t> secret = std::vector<uint8_t>(32, 0x5a);
    Bytes32 deposit_key = Key(8);
    FakeZebra zebra;

    ZecSwap() {
        offer.network = SwapNetwork::Regtest;
        offer.chain = CounterChain::Zcash;
        offer.din_amount_una = 100'000'000'000;
        offer.btc_amount_sat = 1'000'000;
        crypto::CSHA256().Write(secret.data(), secret.size()).Finalize(offer.payment_hash.data());
        offer.din_refund_pubkey = XOnly(3);
        offer.btc_claim_pubkey = detail::CompressedOf(Key(4));
        offer.expires_unix = kNow + 2 * kHour;
        offer.t_btc_unix = kNow + 48 * kHour;
        offer.t_din_unix = kNow + 96 * kHour;
        offer.n_din_confirmations = 30;
        offer.n_btc_confirmations = kMinZecConfirmations;
        accept.offer_id = OfferId(offer);
        accept.din_claim_pubkey = XOnly(5);
        accept.btc_refund_pubkey = detail::CompressedOf(Key(6));
        zebra.deposit_address =
            EncodeZecTAddress(ZecP2pkhAddressOf(detail::CompressedOf(deposit_key), ZecNetwork::Regtest));
    }

    SwapSession Session(Role role) const {
        SwapSession s;
        s.record.role = role;
        s.record.offer = offer;
        s.record.accept = accept;
        if (role == Role::DinSeller) {
            Bytes32 sec{};
            std::copy(secret.begin(), secret.end(), sec.begin());
            s.record.secret = sec;
        }
        s.din_payout_script = {0x51};
        s.btc_payout_script = ZecScriptPubKey({ZecNetwork::Regtest, ZecAddressKind::P2pkh, {0x09}});
        return s;
    }

    std::unique_ptr<ZecSwapChainIo> Io(std::vector<std::string>* sent = nullptr) {
        SwapWatchSpec spec;
        spec.offer = offer;
        spec.accept = accept;
        ZecIoConfig c;
        c.expected_chain = "test";
        c.network = ZecNetwork::Regtest;
        c.deposit_secret_key = deposit_key;
        c.change_script = Session(Role::BtcSeller).btc_payout_script;
        c.clock = [] { return kNow; };
        c.broadcast = [sent](const std::string& hex) {
            if (sent) sent->push_back(hex);
            return std::string(64, 'a');
        };
        DinRpc din = [](const std::string&, const Json::Value&) { return std::optional<Json::Value>{}; };
        return std::make_unique<ZecSwapChainIo>(din, zebra.Rpc(), spec, c);
    }

    std::vector<uint8_t> HtlcSpk() const { return ZecP2shScriptPubKey(BuildZecHtlcRedeemScript(MakeBtcTerms(offer, accept))); }
};

int32_t VerifyInput(const std::vector<uint8_t>& tx, size_t index, const std::vector<ZecTxOut>& prevouts) {
    std::vector<dzs_txout> ffi;
    for (const auto& p : prevouts) ffi.push_back({p.value_zat, p.script_pubkey.data(), p.script_pubkey.size()});
    return dzs_verify_input(tx.data(), tx.size(), index, ffi.data(), ffi.size());
}

}  // namespace

TEST(SwapZecChainIo, RulesAndFees) {
    ZecSwap z;
    auto io = z.Io();
    const auto r = io->CounterRules();
    EXPECT_FALSE(r.exclusive_windows);          // claim and refund race after T_zec
    EXPECT_EQ(r.claim_cutoff_seconds, 6 * kHour);
    EXPECT_EQ(r.early_refund_seconds, 90u * 60);  // block time may run 90 min past the median
    EXPECT_TRUE(io->CounterFundsByPreparing());
    // No RBF: the fee does not grow with time.
    auto s = z.Session(Role::DinSeller);
    EXPECT_EQ(io->CounterSpendFee(s, RunnerConfig{}, kNow), 20'000u);
    EXPECT_EQ(io->CounterSpendFee(s, RunnerConfig{}, kNow + 10 * kHour), 20'000u);
    EXPECT_THROW(io->FundBtc(io->CounterHtlcAddress(s.record, RunnerConfig{}), z.offer.btc_amount_sat),
                 std::runtime_error);
}

TEST(SwapZecChainIo, HtlcAddressIsTheP2shOfTheTerms) {
    ZecSwap z;
    auto io = z.Io();
    const auto addr = io->CounterHtlcAddress(z.Session(Role::BtcSeller).record, RunnerConfig{});
    EXPECT_EQ(addr.substr(0, 2), "t2");  // regtest P2SH
    EXPECT_EQ(ZecScriptPubKey(DecodeZecTAddress(addr)), z.HtlcSpk());
}

TEST(SwapZecChainIo, MayHaveLeftOnlyUnlessZebraJudgedTheTransaction) {
    ZecSwap z;
    auto io = z.Io();
    for (const char* refused : {
             "failed to validate tx: transaction did not pass consensus validation: could not verify a transparent script",
             "transaction did not pass standard validation: transaction is dust",
             "transaction is non-standard",
             "best chain tip has reached transaction expiry height"}) {
        EXPECT_FALSE(io->CounterBroadcastMayHaveLeft(refused)) << refused;
    }
    for (const char* unsure : {
             "sendrawtransaction (ZEC) failed",
             "transaction already exists in mempool",
             "transaction rejected because another transaction in the mempool has already spent some of its inputs",
             ""}) {
        EXPECT_TRUE(io->CounterBroadcastMayHaveLeft(unsure)) << unsure;
    }
}

TEST(SwapZecChainIo, PreparesTheLockFromTheDeposit) {
    ZecSwap z;
    z.zebra.AddCoin(1, 700'000, 12);
    z.zebra.AddCoin(2, 400'000, 10);
    z.zebra.AddCoin(3, 900'000, 3);  // not deep enough: left alone
    auto io = z.Io();
    const auto addr = io->CounterHtlcAddress(z.Session(Role::BtcSeller).record, RunnerConfig{});
    const auto prep = io->PrepareFundBtc(addr, z.offer.btc_amount_sat);
    EXPECT_EQ(prep.vout, 0u);
    EXPECT_EQ(prep.txid.size(), 64u);
    // Branch id from the node, expiry 0 (a lock waiting for the tower must not die).
    EXPECT_EQ(detail::ToHex({prep.raw.begin(), prep.raw.begin() + 20}).substr(16), "f04dec4d0000000000000000");
    const auto dep_spk = ZecScriptPubKey(DecodeZecTAddress(z.zebra.deposit_address));
    const std::vector<ZecTxOut> prevouts{{700'000, dep_spk}, {400'000, dep_spk}};
    EXPECT_EQ(VerifyInput(prep.raw, 0, prevouts), DZS_OK);
    EXPECT_EQ(VerifyInput(prep.raw, 1, prevouts), DZS_OK);
    // HTLC output first, at the swap amount.
    const auto htlc_hex = detail::ToHex(z.HtlcSpk());
    const auto raw_hex = detail::ToHex(prep.raw);
    EXPECT_NE(raw_hex.find("40420f0000000000" "17" + htlc_hex), std::string::npos);

    EXPECT_THROW(io->PrepareFundBtc("t2WrongAddress", z.offer.btc_amount_sat), std::runtime_error);
    EXPECT_THROW(io->PrepareFundBtc(addr, z.offer.btc_amount_sat + 1), std::runtime_error);
}

TEST(SwapZecChainIo, RefusesALockWhoseWindowReachesAnUpgrade) {
    ZecSwap z;
    z.zebra.AddCoin(1, 2'000'000, 12);
    // T_zec + 24 h = 72 h ahead: ~3,800 blocks at 75 s / 1.10.
    z.zebra.upgrade_height = z.zebra.tip + 3'000;
    auto io = z.Io();
    const auto addr = io->CounterHtlcAddress(z.Session(Role::BtcSeller).record, RunnerConfig{});
    try {
        io->PrepareFundBtc(addr, z.offer.btc_amount_sat);
        FAIL() << "locked across an upgrade";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("NU6.2"), std::string::npos) << e.what();
    }
    z.zebra.upgrade_height = z.zebra.tip + 6'000;  // beyond the window
    EXPECT_NO_THROW(z.Io()->PrepareFundBtc(addr, z.offer.btc_amount_sat));
}

TEST(SwapZecChainIo, ShortDepositIsNotLocked) {
    ZecSwap z;
    z.zebra.AddCoin(1, 1'000'000, 12);  // the amount, but not the funding fee
    auto io = z.Io();
    const auto addr = io->CounterHtlcAddress(z.Session(Role::BtcSeller).record, RunnerConfig{});
    EXPECT_THROW(io->PrepareFundBtc(addr, z.offer.btc_amount_sat), std::invalid_argument);
}

TEST(SwapZecChainIo, ClaimAndRefundAreSignedForTheNextBlock) {
    ZecSwap z;
    auto io = z.Io();
    const BtcFunding funding{Key(0x44), 0, z.offer.btc_amount_sat};
    const std::vector<ZecTxOut> prevouts{{funding.value_sat, z.HtlcSpk()}};

    const auto alice = z.Session(Role::DinSeller);
    const auto claim = io->SignCounterClaim(alice, SwapKeys{Key(3), Key(4), {}}, funding, 20'000);
    EXPECT_EQ(VerifyInput(claim, 0, prevouts), DZS_OK);
    // Branch id 4dec4df0 (LE), nLockTime 0, nExpiryHeight tip + 40.
    EXPECT_EQ(detail::ToHex({claim.begin() + 8, claim.begin() + 20}), "f04dec4d" "00000000" "10040000");
    EXPECT_EQ(ExtractPreimageFromZecClaim(
                  [&] {  // the input's scriptSig: after the 32+4-byte outpoint and its CompactSize length
                      const size_t at = 20 + 1 + 36;
                      return std::vector<uint8_t>(claim.begin() + at + 1, claim.begin() + at + 1 + claim[at]);
                  }(),
                  z.offer.payment_hash)
                  .has_value(),
              true);

    const auto bob = z.Session(Role::BtcSeller);
    const auto refund = io->SignCounterRefund(bob, SwapKeys{Key(5), Key(6), {}}, funding, 20'000);
    EXPECT_EQ(VerifyInput(refund, 0, prevouts), DZS_OK);

    // A different next-block branch is signed into the next spend.
    z.zebra.next_branch = "5437f330";
    auto io2 = z.Io();
    const auto claim2 = io2->SignCounterClaim(alice, SwapKeys{Key(3), Key(4), {}}, funding, 20'000);
    EXPECT_EQ(detail::ToHex({claim2.begin() + 8, claim2.begin() + 12}), "30f33754");

    auto bob_knows = bob;  // Bob learns the secret from Alice's claim; still not his to claim with
    bob_knows.record.secret = alice.record.secret;
    EXPECT_THROW(io->SignCounterClaim(bob_knows, SwapKeys{Key(5), Key(6), {}}, funding, 20'000),
                 std::invalid_argument);
    EXPECT_THROW(io->SignCounterRefund(alice, SwapKeys{Key(3), Key(4), {}}, funding, 20'000), std::invalid_argument);
}

TEST(SwapZecChainIo, PreparedFundingSentOrNot) {
    ZecSwap z;
    z.zebra.AddCoin(1, 2'000'000, 12);
    auto io = z.Io();
    const auto addr = io->CounterHtlcAddress(z.Session(Role::BtcSeller).record, RunnerConfig{});
    const auto prep = io->PrepareFundBtc(addr, z.offer.btc_amount_sat);
    EXPECT_EQ(io->PreparedFundingUnsent(prep.raw), std::optional<bool>(true));   // unknown to Zebra, coins there
    z.zebra.known_txids.insert(prep.txid);
    EXPECT_EQ(io->PreparedFundingUnsent(prep.raw), std::optional<bool>(false));  // Zebra knows it
    z.zebra.known_txids.clear();
    z.zebra.utxos = Json::Value(Json::arrayValue);
    EXPECT_EQ(io->PreparedFundingUnsent(prep.raw), std::optional<bool>(false));  // coins gone: it may have left
    z.zebra.reachable = false;
    EXPECT_EQ(io->PreparedFundingUnsent(prep.raw), std::nullopt);
}

TEST(SwapZecChainIo, NoBitcoinRungsForAZcashLock) {
    ZecSwap z;
    auto io = z.Io();
    TowerPackage p;
    EXPECT_THROW(io->AddCounterTowerRungs(p, z.Session(Role::BtcSeller), SwapKeys{Key(5), Key(6), {}},
                                          BtcFunding{Key(0x44), 0, z.offer.btc_amount_sat}, BtcFeeLadderPolicy{}),
                 std::runtime_error);
}

// --- The runner with the Zcash counter chain ---------------------------------------

namespace {

// ZecSwapChainIo with canned observations: the runner gets the Zcash hooks
// (rules, signing, fees) and the test controls what both chains show.
class CannedZecIo : public ZecSwapChainIo {
public:
    using ZecSwapChainIo::ZecSwapChainIo;
    DinWatchReport din;
    BtcWatchReport zec;
    std::vector<std::vector<uint8_t>> zec_sent;
    DinWatchReport ObserveDin() override { return din; }
    BtcWatchReport ObserveBtc() override { return zec; }
    std::string BroadcastBtc(const std::vector<uint8_t>& raw) override {
        zec_sent.push_back(raw);
        return std::string(64, 'b');
    }
};

struct MemoryStore : SwapStore {
    void Save(const SwapSession&) override {}
};

std::unique_ptr<CannedZecIo> CannedIo(ZecSwap& z, uint32_t now) {
    SwapWatchSpec spec;
    spec.offer = z.offer;
    spec.accept = z.accept;
    ZecIoConfig c;
    c.expected_chain = "test";
    c.network = ZecNetwork::Regtest;
    c.clock = [now] { return now; };
    DinRpc din = [](const std::string&, const Json::Value&) { return std::optional<Json::Value>{}; };
    auto io = std::make_unique<CannedZecIo>(din, z.zebra.Rpc(), spec, c);
    io->din.ok = true;
    io->din.mtp_unix = now;
    io->din.htlc.output_seen = true;
    io->din.htlc.output_confirmations = z.offer.n_din_confirmations;
    io->din.htlc.output_value = z.offer.din_amount_una;
    io->zec.ok = true;
    io->zec.mtp_unix = now;
    io->zec.htlc.output_seen = true;
    io->zec.htlc.output_confirmations = z.offer.n_btc_confirmations;
    io->zec.htlc.output_value = z.offer.btc_amount_sat;
    io->zec.funding = BtcFunding{Key(0x44), 0, z.offer.btc_amount_sat};
    io->ObserveBtcChain();
    return io;
}

}  // namespace

TEST(SwapZecChainIo, RunnerUsesZcashCutoffIncludingTheEarlyRefundAllowance) {
    ZecSwap z;
    auto alice = z.Session(Role::DinSeller);
    alice.record.state = SwapState::DinLocked;
    const SwapKeys keys{Key(3), Key(4), {}};
    MemoryStore store;

    // 7 h before T_zec: Bitcoin's 6 h cut-off would still claim; Zcash's
    // 6 h + 90 min (a refund may confirm that early) must not reveal the secret.
    const uint32_t late = z.offer.t_btc_unix - 7 * kHour;
    auto io = CannedIo(z, late);
    SwapRunner runner(alice, keys, RunnerConfig{}, *io, store);
    const auto report = runner.Tick(late);
    ASSERT_TRUE(report.observed);
    EXPECT_EQ(report.after, SwapState::DinLocked);
    EXPECT_TRUE(io->zec_sent.empty());

    // 8 h before: claim, signed with the Zcash hooks.
    const uint32_t early = z.offer.t_btc_unix - 8 * kHour;
    auto io2 = CannedIo(z, early);
    SwapRunner runner2(alice, keys, RunnerConfig{}, *io2, store);
    const auto report2 = runner2.Tick(early);
    EXPECT_EQ(report2.after, SwapState::BtcClaimBroadcast);
    ASSERT_EQ(io2->zec_sent.size(), 1u);
    EXPECT_EQ(VerifyInput(io2->zec_sent[0], 0, {{z.offer.btc_amount_sat, z.HtlcSpk()}}), DZS_OK);
}
