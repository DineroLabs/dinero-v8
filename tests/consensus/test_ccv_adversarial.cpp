// Adversarial coverage for CCV successor binding (CCV_SUCCESSOR_BINDING_V1).
//
// CCV is a Dinero-specific consensus protocol. Unlike CTV, it has no upstream
// BIP, no published vector corpus, and no external implementation to differ
// against — so the only available assurance is deliberately trying to break it.
// These tests attack the numbered verification rules in the spec one at a time.
//
// Method note: each test violates EXACTLY ONE rule and keeps every other
// commitment internally consistent. That matters more than it looks. Changing
// a state field without recomputing the P2TR outputs derived from it would make
// the transition fail on an output-script mismatch instead of the rule under
// test, and the test would pass while proving nothing. RebuildOutputs() below
// exists to keep every unrelated commitment valid.

#include "consensus/chainparams.h"
#include "consensus/covenant_activation.h"
#include "consensus/covenants.h"
#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include "consensus/script_verify.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace dinero;
using namespace dinero::consensus;

void WriteLE32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

std::vector<uint8_t> SerializeState(const ContractState& state) {
    std::vector<uint8_t> out;
    out.insert(out.end(), state.stateHash.begin(), state.stateHash.end());
    out.insert(out.end(), state.codeHash.begin(), state.codeHash.end());
    WriteLE32(out, state.counter);
    WriteLE32(out, static_cast<uint32_t>(state.data.size()));
    out.insert(out.end(), state.data.begin(), state.data.end());
    return out;
}

// ---------------------------------------------------------------------------
// Struct-level attacks against VerifyContractTransition
// ---------------------------------------------------------------------------

class CcvAdversarialTest : public ::testing::Test {
protected:
    static constexpr uint64_t kValue = 250'000;

    const std::vector<uint8_t> script_{
        static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
        static_cast<uint8_t>(OP_TRUE)};
    ContractState previous_;
    ContractState next_;
    std::array<uint8_t, 32> merkle_root_{};
    std::array<uint8_t, 32> internal_key_{};
    uint8_t parity_{0};
    Transaction tx_;
    std::vector<UTXOEntry> inputs_;

    void SetUp() override {
        for (size_t index = 0; index < merkle_root_.size(); ++index) {
            merkle_root_[index] = static_cast<uint8_t>(index + 1);
        }
        previous_.codeHash = ComputeContractCodeHash(script_);
        previous_.counter = 41;
        previous_.data = {0x10, 0x20, 0x30};
        previous_.stateHash = ComputeContractStateHash(previous_);

        next_.codeHash = previous_.codeHash;
        next_.counter = 42;
        next_.data = {0x40, 0x50, 0x60};
        next_.stateHash = ComputeContractStateHash(next_);

        tx_.vin.emplace_back();
        tx_.vout.emplace_back(AmountUna::Una(kValue), std::vector<uint8_t>{});
        inputs_.emplace_back(
            AmountUna::Una(kValue), std::vector<uint8_t>{}, 100, false);
        ASSERT_TRUE(RebuildOutputs());
    }

    // Recompute every commitment derived from the current previous_/next_ so
    // that the ONLY thing wrong in a given test is the rule under attack.
    bool RebuildOutputs() {
        previous_.stateHash = ComputeContractStateHash(previous_);
        next_.stateHash = ComputeContractStateHash(next_);
        if (!DeriveContractInternalKey(previous_, internal_key_)) {
            return false;
        }
        std::vector<uint8_t> spent_script;
        if (!ComputeContractOutputScript(
                previous_, merkle_root_, spent_script, &parity_)) {
            return false;
        }
        std::vector<uint8_t> successor_script;
        if (!ComputeContractOutputScript(
                next_, merkle_root_, successor_script)) {
            return false;
        }
        inputs_[0] = UTXOEntry(AmountUna::Una(kValue), spent_script, 100, false);
        tx_.vout[0] = TxOutput(AmountUna::Una(kValue), successor_script);
        return true;
    }

    bool Verify() const {
        const ContractSpendContext context{
            inputs_, script_, internal_key_, merkle_root_, parity_};
        return VerifyContractTransition(tx_, 0, previous_, next_, context);
    }
};

// Baseline. If this ever fails, every negative test below is meaningless
// because they would reject for the wrong reason.
TEST_F(CcvAdversarialTest, BaselineTransitionIsAccepted) {
    EXPECT_TRUE(Verify());
}

// Spec rule 4: next.counter == previous.counter + 1.
//
// These are the classic state-machine attacks and none of them is covered by
// the existing wrap test, which only exercises UINT32_MAX -> 0.
TEST_F(CcvAdversarialTest, RejectsCounterReplay) {
    // Replay: re-commit the same counter, rolling the contract nowhere while
    // still producing a structurally valid successor.
    next_.counter = previous_.counter;
    ASSERT_TRUE(RebuildOutputs());
    EXPECT_FALSE(Verify());
}

TEST_F(CcvAdversarialTest, RejectsCounterRewind) {
    next_.counter = previous_.counter - 1;
    ASSERT_TRUE(RebuildOutputs());
    EXPECT_FALSE(Verify());
}

TEST_F(CcvAdversarialTest, RejectsCounterSkip) {
    next_.counter = previous_.counter + 2;
    ASSERT_TRUE(RebuildOutputs());
    EXPECT_FALSE(Verify());
}

TEST_F(CcvAdversarialTest, RejectsCounterJumpToMax) {
    next_.counter = std::numeric_limits<uint32_t>::max();
    ASSERT_TRUE(RebuildOutputs());
    EXPECT_FALSE(Verify());
}

// Spec rule 3: previous.counter != UINT32_MAX, so a contract cannot wrap.
// Distinct from the existing test in that the successor commitments here are
// fully rebuilt and internally consistent, isolating the terminal-counter rule.
TEST_F(CcvAdversarialTest, RejectsTerminalPreviousCounterEvenWhenConsistent) {
    previous_.counter = std::numeric_limits<uint32_t>::max();
    next_.counter = 0;
    ASSERT_TRUE(RebuildOutputs());
    EXPECT_FALSE(Verify());
}

// Spec rule 6: previous.codeHash == SHA256(revealed tapscript).
//
// The script-substitution attack: commit to a DIFFERENT contract's code while
// revealing this tapscript. Everything else — state hashes, derived keys, both
// P2TR outputs — is rebuilt consistently from the forged codeHash, so the only
// broken link is code identity.
TEST_F(CcvAdversarialTest, RejectsCodeHashThatIsNotTheRevealedScript) {
    const std::vector<uint8_t> other_script{
        static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
        static_cast<uint8_t>(OP_DROP),
        static_cast<uint8_t>(OP_TRUE)};
    const auto forged = ComputeContractCodeHash(other_script);
    ASSERT_NE(forged, previous_.codeHash);

    previous_.codeHash = forged;
    next_.codeHash = forged;
    ASSERT_TRUE(RebuildOutputs());
    EXPECT_FALSE(Verify());
}

// Spec rule 5: next.codeHash == previous.codeHash. Immutable code across the
// transition — a contract must not be able to rewrite itself mid-chain.
TEST_F(CcvAdversarialTest, RejectsCodeMutationAcrossTransition) {
    const std::vector<uint8_t> other_script{
        static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
        static_cast<uint8_t>(OP_DROP),
        static_cast<uint8_t>(OP_TRUE)};
    next_.codeHash = ComputeContractCodeHash(other_script);
    ASSERT_TRUE(RebuildOutputs());
    EXPECT_FALSE(Verify());
}

// Boundary: the maximum permitted data size must still be ACCEPTED. Testing
// only the rejection side would let an off-by-one silently break every
// legitimate contract that uses a full-size state.
TEST_F(CcvAdversarialTest, AcceptsExactlyMaximumStateDataSize) {
    previous_.data.assign(MAX_CONTRACT_STATE_DATA_SIZE, 0x11);
    next_.data.assign(MAX_CONTRACT_STATE_DATA_SIZE, 0x22);
    ASSERT_TRUE(RebuildOutputs());
    EXPECT_TRUE(Verify());
}

TEST_F(CcvAdversarialTest, RejectsOneByteOverMaximumStateDataSize) {
    previous_.data.assign(MAX_CONTRACT_STATE_DATA_SIZE, 0x11);
    next_.data.assign(MAX_CONTRACT_STATE_DATA_SIZE + 1, 0x22);
    ASSERT_TRUE(RebuildOutputs());
    EXPECT_FALSE(Verify());
}

// Spec rule 2: both state hashes must recompute from their contents.
//
// Found by tools/covenant_mutation_harness.py: deleting the recompute check
// from production left this whole lane green, because every other test either
// builds consistent states or corrupts stateHash in a way that ALSO breaks the
// derived scripts.
//
// The attack the recompute check actually stops: the P2TR scripts commit only
// to stateHash. counter and data are bound to that hash by nothing else. Keep a
// stateHash that still derives the correct spent and successor scripts, but lie
// about the counters, and rule 4 ends up validating attacker-chosen numbers
// that are not committed to anything -- counter monotonicity stops meaning
// anything at all. Note these tests deliberately do NOT call RebuildOutputs().
TEST_F(CcvAdversarialTest, RejectsCounterNotCommittedByTheStateHash) {
    previous_.counter = 1000;
    next_.counter = 1001;  // satisfies rule 4 on its face
    EXPECT_FALSE(Verify());
}

TEST_F(CcvAdversarialTest, RejectsDataNotCommittedByTheStateHash) {
    previous_.data.push_back(0xff);
    next_.data.push_back(0xff);
    EXPECT_FALSE(Verify());
}

// Spec rule 12: no other output may carry the same successor script. The
// existing test appends an adjacent duplicate; this places it far away to be
// sure detection is not merely a neighbour comparison.
TEST_F(CcvAdversarialTest, RejectsDuplicateSuccessorAtDistantIndex) {
    for (int filler = 0; filler < 4; ++filler) {
        tx_.vout.emplace_back(
            AmountUna::Una(10), std::vector<uint8_t>{OP_TRUE});
    }
    tx_.vout.push_back(tx_.vout[0]);
    EXPECT_FALSE(Verify());
}

// Spec rule 1: the index must exist in the outputs.
TEST_F(CcvAdversarialTest, RejectsWhenOutputIndexIsMissing) {
    tx_.vout.clear();
    const ContractSpendContext context{
        inputs_, script_, internal_key_, merkle_root_, parity_};
    EXPECT_FALSE(
        VerifyContractTransition(tx_, 0, previous_, next_, context));
}

// Spec rule 1: the index must exist in the spent-UTXO vector. A short UTXO
// vector must never be read past its end.
TEST_F(CcvAdversarialTest, RejectsWhenSpentUtxoIsMissing) {
    tx_.vin.emplace_back();
    tx_.vout.emplace_back(AmountUna::Una(kValue), std::vector<uint8_t>{OP_TRUE});
    const ContractSpendContext context{
        inputs_, script_, internal_key_, merkle_root_, parity_};
    // inputs_ still has a single entry, so index 1 has no spent output.
    EXPECT_FALSE(
        VerifyContractTransition(tx_, 1, previous_, next_, context));
}

// Spec rule 10: exact value preservation. Inflation is the attack that matters
// most; the existing suite covers only the one-under case.
TEST_F(CcvAdversarialTest, RejectsValueInflation) {
    tx_.vout[0].value = AmountUna::Una(kValue + 1);
    EXPECT_FALSE(Verify());
}

// ---------------------------------------------------------------------------
// Index binding across multiple CCV inputs
// ---------------------------------------------------------------------------
//
// The spec states "the index mapping separates multiple CCV inputs". If that
// mapping were not enforced, two contracts spent in one transaction could have
// their successors swapped — each output is individually a valid successor of
// SOME contract, just not the one at its own index.

TEST(CcvAdversarialMultiInput, RejectsSuccessorsSwappedBetweenInputs) {
    const std::vector<uint8_t> script{
        static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
        static_cast<uint8_t>(OP_TRUE)};
    std::array<uint8_t, 32> merkle_root{};
    for (size_t i = 0; i < merkle_root.size(); ++i) {
        merkle_root[i] = static_cast<uint8_t>(i + 1);
    }

    // Two independent contracts differing only in their data payload.
    const auto build = [&](uint32_t counter, uint8_t tag,
                           ContractState& prev, ContractState& next,
                           std::vector<uint8_t>& spent,
                           std::vector<uint8_t>& successor,
                           std::array<uint8_t, 32>& key, uint8_t& parity) {
        prev.codeHash = ComputeContractCodeHash(script);
        prev.counter = counter;
        prev.data = {tag};
        prev.stateHash = ComputeContractStateHash(prev);
        next.codeHash = prev.codeHash;
        next.counter = counter + 1;
        next.data = {static_cast<uint8_t>(tag + 0x80)};
        next.stateHash = ComputeContractStateHash(next);
        ASSERT_TRUE(DeriveContractInternalKey(prev, key));
        ASSERT_TRUE(
            ComputeContractOutputScript(prev, merkle_root, spent, &parity));
        ASSERT_TRUE(
            ComputeContractOutputScript(next, merkle_root, successor));
    };

    ContractState prev_a, next_a, prev_b, next_b;
    std::vector<uint8_t> spent_a, succ_a, spent_b, succ_b;
    std::array<uint8_t, 32> key_a{}, key_b{};
    uint8_t parity_a = 0, parity_b = 0;
    build(10, 0x01, prev_a, next_a, spent_a, succ_a, key_a, parity_a);
    build(20, 0x02, prev_b, next_b, spent_b, succ_b, key_b, parity_b);
    ASSERT_NE(succ_a, succ_b);

    constexpr uint64_t kA = 111'000;
    constexpr uint64_t kB = 222'000;

    Transaction tx;
    tx.vin.emplace_back();
    tx.vin.emplace_back();
    // Deliberately swapped: input 0's successor sits at index 1 and vice versa.
    tx.vout.emplace_back(AmountUna::Una(kB), succ_b);
    tx.vout.emplace_back(AmountUna::Una(kA), succ_a);

    const std::vector<UTXOEntry> inputs{
        UTXOEntry(AmountUna::Una(kA), spent_a, 100, false),
        UTXOEntry(AmountUna::Una(kB), spent_b, 100, false)};

    const ContractSpendContext ctx_a{
        inputs, script, key_a, merkle_root, parity_a};
    const ContractSpendContext ctx_b{
        inputs, script, key_b, merkle_root, parity_b};

    EXPECT_FALSE(VerifyContractTransition(tx, 0, prev_a, next_a, ctx_a));
    EXPECT_FALSE(VerifyContractTransition(tx, 1, prev_b, next_b, ctx_b));

    // Sanity: the same states in the CORRECT positions must verify, proving
    // the rejections above are about index binding and nothing else.
    Transaction ordered;
    ordered.vin.emplace_back();
    ordered.vin.emplace_back();
    ordered.vout.emplace_back(AmountUna::Una(kA), succ_a);
    ordered.vout.emplace_back(AmountUna::Una(kB), succ_b);
    EXPECT_TRUE(VerifyContractTransition(ordered, 0, prev_a, next_a, ctx_a));
    EXPECT_TRUE(VerifyContractTransition(ordered, 1, prev_b, next_b, ctx_b));
}

// ---------------------------------------------------------------------------
// Wire-encoding attacks through the real script-execution path
// ---------------------------------------------------------------------------
//
// DeserializeContractState is file-local to the interpreter, so these run
// through ScriptVerifier::VerifyTaproot — the production path — rather than
// calling the parser directly.

class CcvWireFormatTest : public ::testing::Test {
protected:
    std::vector<uint8_t> script_;
    std::array<uint8_t, 32> merkle_root_{};
    ContractState previous_;
    ContractState next_;
    std::vector<uint8_t> control_block_;
    std::vector<uint8_t> spent_script_;
    std::vector<uint8_t> successor_script_;
    uint32_t flags_ = 0;

    void SetUp() override {
        script_ = {static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
                   static_cast<uint8_t>(OP_TRUE)};
        const std::vector<uint8_t> leaf_hash = TapLeafHash(0xc0, script_);
        ASSERT_EQ(leaf_hash.size(), 32U);
        std::copy(leaf_hash.begin(), leaf_hash.end(), merkle_root_.begin());

        previous_.codeHash = ComputeContractCodeHash(script_);
        previous_.counter = 7;
        previous_.data = {0xaa};
        previous_.stateHash = ComputeContractStateHash(previous_);
        next_.codeHash = previous_.codeHash;
        next_.counter = 8;
        next_.data = {0xbb};
        next_.stateHash = ComputeContractStateHash(next_);

        std::array<uint8_t, 32> internal_key{};
        ASSERT_TRUE(DeriveContractInternalKey(previous_, internal_key));
        uint8_t parity = 0;
        ASSERT_TRUE(ComputeContractOutputScript(
            previous_, merkle_root_, spent_script_, &parity));
        ASSERT_TRUE(ComputeContractOutputScript(
            next_, merkle_root_, successor_script_));

        control_block_ = {static_cast<uint8_t>(0xc0 | parity)};
        control_block_.insert(
            control_block_.end(), internal_key.begin(), internal_key.end());

        SelectParams(Chain::REGTEST);
        flags_ = CovenantActivationParams::StandardFlags(20, Params());
        ASSERT_NE(flags_ & SCRIPT_VERIFY_CHECKCONTRACT, 0U);
    }

    void TearDown() override { SelectParams(Chain::MAINNET); }

    // Run the production verifier with caller-supplied raw state encodings.
    bool VerifyWith(const std::vector<uint8_t>& prev_bytes,
                    const std::vector<uint8_t>& next_bytes,
                    std::string& error) const {
        Transaction tx;
        tx.vin.emplace_back();
        tx.vin[0].witness = {
            prev_bytes, next_bytes, script_, control_block_};
        tx.vout.emplace_back(AmountUna::Una(75'000), successor_script_);
        const std::vector<UTXOEntry> inputs{
            UTXOEntry(AmountUna::Una(75'000), spent_script_, 100, false)};
        return ScriptVerifier::VerifyTaproot(tx, 0, inputs, error, flags_);
    }
};

TEST_F(CcvWireFormatTest, BaselineWellFormedEncodingIsAccepted) {
    std::string error;
    EXPECT_TRUE(VerifyWith(
        SerializeState(previous_), SerializeState(next_), error)) << error;
}

TEST_F(CcvWireFormatTest, RejectsTrailingBytesAfterState) {
    // Spec: "No trailing bytes are permitted." A tolerated suffix would make
    // the encoding malleable — the same logical state with several byte forms.
    auto next_bytes = SerializeState(next_);
    next_bytes.push_back(0x00);
    std::string error;
    EXPECT_FALSE(VerifyWith(SerializeState(previous_), next_bytes, error));
}

TEST_F(CcvWireFormatTest, RejectsDataLengthLongerThanPayload) {
    auto next_bytes = SerializeState(next_);
    // dataLen field occupies bytes [68, 72).
    next_bytes[68] = static_cast<uint8_t>(next_bytes[68] + 1);
    std::string error;
    EXPECT_FALSE(VerifyWith(SerializeState(previous_), next_bytes, error));
}

TEST_F(CcvWireFormatTest, RejectsDataLengthShorterThanPayload) {
    auto next_bytes = SerializeState(next_);
    ASSERT_GT(next_bytes[68], 0);
    next_bytes[68] = static_cast<uint8_t>(next_bytes[68] - 1);
    std::string error;
    EXPECT_FALSE(VerifyWith(SerializeState(previous_), next_bytes, error));
}

TEST_F(CcvWireFormatTest, RejectsHugeDeclaredDataLength) {
    // A length field near UINT32_MAX must be rejected by the size comparison
    // rather than causing an over-read or an integer overflow.
    auto next_bytes = SerializeState(next_);
    next_bytes[68] = 0xff;
    next_bytes[69] = 0xff;
    next_bytes[70] = 0xff;
    next_bytes[71] = 0xff;
    std::string error;
    EXPECT_FALSE(VerifyWith(SerializeState(previous_), next_bytes, error));
}

TEST_F(CcvWireFormatTest, RejectsTruncatedStateBelowHeaderSize) {
    auto next_bytes = SerializeState(next_);
    next_bytes.resize(71);  // one byte short of the 72-byte header
    std::string error;
    EXPECT_FALSE(VerifyWith(SerializeState(previous_), next_bytes, error));
}

TEST_F(CcvWireFormatTest, RejectsEmptyState) {
    std::string error;
    EXPECT_FALSE(VerifyWith(SerializeState(previous_), {}, error));
}

TEST_F(CcvWireFormatTest, RejectsOversizedStateThroughScriptPath) {
    // The 448-byte cap is enforced by VerifyContractTransition, not by the
    // deserializer, so prove it still holds when the oversized state arrives
    // as witness bytes rather than as a constructed struct.
    ContractState oversized = next_;
    oversized.data.assign(MAX_CONTRACT_STATE_DATA_SIZE + 1, 0x33);
    oversized.stateHash = ComputeContractStateHash(oversized);
    std::string error;
    EXPECT_FALSE(
        VerifyWith(SerializeState(previous_), SerializeState(oversized), error));
}

} // namespace
