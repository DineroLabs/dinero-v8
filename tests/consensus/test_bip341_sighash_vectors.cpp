#include "consensus/script_interpreter.h"
#include "consensus/script_verify.h"
#include "consensus/tx_parser.h"
#include "consensus/utxo_entry.h"
#include "crypto/evp_secp256k1.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

using dinero::Transaction;
using dinero::TransactionSerializer;
using dinero::AmountUna;
using dinero::consensus::ScriptExecutionContext;
using dinero::consensus::ScriptVerifier;
using dinero::consensus::SignatureHashTaproot;
using dinero::consensus::TransactionParser;
using dinero::consensus::UTXOEntry;

std::string ToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const uint8_t byte : bytes) {
        out << std::setw(2) << static_cast<unsigned>(byte);
    }
    return out.str();
}

struct ExpectedSighash {
    uint32_t input;
    uint8_t hash_type;
    const char* hash;
};

// First keyPathSpending transaction from the official BIP341 wallet vectors:
// https://github.com/bitcoin/bips/blob/master/bip-0341/wallet-test-vectors.json
constexpr char kUnsignedTx[] =
    "02000000097de20cbff686da83a54981d2b9bab3586f4ca7e48f57f5b55963115f3b334e9c010000000000000000d7b7cab57b1393ace2d064f4d4a2cb8af6def61273e127517d44759b6dafdd990000000000fffffffff8e1f583384333689228c5d28eac13366be082dc57441760d957275419a418420000000000fffffffff0689180aa63b30cb162a73c6d2a38b7eeda2a83ece74310fda0843ad604853b0100000000feffffffaa5202bdf6d8ccd2ee0f0202afbbb7461d9264a25e5bfd3c5a52ee1239e0ba6c0000000000feffffff956149bdc66faa968eb2be2d2faa29718acbfe3941215893a2a3446d32acd050000000000000000000e664b9773b88c09c32cb70a2a3e4da0ced63b7ba3b22f848531bbb1d5d5f4c94010000000000000000e9aa6b8e6c9de67619e6a3924ae25696bb7b694bb677a632a74ef7eadfd4eabf0000000000ffffffffa778eb6a263dc090464cd125c466b5a99667720b1c110468831d058aa1b82af10100000000ffffffff0200ca9a3b000000001976a91406afd46bcdfd22ef94ac122aa11f241244a37ecc88ac807840cb0000000020ac9a87f5594be208f8532db38cff670c450ed2fea8fcdefcc9a663f78bab962b0065cd1d";

constexpr uint64_t kAmounts[] = {
    420000000, 462000000, 294000000, 504000000, 630000000,
    378000000, 672000000, 546000000, 588000000,
};

constexpr const char* kScriptPubKeys[] = {
    "512053a1f6e454df1aa2776a2814a721372d6258050de330b3c6d10ee8f4e0dda343",
    "5120147c9c57132f6e7ecddba9800bb0c4449251c92a1e60371ee77557b6620f3ea3",
    "76a914751e76e8199196d454941c45d1b3a323f1433bd688ac",
    "5120e4d810fd50586274face62b8a807eb9719cef49c04177cc6b76a9a4251d5450e",
    "512091b64d5324723a985170e4dc5a0f84c041804f2cd12660fa5dec09fc21783605",
    "00147dd65592d0ab2fe0d0257d571abf032cd9db93dc",
    "512075169f4001aa68f15bbed28b218df1d0a62cbbcf1188c6665110c293c907b831",
    "5120712447206d7a5238acc7ff53fbe94a3b64539ad291c7cdbc490b7577e4b17df5",
    "512077e30a5522dd9f894c3f8b8bd4c4b2cf82ca7da8a3ea6a239655c39c050ab220",
};

constexpr ExpectedSighash kExpected[] = {
    {0, 3, "2514a6272f85cfa0f45eb907fcb0d121b808ed37c6ea160a5a9046ed5526d555"},
    {1, 131, "325a644af47e8a5a2591cda0ab0723978537318f10e6a63d4eed783b96a71a4d"},
    {3, 1, "bf013ea93474aa67815b1b6cc441d23b64fa310911d991e713cd34c7f5d46669"},
    {4, 0, "4f900a0bae3f1446fd48490c2958b5a023228f01661cda3496a11da502a7f7ef"},
    {6, 2, "15f25c298eb5cdc7eb1d638dd2d45c97c4c59dcaec6679cfc16ad84f30876b85"},
    {7, 130, "cd292de50313804dabe4685e83f923d2969577191a3e1d2882220dca88cbeb10"},
    {8, 129, "cccb739eca6c13a8a89e6e5cd317ffe55669bbda23f2fd37b0f18755e008edd2"},
};

TEST(BIP341SighashVectors, MatchesOfficialKeyPathMessages) {
    Transaction tx;
    std::string error;
    ASSERT_TRUE(TransactionParser::ParseTransaction(kUnsignedTx, tx, error))
        << error;
    ASSERT_EQ(tx.vin.size(), std::size(kAmounts));

    std::vector<uint64_t> amounts(std::begin(kAmounts), std::end(kAmounts));
    std::vector<std::vector<uint8_t>> scripts;
    for (const char* hex : kScriptPubKeys) {
        scripts.push_back(TransactionSerializer::FromHex(hex));
    }
    const std::vector<uint8_t> confidential_flags(tx.vin.size(), 0);
    const std::vector<std::vector<uint8_t>> commitments(tx.vin.size());

    for (const ExpectedSighash& expected : kExpected) {
        SCOPED_TRACE(expected.input);
        ScriptExecutionContext context(
            &tx, expected.input, amounts[expected.input],
            dinero::consensus::SCRIPT_VERIFY_TAPROOT, amounts, scripts,
            confidential_flags, commitments);
        EXPECT_EQ(ToHex(SignatureHashTaproot(
                      context, expected.hash_type, {}, {})),
                  expected.hash);
        EXPECT_EQ(ToHex(ScriptVerifier::ComputeTaprootSighash(
                      tx, expected.input, amounts, scripts,
                      expected.hash_type)),
                  expected.hash);
    }
}

TEST(BIP341SighashVectors, RejectsUndefinedHashTypesAndSingleWithoutOutput) {
    Transaction tx;
    tx.vin.resize(2);
    tx.vout.resize(1);
    const std::vector<uint64_t> amounts(2, 1);
    const std::vector<std::vector<uint8_t>> scripts(2, {0x51});
    ScriptExecutionContext context(
        &tx, 1, 1, dinero::consensus::SCRIPT_VERIFY_TAPROOT,
        amounts, scripts, std::vector<uint8_t>(2, 0),
        std::vector<std::vector<uint8_t>>(2));

    EXPECT_TRUE(SignatureHashTaproot(context, 0x04, {}, {}).empty());
    EXPECT_TRUE(SignatureHashTaproot(context, 0x80, {}, {}).empty());
    EXPECT_TRUE(SignatureHashTaproot(context, 0x03, {}, {}).empty());
}

TEST(BIP341SighashVectors, SharedVerifierAcceptsCanonicalSignedKeyPath) {
    auto* secp = dinero::crypto::GetSecp256k1ContextSignVerify();
    std::array<uint8_t, 32> secret{};
    secret[31] = 1;
    secp256k1_keypair keypair;
    ASSERT_EQ(secp256k1_keypair_create(secp, &keypair, secret.data()), 1);
    secp256k1_xonly_pubkey xonly;
    ASSERT_EQ(secp256k1_keypair_xonly_pub(secp, &xonly, nullptr, &keypair), 1);
    std::array<uint8_t, 32> pubkey{};
    ASSERT_EQ(secp256k1_xonly_pubkey_serialize(
                  secp, pubkey.data(), &xonly),
              1);

    Transaction tx;
    tx.version = 2;
    tx.vin.emplace_back();
    tx.vout.emplace_back(AmountUna::Una(9'000), std::vector<uint8_t>{0x51});

    std::vector<uint8_t> p2tr{0x51, 0x20};
    p2tr.insert(p2tr.end(), pubkey.begin(), pubkey.end());
    std::vector<UTXOEntry> prevouts{
        UTXOEntry(AmountUna::Una(10'000), p2tr, 1, false)};
    const std::vector<uint64_t> amounts{10'000};
    const std::vector<std::vector<uint8_t>> scripts{p2tr};
    ScriptExecutionContext context(
        &tx, 0, 10'000, dinero::consensus::SCRIPT_VERIFY_TAPROOT,
        amounts, scripts, std::vector<uint8_t>{0},
        std::vector<std::vector<uint8_t>>{{}});
    const std::vector<uint8_t> message =
        SignatureHashTaproot(context, 0, {}, {});
    ASSERT_EQ(message.size(), 32U);

    std::vector<uint8_t> signature(64);
    const std::array<uint8_t, 32> aux{};
    ASSERT_EQ(secp256k1_schnorrsig_sign32(
                  secp, signature.data(), message.data(), &keypair,
                  aux.data()),
              1);
    tx.vin[0].witness = {signature};

    std::string error;
    EXPECT_TRUE(ScriptVerifier::VerifyTaproot(
        tx, 0, prevouts, error,
        dinero::consensus::SCRIPT_VERIFY_STANDARD))
        << error;

    tx.vout[0].value = AmountUna::Una(8'999);
    error.clear();
    EXPECT_FALSE(ScriptVerifier::VerifyTaproot(
        tx, 0, prevouts, error,
        dinero::consensus::SCRIPT_VERIFY_STANDARD));
}

} // namespace
