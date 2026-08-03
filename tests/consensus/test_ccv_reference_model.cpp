// Implementation-independent CCV reference model + differential test.
//
// WHY THIS EXISTS
// ---------------
// CTV can be validated against upstream BIP-119 vectors (see
// test_bip119_ctv_vectors.cpp). CCV has no upstream BIP, no published vectors,
// and no second implementation anywhere to differ against. This file supplies
// the missing second implementation.
//
// INDEPENDENCE DISCIPLINE
// -----------------------
// Everything in namespace `refmodel` is derived from the normative text of
// docs/consensus/CCV_SUCCESSOR_BINDING_V1.md, NOT from src/consensus/covenants.cpp.
// It deliberately does NOT call ComputeContractStateHash, ComputeContractCodeHash,
// DeriveContractInternalKey, ComputeContractOutputScript, or
// VerifyContractTransition. Reusing those would only prove the implementation
// agrees with itself — the same trap the BIP-119 vector file warns about.
//
// The model uses SHA-256 and secp256k1 as PRIMITIVES. Independence is claimed at
// the protocol-construction level (what is hashed, in what order, how the key is
// derived and tweaked, and the transition rules), not at the level of the
// underlying curve and hash arithmetic. A shared bug inside secp256k1 itself
// would not be caught here — that is a genuine and deliberate limit of this
// technique.
//
// WHAT THIS IS, AND WHAT IT IS NOT
// --------------------------------
// This is an independent implementation derived from the specification. It is
// NOT a clean-room implementation, and it is NOT an independently authored
// review.
//
// Specifically: the author had already read parts of covenants.cpp while
// reviewing the activation PR, so this is weaker than a model written by
// someone who has never seen the production code. And an implementation that
// agrees with production is evidence about the implementation — it is not a
// substitute for a human reviewer examining the design itself.
//
// Both limits are recorded here so no reader, now or later, overstates the
// strength of this evidence.
//
// WHAT THE TEST DOES
// ------------------
// Generates randomized CCV transitions — valid ones, and ones mutated to break a
// specific rule — then requires the reference model and production to return the
// SAME verdict on every case. Disagreement in either direction is a finding:
//   - production accepts where the spec model rejects  -> possible consensus hole
//   - production rejects where the spec model accepts  -> possible over-rejection
// The seed is fixed so any failure reproduces exactly, and the failing case is
// printed in full.

#include "consensus/covenants.h"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
}

namespace {

using dinero::AmountUna;
using dinero::Transaction;
using dinero::TxOutput;
using dinero::consensus::UTXOEntry;

// ===========================================================================
// namespace refmodel — spec-derived. Must not reference production CCV logic.
// ===========================================================================
namespace refmodel {

// Spec: "data is limited to 448 bytes so the whole state fits the 520-byte
// tapscript stack-element limit." Derived here from the spec's own arithmetic
// rather than imported, so a change to the production constant cannot silently
// move the model's boundary too.
constexpr size_t kMaxData = 520 - 72;

struct State {
    std::array<uint8_t, 32> stateHash{};
    std::array<uint8_t, 32> codeHash{};
    uint32_t counter = 0;
    std::vector<uint8_t> data;
};

std::array<uint8_t, 32> Sha256(const std::vector<uint8_t>& bytes) {
    std::array<uint8_t, 32> out{};
    dinero::crypto::CSHA256().Write(bytes.data(), bytes.size()).Finalize(out.data());
    return out;
}

void PutLE32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

// BIP340 tagged hash: SHA256(SHA256(tag) || SHA256(tag) || data).
std::array<uint8_t, 32> TaggedHash(const std::string& tag,
                                   const std::vector<uint8_t>& data) {
    std::vector<uint8_t> tag_bytes(tag.begin(), tag.end());
    const auto tag_hash = Sha256(tag_bytes);
    std::vector<uint8_t> preimage;
    preimage.insert(preimage.end(), tag_hash.begin(), tag_hash.end());
    preimage.insert(preimage.end(), tag_hash.begin(), tag_hash.end());
    preimage.insert(preimage.end(), data.begin(), data.end());
    return Sha256(preimage);
}

// Spec: codeHash = SHA256(revealed_tapscript)
std::array<uint8_t, 32> CodeHash(const std::vector<uint8_t>& tapscript) {
    return Sha256(tapscript);
}

// Spec: stateHash = SHA256(codeHash || counter_le32 || data)
std::array<uint8_t, 32> StateHash(const State& state) {
    std::vector<uint8_t> preimage;
    preimage.insert(preimage.end(), state.codeHash.begin(), state.codeHash.end());
    PutLE32(preimage, state.counter);
    preimage.insert(preimage.end(), state.data.begin(), state.data.end());
    return Sha256(preimage);
}

// Spec: candidate = TaggedHash("Dinero/CCVInternalKey/v1", stateHash || retry_le32),
// retry from zero; first candidate that parses as an x-only public key wins.
bool InternalKey(const std::array<uint8_t, 32>& state_hash,
                 std::array<uint8_t, 32>& out) {
    secp256k1_context* ctx = dinero::crypto::GetSecp256k1ContextSignVerify();
    for (uint32_t retry = 0; retry < 1000; ++retry) {
        std::vector<uint8_t> preimage(state_hash.begin(), state_hash.end());
        PutLE32(preimage, retry);
        const auto candidate =
            TaggedHash("Dinero/CCVInternalKey/v1", preimage);
        secp256k1_xonly_pubkey parsed;
        if (secp256k1_xonly_pubkey_parse(ctx, &parsed, candidate.data()) == 1) {
            out = candidate;
            return true;
        }
    }
    return false;
}

// Spec: t = TaggedHash("TapTweak", internalKey || m); P = internalKey + t*G;
//       scriptPubKey = OP_1 PUSH32 xonly(P); control-block parity == parity of P.
bool OutputScript(const State& state,
                  const std::array<uint8_t, 32>& merkle_root,
                  std::vector<uint8_t>& script_pubkey,
                  uint8_t* out_parity) {
    std::array<uint8_t, 32> internal_key{};
    if (!InternalKey(state.stateHash, internal_key)) {
        return false;
    }
    secp256k1_context* ctx = dinero::crypto::GetSecp256k1ContextSignVerify();

    secp256k1_xonly_pubkey internal_parsed;
    if (secp256k1_xonly_pubkey_parse(ctx, &internal_parsed,
                                     internal_key.data()) != 1) {
        return false;
    }

    std::vector<uint8_t> tweak_preimage(internal_key.begin(), internal_key.end());
    tweak_preimage.insert(
        tweak_preimage.end(), merkle_root.begin(), merkle_root.end());
    const auto tweak = TaggedHash("TapTweak", tweak_preimage);

    secp256k1_pubkey tweaked;
    if (secp256k1_xonly_pubkey_tweak_add(
            ctx, &tweaked, &internal_parsed, tweak.data()) != 1) {
        return false;
    }
    secp256k1_xonly_pubkey output_key;
    int parity = 0;
    if (secp256k1_xonly_pubkey_from_pubkey(
            ctx, &output_key, &parity, &tweaked) != 1) {
        return false;
    }
    std::array<uint8_t, 32> serialized{};
    secp256k1_xonly_pubkey_serialize(ctx, serialized.data(), &output_key);

    script_pubkey.clear();
    script_pubkey.push_back(0x51);  // OP_1
    script_pubkey.push_back(0x20);  // PUSH32
    script_pubkey.insert(
        script_pubkey.end(), serialized.begin(), serialized.end());
    if (out_parity != nullptr) {
        *out_parity = static_cast<uint8_t>(parity);
    }
    return true;
}

// The twelve numbered rules of "Verification rule" in CCV_SUCCESSOR_BINDING_V1.
bool VerifyTransition(const Transaction& tx,
                      uint32_t index,
                      const State& previous,
                      const State& next,
                      const std::vector<UTXOEntry>& spent_utxos,
                      const std::vector<uint8_t>& tapscript,
                      const std::array<uint8_t, 32>& internal_key,
                      const std::array<uint8_t, 32>& merkle_root,
                      uint8_t output_key_parity) {
    // 1. index exists in inputs, outputs, and a complete spent-UTXO vector.
    if (index >= tx.vin.size() || index >= tx.vout.size() ||
        index >= spent_utxos.size() || spent_utxos.size() != tx.vin.size()) {
        return false;
    }

    // 2. size limits, and both state hashes recompute correctly.
    if (previous.data.size() > kMaxData || next.data.size() > kMaxData) {
        return false;
    }
    if (StateHash(previous) != previous.stateHash ||
        StateHash(next) != next.stateHash) {
        return false;
    }

    // 3. previous.counter != UINT32_MAX  (no wrap)
    if (previous.counter == std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    // 4. next.counter == previous.counter + 1
    if (next.counter != previous.counter + 1) {
        return false;
    }
    // 5. code immutable across the transition
    if (next.codeHash != previous.codeHash) {
        return false;
    }
    // 6. previous.codeHash commits to the revealed tapscript
    if (CodeHash(tapscript) != previous.codeHash) {
        return false;
    }

    // 7. the authenticated internal key and parity match `previous`.
    std::array<uint8_t, 32> expected_internal{};
    if (!InternalKey(previous.stateHash, expected_internal)) {
        return false;
    }
    if (expected_internal != internal_key) {
        return false;
    }
    std::vector<uint8_t> expected_spent_script;
    uint8_t expected_parity = 0;
    if (!OutputScript(previous, merkle_root,
                      expected_spent_script, &expected_parity)) {
        return false;
    }
    if (expected_parity != output_key_parity) {
        return false;
    }

    // 8. spent script is exactly the P2TR output derived from `previous`.
    if (spent_utxos[index].scriptPubKey != expected_spent_script) {
        return false;
    }
    // 9. the spent output is transparent.
    if (spent_utxos[index].is_confidential) {
        return false;
    }
    // 10. vout[index] is transparent and preserves the value exactly.
    if (tx.vout[index].is_confidential) {
        return false;
    }
    if (tx.vout[index].value.GetUna() != spent_utxos[index].value.GetUna()) {
        return false;
    }
    // 11. vout[index] is exactly the successor P2TR output under the same root.
    std::vector<uint8_t> expected_successor;
    if (!OutputScript(next, merkle_root, expected_successor, nullptr)) {
        return false;
    }
    if (tx.vout[index].scriptPubKey != expected_successor) {
        return false;
    }
    // 12. no OTHER output carries the same successor script.
    for (size_t other = 0; other < tx.vout.size(); ++other) {
        if (other == index) {
            continue;
        }
        if (tx.vout[other].scriptPubKey == expected_successor) {
            return false;
        }
    }
    return true;
}

}  // namespace refmodel

// ===========================================================================
// Differential harness
// ===========================================================================

// One generated scenario, in a form both verifiers can consume.
struct Scenario {
    std::string label;
    std::vector<uint8_t> tapscript;
    std::array<uint8_t, 32> merkle_root{};
    std::array<uint8_t, 32> internal_key{};
    uint8_t parity = 0;
    refmodel::State previous;
    refmodel::State next;
    Transaction tx;
    std::vector<UTXOEntry> spent;
    uint32_t index = 0;
};

std::string HexOf(const std::vector<uint8_t>& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    for (const uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0f]);
    }
    return out;
}

// Translate the reference model's state into the production struct. This is a
// pure field copy — no production hashing helper is used to build it.
dinero::consensus::ContractState ToProduction(const refmodel::State& state) {
    dinero::consensus::ContractState out;
    out.stateHash = state.stateHash;
    out.codeHash = state.codeHash;
    out.counter = state.counter;
    out.data = state.data;
    return out;
}

bool ProductionVerdict(const Scenario& scenario) {
    const dinero::consensus::ContractSpendContext context{
        scenario.spent, scenario.tapscript, scenario.internal_key,
        scenario.merkle_root, scenario.parity};
    return dinero::consensus::VerifyContractTransition(
        scenario.tx, scenario.index, ToProduction(scenario.previous),
        ToProduction(scenario.next), context);
}

bool ReferenceVerdict(const Scenario& scenario) {
    return refmodel::VerifyTransition(
        scenario.tx, scenario.index, scenario.previous, scenario.next,
        scenario.spent, scenario.tapscript, scenario.internal_key,
        scenario.merkle_root, scenario.parity);
}

// Build a fully valid transition. Every commitment is computed by the reference
// model, so a production-side construction bug cannot make the baseline pass.
bool BuildValid(std::mt19937& rng, Scenario& out) {
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> script_len(1, 8);
    std::uniform_int_distribution<int> data_len(0, 24);
    std::uniform_int_distribution<uint32_t> counter_dist(0, 1u << 20);
    std::uniform_int_distribution<uint64_t> value_dist(1, 1'000'000);

    out.tapscript.clear();
    const int slen = script_len(rng);
    for (int i = 0; i < slen; ++i) {
        out.tapscript.push_back(static_cast<uint8_t>(byte_dist(rng)));
    }
    for (auto& byte : out.merkle_root) {
        byte = static_cast<uint8_t>(byte_dist(rng));
    }

    out.previous = refmodel::State{};
    out.previous.codeHash = refmodel::CodeHash(out.tapscript);
    out.previous.counter = counter_dist(rng);
    out.previous.data.clear();
    const int plen = data_len(rng);
    for (int i = 0; i < plen; ++i) {
        out.previous.data.push_back(static_cast<uint8_t>(byte_dist(rng)));
    }
    out.previous.stateHash = refmodel::StateHash(out.previous);

    out.next = refmodel::State{};
    out.next.codeHash = out.previous.codeHash;
    out.next.counter = out.previous.counter + 1;
    out.next.data.clear();
    const int nlen = data_len(rng);
    for (int i = 0; i < nlen; ++i) {
        out.next.data.push_back(static_cast<uint8_t>(byte_dist(rng)));
    }
    out.next.stateHash = refmodel::StateHash(out.next);

    if (!refmodel::InternalKey(out.previous.stateHash, out.internal_key)) {
        return false;
    }
    std::vector<uint8_t> spent_script;
    if (!refmodel::OutputScript(
            out.previous, out.merkle_root, spent_script, &out.parity)) {
        return false;
    }
    std::vector<uint8_t> successor_script;
    if (!refmodel::OutputScript(
            out.next, out.merkle_root, successor_script, nullptr)) {
        return false;
    }

    const uint64_t value = value_dist(rng);
    out.index = 0;
    out.tx = Transaction{};
    out.tx.vin.emplace_back();
    out.tx.vout.clear();
    out.tx.vout.emplace_back(AmountUna::Una(value), successor_script);
    out.spent.clear();
    out.spent.emplace_back(AmountUna::Una(value), spent_script, 100, false);
    out.label = "valid";
    return true;
}

// Each mutation targets one spec rule. Mutations that change state contents
// deliberately refresh the affected stateHash so the scenario fails on the rule
// under test rather than on a stale hash — except where the stale hash IS the
// attack, which is its own mutation.
using Mutation = void (*)(std::mt19937&, Scenario&);

void MutCounterReplay(std::mt19937&, Scenario& s) {
    s.next.counter = s.previous.counter;
    s.next.stateHash = refmodel::StateHash(s.next);
    std::vector<uint8_t> script;
    if (refmodel::OutputScript(s.next, s.merkle_root, script, nullptr)) {
        s.tx.vout[s.index].scriptPubKey = script;
    }
    s.label = "counter_replay";
}

void MutCounterSkip(std::mt19937&, Scenario& s) {
    s.next.counter = s.previous.counter + 2;
    s.next.stateHash = refmodel::StateHash(s.next);
    std::vector<uint8_t> script;
    if (refmodel::OutputScript(s.next, s.merkle_root, script, nullptr)) {
        s.tx.vout[s.index].scriptPubKey = script;
    }
    s.label = "counter_skip";
}

void MutPreviousCounterMax(std::mt19937&, Scenario& s) {
    s.previous.counter = std::numeric_limits<uint32_t>::max();
    s.previous.stateHash = refmodel::StateHash(s.previous);
    s.next.counter = 0;
    s.next.stateHash = refmodel::StateHash(s.next);
    std::vector<uint8_t> spent_script;
    if (refmodel::OutputScript(
            s.previous, s.merkle_root, spent_script, &s.parity)) {
        s.spent[s.index].scriptPubKey = spent_script;
    }
    refmodel::InternalKey(s.previous.stateHash, s.internal_key);
    std::vector<uint8_t> successor;
    if (refmodel::OutputScript(s.next, s.merkle_root, successor, nullptr)) {
        s.tx.vout[s.index].scriptPubKey = successor;
    }
    s.label = "previous_counter_max";
}

void MutCodeMutation(std::mt19937& rng, Scenario& s) {
    std::uniform_int_distribution<int> pick(0, 31);
    s.next.codeHash[pick(rng)] ^= 0x01;
    s.next.stateHash = refmodel::StateHash(s.next);
    std::vector<uint8_t> script;
    if (refmodel::OutputScript(s.next, s.merkle_root, script, nullptr)) {
        s.tx.vout[s.index].scriptPubKey = script;
    }
    s.label = "code_mutation";
}

void MutCodeSubstitution(std::mt19937& rng, Scenario& s) {
    // Commit to a different contract's code while revealing this tapscript.
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::vector<uint8_t> other{static_cast<uint8_t>(byte_dist(rng)),
                               static_cast<uint8_t>(byte_dist(rng))};
    const auto forged = refmodel::CodeHash(other);
    if (forged == s.previous.codeHash) {
        return;
    }
    s.previous.codeHash = forged;
    s.next.codeHash = forged;
    s.previous.stateHash = refmodel::StateHash(s.previous);
    s.next.stateHash = refmodel::StateHash(s.next);
    refmodel::InternalKey(s.previous.stateHash, s.internal_key);
    std::vector<uint8_t> spent_script;
    if (refmodel::OutputScript(
            s.previous, s.merkle_root, spent_script, &s.parity)) {
        s.spent[s.index].scriptPubKey = spent_script;
    }
    std::vector<uint8_t> successor;
    if (refmodel::OutputScript(s.next, s.merkle_root, successor, nullptr)) {
        s.tx.vout[s.index].scriptPubKey = successor;
    }
    s.label = "code_substitution";
}

void MutStaleStateHash(std::mt19937& rng, Scenario& s) {
    std::uniform_int_distribution<int> pick(0, 31);
    s.next.stateHash[pick(rng)] ^= 0x01;
    s.label = "stale_state_hash";
}

void MutFlipParity(std::mt19937&, Scenario& s) {
    s.parity ^= 1;
    s.label = "flipped_parity";
}

void MutWrongMerkleRoot(std::mt19937& rng, Scenario& s) {
    std::uniform_int_distribution<int> pick(0, 31);
    s.merkle_root[pick(rng)] ^= 0x01;
    s.label = "wrong_merkle_root";
}

void MutValueInflation(std::mt19937&, Scenario& s) {
    s.tx.vout[s.index].value =
        AmountUna::Una(s.spent[s.index].value.GetUna() + 1);
    s.label = "value_inflation";
}

void MutValueDeflation(std::mt19937&, Scenario& s) {
    const uint64_t spent = s.spent[s.index].value.GetUna();
    s.tx.vout[s.index].value = AmountUna::Una(spent > 0 ? spent - 1 : 0);
    s.label = "value_deflation";
}

void MutDuplicateSuccessor(std::mt19937&, Scenario& s) {
    s.tx.vout.push_back(s.tx.vout[s.index]);
    s.label = "duplicate_successor";
}

void MutConfidentialSpent(std::mt19937&, Scenario& s) {
    s.spent[s.index].is_confidential = true;
    s.label = "confidential_spent";
}

void MutConfidentialSuccessor(std::mt19937&, Scenario& s) {
    s.tx.vout[s.index].is_confidential = true;
    s.label = "confidential_successor";
}

void MutOversizedData(std::mt19937&, Scenario& s) {
    s.next.data.assign(refmodel::kMaxData + 1, 0x5a);
    s.next.stateHash = refmodel::StateHash(s.next);
    std::vector<uint8_t> script;
    if (refmodel::OutputScript(s.next, s.merkle_root, script, nullptr)) {
        s.tx.vout[s.index].scriptPubKey = script;
    }
    s.label = "oversized_data";
}

void MutMaximumData(std::mt19937&, Scenario& s) {
    // Boundary that must remain ACCEPTED by both implementations.
    s.next.data.assign(refmodel::kMaxData, 0x5a);
    s.next.stateHash = refmodel::StateHash(s.next);
    std::vector<uint8_t> script;
    if (refmodel::OutputScript(s.next, s.merkle_root, script, nullptr)) {
        s.tx.vout[s.index].scriptPubKey = script;
    }
    s.label = "maximum_data";
}

void MutCorruptSpentScript(std::mt19937& rng, Scenario& s) {
    std::uniform_int_distribution<size_t> pick(
        0, s.spent[s.index].scriptPubKey.size() - 1);
    s.spent[s.index].scriptPubKey[pick(rng)] ^= 0x01;
    s.label = "corrupt_spent_script";
}

void MutCorruptSuccessorScript(std::mt19937& rng, Scenario& s) {
    std::uniform_int_distribution<size_t> pick(
        0, s.tx.vout[s.index].scriptPubKey.size() - 1);
    s.tx.vout[s.index].scriptPubKey[pick(rng)] ^= 0x01;
    s.label = "corrupt_successor_script";
}

void MutExtraInputWithoutUtxo(std::mt19937&, Scenario& s) {
    s.tx.vin.emplace_back();
    s.label = "extra_input_without_utxo";
}

void MutIndexOutOfRange(std::mt19937&, Scenario& s) {
    s.index = static_cast<uint32_t>(s.tx.vout.size()) + 5;
    s.label = "index_out_of_range";
}

void MutWrongInternalKey(std::mt19937& rng, Scenario& s) {
    std::uniform_int_distribution<int> pick(0, 31);
    s.internal_key[pick(rng)] ^= 0x01;
    s.label = "wrong_internal_key";
}

constexpr Mutation kMutations[] = {
    MutCounterReplay,      MutCounterSkip,           MutPreviousCounterMax,
    MutCodeMutation,       MutCodeSubstitution,      MutStaleStateHash,
    MutFlipParity,         MutWrongMerkleRoot,       MutValueInflation,
    MutValueDeflation,     MutDuplicateSuccessor,    MutConfidentialSpent,
    MutConfidentialSuccessor, MutOversizedData,      MutMaximumData,
    MutCorruptSpentScript, MutCorruptSuccessorScript,
    MutExtraInputWithoutUtxo, MutIndexOutOfRange,    MutWrongInternalKey,
};

std::string Describe(const Scenario& s, bool reference, bool production) {
    std::ostringstream out;
    out << "\n  mutation:   " << s.label
        << "\n  reference:  " << (reference ? "ACCEPT" : "reject")
        << "\n  production: " << (production ? "ACCEPT" : "reject")
        << "\n  index:      " << s.index
        << "\n  prev.ctr:   " << s.previous.counter
        << "\n  next.ctr:   " << s.next.counter
        << "\n  parity:     " << static_cast<int>(s.parity)
        << "\n  tapscript:  " << HexOf(s.tapscript)
        << "\n  spent spk:  " << HexOf(s.spent.empty()
                                           ? std::vector<uint8_t>{}
                                           : s.spent[0].scriptPubKey)
        << "\n  vout0 spk:  " << HexOf(s.tx.vout.empty()
                                           ? std::vector<uint8_t>{}
                                           : s.tx.vout[0].scriptPubKey);
    return out.str();
}

// ---------------------------------------------------------------------------

// The model must reproduce the spec's published golden vector. If this fails,
// the model is wrong and every differential result below is worthless.
TEST(CcvReferenceModel, ReproducesSpecGoldenVectorIndependently) {
    const std::vector<uint8_t> tapscript{0xbe, 0x51};
    std::array<uint8_t, 32> merkle_root{};
    for (size_t i = 0; i < merkle_root.size(); ++i) {
        merkle_root[i] = static_cast<uint8_t>(i + 1);
    }

    refmodel::State previous;
    previous.codeHash = refmodel::CodeHash(tapscript);
    previous.counter = 41;
    previous.data = {0x10, 0x20, 0x30};
    previous.stateHash = refmodel::StateHash(previous);

    refmodel::State next;
    next.codeHash = previous.codeHash;
    next.counter = 42;
    next.data = {0x40, 0x50, 0x60};
    next.stateHash = refmodel::StateHash(next);

    std::array<uint8_t, 32> internal_key{};
    ASSERT_TRUE(refmodel::InternalKey(previous.stateHash, internal_key));
    std::vector<uint8_t> previous_spk;
    std::vector<uint8_t> next_spk;
    ASSERT_TRUE(
        refmodel::OutputScript(previous, merkle_root, previous_spk, nullptr));
    ASSERT_TRUE(refmodel::OutputScript(next, merkle_root, next_spk, nullptr));

    // Values published in docs/consensus/CCV_SUCCESSOR_BINDING_V1.md.
    EXPECT_EQ(HexOf({previous.codeHash.begin(), previous.codeHash.end()}),
              "bce3b94e7f9f1a041b490e366d98e38442cbcef077610b90c4e5a7b63a80c8f7");
    EXPECT_EQ(HexOf({previous.stateHash.begin(), previous.stateHash.end()}),
              "820f04fe93cdb43b27668a99fae6b47c1b1ca67258f59ecdeb2261d8615043ac");
    EXPECT_EQ(HexOf({internal_key.begin(), internal_key.end()}),
              "1110c456999cb753d39d73a8f57e0b0f669760e9ddafc15f339c5ee05a4216ee");
    EXPECT_EQ(HexOf(previous_spk),
              "51202c06cfbb5f2149007203323d7cc79fa8cfed6cfab865fb2809bcefac7603b507");
    EXPECT_EQ(HexOf({next.stateHash.begin(), next.stateHash.end()}),
              "4488f5efa957c58b78aeffa4d81e02cd22ecde7b31c1e47b279eb1d4c203c5e0");
    EXPECT_EQ(HexOf(next_spk),
              "5120a4bb46440cba36303dcbc734e5a6145340ae8448a057c770ff18bac86a3fa8dd");
}

TEST(CcvReferenceModel, AgreesWithProductionAcrossRandomizedTransitions) {
    // Fixed seed: any disagreement reproduces exactly on re-run.
    std::mt19937 rng(0x0CCF2026U);
    std::uniform_int_distribution<size_t> mutation_pick(
        0, (sizeof(kMutations) / sizeof(kMutations[0])) - 1);
    std::uniform_int_distribution<int> mutate_roll(0, 99);

    constexpr int kCases = 600;
    int accepted_by_both = 0;
    int rejected_by_both = 0;
    int disagreements = 0;

    for (int iteration = 0; iteration < kCases; ++iteration) {
        Scenario scenario;
        ASSERT_TRUE(BuildValid(rng, scenario))
            << "failed to construct a valid baseline at iteration " << iteration;

        // ~75% of cases carry a mutation; the rest stay valid so the run cannot
        // degenerate into "both reject everything".
        if (mutate_roll(rng) < 75) {
            kMutations[mutation_pick(rng)](rng, scenario);
        }

        const bool reference = ReferenceVerdict(scenario);
        const bool production = ProductionVerdict(scenario);

        if (reference != production) {
            ++disagreements;
            ADD_FAILURE()
                << "reference model and production disagree at iteration "
                << iteration << Describe(scenario, reference, production);
            if (disagreements >= 5) {
                break;  // enough evidence; do not flood the log
            }
        } else if (reference) {
            ++accepted_by_both;
        } else {
            ++rejected_by_both;
        }
    }

    EXPECT_EQ(disagreements, 0);

    // Anti-vacuity. A model that rejected everything would "agree" with a
    // production verifier that also rejected everything, proving nothing. Both
    // outcomes must be well represented.
    EXPECT_GT(accepted_by_both, 50)
        << "too few mutually accepted cases; the differential is not meaningful";
    EXPECT_GT(rejected_by_both, 200)
        << "too few mutually rejected cases; mutations are not biting";

    std::cout << "[ccv-diff] accepted_by_both=" << accepted_by_both
              << " rejected_by_both=" << rejected_by_both
              << " disagreements=" << disagreements << std::endl;
}

}  // namespace
