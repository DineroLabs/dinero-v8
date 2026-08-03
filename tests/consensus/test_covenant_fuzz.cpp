// Bounded, deterministic fuzz gate for the covenant consensus paths.
//
// WHAT IT DRIVES
// --------------
// The REAL production paths, never a reimplementation:
//
//   CCV: crafted witness bytes -> ScriptVerifier::VerifyTaproot(...)
//        which reaches DeserializeContractState and VerifyContractTransition.
//        The decoder is file-local to tapscript_interpreter.cpp, so driving it
//        through the script verifier is both the only way in and the honest
//        one -- it is the path a real peer's transaction takes.
//
//   CTV: arbitrary bytes -> TransactionSerializer::Deserialize -> TryComputeCTVHash.
//
// WHY IT IS A NORMAL TEST AND NOT LABELLED `fuzz`
// ----------------------------------------------
// CI excludes the `fuzz` label (integration|gate|release|canonicality|fuzz), so
// a fuzz-labelled target produces exactly zero CI signal -- see issue #486,
// where ConsensusFuzzer has been registered and never executed. This target is
// therefore deliberately unlabelled and bounded so it runs on every push.
//
// The same binary is the long campaign: DINERO_COVENANT_FUZZ_ITERATIONS raises
// the budget, and the documented sanitizer build re-runs it under
// ASan + UBSan + unsigned-integer-overflow. One harness, two intensities.
//
// WHAT IT ASSERTS
// ---------------
// "It didn't crash" is the weakest possible fuzz oracle, so this also checks:
//
//   * determinism -- the same bytes must produce the same verdict twice. A
//     consensus verifier whose answer depends on uninitialised memory or
//     iteration order would split the network.
//   * CTV hash stability -- a digest that is computable must recompute equal.
//
// Under the sanitizer build these become UB, overflow, and memory-safety
// oracles as well. Unsigned wraparound in particular is NOT caught by plain
// UBSan (unsigned overflow is well-defined in C++), which is why the documented
// build adds -fsanitize=unsigned-integer-overflow explicitly. The decoder's
// `offset + dataLen` comparison against a 32-bit attacker-controlled length is
// exactly the arithmetic that needs an empirical answer rather than an argument
// about what size_t happens to be on this platform.
//
// SEED CORPUS
// -----------
// tests/vectors/covenant_fuzz_seeds/*.hex is replayed before random search and
// is where any input that ever crashes must be committed, permanently. The
// corpus ships non-empty: it already encodes the decoder's boundary cases, so
// the mechanism has teeth from its first run rather than being an empty folder
// that only matters after something goes wrong.

#include "consensus/chainparams.h"
#include "consensus/covenant_activation.h"
#include "consensus/covenants.h"
#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include "consensus/script_verify.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
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

std::vector<uint8_t> FromHex(const std::string& hex) {
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            continue;
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

int64_t IterationBudget(int64_t fallback) {
    if (const char* env = std::getenv("DINERO_COVENANT_FUZZ_ITERATIONS")) {
        const int64_t parsed = std::atoll(env);
        if (parsed > 0) {
            return parsed;
        }
    }
    return fallback;
}

// A well-formed CCV spend that fuzzed bytes are substituted into, so the
// crafted element is the only thing wrong and actually reaches the decoder
// instead of being rejected earlier for unrelated reasons.
class CovenantFuzzFixture : public ::testing::Test {
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

    // Drive the production verifier. Returns the verdict; never throws.
    bool RunCcv(const std::vector<uint8_t>& prev_bytes,
                const std::vector<uint8_t>& next_bytes,
                const std::vector<uint8_t>& script,
                const std::vector<uint8_t>& control) const {
        Transaction tx;
        tx.vin.emplace_back();
        tx.vin[0].witness = {prev_bytes, next_bytes, script, control};
        tx.vout.emplace_back(AmountUna::Una(75'000), successor_script_);
        const std::vector<UTXOEntry> inputs{
            UTXOEntry(AmountUna::Una(75'000), spent_script_, 100, false)};
        std::string error;
        return ScriptVerifier::VerifyTaproot(tx, 0, inputs, error, flags_);
    }

    // Consensus verdicts must not depend on uninitialised memory, hash-map
    // iteration order, or anything else that varies run to run.
    void ExpectDeterministic(const std::vector<uint8_t>& prev_bytes,
                             const std::vector<uint8_t>& next_bytes,
                             const std::vector<uint8_t>& script,
                             const std::vector<uint8_t>& control) const {
        const bool first = RunCcv(prev_bytes, next_bytes, script, control);
        const bool second = RunCcv(prev_bytes, next_bytes, script, control);
        ASSERT_EQ(first, second) << "non-deterministic CCV verdict";
    }
};

// ---------------------------------------------------------------------------

// Replay the permanent seed corpus first. A seed that ever crashed must stay
// here forever; this is the regression half of fuzzing.
TEST_F(CovenantFuzzFixture, ReplaysPermanentSeedCorpus) {
    const std::filesystem::path dir(DINERO_COVENANT_FUZZ_SEEDS);
    ASSERT_TRUE(std::filesystem::exists(dir))
        << "seed corpus missing at " << dir
        << " -- a fuzz gate with no corpus is not a gate";

    int replayed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".hex") {
            continue;
        }
        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            SCOPED_TRACE(entry.path().filename().string() + ": " + line);
            const std::vector<uint8_t> bytes = FromHex(line);
            // Each seed is exercised in both witness state positions.
            ExpectDeterministic(
                SerializeState(previous_), bytes, script_, control_block_);
            ExpectDeterministic(
                bytes, SerializeState(next_), script_, control_block_);
            ++replayed;
        }
    }
    EXPECT_GT(replayed, 0) << "seed corpus contained no usable entries";
    std::cout << "[covenant-fuzz] replayed " << replayed << " seeds"
              << std::endl;
}

// Random and structured mutation of the CCV witness state elements.
TEST_F(CovenantFuzzFixture, CcvWitnessStateSurvivesFuzzing) {
    std::mt19937 rng(0x0C0FFEE1U);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> mode(0, 5);

    const auto valid_next = SerializeState(next_);
    const int64_t iterations = IterationBudget(2000);

    for (int64_t i = 0; i < iterations; ++i) {
        std::vector<uint8_t> crafted = valid_next;
        switch (mode(rng)) {
            case 0: {  // free-form random buffer, including absurd lengths
                std::uniform_int_distribution<size_t> len(0, 700);
                crafted.assign(len(rng), 0);
                for (auto& byte : crafted) {
                    byte = static_cast<uint8_t>(byte_dist(rng));
                }
                break;
            }
            case 1: {  // single bit flip in a valid encoding
                if (!crafted.empty()) {
                    std::uniform_int_distribution<size_t> pos(
                        0, crafted.size() - 1);
                    crafted[pos(rng)] ^= static_cast<uint8_t>(
                        1 << (byte_dist(rng) % 8));
                }
                break;
            }
            case 2: {  // truncation, including below the 72-byte header
                std::uniform_int_distribution<size_t> cut(0, crafted.size());
                crafted.resize(cut(rng));
                break;
            }
            case 3: {  // attacker-controlled declared length field
                if (crafted.size() >= 72) {
                    crafted[68] = static_cast<uint8_t>(byte_dist(rng));
                    crafted[69] = static_cast<uint8_t>(byte_dist(rng));
                    crafted[70] = static_cast<uint8_t>(byte_dist(rng));
                    crafted[71] = static_cast<uint8_t>(byte_dist(rng));
                }
                break;
            }
            case 4: {  // extension past the stack-element limit
                std::uniform_int_distribution<size_t> extra(1, 200);
                const size_t count = extra(rng);
                for (size_t n = 0; n < count; ++n) {
                    crafted.push_back(static_cast<uint8_t>(byte_dist(rng)));
                }
                break;
            }
            default: {  // near-maximum declared length: the overflow candidate
                if (crafted.size() >= 72) {
                    crafted[68] = 0xff;
                    crafted[69] = 0xff;
                    crafted[70] = 0xff;
                    crafted[71] = static_cast<uint8_t>(byte_dist(rng));
                }
                break;
            }
        }

        ExpectDeterministic(
            SerializeState(previous_), crafted, script_, control_block_);
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
    std::cout << "[covenant-fuzz] ccv iterations=" << iterations << std::endl;
}

// Fuzz the tapscript and control block too: those feed the authenticated
// Taproot context that the transition binding depends on.
TEST_F(CovenantFuzzFixture, CcvScriptAndControlBlockSurviveFuzzing) {
    std::mt19937 rng(0x0C0FFEE2U);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<size_t> script_len(0, 64);
    std::uniform_int_distribution<size_t> control_len(0, 100);

    const int64_t iterations = IterationBudget(1000);
    for (int64_t i = 0; i < iterations; ++i) {
        std::vector<uint8_t> script(script_len(rng));
        for (auto& byte : script) {
            byte = static_cast<uint8_t>(byte_dist(rng));
        }
        std::vector<uint8_t> control(control_len(rng));
        for (auto& byte : control) {
            byte = static_cast<uint8_t>(byte_dist(rng));
        }
        ExpectDeterministic(
            SerializeState(previous_), SerializeState(next_), script, control);
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
    }
    std::cout << "[covenant-fuzz] script/control iterations=" << iterations
              << std::endl;
}

// CTV: arbitrary bytes through the real deserializer and template hash.
TEST(CovenantFuzzCtv, TemplateHashSurvivesArbitraryTransactionBytes) {
    std::mt19937 rng(0x0C0FFEE3U);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<size_t> len_dist(0, 400);
    std::uniform_int_distribution<uint32_t> index_dist(0, 8);

    const int64_t iterations = IterationBudget(3000);
    for (int64_t i = 0; i < iterations; ++i) {
        std::vector<uint8_t> raw(len_dist(rng));
        for (auto& byte : raw) {
            byte = static_cast<uint8_t>(byte_dist(rng));
        }

        Transaction tx;
        size_t consumed = 0;
        if (!TransactionSerializer::Deserialize(tx, raw, consumed)) {
            continue;  // not a transaction; nothing further to exercise
        }

        const uint32_t index = index_dist(rng);
        std::array<uint8_t, 32> first{};
        std::array<uint8_t, 32> second{};
        const bool ok_first = TryComputeCTVHash(tx, index, first);
        const bool ok_second = TryComputeCTVHash(tx, index, second);
        ASSERT_EQ(ok_first, ok_second) << "non-deterministic CTV eligibility";
        if (ok_first) {
            // A computable template hash must be stable; CTV commitments are
            // meaningless if the same transaction hashes two ways.
            ASSERT_EQ(first, second) << "non-deterministic CTV digest";
        }
    }
    std::cout << "[covenant-fuzz] ctv iterations=" << iterations << std::endl;
}

}  // namespace
