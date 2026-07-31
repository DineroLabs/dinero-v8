#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include "consensus/script_verify.h"
#include "consensus/tapscript_interpreter.h"
#include "consensus/utxo_entry.h"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using dinero::AmountUna;
using dinero::Transaction;
using dinero::TxInput;
using dinero::TxOutput;
using dinero::consensus::ScriptVerifier;
using dinero::consensus::TapscriptInterpreter;
using dinero::consensus::UTXOEntry;

void WriteCompactSize(std::vector<uint8_t>& out, uint64_t value) {
    if (value < 0xfd) {
        out.push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffff) {
        out.push_back(0xfd);
        out.push_back(static_cast<uint8_t>(value));
        out.push_back(static_cast<uint8_t>(value >> 8));
    } else if (value <= 0xffffffffULL) {
        out.push_back(0xfe);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<uint8_t>(value >> shift));
        }
    } else {
        out.push_back(0xff);
        for (unsigned shift = 0; shift < 64; shift += 8) {
            out.push_back(static_cast<uint8_t>(value >> shift));
        }
    }
}

std::array<uint8_t, 32> TaggedHash(const char* tag,
                                   const std::vector<uint8_t>& message) {
    std::array<uint8_t, 32> tag_hash{};
    dinero::crypto::CSHA256()
        .Write(reinterpret_cast<const uint8_t*>(tag), std::strlen(tag))
        .Finalize(tag_hash.data());

    std::array<uint8_t, 32> result{};
    dinero::crypto::CSHA256()
        .Write(tag_hash.data(), tag_hash.size())
        .Write(tag_hash.data(), tag_hash.size())
        .Write(message.data(), message.size())
        .Finalize(result.data());
    return result;
}

struct ScriptPathCase {
    Transaction tx;
    std::vector<UTXOEntry> prevouts;
    std::vector<uint8_t> control_block;
};

ScriptPathCase BuildScriptPathCase(const std::vector<uint8_t>& script,
                                   uint8_t leaf_version = 0xc0) {
    auto* secp = dinero::crypto::GetSecp256k1ContextVerify();

    std::array<uint8_t, 32> secret{};
    secret[31] = 1;
    secp256k1_keypair keypair;
    EXPECT_EQ(secp256k1_keypair_create(secp, &keypair, secret.data()), 1);

    secp256k1_xonly_pubkey internal_key;
    EXPECT_EQ(secp256k1_keypair_xonly_pub(secp, &internal_key, nullptr, &keypair), 1);
    std::array<uint8_t, 32> internal_bytes{};
    EXPECT_EQ(secp256k1_xonly_pubkey_serialize(
                  secp, internal_bytes.data(), &internal_key),
              1);

    std::vector<uint8_t> leaf_preimage{leaf_version};
    WriteCompactSize(leaf_preimage, script.size());
    leaf_preimage.insert(leaf_preimage.end(), script.begin(), script.end());
    const auto leaf_hash = TaggedHash("TapLeaf", leaf_preimage);

    std::vector<uint8_t> tweak_preimage(internal_bytes.begin(),
                                       internal_bytes.end());
    tweak_preimage.insert(tweak_preimage.end(),
                          leaf_hash.begin(), leaf_hash.end());
    const auto tweak = TaggedHash("TapTweak", tweak_preimage);

    secp256k1_pubkey output_key;
    EXPECT_EQ(secp256k1_xonly_pubkey_tweak_add(
                  secp, &output_key, &internal_key, tweak.data()),
              1);
    secp256k1_xonly_pubkey output_xonly;
    int output_parity = -1;
    EXPECT_EQ(secp256k1_xonly_pubkey_from_pubkey(
                  secp, &output_xonly, &output_parity, &output_key),
              1);
    std::array<uint8_t, 32> output_bytes{};
    EXPECT_EQ(secp256k1_xonly_pubkey_serialize(
                  secp, output_bytes.data(), &output_xonly),
              1);

    ScriptPathCase test_case;
    test_case.control_block.push_back(
        static_cast<uint8_t>(leaf_version | output_parity));
    test_case.control_block.insert(test_case.control_block.end(),
                                   internal_bytes.begin(), internal_bytes.end());

    TxInput input;
    input.witness = {script, test_case.control_block};
    test_case.tx.vin.push_back(std::move(input));

    TxOutput output;
    output.value = AmountUna::Una(9'000);
    output.scriptPubKey = {dinero::consensus::OP_1, 0x20};
    output.scriptPubKey.insert(output.scriptPubKey.end(),
                               output_bytes.begin(), output_bytes.end());
    test_case.tx.vout.push_back(std::move(output));

    UTXOEntry prevout;
    prevout.value = AmountUna::Una(10'000);
    prevout.scriptPubKey = test_case.tx.vout[0].scriptPubKey;
    test_case.prevouts.push_back(std::move(prevout));
    return test_case;
}

bool Verify(const ScriptPathCase& test_case,
            uint32_t flags = dinero::consensus::SCRIPT_VERIFY_STANDARD) {
    std::string error;
    return ScriptVerifier::VerifyTaproot(
        test_case.tx, 0, test_case.prevouts, error, flags);
}

bool ExecuteBareTapscript(const std::vector<uint8_t>& script,
                          const std::vector<std::vector<uint8_t>>& stack,
                          uint32_t flags) {
    Transaction tx;
    tx.vin.emplace_back();
    std::vector<UTXOEntry> prevouts(1);
    const std::vector<uint8_t> leaf_hash(32);
    const std::array<uint8_t, 32> internal_key{};
    const std::array<uint8_t, 32> merkle_root{};
    std::string error;
    return TapscriptInterpreter::ExecuteTapscript(
        script, stack, tx, 0, prevouts, leaf_hash,
        internal_key, merkle_root, 0, flags, error);
}

TEST(TaprootScriptPathConsensus, AcceptsValidLeaf) {
    const ScriptPathCase test_case =
        BuildScriptPathCase({dinero::consensus::OP_1});
    EXPECT_TRUE(Verify(test_case));
}

TEST(TaprootScriptPathConsensus, CommitsToOutputKeyParity) {
    ScriptPathCase test_case =
        BuildScriptPathCase({dinero::consensus::OP_1});
    test_case.tx.vin[0].witness.back()[0] ^= 1;
    EXPECT_FALSE(Verify(test_case));
}

TEST(TaprootScriptPathConsensus, RemovesTrailingAnnexBeforePathSelection) {
    ScriptPathCase test_case =
        BuildScriptPathCase({dinero::consensus::OP_1});
    test_case.tx.vin[0].witness.push_back({0x50, 0x01, 0x02});
    EXPECT_TRUE(Verify(test_case));
}

TEST(TaprootScriptPathConsensus, SupportsCanonicalCompactSizeAbove252Bytes) {
    std::vector<uint8_t> script;
    for (size_t i = 0; i < 126; ++i) {
        script.push_back(dinero::consensus::OP_1);
        script.push_back(dinero::consensus::OP_DROP);
    }
    script.push_back(dinero::consensus::OP_1);
    ASSERT_EQ(script.size(), 253U);

    const ScriptPathCase test_case = BuildScriptPathCase(script);
    EXPECT_TRUE(Verify(test_case));
}

TEST(TaprootScriptPathConsensus, UnknownLeafVersionIsConsensusSuccess) {
    const ScriptPathCase test_case =
        BuildScriptPathCase({dinero::consensus::OP_RETURN}, 0xc2);
    EXPECT_TRUE(Verify(test_case, dinero::consensus::SCRIPT_VERIFY_STANDARD));
    EXPECT_FALSE(Verify(
        test_case,
        dinero::consensus::SCRIPT_VERIFY_STANDARD |
            dinero::consensus::SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_TAPROOT_VERSION));
}

TEST(TaprootScriptPathConsensus, UsesExactBIP342OpSuccessSet) {
    using dinero::consensus::TapscriptOpcodes::IsOpSuccess;

    EXPECT_TRUE(IsOpSuccess(0x50));
    EXPECT_TRUE(IsOpSuccess(0x62));
    EXPECT_TRUE(IsOpSuccess(0x7e));
    EXPECT_TRUE(IsOpSuccess(0x81));
    EXPECT_TRUE(IsOpSuccess(0x83));
    EXPECT_TRUE(IsOpSuccess(0x86));
    EXPECT_TRUE(IsOpSuccess(0x89));
    EXPECT_TRUE(IsOpSuccess(0x8a));
    EXPECT_TRUE(IsOpSuccess(0x8d));
    EXPECT_TRUE(IsOpSuccess(0x8e));
    EXPECT_TRUE(IsOpSuccess(0x95));
    EXPECT_TRUE(IsOpSuccess(0x99));
    EXPECT_TRUE(IsOpSuccess(0xbb));
    EXPECT_TRUE(IsOpSuccess(0xfe));

    EXPECT_FALSE(IsOpSuccess(0x7d));
    EXPECT_FALSE(IsOpSuccess(0x82));
    EXPECT_FALSE(IsOpSuccess(0x8b));
    EXPECT_FALSE(IsOpSuccess(0x8f));
    EXPECT_FALSE(IsOpSuccess(0x94));
    EXPECT_FALSE(IsOpSuccess(0x9a));
    EXPECT_FALSE(IsOpSuccess(0xb0));
    EXPECT_FALSE(IsOpSuccess(0xba));
    EXPECT_FALSE(IsOpSuccess(0xff));
}

TEST(TaprootScriptPathConsensus, InactiveUpgradeOpcodesShortCircuitSuccess) {
    EXPECT_TRUE(ExecuteBareTapscript(
        {dinero::consensus::OP_CHECKSIGFROMSTACK},
        {},
        dinero::consensus::SCRIPT_VERIFY_NONE));
    EXPECT_FALSE(ExecuteBareTapscript(
        {dinero::consensus::OP_CHECKSIGFROMSTACK},
        {},
        dinero::consensus::SCRIPT_VERIFY_CHECKSIGFROMSTACK));
}

TEST(TaprootScriptPathConsensus, OpSuccessPrecedesExecutionAndResourceRules) {
    EXPECT_TRUE(ExecuteBareTapscript(
        {dinero::consensus::OP_RETURN, 0xbb},
        {},
        dinero::consensus::SCRIPT_VERIFY_NONE));
    EXPECT_TRUE(ExecuteBareTapscript(
        {0xbb, dinero::consensus::OP_PUSHDATA1},
        std::vector<std::vector<uint8_t>>(1'001),
        dinero::consensus::SCRIPT_VERIFY_NONE));
}

TEST(TaprootScriptPathConsensus, OpSuccessScanParsesPushOperations) {
    // The 0xbb byte is pushed data, not an opcode.
    EXPECT_FALSE(ExecuteBareTapscript(
        {0x01, 0xbb, dinero::consensus::OP_RETURN},
        {},
        dinero::consensus::SCRIPT_VERIFY_NONE));

    // A malformed push before 0xbb fails decoding; the trailing byte is not
    // reinterpreted as an opcode.
    EXPECT_FALSE(ExecuteBareTapscript(
        {dinero::consensus::OP_PUSHDATA1, 0xff, 0xbb},
        {},
        dinero::consensus::SCRIPT_VERIFY_NONE));
}

TEST(TaprootScriptPathConsensus, InactiveCTVRetainsNop4Semantics) {
    EXPECT_TRUE(ExecuteBareTapscript(
        {dinero::consensus::OP_CHECKTEMPLATEVERIFY},
        {{0x01}},
        dinero::consensus::SCRIPT_VERIFY_NONE));
    EXPECT_TRUE(ExecuteBareTapscript(
        {dinero::consensus::OP_CHECKTEMPLATEVERIFY},
        {{0x01}},
        dinero::consensus::SCRIPT_VERIFY_CHECKTEMPLATEVERIFY));
    EXPECT_FALSE(ExecuteBareTapscript(
        {dinero::consensus::OP_CHECKTEMPLATEVERIFY},
        {{0x01}},
        dinero::consensus::SCRIPT_VERIFY_CHECKTEMPLATEVERIFY |
            dinero::consensus::SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS));
}

TEST(TaprootScriptPathConsensus, TapscriptHasNoLegacyScriptSizeLimit) {
    std::vector<uint8_t> script(10'001, 0xbb);
    EXPECT_TRUE(ExecuteBareTapscript(
        script, {}, dinero::consensus::SCRIPT_VERIFY_NONE));
}

TEST(TaprootScriptPathConsensus, UpgradeablePubkeyTypesAreConsensusSuccess) {
    const std::vector<uint8_t> signature{0x01};
    const std::vector<uint8_t> future_pubkey(33, 0x02);
    EXPECT_TRUE(ExecuteBareTapscript(
        {dinero::consensus::OP_CHECKSIG},
        {signature, future_pubkey},
        dinero::consensus::SCRIPT_VERIFY_NONE));
    EXPECT_FALSE(ExecuteBareTapscript(
        {dinero::consensus::OP_CHECKSIG},
        {signature, future_pubkey},
        dinero::consensus::SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_PUBKEYTYPE));
}

TEST(TaprootScriptPathConsensus, NonEmptyInvalidSchnorrSignatureFails) {
    const std::vector<uint8_t> invalid_signature(64, 0x01);
    const std::vector<uint8_t> invalid_pubkey(32, 0x02);
    EXPECT_FALSE(ExecuteBareTapscript(
        {dinero::consensus::OP_CHECKSIG},
        {invalid_signature, invalid_pubkey},
        dinero::consensus::SCRIPT_VERIFY_NONE));
}

TEST(TaprootScriptPathConsensus, ExplicitDefaultSighashByteIsInvalid) {
    const std::vector<uint8_t> explicit_default_signature(65, 0x01);
    const std::vector<uint8_t> pubkey(32, 0x02);
    EXPECT_FALSE(ExecuteBareTapscript(
        {dinero::consensus::OP_CHECKSIG},
        {explicit_default_signature, pubkey},
        dinero::consensus::SCRIPT_VERIFY_NONE));
}

TEST(TaprootScriptPathConsensus, CheckSigAddAcceptsScriptNumberZero) {
    const std::vector<uint8_t> pubkey(32, 0x02);
    EXPECT_TRUE(ExecuteBareTapscript(
        {dinero::consensus::OP_CHECKSIGADD,
         dinero::consensus::OP_0,
         dinero::consensus::OP_EQUAL},
        {{}, {}, pubkey},
        dinero::consensus::SCRIPT_VERIFY_NONE));
}

TEST(TaprootScriptPathConsensus, EnforcesTapscriptSignatureBudget) {
    const std::vector<uint8_t> signature{0x01};
    const std::vector<uint8_t> future_pubkey(33, 0x02);
    EXPECT_FALSE(ExecuteBareTapscript(
        {dinero::consensus::OP_CHECKSIG,
         dinero::consensus::OP_DROP,
         dinero::consensus::OP_CHECKSIG},
        {signature, future_pubkey, signature, future_pubkey},
        dinero::consensus::SCRIPT_VERIFY_NONE));
}

} // namespace
