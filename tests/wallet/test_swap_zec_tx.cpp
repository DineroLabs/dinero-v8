// ZEC side of DIN <-> ZEC swaps: v5 transactions, ZIP 244 sighash and
// t-addresses through rust/zcash_swap, and the P2SH HTLC spends run through
// the zcash_script interpreter (milestone 3 of docs/design/din-zec-atomic-swaps.md).

#include "wallet/swap/zec_tx.h"

#include "crypto/sha256.h"
#include "dinero_zcash_swap.h"
#include "wallet/swap/swap_crypto.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace dinero::swap;
using detail::FromHex;
using detail::ToHex;

constexpr uint32_t kNu5 = 0xc2d6d0b4;
constexpr uint32_t kNu6 = 0xc8e71055;
constexpr uint32_t kCanopy = 0xe9ff75a6;  // before v5 existed
constexpr uint32_t kRefundLock = 1'900'000'000;
constexpr uint64_t kFee = 20'000;

Bytes32 Fill(uint8_t b) {
    Bytes32 out{};
    out.fill(b);
    return out;
}

std::string Hex(const Bytes32& b) { return ToHex({b.begin(), b.end()}); }

struct Swap {
    Bytes32 alice_sk = Fill(0x11);  // claims ZEC with the preimage
    Bytes32 bob_sk = Fill(0x22);    // refunds after the lock
    std::vector<uint8_t> preimage = std::vector<uint8_t>(32, 0x33);
    ZecHtlcTerms terms;
    ZecFunding funding;
    std::vector<uint8_t> payout = ZecScriptPubKey({ZecNetwork::Main, ZecAddressKind::P2pkh, {}});

    Swap() {
        dinero::crypto::CSHA256().Write(preimage.data(), preimage.size()).Finalize(terms.payment_hash.data());
        terms.claim_pubkey = detail::CompressedOf(alice_sk);
        terms.refund_pubkey = detail::CompressedOf(bob_sk);
        terms.refund_locktime_unix = kRefundLock;
        funding.txid = Fill(0x44);
        funding.vout = 1;
        funding.value_zat = 1'000'000;
    }

    std::vector<ZecTxOut> Prevouts() const {
        return {{funding.value_zat, ZecP2shScriptPubKey(BuildZecHtlcRedeemScript(terms))}};
    }

    ZecTx SignedClaim(uint32_t expiry = 100) const {
        auto tx = BuildZecClaimTx(funding, payout, kFee, kNu6, expiry);
        SetZecClaimScriptSig(tx, terms, detail::EcdsaSignAll(alice_sk, ZecHtlcSighash(tx, terms, funding)), preimage);
        return tx;
    }

    ZecTx SignedRefund(uint32_t expiry = 100) const {
        auto tx = BuildZecRefundTx(terms, funding, payout, kFee, kNu6, expiry);
        SetZecRefundScriptSig(tx, terms, detail::EcdsaSignAll(bob_sk, ZecHtlcSighash(tx, terms, funding)));
        return tx;
    }
};

int32_t Verify(const ZecTx& tx, const std::vector<ZecTxOut>& prevouts) {
    const auto bytes = SerializeZecTx(tx);
    std::vector<dzs_txout> ffi;
    for (const auto& p : prevouts) ffi.push_back({p.value_zat, p.script_pubkey.data(), p.script_pubkey.size()});
    return dzs_verify_input(bytes.data(), bytes.size(), 0, ffi.data(), ffi.size());
}

// <sig> <pushes...> <redeem script>, built by hand for scriptSigs the setters refuse.
std::vector<uint8_t> ScriptSig(const std::vector<std::vector<uint8_t>>& pushes, uint8_t branch,
                               const std::vector<uint8_t>& redeem) {
    std::vector<uint8_t> s;
    for (const auto& p : pushes) {
        s.push_back(static_cast<uint8_t>(p.size()));
        s.insert(s.end(), p.begin(), p.end());
    }
    s.push_back(branch);
    s.push_back(0x4c);
    s.push_back(static_cast<uint8_t>(redeem.size()));
    s.insert(s.end(), redeem.begin(), redeem.end());
    return s;
}

// --- ZIP 244 test vectors ---------------------------------------------------

TEST(SwapZecTx, Zip244OfficialVectors) {
    std::ifstream in(DINERO_ZIP244_JSON);
    ASSERT_TRUE(in) << "missing " << DINERO_ZIP244_JSON;
    const auto rows = nlohmann::json::parse(in);
    ASSERT_EQ(rows.size(), 12u);  // two header rows, ten vectors

    int txids = 0, sighashes = 0, later_inputs = 0;
    for (size_t r = 2; r < rows.size(); ++r) {
        const auto& v = rows[r];
        const auto tx = FromHex(v[0].get<std::string>());
        EXPECT_EQ(Hex(ZecTxid(tx)), v[1].get<std::string>()) << "vector " << r - 2;
        ++txids;
        if (v[5].is_null()) continue;  // no transparent input

        std::vector<ZecTxOut> prevouts;
        for (size_t i = 0; i < v[3].size(); ++i) {
            prevouts.push_back({v[3][i].get<uint64_t>(), FromHex(v[4][i].get<std::string>())});
        }
        const auto index = v[5].get<size_t>();
        EXPECT_EQ(Hex(Zip244SighashAll(tx, index, prevouts)), v[7].get<std::string>()) << "vector " << r - 2;
        ++sighashes;
        if (index > 0) ++later_inputs;
    }
    EXPECT_EQ(txids, 10);
    EXPECT_EQ(sighashes, 6);
    EXPECT_EQ(later_inputs, 2);  // the input index reaches the digest, not just input 0
}

// --- Serialization -----------------------------------------------------------

TEST(SwapZecTx, V5HeaderCarriesBranchLockTimeAndExpiry) {
    const Swap s;
    const auto tx = BuildZecRefundTx(s.terms, s.funding, s.payout, kFee, kNu6, 3'000'123);
    const auto bytes = SerializeZecTx(tx);
    // ZIP 225: header 0x80000005, version group 0x26A7270A, branch, nLockTime, nExpiryHeight (LE).
    ASSERT_GE(bytes.size(), 20u);
    EXPECT_EQ(ToHex({bytes.begin(), bytes.begin() + 20}),
              "050000800a27a726" "5510e7c8" "00b33f71" "3bc72d00");
}

TEST(SwapZecTx, SpendShapes) {
    const Swap s;
    const auto claim = BuildZecClaimTx(s.funding, s.payout, kFee, kNu6, 140);
    EXPECT_EQ(claim.locktime, 0u);
    EXPECT_EQ(claim.vin.at(0).sequence, 0xffffffffu);
    EXPECT_EQ(claim.vout.at(0).value_zat, s.funding.value_zat - kFee);
    EXPECT_EQ(claim.expiry_height, 140u);

    const auto refund = BuildZecRefundTx(s.terms, s.funding, s.payout, kFee, kNu6, 0);
    EXPECT_EQ(refund.locktime, kRefundLock);
    EXPECT_EQ(refund.vin.at(0).sequence, 0xfffffffeu);
    EXPECT_EQ(refund.expiry_height, 0u);
    EXPECT_EQ(refund.vin.at(0).prev_txid, s.funding.txid);
    EXPECT_EQ(refund.vin.at(0).prev_vout, s.funding.vout);
}

TEST(SwapZecTx, GoldenSignedSpends) {
    const Swap s;
    const auto claim = SerializeZecTx(s.SignedClaim());
    const auto refund = SerializeZecTx(s.SignedRefund(0));
    EXPECT_EQ(ToHex(claim), "050000800a27a7265510e7c8000000006400000001444444444444444444444444444444444444444444444444444444"
              "444444444401000000e34830450221009531160ed860544b2571591e30ac8318d66732a6ce4fdba058f1e9c3a14aa62e"
              "02204636db57578e8b4cb5dc8643a909e8ffeccd8693e23cfc1e4ef49f3e28030ee00120333333333333333333333333"
              "3333333333333333333333333333333333333333514c766382012088a820deb0e38ced1e41de6f92e70e80c418d2d356"
              "afaaa99e26f5939dbc7d3ef4772a8821034f355bdcb7cc0af728ef3cceb9615d90684bb5b2ca5f859ab0f0b704075871"
              "aa670400b33f71b1752102466d7fcae563e5cb09a0d1870bb580344804617879a14949cf22285f1bae3f2768acffffff"
              "ff0120f40e00000000001976a914000000000000000000000000000000000000000088ac000000");
    EXPECT_EQ(ToHex(refund), "050000800a27a7265510e7c800b33f710000000001444444444444444444444444444444444444444444444444444444"
              "444444444401000000c248304502210083cf641b60d8f71c6f222a8957e576bc3b4943eea965cbc363466e61e12d6215"
              "022031443fac03ef8b91338d3a34703126d3ee8a0b3408a072af1c442793bf826fc001004c766382012088a820deb0e3"
              "8ced1e41de6f92e70e80c418d2d356afaaa99e26f5939dbc7d3ef4772a8821034f355bdcb7cc0af728ef3cceb9615d90"
              "684bb5b2ca5f859ab0f0b704075871aa670400b33f71b1752102466d7fcae563e5cb09a0d1870bb580344804617879a1"
              "4949cf22285f1bae3f2768acfeffffff0120f40e00000000001976a91400000000000000000000000000000000000000"
              "0088ac000000");
    EXPECT_EQ(Hex(ZecTxid(claim)), "bea1de9074f832cffaf89a53a23039cf842aedf35729bdd949457d22ff7efbef");
}

TEST(SwapZecTx, RejectsBadTransactions) {
    const Swap s;
    auto tx = BuildZecClaimTx(s.funding, s.payout, kFee, kNu6, 100);
    tx.consensus_branch_id = 0x12345678;
    EXPECT_THROW(SerializeZecTx(tx), std::invalid_argument);
    tx.consensus_branch_id = kCanopy;
    EXPECT_THROW(SerializeZecTx(tx), std::invalid_argument);
    tx.consensus_branch_id = kNu5;
    tx.expiry_height = 500'000'000;
    EXPECT_THROW(SerializeZecTx(tx), std::invalid_argument);
    tx.expiry_height = 0;
    tx.vout[0].value_zat = 21'000'000ull * 100'000'000ull + 1;  // above MAX_MONEY
    EXPECT_THROW(SerializeZecTx(tx), std::invalid_argument);
    tx.vout.clear();
    EXPECT_THROW(SerializeZecTx(tx), std::invalid_argument);

    auto bytes = SerializeZecTx(s.SignedClaim());
    EXPECT_THROW(ZecTxid(std::vector<uint8_t>(bytes.begin(), bytes.end() - 1)), std::invalid_argument);
    bytes.push_back(0x00);
    EXPECT_THROW(ZecTxid(bytes), std::invalid_argument);  // trailing byte
    EXPECT_THROW(ZecTxid(std::vector<uint8_t>{0x01, 0x00, 0x00, 0x00}), std::invalid_argument);
}

TEST(SwapZecTx, SighashNeedsEveryPrevoutAndAValidIndex) {
    const Swap s;
    const auto bytes = SerializeZecTx(BuildZecClaimTx(s.funding, s.payout, kFee, kNu6, 100));
    auto prevouts = s.Prevouts();
    EXPECT_THROW(Zip244SighashAll(bytes, 1, prevouts), std::invalid_argument);
    EXPECT_THROW(Zip244SighashAll(bytes, 0, {}), std::invalid_argument);
    prevouts.push_back(prevouts[0]);
    EXPECT_THROW(Zip244SighashAll(bytes, 0, prevouts), std::invalid_argument);
}

// --- What the signature commits to ------------------------------------------

TEST(SwapZecTx, SighashCommitsToBranchExpiryAmountAndScript) {
    const Swap s;
    const auto base = BuildZecClaimTx(s.funding, s.payout, kFee, kNu6, 100);
    const auto h = ZecHtlcSighash(base, s.terms, s.funding);

    auto other_branch = base;
    other_branch.consensus_branch_id = kNu5;
    EXPECT_NE(ZecHtlcSighash(other_branch, s.terms, s.funding), h);  // a rung dies at the next upgrade

    auto no_expiry = base;
    no_expiry.expiry_height = 0;
    EXPECT_NE(ZecHtlcSighash(no_expiry, s.terms, s.funding), h);

    auto more = s.funding;
    more.value_zat += 1;
    EXPECT_NE(ZecHtlcSighash(base, s.terms, more), h);

    auto other_terms = s.terms;
    other_terms.refund_locktime_unix += 1;
    EXPECT_NE(ZecHtlcSighash(base, other_terms, s.funding), h);
}

TEST(SwapZecTx, SighashIgnoresScriptSig) {
    const Swap s;
    const auto unsigned_tx = BuildZecClaimTx(s.funding, s.payout, kFee, kNu6, 100);
    const auto signed_tx = s.SignedClaim();
    EXPECT_EQ(Zip244SighashAll(SerializeZecTx(unsigned_tx), 0, s.Prevouts()),
              Zip244SighashAll(SerializeZecTx(signed_tx), 0, s.Prevouts()));
}

// --- The HTLC under the zcash_script interpreter ----------------------------

TEST(SwapZecTx, ClaimVerifies) {
    const Swap s;
    EXPECT_EQ(Verify(s.SignedClaim(), s.Prevouts()), DZS_OK);
}

TEST(SwapZecTx, RefundAndTowerRungVerify) {
    const Swap s;
    EXPECT_EQ(Verify(s.SignedRefund(100), s.Prevouts()), DZS_OK);
    EXPECT_EQ(Verify(s.SignedRefund(0), s.Prevouts()), DZS_OK);  // nExpiryHeight 0: never expires
}

TEST(SwapZecTx, ClaimWithWrongPreimageFails) {
    const Swap s;
    auto tx = BuildZecClaimTx(s.funding, s.payout, kFee, kNu6, 100);
    EXPECT_THROW(SetZecClaimScriptSig(tx, s.terms, {0x30}, std::vector<uint8_t>(32, 0x34)),
                 std::invalid_argument);
    const auto sig = detail::EcdsaSignAll(s.alice_sk, ZecHtlcSighash(tx, s.terms, s.funding));
    tx.vin[0].script_sig = ScriptSig({sig, std::vector<uint8_t>(32, 0x34)}, 0x51, BuildZecHtlcRedeemScript(s.terms));
    EXPECT_EQ(Verify(tx, s.Prevouts()), DZS_ERR_SCRIPT_INVALID);
    tx.vin[0].script_sig = ScriptSig({sig, s.preimage}, 0x51, BuildZecHtlcRedeemScript(s.terms));
    EXPECT_EQ(Verify(tx, s.Prevouts()), DZS_OK);  // same scriptSig with the right preimage
}

TEST(SwapZecTx, ClaimSignedByTheRefundKeyFails) {
    const Swap s;
    auto tx = BuildZecClaimTx(s.funding, s.payout, kFee, kNu6, 100);
    SetZecClaimScriptSig(tx, s.terms, detail::EcdsaSignAll(s.bob_sk, ZecHtlcSighash(tx, s.terms, s.funding)),
                         s.preimage);
    EXPECT_EQ(Verify(tx, s.Prevouts()), DZS_ERR_SCRIPT_INVALID);
}

TEST(SwapZecTx, ChangingTheTransactionAfterSigningFails) {
    const Swap s;
    auto tx = s.SignedClaim();
    tx.vout[0].value_zat -= 1;
    EXPECT_EQ(Verify(tx, s.Prevouts()), DZS_ERR_SCRIPT_INVALID);
    tx = s.SignedClaim();
    tx.consensus_branch_id = kNu5;
    EXPECT_EQ(Verify(tx, s.Prevouts()), DZS_ERR_SCRIPT_INVALID);
    tx = s.SignedClaim();
    auto prevouts = s.Prevouts();
    prevouts[0].value_zat += 1;  // the signer was told a different amount
    EXPECT_EQ(Verify(tx, prevouts), DZS_ERR_SCRIPT_INVALID);
}

TEST(SwapZecTx, RefundBeforeTheLockFails) {
    const Swap s;
    auto tx = BuildZecRefundTx(s.terms, s.funding, s.payout, kFee, kNu6, 100);
    tx.locktime = kRefundLock - 1;  // signed properly, but below the CLTV argument
    SetZecRefundScriptSig(tx, s.terms, detail::EcdsaSignAll(s.bob_sk, ZecHtlcSighash(tx, s.terms, s.funding)));
    EXPECT_EQ(Verify(tx, s.Prevouts()), DZS_ERR_SCRIPT_INVALID);
}

TEST(SwapZecTx, RefundWithFinalSequenceFails) {
    const Swap s;
    auto tx = BuildZecRefundTx(s.terms, s.funding, s.payout, kFee, kNu6, 100);
    tx.vin[0].sequence = 0xffffffff;  // final input: CLTV must refuse it
    SetZecRefundScriptSig(tx, s.terms, detail::EcdsaSignAll(s.bob_sk, ZecHtlcSighash(tx, s.terms, s.funding)));
    EXPECT_EQ(Verify(tx, s.Prevouts()), DZS_ERR_SCRIPT_INVALID);
}

TEST(SwapZecTx, RefundSignedByTheClaimKeyFails) {
    const Swap s;
    auto tx = BuildZecRefundTx(s.terms, s.funding, s.payout, kFee, kNu6, 100);
    SetZecRefundScriptSig(tx, s.terms, detail::EcdsaSignAll(s.alice_sk, ZecHtlcSighash(tx, s.terms, s.funding)));
    EXPECT_EQ(Verify(tx, s.Prevouts()), DZS_ERR_SCRIPT_INVALID);
}

TEST(SwapZecTx, BuilderGuards) {
    const Swap s;
    EXPECT_THROW(BuildZecClaimTx(s.funding, s.payout, s.funding.value_zat, kNu6, 0), std::invalid_argument);
    EXPECT_THROW(BuildZecClaimTx(s.funding, {}, kFee, kNu6, 0), std::invalid_argument);
    auto height_lock = s.terms;
    height_lock.refund_locktime_unix = 3'000'000;  // a block height, never allowed
    EXPECT_THROW(BuildZecRefundTx(height_lock, s.funding, s.payout, kFee, kNu6, 0), std::invalid_argument);
    auto other = s.funding;
    other.vout = 0;
    const auto tx = BuildZecClaimTx(s.funding, s.payout, kFee, kNu6, 0);
    EXPECT_THROW(ZecHtlcSighash(tx, s.terms, other), std::invalid_argument);
}

// --- t-addresses -------------------------------------------------------------

TEST(SwapZecTx, TransparentAddresses) {
    ZecTAddress a;
    for (uint8_t i = 0; i < 20; ++i) a.hash[i] = i;
    // Expected strings from an independent Base58Check with the protocol's prefixes.
    const struct { ZecNetwork net; ZecAddressKind kind; const char* text; } cases[] = {
        {ZecNetwork::Main, ZecAddressKind::P2pkh, "t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCi"},
        {ZecNetwork::Main, ZecAddressKind::P2sh, "t3JZe8uVCra9T1mot8DC99s7GVsDKFy2Xa2"},
        {ZecNetwork::Test, ZecAddressKind::P2pkh, "tm9iNYCVAhLLa4rJtfqqHauR5xL1REdpiDs"},
        {ZecNetwork::Test, ZecAddressKind::P2sh, "t26YqBabLj2kpZUPd3xCBhVHucMSV83GWSw"},
    };
    for (const auto& c : cases) {
        a.network = c.net;
        a.kind = c.kind;
        EXPECT_EQ(EncodeZecTAddress(a), c.text);
        const auto back = DecodeZecTAddress(c.text);
        EXPECT_EQ(back.network, c.net);
        EXPECT_EQ(back.kind, c.kind);
        EXPECT_EQ(back.hash, a.hash);
    }
    a.network = ZecNetwork::Regtest;
    a.kind = ZecAddressKind::P2pkh;
    EXPECT_EQ(EncodeZecTAddress(a), "tm9iNYCVAhLLa4rJtfqqHauR5xL1REdpiDs");

    EXPECT_THROW(DecodeZecTAddress("t1HsdDMzmJfq4vc7T17XYjEkLMLvbgM1fCj"), std::invalid_argument);  // checksum
    EXPECT_THROW(DecodeZecTAddress(""), std::invalid_argument);
    EXPECT_THROW(DecodeZecTAddress("1BoatSLRHtKNngkdXEeobR76b53LETtpyT"), std::invalid_argument);  // Bitcoin
}

TEST(SwapZecTx, HtlcPaysToTheP2shOfTheRedeemScript) {
    const Swap s;
    const auto redeem = BuildZecHtlcRedeemScript(s.terms);
    EXPECT_EQ(redeem, BuildBtcHtlcWitnessScript(s.terms));
    const auto spk = ZecP2shScriptPubKey(redeem);
    ASSERT_EQ(spk.size(), 23u);
    EXPECT_EQ(spk.front(), 0xa9);
    EXPECT_EQ(spk.back(), 0x87);
    ZecTAddress p2sh{ZecNetwork::Main, ZecAddressKind::P2sh, {}};
    std::copy(spk.begin() + 2, spk.begin() + 22, p2sh.hash.begin());
    EXPECT_EQ(ZecScriptPubKey(DecodeZecTAddress(EncodeZecTAddress(p2sh))), spk);
}

}  // namespace
