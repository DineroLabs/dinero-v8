// Saved randomized proofs, checked against independent Python byte literals.
// No proving takes place here. The production verifier checks all fixed proofs.
#include <gtest/gtest.h>
#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/compact_regtest.h"
#include "consensus/shielded/resource_limits.h"
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_validation.h"
#include "consensus/utreexo_accumulator.h"
#include "crypto/sha256.h"
#include "primitives/transaction.h"
#include <json/json.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <map>

namespace {
namespace sh = dinero::consensus::shielded;
using dinero::Transaction;
using Bytes = std::vector<uint8_t>;
const std::filesystem::path kDirectory(COMPACT_REGTEST_VECTOR_DIRECTORY);

Bytes Unhex(const std::string& text) {
    if (text.size() % 2) throw std::runtime_error("odd hex length");
    Bytes out;
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error("noncanonical fixture hex");
    };
    for (size_t i = 0; i < text.size(); i += 2) out.push_back(nibble(text[i])*16 + nibble(text[i+1]));
    return out;
}
template<class C> std::string Hex(const C& bytes) {
    const char* alphabet = "0123456789abcdef";
    std::string result;
    for (auto b : bytes) { result += alphabet[b >> 4]; result += alphabet[b & 15]; }
    return result;
}
sh::Hash Hash(const std::string& text) {
    const auto bytes = Unhex(text);
    if (bytes.size() != 32) throw std::runtime_error("wrong hash length");
    sh::Hash result{}; std::copy(bytes.begin(), bytes.end(), result.begin()); return result;
}
Bytes ReadHex(const std::string& name) {
    std::ifstream input(kDirectory / name);
    std::string text, extra;
    if (!(input >> text) || (input >> extra)) throw std::runtime_error("missing or malformed hex fixture: " + name);
    return Unhex(text);
}
std::string Digest(const Bytes& bytes) {
    sh::Hash hash{};
    dinero::crypto::CSHA256().Write(bytes.data(), bytes.size()).Finalize(hash.data());
    return Hex(hash);
}
const Json::Value& Manifest() {
    static const auto manifest = [] {
        std::ifstream input(kDirectory / "manifest.json");
        Json::Value value; Json::CharReaderBuilder builder; std::string error;
        if (!Json::parseFromStream(builder, input, &value, &error)) throw std::runtime_error(error);
        return value;
    }();
    return manifest;
}
Transaction Decode(const Json::Value& fixture) {
    const auto raw = ReadHex(fixture["wire_file"].asString());
    Transaction tx; size_t used = 0;
    if (!dinero::TransactionSerializer::Deserialize(tx, raw, used) || used != raw.size())
        throw std::runtime_error("fixture did not decode completely");
    return tx;
}
sh::ShieldedBundle Bundle(const Transaction& tx) {
    sh::ShieldedBundle result;
    if (sh::DeserializeShieldedBundle(tx.shielded_bundle_bytes, &result) != sh::BundleDecodeError::Ok)
        throw std::runtime_error("fixture bundle decode failed");
    return result;
}

TEST(CompactFixedVectors, IdentityExpansionSigningAndUtreexoMatchIndependentLiterals) {
    ASSERT_EQ(Manifest()["format"].asString(), "dinero-compact-regtest-fixed-v1");
    ASSERT_EQ(Manifest()["cases"].size(), 4u);
    std::map<std::string, std::pair<size_t, size_t>> shapes{
        {"shield", {0, 1}}, {"transfer", {1, 2}}, {"unshield", {1, 0}}, {"maximum", {4, 2}}};
    for (const auto& fixture : Manifest()["cases"]) {
        SCOPED_TRACE(fixture["name"].asString());
        const auto& expected = fixture["expected"];
        const auto raw = ReadHex(fixture["wire_file"].asString());
        const auto tx = Decode(fixture);
        EXPECT_EQ(tx.Serialize(true), raw);
        EXPECT_EQ(Digest(raw), expected["wire_sha256"].asString());
        EXPECT_EQ(tx.GetTxid().AsUint256().GetHex(), expected["txid"].asString());
        EXPECT_EQ(tx.GetWtxid().AsUint256().GetHex(), expected["wtxid"].asString());
        EXPECT_EQ(tx.GetSize(), expected["wire_bytes"].asUInt64());
        EXPECT_EQ(tx.Serialize(false).size(), expected["base_bytes"].asUInt64());
        EXPECT_EQ(tx.GetWeight(), expected["weight"].asUInt64());
        EXPECT_EQ(tx.GetVirtualSize(), expected["vsize"].asUInt64());
        EXPECT_EQ(tx.GetExplicitFee(), expected["fee_una"].asUInt64());
        const auto bundle = Bundle(tx);
        const auto shape = shapes.find(fixture["name"].asString());
        ASSERT_NE(shape, shapes.end());
        EXPECT_EQ(bundle.spends.size(), shape->second.first);
        EXPECT_EQ(bundle.outputs.size(), shape->second.second);
        shapes.erase(shape);
        EXPECT_EQ(bundle.spends.size(), expected["spends"].asUInt());
        EXPECT_EQ(bundle.outputs.size(), expected["outputs"].asUInt());
        EXPECT_EQ(bundle.value_balance, expected["value_balance"].asInt64());
        size_t proof_slots = 0; std::string resource_error;
        EXPECT_TRUE(sh::CheckAuthTransactionResources(tx, fixture["height"].asUInt(),
            2, proof_slots, resource_error, {true, 124})) << resource_error;
        EXPECT_EQ(proof_slots, bundle.spends.size() + bundle.outputs.size());
        EXPECT_EQ(Hex(sh::ComputeShieldedTxSighash(tx)), expected["tx_sighash"].asString());
        EXPECT_EQ(Hex(sh::ComputeBindingSighash(bundle, sh::ComputeShieldedTxSighash(tx))),
                  expected["binding_sighash"].asString());
        sh::ShieldedBundle expanded;
        ASSERT_TRUE(sh::ExpandCompactRegtestBundle(bundle, expanded));
        const auto expanded_bytes = sh::SerializeShieldedBundle(expanded);
        EXPECT_EQ(expanded_bytes, ReadHex(fixture["expanded_bundle_file"].asString()));
        EXPECT_EQ(Digest(expanded_bytes), expected["expanded_bundle_sha256"].asString());
        auto expanded_tx = tx; expanded_tx.shielded_bundle_bytes = expanded_bytes;
        EXPECT_EQ(expanded_tx.GetTxid().AsUint256().GetHex(), expected["expanded_view_txid"].asString());
        EXPECT_NE(expanded_tx.GetTxid(), tx.GetTxid());
        auto roundtrip = expanded;
        ASSERT_TRUE(sh::PackCompactRegtestBundle(roundtrip));
        EXPECT_EQ(sh::SerializeShieldedBundle(roundtrip), tx.shielded_bundle_bytes);
        size_t receipt = 0;
        auto check_proofs = [&](const auto& compact, const auto& ordinary) {
            for (size_t i = 0; i < compact.size(); ++i) {
                const auto& p = expected["proofs"][static_cast<Json::ArrayIndex>(receipt++)];
                EXPECT_EQ(Digest(compact[i].zk_proof), p["compact_sha256"].asString());
                EXPECT_EQ(Digest(ordinary[i].zk_proof), p["expanded_sha256"].asString());
                EXPECT_EQ(compact[i].zk_proof.size(), p["compact_bytes"].asUInt64());
                EXPECT_EQ(ordinary[i].zk_proof.size(), p["expanded_bytes"].asUInt64());
            }
        };
        check_proofs(bundle.spends, expanded.spends); check_proofs(bundle.outputs, expanded.outputs);
        ASSERT_EQ(tx.vout.size(), expected["utreexo_leaves"].size());
        for (size_t i = 0; i < tx.vout.size(); ++i) {
            const auto& out = tx.vout[i];
            const auto value = dinero::consensus::HashUTXOV2(tx.GetTxid().AsUint256(), i,
                out.value.GetUna(), out.scriptPubKey, fixture["height"].asUInt(), false);
            EXPECT_EQ(Hex(value), expected["utreexo_leaves"][static_cast<Json::ArrayIndex>(i)].asString());
            EXPECT_NE(value, dinero::consensus::HashUTXOV2(expanded_tx.GetTxid().AsUint256(), i,
                out.value.GetUna(), out.scriptPubKey, fixture["height"].asUInt(), false));
            if (i == 0 && fixture.isMember("utreexo_proof")) {
                dinero::consensus::UtreexoProof proof;
                const auto& saved = fixture["utreexo_proof"];
                proof.position = saved["position"].asUInt64(); proof.numLeaves = saved["num_leaves"].asUInt64();
                for (const auto& sibling : saved["siblings"]) proof.siblings.push_back(Unhex(sibling.asString()));
                std::vector<Bytes> roots;
                for (const auto& root : fixture["utreexo_roots"]) roots.push_back(Unhex(root.asString()));
                EXPECT_TRUE(proof.verify(value, roots));
                auto wrong = value; wrong[0] ^= 1; EXPECT_FALSE(proof.verify(wrong, roots));
            }
        }
    }
    EXPECT_TRUE(shapes.empty());
}

TEST(CompactFixedVectors, SavedProofsVerifyAndFalseClaimsLeaveStateUnchanged) {
    for (const auto& fixture : Manifest()["cases"]) {
        SCOPED_TRACE(fixture["name"].asString());
        sh::CommitmentTree tree;
        for (const auto& cm : fixture["initial_commitments"]) tree.Append(Hash(cm.asString()));
        sh::NullifierSet nullifiers;
        ASSERT_EQ(nullifiers.Open(":memory:"), sh::NullifierSet::OpenResult::Ok);
        for (const auto& nf : fixture["initial_nullifiers"])
            ASSERT_TRUE(nullifiers.Insert(Hash(nf.asString()), 125));
        const auto before = tree.Root(); const auto count_before = nullifiers.Size();
        EXPECT_EQ(Hex(before), fixture["expected"]["root_before"].asString());
        const auto tx = Decode(fixture); const auto bundle = Bundle(tx);
        const auto height = fixture["height"].asUInt();
        auto ctx = sh::BuildShieldedValidationContext(tx, &nullifiers, &tree,
            height, fixture["expected"]["value_balance"].asInt64(), 0, nullptr,
            0, 0, 2, UINT32_MAX, {true, 124});
        EXPECT_EQ(sh::ValidateShieldedBundle(bundle, ctx), sh::ShieldedValidationError::Ok);
        // Repeated proof verification must not cache the chain-state decision.
        EXPECT_EQ(sh::ValidateShieldedBundle(bundle, ctx), sh::ShieldedValidationError::Ok);
        if (!bundle.spends.empty()) {
            sh::NullifierSet spent;
            ASSERT_EQ(spent.Open(":memory:"), sh::NullifierSet::OpenResult::Ok);
            ASSERT_TRUE(spent.Insert(bundle.spends[0].nullifier, height));
            auto spent_ctx = ctx; spent_ctx.nullifier_set = &spent;
            EXPECT_EQ(sh::ValidateShieldedBundle(bundle, spent_ctx), sh::ShieldedValidationError::NullifierDuplicate);
            sh::CommitmentTree wrong_tree;
            auto wrong_ctx = ctx; wrong_ctx.commitment_tree = &wrong_tree;
            EXPECT_EQ(sh::ValidateShieldedBundle(bundle, wrong_ctx), sh::ShieldedValidationError::AnchorInvalid);
        }
        auto wrong = bundle;
        if (!wrong.spends.empty()) wrong.spends[0].nullifier[0] ^= 1;
        else wrong.outputs[0].commitment[0] ^= 1;
        EXPECT_EQ(sh::ValidateShieldedBundle(wrong, ctx), sh::ShieldedValidationError::ProofInvalid);
        ctx.block_height = 123;
        EXPECT_EQ(sh::ValidateShieldedBundle(bundle, ctx), sh::ShieldedValidationError::NotActive);
        EXPECT_EQ(tree.Root(), before); EXPECT_EQ(nullifiers.Size(), count_before);
        ASSERT_TRUE(sh::ApplyShieldedBundle(bundle, &tree, &nullifiers, height));
        EXPECT_EQ(Hex(tree.Root()), fixture["expected"]["root_after"].asString());
        EXPECT_EQ(nullifiers.Size(), count_before + bundle.spends.size());
    }
}

TEST(CompactFixedVectors, WarmProofsRejectChangedBytesInputsAndProfiles) {
    for (const auto& fixture : Manifest()["cases"]) {
        const auto name = fixture["name"].asString();
        if (name != "shield" && name != "unshield") continue;
        SCOPED_TRACE(name);
        sh::ShieldedBundle expanded;
        ASSERT_TRUE(sh::ExpandCompactRegtestBundle(Bundle(Decode(fixture)), expanded));
        if (!expanded.outputs.empty()) {
            const auto& out = expanded.outputs[0];
            const sh::OutputPublicInputs pub{out.commitment, out.cv};
            ASSERT_TRUE(sh::VerifyOutput(out.zk_proof, pub, nullptr, true, true));
            ASSERT_TRUE(sh::VerifyOutput(out.zk_proof, pub, nullptr, true, true));
            auto wrong = pub; wrong.commitment[0] ^= 1;
            EXPECT_FALSE(sh::VerifyOutput(out.zk_proof, wrong, nullptr, true, true));
            wrong = pub; wrong.cv[0] ^= 1;
            EXPECT_FALSE(sh::VerifyOutput(out.zk_proof, wrong, nullptr, true, true));
            EXPECT_FALSE(sh::VerifyOutput(out.zk_proof, pub, nullptr, true, false));
            auto bytes = out.zk_proof; bytes.back() ^= 1;
            EXPECT_FALSE(sh::VerifyOutput(bytes, pub, nullptr, true, true));
            bytes = out.zk_proof; bytes.pop_back();
            EXPECT_FALSE(sh::VerifyOutput(bytes, pub, nullptr, true, true));
        } else {
            ASSERT_EQ(expanded.spends.size(), 1u);
            const auto& spend = expanded.spends[0];
            const sh::SpendPublicInputs pub{spend.nullifier, spend.anchor, spend.cv};
            ASSERT_TRUE(sh::VerifySpend(spend.zk_proof, pub, nullptr, true, true, true));
            ASSERT_TRUE(sh::VerifySpend(spend.zk_proof, pub, nullptr, true, true, true));
            auto wrong = pub; wrong.nullifier[0] ^= 1;
            EXPECT_FALSE(sh::VerifySpend(spend.zk_proof, wrong, nullptr, true, true, true));
            wrong = pub; wrong.anchor[0] ^= 1;
            EXPECT_FALSE(sh::VerifySpend(spend.zk_proof, wrong, nullptr, true, true, true));
            wrong = pub; wrong.cv[0] ^= 1;
            EXPECT_FALSE(sh::VerifySpend(spend.zk_proof, wrong, nullptr, true, true, true));
            EXPECT_FALSE(sh::VerifySpend(spend.zk_proof, pub, nullptr, true, true, false));
            EXPECT_FALSE(sh::VerifySpend(spend.zk_proof, pub, nullptr, true, true, true, true));
            auto bytes = spend.zk_proof; bytes.back() ^= 1;
            EXPECT_FALSE(sh::VerifySpend(bytes, pub, nullptr, true, true, true));
            bytes = spend.zk_proof; bytes.pop_back();
            EXPECT_FALSE(sh::VerifySpend(bytes, pub, nullptr, true, true, true));
        }
    }
}
} // namespace
