// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/taproot_path_circuit.h"
#include "crypto/sha256.h"
#include "crypto/tagged_hash.h"
#include "zk/zkvm/schnorr_gadget.h"

namespace dinero {
namespace zk {
namespace zkvm {

namespace {

static const std::array<uint8_t, 32>& TapTweakTagHash() {
    static const std::array<uint8_t, 32> kTagHash = [] {
        std::array<uint8_t, 32> tag{};
        uint8_t out[32];
        dinero::crypto::CSHA256()
            .Write(reinterpret_cast<const uint8_t*>("TapTweak"), sizeof("TapTweak") - 1)
            .Finalize(out);
        std::copy(out, out + 32, tag.begin());
        return tag;
    }();
    return kTagHash;
}

static const std::array<uint32_t, 8>& TapTweakTagMidstate() {
    static const std::array<uint32_t, 8> kMidstate = [] {
        std::array<uint32_t, 8> midstate{};
        const auto& tag_hash = TapTweakTagHash();
        dinero::crypto::CSHA256 hasher;
        hasher.Write(tag_hash.data(), tag_hash.size());
        hasher.Write(tag_hash.data(), tag_hash.size());
        hasher.GetMidstate(midstate.data());
        return midstate;
    }();
    return kMidstate;
}

static std::vector<Word32> alloc_hash_input(R1CS& cs,
                                            const std::array<uint8_t, 32>& hash,
                                            const std::string& /*label*/) {
    std::vector<Word32> bytes(32);
    for (unsigned i = 0; i < 32; ++i) {
        Word32 w;
        for (unsigned bit = 0; bit < 32; ++bit) {
            const uint32_t bv = (static_cast<uint32_t>(hash[i]) >> bit) & 1;
            w.bits[bit] = cs.alloc_input(Scalar(uint64_t(bv)));
        }
        bytes[i] = w;
    }
    return bytes;
}

static std::vector<Word32> alloc_hash_witness(R1CS& cs,
                                              const std::array<uint8_t, 32>& hash,
                                              const std::string& label) {
    std::vector<Word32> bytes(32);
    for (unsigned i = 0; i < 32; ++i) {
        bytes[i] = word32_alloc(cs, hash[i], label + std::to_string(i));
    }
    return bytes;
}

static std::vector<Word32> alloc_hash_constant(R1CS& cs,
                                               const std::array<uint8_t, 32>& hash,
                                               const std::string& /*label*/) {
    std::vector<Word32> bytes(32);
    for (unsigned i = 0; i < 32; ++i) {
        Word32 w;
        for (unsigned bit = 0; bit < 32; ++bit) {
            const bool set = ((static_cast<uint32_t>(hash[i]) >> bit) & 1u) != 0;
            w.bits[bit] = set ? cs.const_one() : cs.const_zero();
        }
        bytes[i] = w;
    }
    return bytes;
}

static Word32 alloc_byte_constant(R1CS& cs, uint8_t value) {
    Word32 byte;
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (bit < 8) {
            byte.bits[bit] = ((value >> bit) & 1u) ? cs.const_one() : cs.const_zero();
        } else {
            byte.bits[bit] = cs.const_zero();
        }
    }
    return byte;
}

static std::array<Word32, 16> pack_sha256_block(
    R1CS& cs,
    const std::vector<Word32>& bytes
) {
    std::array<Word32, 16> block;
    Variable zero = cs.const_zero();

    for (unsigned word_index = 0; word_index < 16; ++word_index) {
        Word32 word;
        for (unsigned byte_index = 0; byte_index < 4; ++byte_index) {
            const size_t idx = word_index * 4 + byte_index;
            const unsigned shift = (3 - byte_index) * 8;
            for (unsigned bit = 0; bit < 8; ++bit) {
                if (idx < bytes.size()) {
                    word.bits[shift + bit] = bytes[idx].bits[bit];
                } else {
                    word.bits[shift + bit] = zero;
                }
            }
        }
        block[word_index] = word;
    }

    return block;
}

static SHA256State alloc_sha256_state_constant(
    R1CS& cs,
    const std::array<uint32_t, 8>& state_words
) {
    SHA256State state;
    for (unsigned i = 0; i < 8; ++i) {
        Word32 word;
        for (unsigned bit = 0; bit < 32; ++bit) {
            word.bits[bit] =
                ((state_words[i] >> bit) & 1u) ? cs.const_one() : cs.const_zero();
        }
        state.h[i] = word;
    }
    return state;
}

// Constant padding block for any 128-byte SHA256 message (bit-length = 1024 = 0x400).
// W[0] = 0x80000000 (0x80 pad byte), W[1..14] = 0, W[15] = 0x00000400.
static constexpr std::array<uint32_t, 16> kPaddingBlock128 = {
    0x80000000u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u,          0u, 0u, 0u, 0u, 0u, 0u, 0x00000400u
};

static std::array<Word32, 8> build_single_leaf_taptweak_message(
    R1CS& cs,
    const FieldElement& internal_xonly,
    const std::vector<Word32>& tapleaf_hash_words
) {
    auto internal_xonly_bytes = fe_to_bytes(cs, internal_xonly, "ix_");
    std::vector<Word32> second_block_bytes;
    second_block_bytes.reserve(64);
    second_block_bytes.insert(second_block_bytes.end(),
                              internal_xonly_bytes.begin(),
                              internal_xonly_bytes.end());
    second_block_bytes.insert(second_block_bytes.end(),
                              tapleaf_hash_words.begin(),
                              tapleaf_hash_words.end());

    SHA256State state = alloc_sha256_state_constant(cs, TapTweakTagMidstate());
    state = sha256_compress(cs, state, pack_sha256_block(cs, second_block_bytes));
    // Padding block: all words are constants — use const-block variant to skip
    // the ~9,840-constraint message schedule.
    state = sha256_compress_const_block(cs, state, kPaddingBlock128);
    return state.h;
}

static void constrain_hash_words_equal(
    R1CS& cs,
    const std::array<Word32, 8>& lhs_words,
    const std::vector<Word32>& rhs_words,
    const std::string& label
) {
    for (unsigned w = 0; w < 8; ++w) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            const Word32& rhs_byte = rhs_words[w * 4 + byte];
            for (unsigned bit = 0; bit < 8; ++bit) {
                const unsigned lhs_bit = (3 - byte) * 8 + bit;
                cs.enforce_equal(
                    LinearCombination(lhs_words[w].bits[lhs_bit]),
                    LinearCombination(rhs_byte.bits[bit]),
                    label + "_" + std::to_string(w) + "_" +
                        std::to_string(byte) + "_" + std::to_string(bit));
            }
        }
    }
}

} // namespace

std::array<Word32, 8> single_leaf_taptweak_circuit(
    R1CS& cs,
    const SingleLeafTapTweakWitness& witness
) {
    FieldElement internal_xonly_fe =
        fe_alloc_uint256(cs, Uint256(witness.internal_xonly.data()), "ix_fe");
    return single_leaf_taptweak_circuit_from_field_element(
        cs, internal_xonly_fe, witness.tapleaf_hash);
}

std::array<Word32, 8> single_leaf_taptweak_circuit_from_field_element(
    R1CS& cs,
    const FieldElement& internal_xonly,
    const std::array<uint8_t, 32>& tapleaf_hash
) {
    auto tapleaf_hash_words = alloc_hash_witness(cs, tapleaf_hash, "tl_");
    return build_single_leaf_taptweak_message(cs, internal_xonly, tapleaf_hash_words);
}

std::array<Word32, 8> single_leaf_taptweak_circuit_from_field_element_with_constant_tapleaf(
    R1CS& cs,
    const FieldElement& internal_xonly,
    const std::array<uint8_t, 32>& tapleaf_hash
) {
    auto tapleaf_hash_words = alloc_hash_constant(cs, tapleaf_hash, "tlc_");
    return build_single_leaf_taptweak_message(cs, internal_xonly, tapleaf_hash_words);
}

void single_leaf_taptweak_constrain_expected_hash(
    R1CS& cs,
    const std::array<Word32, 8>& tweak_words,
    const std::array<uint8_t, 32>& expected_tweak_hash,
    const std::string& label
) {
    for (unsigned w = 0; w < 8; ++w) {
        uint32_t expected_word =
            (static_cast<uint32_t>(expected_tweak_hash[w * 4 + 0]) << 24) |
            (static_cast<uint32_t>(expected_tweak_hash[w * 4 + 1]) << 16) |
            (static_cast<uint32_t>(expected_tweak_hash[w * 4 + 2]) << 8) |
            static_cast<uint32_t>(expected_tweak_hash[w * 4 + 3]);

        for (unsigned i = 0; i < 32; ++i) {
            const uint32_t expected_bit = (expected_word >> i) & 1;
            cs.constrain(
                LinearCombination(tweak_words[w].bits[i]),
                LinearCombination(VAR_ONE),
                LinearCombination::constant(expected_bit ? Scalar::one() : Scalar::zero()),
                label + "_" + std::to_string(w) + "_" + std::to_string(i)
            );
        }
    }
}

bool single_leaf_taptweak_verify_circuit(
    R1CS& cs,
    const SingleLeafTapTweakWitness& witness,
    const std::array<uint8_t, 32>& expected_tweak_hash
) {
    // Public inputs must be allocated before witness variables because
    // alloc_input() inserts them at the front of the witness vector.
    auto tapleaf_hash_words = alloc_hash_input(cs, witness.tapleaf_hash, "tl_");
    auto expected_hash_words = alloc_hash_input(cs, expected_tweak_hash, "exp_");
    FieldElement internal_xonly_fe =
        fe_alloc_uint256(cs, Uint256(witness.internal_xonly.data()), "ix_fe");
    auto tweak_words =
        build_single_leaf_taptweak_message(cs, internal_xonly_fe, tapleaf_hash_words);
    constrain_hash_words_equal(cs, tweak_words, expected_hash_words, "taptweak_pub");

    return true;
}

bool single_leaf_taptweak_verify_native(
    const SingleLeafTapTweakWitness& witness,
    const std::array<uint8_t, 32>& expected_tweak_hash
) {
    std::vector<uint8_t> tweak_input;
    tweak_input.reserve(64);
    tweak_input.insert(tweak_input.end(),
                       witness.internal_xonly.begin(),
                       witness.internal_xonly.end());
    tweak_input.insert(tweak_input.end(),
                       witness.tapleaf_hash.begin(),
                       witness.tapleaf_hash.end());
    const auto tweak = dinero::crypto::TaggedHashArray("TapTweak", tweak_input);
    return tweak == expected_tweak_hash;
}

// ============================================================
// TapBranch tagged hash helpers
// ============================================================

static const std::array<uint32_t, 8>& TapBranchTagMidstate() {
    static const std::array<uint32_t, 8> kMidstate = [] {
        uint8_t tag_hash[32];
        dinero::crypto::CSHA256()
            .Write(reinterpret_cast<const uint8_t*>("TapBranch"), 9)
            .Finalize(tag_hash);
        std::array<uint32_t, 8> midstate{};
        dinero::crypto::CSHA256 hasher;
        hasher.Write(tag_hash, 32);
        hasher.Write(tag_hash, 32);
        hasher.GetMidstate(midstate.data());
        return midstate;
    }();
    return kMidstate;
}

// Generic 64-byte tagged hash circuit: SHA256_compress(midstate || left||right || padding)
static std::array<Word32, 8> build_tagged_hash_64byte_circuit(
    R1CS& cs,
    const std::array<uint32_t, 8>& tag_midstate,
    const std::vector<Word32>& left_bytes,
    const std::vector<Word32>& right_bytes
) {
    std::vector<Word32> second_block;
    second_block.reserve(64);
    second_block.insert(second_block.end(), left_bytes.begin(), left_bytes.end());
    second_block.insert(second_block.end(), right_bytes.begin(), right_bytes.end());

    SHA256State state = alloc_sha256_state_constant(cs, tag_midstate);
    state = sha256_compress(cs, state, pack_sha256_block(cs, second_block));
    // Padding block for 128-byte message: all words are compile-time constants.
    // Use sha256_compress_const_block to skip the ~9,840-constraint message schedule.
    state = sha256_compress_const_block(cs, state, kPaddingBlock128);
    return state.h;
}

// Unpack 8 SHA256 output words (32-bit big-endian) to 32 byte-Words.
// Each output Word32 uses bits[0..7] for the byte value; bits[8..31] are zero.
static std::vector<Word32> unpack_sha256_words_to_bytes(
    R1CS& cs,
    const std::array<Word32, 8>& words
) {
    std::vector<Word32> bytes(32);
    for (unsigned w = 0; w < 8; ++w) {
        for (unsigned b = 0; b < 4; ++b) {
            Word32 bw;
            const unsigned shift = (3 - b) * 8; // big-endian: byte 0 = bits 24-31
            for (unsigned bit = 0; bit < 8; ++bit)
                bw.bits[bit] = words[w].bits[shift + bit];
            for (unsigned bit = 8; bit < 32; ++bit)
                bw.bits[bit] = cs.const_zero();
            bytes[w * 4 + b] = bw;
        }
    }
    return bytes;
}

// Sort-mux: left = sort_bit ? node : sibling; right = sort_bit ? sibling : node.
static std::pair<std::vector<Word32>, std::vector<Word32>> build_tapbranch_sort_mux(
    R1CS& cs,
    const Variable& sort_bit_var,
    const std::vector<Word32>& node_words,
    const std::vector<Word32>& sibling_words,
    const uint8_t* native_node,    // 32 bytes
    const uint8_t* native_sibling, // 32 bytes
    bool sort_bit_val,
    const std::string& label
) {
    std::vector<Word32> left_words(32), right_words(32);
    for (unsigned i = 0; i < 32; ++i) {
        Word32 lw, rw;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const int nv = (native_node[i] >> bit) & 1;
            const int sv = (native_sibling[i] >> bit) & 1;
            const int lv = sort_bit_val ? nv : sv;
            const int rv = sort_bit_val ? sv : nv;

            lw.bits[bit] = cs.alloc(lv ? Scalar::one() : Scalar::zero());
            rw.bits[bit] = cs.alloc(rv ? Scalar::one() : Scalar::zero());

            // sort_bit * (node_bit - sib_bit) = left_bit - sib_bit
            cs.constrain(
                LinearCombination(sort_bit_var),
                LinearCombination(node_words[i].bits[bit]) - LinearCombination(sibling_words[i].bits[bit]),
                LinearCombination(lw.bits[bit])            - LinearCombination(sibling_words[i].bits[bit]),
                label + "_L_" + std::to_string(i * 8 + bit));

            // sort_bit * (node_bit - sib_bit) = node_bit - right_bit
            cs.constrain(
                LinearCombination(sort_bit_var),
                LinearCombination(node_words[i].bits[bit]) - LinearCombination(sibling_words[i].bits[bit]),
                LinearCombination(node_words[i].bits[bit]) - LinearCombination(rw.bits[bit]),
                label + "_R_" + std::to_string(i * 8 + bit));
        }
        for (unsigned bit = 8; bit < 32; ++bit) {
            lw.bits[bit] = cs.const_zero();
            rw.bits[bit] = cs.const_zero();
        }
        left_words[i]  = lw;
        right_words[i] = rw;
    }
    return {left_words, right_words};
}

// ============================================================
// Public multi-leaf circuit functions
// ============================================================

std::array<Word32, 8> taproot_path_tweak_circuit_with_constant_tapleaf(
    R1CS& cs,
    const FieldElement& internal_xonly,
    const TaprootPathTweakWitness& witness,
    size_t path_len
) {
    // tapleaf_hash is a statement constant (matches public covenant message)
    auto current_node_words = alloc_hash_constant(cs, witness.tapleaf_hash, "path_n0_");

    // Track native intermediate values for mux witness computation
    std::array<uint8_t, 32> current_node_native = witness.tapleaf_hash;

    for (size_t k = 0; k < path_len; ++k) {
        const auto& sibling_native = witness.merkle_path[k];
        const bool sort_bit_val = witness.sort_bits[k];

        // Allocate sort_bit as private boolean witness
        Variable sort_bit_var = cs.alloc(sort_bit_val ? Scalar::one() : Scalar::zero());
        // Boolean constraint: sort_bit * (1 - sort_bit) = 0
        cs.constrain(
            LinearCombination(sort_bit_var),
            LinearCombination(cs.const_one()) - LinearCombination(sort_bit_var),
            LinearCombination(),
            "tapbranch_bool_" + std::to_string(k));

        // Allocate sibling bytes as private witness
        auto sibling_words = alloc_hash_witness(cs, sibling_native, "sib" + std::to_string(k) + "_");

        // Build sort mux
        auto [left_words, right_words] = build_tapbranch_sort_mux(
            cs, sort_bit_var, current_node_words, sibling_words,
            current_node_native.data(), sibling_native.data(),
            sort_bit_val, "mux" + std::to_string(k));

        // Compute TapBranch hash in circuit
        auto tapbranch_out = build_tagged_hash_64byte_circuit(
            cs, TapBranchTagMidstate(), left_words, right_words);

        // Unpack output for next level
        current_node_words = unpack_sha256_words_to_bytes(cs, tapbranch_out);

        // Update native current node (sort determines order)
        const uint8_t* ln = sort_bit_val ? current_node_native.data() : sibling_native.data();
        const uint8_t* rn = sort_bit_val ? sibling_native.data() : current_node_native.data();
        std::vector<uint8_t> tb_input(ln, ln + 32);
        tb_input.insert(tb_input.end(), rn, rn + 32);
        auto tb_native = dinero::crypto::TaggedHashArray("TapBranch", tb_input);
        std::copy(tb_native.begin(), tb_native.end(), current_node_native.begin());
    }

    // TapTweak(internal_xonly || merkle_root)
    auto ix_bytes = fe_to_bytes(cs, internal_xonly, "path_ix_");
    return build_tagged_hash_64byte_circuit(cs, TapTweakTagMidstate(), ix_bytes, current_node_words);
}

std::array<Word32, 8> taproot_path_tweak_circuit(
    R1CS& cs,
    const TaprootPathTweakWitness& witness,
    size_t path_len
) {
    FieldElement ix_fe = fe_alloc_uint256(cs, Uint256(witness.internal_xonly.data()), "path_ix_fe");
    return taproot_path_tweak_circuit_with_constant_tapleaf(cs, ix_fe, witness, path_len);
}

bool taproot_path_tweak_verify_native(
    const TaprootPathTweakWitness& witness,
    size_t path_len,
    const std::array<uint8_t, 32>& expected_tweak_hash
) {
    if (path_len > MAX_TAPROOT_DEPTH) return false;

    std::array<uint8_t, 32> current = witness.tapleaf_hash;

    for (size_t k = 0; k < path_len; ++k) {
        const auto& sib = witness.merkle_path[k];
        const uint8_t* ln = witness.sort_bits[k] ? current.data() : sib.data();
        const uint8_t* rn = witness.sort_bits[k] ? sib.data() : current.data();
        std::vector<uint8_t> tb_input(ln, ln + 32);
        tb_input.insert(tb_input.end(), rn, rn + 32);
        auto result = dinero::crypto::TaggedHashArray("TapBranch", tb_input);
        std::copy(result.begin(), result.end(), current.begin());
    }

    std::vector<uint8_t> tweak_input(witness.internal_xonly.begin(), witness.internal_xonly.end());
    tweak_input.insert(tweak_input.end(), current.begin(), current.end());
    auto tweak = dinero::crypto::TaggedHashArray("TapTweak", tweak_input);
    return tweak == expected_tweak_hash;
}

bool taproot_path_tweak_verify_circuit(
    R1CS& cs,
    const TaprootPathTweakWitness& witness,
    size_t path_len,
    const std::array<uint8_t, 32>& expected_tweak_hash
) {
    auto tapleaf_words   = alloc_hash_input(cs, witness.tapleaf_hash, "vtl_");
    auto expected_words  = alloc_hash_input(cs, expected_tweak_hash,  "vexp_");
    FieldElement ix_fe   = fe_alloc_uint256(cs, Uint256(witness.internal_xonly.data()), "vix_");
    auto tweak_words     = taproot_path_tweak_circuit_with_constant_tapleaf(cs, ix_fe, witness, path_len);
    constrain_hash_words_equal(cs, tweak_words, expected_words, "path_taptweak_pub");
    // Suppress unused variable warning for tapleaf_words — it is a public input but
    // not constrained further because the circuit uses constant tapleaf allocation.
    (void)tapleaf_words;
    return true;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
