/**
 * Tests the Pedersen commitment balance verification logic used by
 * ConfidentialTxBuilder::VerifyCommitmentBalance.
 *
 * Exercises secp256k1_pedersen_verify_tally with real commitments
 * including the CT→CT path (random-blind inputs) that previously hung
 * due to unchecked parse return values.
 *
 * Uses the secp256k1-zkp API directly to avoid wallet link dependencies.
 */

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <cstring>

extern "C" {
#include <secp256k1.h>
#include <secp256k1_generator.h>
#include <secp256k1_rangeproof.h>
}

class CTVerifyBalanceTest : public ::testing::Test {
protected:
    secp256k1_context* ctx_;

    void SetUp() override {
        ctx_ = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    }
    void TearDown() override {
        secp256k1_context_destroy(ctx_);
    }

    // Create Pedersen commitment: C = blind·G + amount·H
    std::vector<uint8_t> MakeCommitment(uint64_t amount, const unsigned char* blind) {
        secp256k1_pedersen_commitment commit;
        EXPECT_EQ(1, secp256k1_pedersen_commit(ctx_, &commit, blind, amount,
                                                secp256k1_generator_h));
        std::vector<uint8_t> out(33);
        EXPECT_EQ(1, secp256k1_pedersen_commitment_serialize(ctx_, out.data(), &commit));
        return out;
    }

    // Deterministic non-zero blind from seed (valid secp256k1 scalar)
    void MakeBlind(unsigned char out[32], uint8_t seed) {
        memset(out, 0, 32);
        out[0] = seed;
        out[1] = 0x01;  // ensure non-zero
        out[31] = seed ^ 0xFF;
    }

    // Mirrors ConfidentialTxBuilder::VerifyCommitmentBalance exactly
    bool VerifyBalance(
        const std::vector<std::vector<uint8_t>>& input_commits,
        const std::vector<std::vector<uint8_t>>& output_commits,
        uint64_t fee
    ) {
        // Parse input commitments (positive side)
        std::vector<secp256k1_pedersen_commitment> pos;
        for (size_t i = 0; i < input_commits.size(); ++i) {
            if (input_commits[i].size() != 33) return false;
            secp256k1_pedersen_commitment pc;
            if (!secp256k1_pedersen_commitment_parse(ctx_, &pc, input_commits[i].data()))
                return false;
            pos.push_back(pc);
        }

        // Parse output commitments (negative side)
        std::vector<secp256k1_pedersen_commitment> neg;
        for (size_t i = 0; i < output_commits.size(); ++i) {
            if (output_commits[i].size() != 33) return false;
            secp256k1_pedersen_commitment pc;
            if (!secp256k1_pedersen_commitment_parse(ctx_, &pc, output_commits[i].data()))
                return false;
            neg.push_back(pc);
        }

        // Fee pseudo-commitment (blind=0)
        secp256k1_pedersen_commitment fee_commit;
        unsigned char zero[32] = {0};
        if (secp256k1_pedersen_commit(ctx_, &fee_commit, zero, fee,
                                       secp256k1_generator_h) != 1)
            return false;
        neg.push_back(fee_commit);

        // Build pointer arrays
        std::vector<const secp256k1_pedersen_commitment*> pp, np;
        for (auto& c : pos) pp.push_back(&c);
        for (auto& c : neg) np.push_back(&c);

        return secp256k1_pedersen_verify_tally(ctx_,
            pp.data(), pp.size(), np.data(), np.size()) == 1;
    }
};

// Shield path: zero-blind input, balanced output, explicit fee
TEST_F(CTVerifyBalanceTest, ShieldPath_Balanced) {
    unsigned char zero[32] = {0};
    uint64_t in_amt = 10000, out_amt = 7000, fee = 3000;

    auto in_c = MakeCommitment(in_amt, zero);

    // Output blind must balance: sum(in_blinds) = sum(out_blinds)
    // With only zero-blind inputs and one output, last_blind = 0
    auto out_c = MakeCommitment(out_amt, zero);

    EXPECT_TRUE(VerifyBalance({in_c}, {out_c}, fee));
}

// CT→CT: random-blind inputs AND outputs (the previously hanging path)
TEST_F(CTVerifyBalanceTest, CTtoCT_SingleInput_Balanced) {
    unsigned char blind_in[32], blind_out1[32];
    MakeBlind(blind_in, 42);
    MakeBlind(blind_out1, 99);

    uint64_t in_amt = 50000, out1_amt = 30000, out2_amt = 15000, fee = 5000;

    // Balance last output's blind: last = in_blind - out1_blind
    unsigned char blind_out2[32];
    const unsigned char* ptrs[] = { blind_in, blind_out1 };
    ASSERT_EQ(1, secp256k1_pedersen_blind_sum(ctx_, blind_out2, ptrs, 2, 1));

    auto in_c  = MakeCommitment(in_amt, blind_in);
    auto out1  = MakeCommitment(out1_amt, blind_out1);
    auto out2  = MakeCommitment(out2_amt, blind_out2);

    EXPECT_TRUE(VerifyBalance({in_c}, {out1, out2}, fee));
}

// CT→CT: multiple random-blind inputs
TEST_F(CTVerifyBalanceTest, CTtoCT_MultiInput_Balanced) {
    unsigned char b_in1[32], b_in2[32];
    MakeBlind(b_in1, 10);
    MakeBlind(b_in2, 20);

    uint64_t in1 = 25000, in2 = 35000, out1 = 55000, fee = 5000;

    // Balance: last_blind = sum(in1, in2) - (nothing)
    unsigned char b_out[32];
    const unsigned char* ptrs[] = { b_in1, b_in2 };
    ASSERT_EQ(1, secp256k1_pedersen_blind_sum(ctx_, b_out, ptrs, 2, 2));

    auto c_in1 = MakeCommitment(in1, b_in1);
    auto c_in2 = MakeCommitment(in2, b_in2);
    auto c_out = MakeCommitment(out1, b_out);

    EXPECT_TRUE(VerifyBalance({c_in1, c_in2}, {c_out}, fee));
}

// Wrong fee → returns false, does NOT hang
TEST_F(CTVerifyBalanceTest, Unbalanced_WrongFee) {
    unsigned char blind[32];
    MakeBlind(blind, 42);

    uint64_t in_amt = 50000, out_amt = 30000;

    unsigned char b_out[32];
    const unsigned char* ptrs[] = { blind };
    ASSERT_EQ(1, secp256k1_pedersen_blind_sum(ctx_, b_out, ptrs, 1, 1));

    auto in_c  = MakeCommitment(in_amt, blind);
    auto out_c = MakeCommitment(out_amt, b_out);

    EXPECT_TRUE(VerifyBalance({in_c}, {out_c}, 20000));   // correct fee
    EXPECT_FALSE(VerifyBalance({in_c}, {out_c}, 19999));  // wrong fee
    EXPECT_FALSE(VerifyBalance({in_c}, {out_c}, 20001));  // wrong fee
}

// Malformed commitment (wrong size) → returns false, does NOT hang
TEST_F(CTVerifyBalanceTest, MalformedCommitment_WrongSize) {
    std::vector<uint8_t> bad(16, 0xAB);
    unsigned char zero[32] = {0};
    auto good = MakeCommitment(1000, zero);

    EXPECT_FALSE(VerifyBalance({bad}, {good}, 0));
    EXPECT_FALSE(VerifyBalance({good}, {bad}, 0));
}

// Invalid EC point (right size, bad data) → returns false, does NOT hang
TEST_F(CTVerifyBalanceTest, InvalidECPoint) {
    std::vector<uint8_t> bad(33, 0xFF);
    unsigned char zero[32] = {0};
    auto good = MakeCommitment(1000, zero);

    EXPECT_FALSE(VerifyBalance({bad}, {good}, 0));
}

// Empty commitment → returns false, does NOT hang
TEST_F(CTVerifyBalanceTest, EmptyCommitment) {
    std::vector<uint8_t> empty;
    unsigned char zero[32] = {0};
    auto good = MakeCommitment(1000, zero);

    EXPECT_FALSE(VerifyBalance({empty}, {good}, 0));
}
