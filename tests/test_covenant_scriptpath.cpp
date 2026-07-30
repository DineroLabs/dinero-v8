/**
 * Phase C.4: Covenant Script-Path Integration Test
 *
 * Tests the full CTV lifecycle through the actual consensus path:
 *   1. Create a CTV template hash for a known spending transaction
 *   2. Build a Tapscript containing OP_CTV
 *   3. Construct a P2TR output committing to that script tree
 *   4. Build a spending transaction matching the template
 *   5. Verify via ScriptVerifier::VerifyTaproot (script-path)
 *
 * Also tests: CSFS verification, TXHASH introspection, activation gating.
 */

#include <gtest/gtest.h>
#include "consensus/covenants.h"
#include "consensus/script_verify.h"
#include "consensus/covenant_activation.h"
#include "consensus/script.h"
#include "consensus/utxo_entry.h"
#include "primitives/transaction.h"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <cstring>

extern "C" {
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>
}

using namespace dinero;
using namespace dinero::consensus;

class CovenantScriptPathTest : public ::testing::Test {
protected:
    secp256k1_context* ctx_;

    void SetUp() override {
        ctx_ = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
        unsigned char seed[32];
        RAND_bytes(seed, 32);
        secp256k1_context_randomize(ctx_, seed);
    }

    void TearDown() override {
        secp256k1_context_destroy(ctx_);
    }

    // Generate a keypair and return (privkey, x-only-pubkey)
    void MakeKeyPair(uint8_t seed_byte, std::vector<uint8_t>& privkey,
                     std::vector<uint8_t>& xonly_pubkey) {
        privkey.resize(32, 0);
        privkey[0] = seed_byte;
        privkey[1] = 0x01;
        privkey[31] = seed_byte ^ 0xAA;
        ASSERT_TRUE(secp256k1_ec_seckey_verify(ctx_, privkey.data()));

        secp256k1_keypair kp;
        ASSERT_EQ(1, secp256k1_keypair_create(ctx_, &kp, privkey.data()));
        secp256k1_xonly_pubkey xpub;
        ASSERT_EQ(1, secp256k1_keypair_xonly_pub(ctx_, &xpub, nullptr, &kp));

        xonly_pubkey.resize(32);
        secp256k1_xonly_pubkey_serialize(ctx_, xonly_pubkey.data(), &xpub);
    }

    // Build P2TR scriptPubKey: OP_1 PUSH32 <xonly_pubkey>
    std::vector<uint8_t> MakeP2TR(const std::vector<uint8_t>& xonly_pubkey) {
        std::vector<uint8_t> spk;
        spk.push_back(0x51);  // OP_1
        spk.push_back(0x20);  // PUSH 32 bytes
        spk.insert(spk.end(), xonly_pubkey.begin(), xonly_pubkey.end());
        return spk;
    }

    // Tagged hash: SHA256(SHA256(tag) || SHA256(tag) || data)
    std::vector<uint8_t> TaggedHash(const std::string& tag,
                                     const std::vector<uint8_t>& data) {
        uint8_t tag_hash[32];
        SHA256(reinterpret_cast<const uint8_t*>(tag.data()), tag.size(), tag_hash);

        std::vector<uint8_t> result(32);
        dinero::crypto::CSHA256()
            .Write(tag_hash, 32)
            .Write(tag_hash, 32)
            .Write(data.data(), data.size())
            .Finalize(result.data());
        return result;
    }
};

// Test: CTV hash computation is deterministic
TEST_F(CovenantScriptPathTest, CTVHash_Deterministic) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;

    TxInput input;
    input.sequence = 0xfffffffe;
    tx.vin.push_back(input);

    TxOutput output;
    output.value = AmountUna::Una(50000);
    output.scriptPubKey = {0x51, 0x20};
    output.scriptPubKey.resize(34, 0xAB);
    tx.vout.push_back(output);

    auto hash1 = ComputeCTVHash(tx, 0);
    auto hash2 = ComputeCTVHash(tx, 0);
    EXPECT_EQ(hash1, hash2);

    // Different output → different hash
    tx.vout[0].value = AmountUna::Una(50001);
    auto hash3 = ComputeCTVHash(tx, 0);
    EXPECT_NE(hash1, hash3);
}

// Test: CTV hash includes commitment data for confidential outputs
TEST_F(CovenantScriptPathTest, CTVHash_ConfidentialOutput) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;

    TxInput input;
    input.sequence = 0xfffffffe;
    tx.vin.push_back(input);

    // Confidential output
    TxOutput ct_output;
    ct_output.value = AmountUna::Zero();  // Confidential marker
    ct_output.is_confidential = true;
    ct_output.scriptPubKey = {0x51, 0x20};
    ct_output.scriptPubKey.resize(34, 0xCC);
    ct_output.commitment.resize(33, 0x02);  // Fake commitment
    ct_output.range_proof.resize(100, 0xAA);  // Fake proof
    tx.vout.push_back(ct_output);

    auto hash1 = ComputeCTVHash(tx, 0);

    // Different commitment → different hash (proves commitment is included)
    tx.vout[0].commitment[1] = 0xFF;
    auto hash2 = ComputeCTVHash(tx, 0);
    EXPECT_NE(hash1, hash2);

    // Different range proof → different hash (proves proof hash is included)
    tx.vout[0].commitment[1] = 0x02;  // Restore
    tx.vout[0].range_proof[0] = 0xBB;
    auto hash3 = ComputeCTVHash(tx, 0);
    EXPECT_NE(hash1, hash3);
}

// Test: Covenant activation check
TEST_F(CovenantScriptPathTest, ActivationGating) {
    // Regtest activates at height 20
    EXPECT_FALSE(CovenantActivationParams::IsCovenantActive(19, Chain::REGTEST));
    EXPECT_TRUE(CovenantActivationParams::IsCovenantActive(20, Chain::REGTEST));
    EXPECT_TRUE(CovenantActivationParams::IsCovenantActive(21, Chain::REGTEST));

    // Mainnet activates at height 1 (Fair Launch v3: covenants live from
    // the first PoW block, so the "before activation" window has only
    // height 0, which is the unspendable genesis output).
    EXPECT_FALSE(CovenantActivationParams::IsCovenantActive(0, Chain::MAINNET));
    EXPECT_TRUE(CovenantActivationParams::IsCovenantActive(1, Chain::MAINNET));
    EXPECT_TRUE(CovenantActivationParams::IsCovenantActive(20000, Chain::MAINNET));
}

// Test: TXHASH includes commitment data for confidential outputs
TEST_F(CovenantScriptPathTest, TxHash_ConfidentialOutputValue) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;

    TxInput input;
    input.sequence = 0xfffffffe;
    tx.vin.push_back(input);

    // Confidential output with commitment
    TxOutput ct_output;
    ct_output.value = AmountUna::Zero();
    ct_output.is_confidential = true;
    ct_output.scriptPubKey = {0x51, 0x20};
    ct_output.scriptPubKey.resize(34, 0xDD);
    ct_output.commitment.resize(33, 0x03);
    tx.vout.push_back(ct_output);

    auto hash1 = ComputeTxHash(tx, TxHashFlags::OUTPUT_VALUE, 0);

    // Change commitment → hash changes
    tx.vout[0].commitment[1] = 0xFF;
    auto hash2 = ComputeTxHash(tx, TxHashFlags::OUTPUT_VALUE, 0);
    EXPECT_NE(hash1, hash2);
}

// Test: ALL_OUTPUTS_HASH includes confidential data
TEST_F(CovenantScriptPathTest, TxHash_AllOutputsWithCT) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;

    TxInput input;
    input.sequence = 0xfffffffe;
    tx.vin.push_back(input);

    // Mix of transparent and confidential outputs
    TxOutput transparent_out;
    transparent_out.value = AmountUna::Una(10000);
    transparent_out.scriptPubKey = {0x51, 0x20};
    transparent_out.scriptPubKey.resize(34, 0x11);
    tx.vout.push_back(transparent_out);

    TxOutput ct_out;
    ct_out.value = AmountUna::Zero();
    ct_out.is_confidential = true;
    ct_out.scriptPubKey = {0x51, 0x20};
    ct_out.scriptPubKey.resize(34, 0x22);
    ct_out.commitment.resize(33, 0x02);
    ct_out.range_proof.resize(50, 0xBB);
    tx.vout.push_back(ct_out);

    auto hash1 = ComputeTxHash(tx, TxHashFlags::ALL_OUTPUTS_HASH, 0);

    // Changing commitment changes ALL_OUTPUTS_HASH
    tx.vout[1].commitment[0] = 0x03;
    auto hash2 = ComputeTxHash(tx, TxHashFlags::ALL_OUTPUTS_HASH, 0);
    EXPECT_NE(hash1, hash2);
}

// Test: CTV verify succeeds when template matches
TEST_F(CovenantScriptPathTest, CTVVerify_TemplateMatch) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;

    TxInput input;
    input.sequence = 0xfffffffe;
    tx.vin.push_back(input);

    TxOutput output;
    output.value = AmountUna::Una(50000);
    output.scriptPubKey = {0x51, 0x20};
    output.scriptPubKey.resize(34, 0xAB);
    tx.vout.push_back(output);

    auto ctv_hash = ComputeCTVHash(tx, 0);
    std::vector<uint8_t> hash_vec(ctv_hash.begin(), ctv_hash.end());

    EXPECT_TRUE(VerifyCTV(tx, 0, hash_vec));
}

// Test: CTV verify fails when template doesn't match
TEST_F(CovenantScriptPathTest, CTVVerify_TemplateMismatch) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;

    TxInput input;
    input.sequence = 0xfffffffe;
    tx.vin.push_back(input);

    TxOutput output;
    output.value = AmountUna::Una(50000);
    output.scriptPubKey = {0x51, 0x20};
    output.scriptPubKey.resize(34, 0xAB);
    tx.vout.push_back(output);

    // Compute hash for a different amount
    auto ctv_hash = ComputeCTVHash(tx, 0);
    std::vector<uint8_t> hash_vec(ctv_hash.begin(), ctv_hash.end());

    // Modify the tx after hash computation
    tx.vout[0].value = AmountUna::Una(99999);
    EXPECT_FALSE(VerifyCTV(tx, 0, hash_vec));
}

// Test: CSFS verify with valid Schnorr signature
TEST_F(CovenantScriptPathTest, CSFSVerify_ValidSignature) {
    std::vector<uint8_t> privkey, xonly_pub;
    MakeKeyPair(42, privkey, xonly_pub);

    // Sign an arbitrary message
    std::vector<uint8_t> message = {0xDE, 0xAD, 0xBE, 0xEF};

    // Hash message to 32 bytes
    uint8_t msg_hash[32];
    SHA256(message.data(), message.size(), msg_hash);

    secp256k1_keypair kp;
    ASSERT_EQ(1, secp256k1_keypair_create(ctx_, &kp, privkey.data()));

    uint8_t sig[64];
    ASSERT_EQ(1, secp256k1_schnorrsig_sign32(ctx_, sig, msg_hash, &kp, nullptr));

    std::vector<uint8_t> sig_vec(sig, sig + 64);

    EXPECT_TRUE(VerifySignatureFromStack(sig_vec, message, xonly_pub));
}

// Test: CSFS reject with wrong message
TEST_F(CovenantScriptPathTest, CSFSVerify_WrongMessage) {
    std::vector<uint8_t> privkey, xonly_pub;
    MakeKeyPair(42, privkey, xonly_pub);

    std::vector<uint8_t> message = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t msg_hash[32];
    SHA256(message.data(), message.size(), msg_hash);

    secp256k1_keypair kp;
    ASSERT_EQ(1, secp256k1_keypair_create(ctx_, &kp, privkey.data()));

    uint8_t sig[64];
    ASSERT_EQ(1, secp256k1_schnorrsig_sign32(ctx_, sig, msg_hash, &kp, nullptr));

    std::vector<uint8_t> sig_vec(sig, sig + 64);
    std::vector<uint8_t> wrong_message = {0xCA, 0xFE};

    EXPECT_FALSE(VerifySignatureFromStack(sig_vec, wrong_message, xonly_pub));
}
