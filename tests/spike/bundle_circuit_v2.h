// tests/spike/bundle_circuit_v2.h
// THROWAWAY spike code — the Shielded v2 bundle statement (spec §3.1) built from the
// existing R1CS gadgets. Not consensus code. Legacy nullifier derivation (Poseidon(sk, idx))
// is used for the fixture; the auth profile adds two Poseidon calls per spend.
#pragma once

#include "consensus/shielded/commitment_tree.h"
#include "zk/zkvm/gadgets.h"
#include "zk/zkvm/poseidon_gadget.h"
#include "zk/zkvm/r1cs.h"

#include <array>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace spike {

using dinero::consensus::shielded::AddrBindTag;
using dinero::consensus::shielded::CommitmentTree;
using dinero::consensus::shielded::Hash;
using dinero::consensus::shielded::TREE_DEPTH;
using dinero::zk::zkvm::LinearCombination;
using dinero::zk::zkvm::R1CS;
using dinero::zk::zkvm::Scalar;
using dinero::zk::zkvm::Variable;
using dinero::zk::zkvm::poseidon2_gadget;
using dinero::zk::zkvm::poseidon2_native;
namespace gadgets = dinero::zk::zkvm::gadgets;

inline Scalar HashToScalarV2(const Hash& h) { return Scalar(h.data()); }

struct SpendLeg {
    Scalar secret_key, value, randomness, diversifier;
    uint32_t leaf_index = 0;
    std::array<Hash, TREE_DEPTH> siblings{};
    Scalar anchor, nullifier;  // public
};
struct OutputLeg {
    Scalar value, public_key, randomness, diversifier;
    Scalar commitment;  // public
};
struct BundleV2 {
    std::vector<SpendLeg> spends;
    std::vector<OutputLeg> outputs;
    uint64_t fee = 0;
    Scalar sighash;  // public
};

// Copy of the legacy merkle_path_gadget (shielded_circuit.cpp anonymous namespace).
inline Variable MerklePathV2(R1CS& cs, Variable leaf, Variable leaf_index,
                             const std::array<Hash, TREE_DEPTH>& siblings, const std::string& p) {
    Variable current = leaf;
    const auto bits = gadgets::to_bits(cs, leaf_index, TREE_DEPTH, p + "_idx_bits");
    for (size_t d = 0; d < TREE_DEPTH; ++d) {
        Variable sib = cs.alloc(HashToScalarV2(siblings[d]));
        Variable left = gadgets::select(cs, bits[d], sib, current, p + "_l" + std::to_string(d));
        Variable right = gadgets::select(cs, bits[d], current, sib, p + "_r" + std::to_string(d));
        current = poseidon2_gadget(cs, left, right, p + "_h" + std::to_string(d));
    }
    return current;
}

// commitment = Poseidon(Poseidon(Poseidon(ADDR_TAG, Poseidon(d, pk)), value), randomness)
inline Variable NoteCommitmentV2(R1CS& cs, Variable diversifier, Variable public_key, Variable value,
                                 Variable randomness, const std::string& p) {
    Variable dpk = poseidon2_gadget(cs, diversifier, public_key, p + "_dpk");
    Variable tag = gadgets::constant(cs, HashToScalarV2(AddrBindTag()), p + "_tag");
    Variable bind = poseidon2_gadget(cs, tag, dpk, p + "_bind");
    Variable bv = poseidon2_gadget(cs, bind, value, p + "_bv");
    return poseidon2_gadget(cs, bv, randomness, p + "_cm");
}

inline Scalar NoteCommitmentNativeV2(const Scalar& d, const Scalar& pk, const Scalar& value,
                                     const Scalar& randomness) {
    const Scalar dpk = poseidon2_native(d, pk);
    const Scalar bind = poseidon2_native(HashToScalarV2(AddrBindTag()), dpk);
    const Scalar bv = poseidon2_native(bind, value);
    return poseidon2_native(bv, randomness);
}

// Public inputs are allocated first, in this fixed order: sighash, fee, anchors[], nullifiers[], commitments[].
inline R1CS BuildBundleCircuitV2(const BundleV2& b) {
    R1CS cs;
    Variable sighash = cs.alloc_input(b.sighash);
    Variable fee = cs.alloc_input(Scalar(b.fee));
    std::vector<Variable> anchors, nullifiers, commitments;
    for (const auto& s : b.spends) {
        anchors.push_back(cs.alloc_input(s.anchor));
        nullifiers.push_back(cs.alloc_input(s.nullifier));
    }
    for (const auto& o : b.outputs) commitments.push_back(cs.alloc_input(o.commitment));
    // sighash is bound by being a verifier-supplied public input; touch it once (sighash * 1 = sighash).
    cs.constrain(LinearCombination(sighash), LinearCombination::constant(Scalar::one()),
                 LinearCombination(sighash), "sighash_bound");

    Variable sum_in = gadgets::constant(cs, Scalar::zero(), "sum_in0");
    for (size_t i = 0; i < b.spends.size(); ++i) {
        const auto& s = b.spends[i];
        const std::string p = "spend" + std::to_string(i);
        Variable sk = cs.alloc(s.secret_key);
        Variable val = cs.alloc(s.value);
        Variable rnd = cs.alloc(s.randomness);
        Variable div = cs.alloc(s.diversifier);
        Variable idx = cs.alloc(Scalar(static_cast<uint64_t>(s.leaf_index)));
        Variable zero = gadgets::constant(cs, Scalar::zero(), p + "_zero");
        Variable pk = poseidon2_gadget(cs, sk, zero, p + "_pk");
        Variable cm = NoteCommitmentV2(cs, div, pk, val, rnd, p);
        Variable root = MerklePathV2(cs, cm, idx, s.siblings, p);
        gadgets::assert_equal(cs, root, anchors[i], p + "_anchor");
        Variable nf = poseidon2_gadget(cs, sk, idx, p + "_nf");
        gadgets::assert_equal(cs, nf, nullifiers[i], p + "_nullifier");
        gadgets::range_check(cs, val, 64, p + "_range");
        sum_in = gadgets::add(cs, sum_in, val, p + "_sum");
    }
    Variable sum_out = fee;
    for (size_t j = 0; j < b.outputs.size(); ++j) {
        const auto& o = b.outputs[j];
        const std::string p = "out" + std::to_string(j);
        Variable val = cs.alloc(o.value), pk = cs.alloc(o.public_key), rnd = cs.alloc(o.randomness),
                 div = cs.alloc(o.diversifier);
        Variable cm = NoteCommitmentV2(cs, div, pk, val, rnd, p);
        gadgets::assert_equal(cs, cm, commitments[j], p + "_cm");
        gadgets::range_check(cs, val, 64, p + "_range");
        sum_out = gadgets::add(cs, sum_out, val, p + "_sum");
    }
    gadgets::range_check(cs, fee, 64, "fee_range");
    gadgets::assert_equal(cs, sum_in, sum_out, "balance");
    return cs;
}

// Deterministic honest fixture: n_in notes in one tree, spends of all of them, n_out outputs.
inline BundleV2 MakeHonestBundle(size_t n_in, size_t n_out, uint64_t fee) {
    BundleV2 b;
    b.fee = fee;
    b.sighash = Scalar(uint64_t{0x5148});
    CommitmentTree tree;
    const uint64_t value_each = 1'000'000;
    std::vector<Scalar> cms;
    for (size_t i = 0; i < n_in; ++i) {
        SpendLeg s;
        s.secret_key = Scalar(uint64_t{1000 + i});
        s.value = Scalar(value_each);
        s.randomness = Scalar(uint64_t{2000 + i});
        s.diversifier = Scalar(uint64_t{3000 + i});
        const Scalar pk = poseidon2_native(s.secret_key, Scalar::zero());
        const Scalar cm = NoteCommitmentNativeV2(s.diversifier, pk, s.value, s.randomness);
        Hash cm_hash{};
        std::memcpy(cm_hash.data(), cm.data(), cm_hash.size());
        s.leaf_index = static_cast<uint32_t>(tree.Append(cm_hash));
        b.spends.push_back(s);
    }
    for (auto& s : b.spends) {
        const auto path = tree.GetAuthPath(s.leaf_index);
        s.siblings = path->siblings;
        s.anchor = HashToScalarV2(tree.Root());
        s.nullifier = poseidon2_native(s.secret_key, Scalar(static_cast<uint64_t>(s.leaf_index)));
    }
    const uint64_t total_out = value_each * n_in - fee;
    for (size_t j = 0; j < n_out; ++j) {
        OutputLeg o;
        const uint64_t v = (j + 1 == n_out) ? total_out - (total_out / n_out) * (n_out - 1) : total_out / n_out;
        o.value = Scalar(v);
        o.public_key = Scalar(uint64_t{4000 + j});
        o.randomness = Scalar(uint64_t{5000 + j});
        o.diversifier = Scalar(uint64_t{6000 + j});
        o.commitment = NoteCommitmentNativeV2(o.diversifier, o.public_key, o.value, o.randomness);
        b.outputs.push_back(o);
    }
    return b;
}

}  // namespace spike
