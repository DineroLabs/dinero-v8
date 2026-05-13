// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * SHA256 Circuit Gadget for R1CS
 *
 * Implements SHA256 as R1CS constraints for zero-knowledge proof of hash
 * computation. Used by the ZKVM to prove CTV template hash evaluation
 * inside a ring-anonymous covenant spend.
 *
 * Architecture:
 *   Word32 — 32 boolean R1CS variables representing a 32-bit word.
 *            Rotations/shifts are free (reindex). Bitwise ops and
 *            addition are the constraint-bearing operations.
 *
 *   SHA256Block — one 512-bit compression block (~35K constraints)
 *   SHA256Full  — full SHA256 with padding (1+ blocks)
 *   DoubleSHA256 — SHA256(SHA256(data)) for CTV/Bitcoin-style hashing
 *
 * Cost estimates (per compression block):
 *   64 rounds × ~336 constraints/round ≈ 21,500  (ch: 32, maj: 64 per round)
 *   48 schedule words × ~200 constraints ≈ 9,600
 *   Overhead (init, final add) ≈ 800
 *   Total: ~30,000 constraints per block  (~17% reduction from original ~36K)
 *
 * For CTV double-SHA256 over 116-byte preimage:
 *   Block 1 (bytes 0-63): ~30,000
 *   Block 2 (bytes 64-127 + padding): ~30,000
 *   Block 3 (outer SHA256 of 32-byte hash): ~30,000
 *   Total: ~90,000 constraints (upper bound; was ~108,000)
 */

#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/gadgets.h"
#include <array>
#include <vector>

namespace dinero {
namespace zk {
namespace zkvm {

// ============================================================================
// Word32: 32-bit word as boolean R1CS variables
// ============================================================================

struct Word32 {
    std::array<Variable, 32> bits;  // bits[0] = LSB, bits[31] = MSB

    // Default: uninitialized variables (must be allocated before use)
    Word32() = default;
};

/**
 * Allocate a Word32 from a known 32-bit value (witness).
 * Allocates 32 boolean variables and constrains them.
 * Also constrains the packed reconstruction equals the value.
 * Returns the Word32.
 */
Word32 word32_alloc(R1CS& cs, uint32_t value, const std::string& label = "w32");

/**
 * Pack a Word32 back into a single field element.
 * Returns variable = sum(bits[i] * 2^i).
 * Cost: 1 constraint (linear combination equality).
 */
Variable word32_pack(R1CS& cs, const Word32& w, const std::string& label = "pack");

/**
 * Decompose a field-element variable into a Word32.
 * The variable must hold a value < 2^32.
 * Allocates 32 boolean bits and constrains pack(bits) == var.
 * Cost: 33 constraints (32 boolean + 1 equality).
 */
Word32 word32_from_var(R1CS& cs, Variable var, const std::string& label = "unpack");

// ============================================================================
// Word32 operations
// ============================================================================

/**
 * Bitwise XOR: z[i] = a[i] XOR b[i] for all 32 bits.
 * Cost: 32 constraints (one multiplication per bit: a+b-2ab).
 */
Word32 word32_xor(R1CS& cs, const Word32& a, const Word32& b,
                   const std::string& label = "xor");

/**
 * Bitwise AND: z[i] = a[i] AND b[i] for all 32 bits.
 * Cost: 32 constraints (one multiplication per bit: ab).
 */
Word32 word32_and(R1CS& cs, const Word32& a, const Word32& b,
                   const std::string& label = "and");

/**
 * Bitwise NOT: z[i] = 1 - a[i] for all 32 bits.
 * Cost: 0 constraints (linear operation, allocated but no multiplication).
 */
Word32 word32_not(R1CS& cs, const Word32& a, const std::string& label = "not");

/**
 * Right rotation: z = ROTR(n, a).
 * Cost: 0 constraints (just reindexes bits).
 */
Word32 word32_rotr(const Word32& a, unsigned n);

/**
 * Right shift: z = SHR(n, a). Top n bits become zero constants.
 * Cost: 0 constraints (reindex + zero substitution).
 */
Word32 word32_shr(R1CS& cs, const Word32& a, unsigned n,
                   const std::string& label = "shr");

/**
 * Addition mod 2^32 with carry chain.
 * Computes z = (a + b) mod 2^32.
 *
 * Implementation: full adder chain with explicit carry bits.
 * Each bit: sum_i = a_i XOR b_i XOR carry_i
 *           carry_{i+1} = maj(a_i, b_i, carry_i)
 *
 * Cost: ~96 constraints (3 per bit: XOR for sum, majority for carry,
 *       boolean constraint for carry).
 * The carry out of bit 31 is discarded (mod 2^32).
 */
Word32 word32_add(R1CS& cs, const Word32& a, const Word32& b,
                   const std::string& label = "add32");

/**
 * Three-input addition mod 2^32: z = (a + b + c) mod 2^32.
 * More efficient than two separate word32_add calls because
 * intermediate carries are shared.
 * Cost: ~128 constraints.
 */
Word32 word32_add3(R1CS& cs, const Word32& a, const Word32& b,
                    const Word32& c, const std::string& label = "add3");

// ============================================================================
// SHA256 functions (building blocks for the compression function)
// ============================================================================

/** Ch(e,f,g) = e*(f-g) + g. Cost: 32 constraints (1 per bit). */
Word32 sha256_ch(R1CS& cs, const Word32& e, const Word32& f, const Word32& g);

/** Maj(a,b,c): majority function. Cost: 64 constraints (2 per bit). */
Word32 sha256_maj(R1CS& cs, const Word32& a, const Word32& b, const Word32& c);

/** Σ0(a) = ROTR(2,a) XOR ROTR(13,a) XOR ROTR(22,a). Cost: 64 constraints. */
Word32 sha256_big_sigma0(R1CS& cs, const Word32& a);

/** Σ1(e) = ROTR(6,e) XOR ROTR(11,e) XOR ROTR(25,e). Cost: 64 constraints. */
Word32 sha256_big_sigma1(R1CS& cs, const Word32& e);

/** σ0(x) = ROTR(7,x) XOR ROTR(18,x) XOR SHR(3,x). Cost: 64 constraints. */
Word32 sha256_small_sigma0(R1CS& cs, const Word32& x);

/** σ1(x) = ROTR(17,x) XOR ROTR(19,x) XOR SHR(10,x). Cost: 64 constraints. */
Word32 sha256_small_sigma1(R1CS& cs, const Word32& x);

// ============================================================================
// SHA256 compression and full hash
// ============================================================================

/** SHA256 initial hash values (H0..H7). */
extern const std::array<uint32_t, 8> SHA256_H_INIT;

/** SHA256 round constants (K0..K63). */
extern const std::array<uint32_t, 64> SHA256_K;

/**
 * SHA256 state: eight 32-bit words.
 */
struct SHA256State {
    std::array<Word32, 8> h;  // h[0]=a, h[1]=b, ..., h[7]=h
};

/**
 * Allocate the SHA256 initial state as circuit constants.
 * Cost: 0 constraints (uses shared const_zero/const_one variables).
 */
SHA256State sha256_init(R1CS& cs);

/**
 * Build a SHA256State from a known midstate (e.g. from CSHA256::GetMidstate()).
 * All 8 × 32 bits are allocated as structural constants — zero constraint cost.
 * Use this to skip over already-constant prefix blocks (e.g. double-tag blocks).
 * Cost: 0 constraints.
 */
SHA256State sha256_state_from_midstate(R1CS& cs,
                                        const std::array<uint32_t, 8>& midstate);

/**
 * Pack a flat vector of byte-Words into 16 × 32-bit SHA256 block words.
 * Each Word32 in the input represents one byte (bits[0..7] are the byte value,
 * bits[8..31] must be cs.const_zero()).  The block is packed big-endian
 * (byte 0 → high bits of W[0]).
 * Cost: 0 constraints (just bit remapping).
 */
std::array<Word32, 16> sha256_pack_block(R1CS& cs,
                                          const std::vector<Word32>& bytes);

/**
 * SHA256 compression function: compress one 512-bit block.
 *
 * @param cs    Constraint system
 * @param state Current hash state (8 words)
 * @param block 16 message words (512 bits / 16 × 32-bit words)
 * @return      Updated hash state
 *
 * Cost: ~30,000 constraints.
 */
SHA256State sha256_compress(R1CS& cs, const SHA256State& state,
                             const std::array<Word32, 16>& block);

/**
 * SHA256 compression with all-constant message words.
 *
 * When all 16 block words are known compile-time constants (e.g. the NIST
 * padding block appended to a 128-byte message), the message schedule
 * W[16..63] is fully precomputed outside the circuit.  This eliminates the
 * ~9,840 constraints that sha256_compress would spend on the message schedule,
 * and replaces word32_add3(K[i], W[i], t1a) with word32_add_const() which
 * packs one fewer word.
 *
 * @param cs          Constraint system
 * @param state       Current hash state (8 private words)
 * @param block_words 16 known constant block words (uint32_t, big-endian order)
 * @return            Updated hash state
 *
 * Cost: ~20,000 constraints (vs ~30,000 for sha256_compress).
 */
SHA256State sha256_compress_const_block(R1CS& cs, const SHA256State& state,
                                         const std::array<uint32_t, 16>& block_words);

/**
 * Full SHA256 with NIST padding.
 *
 * @param cs       Constraint system
 * @param message  Input bytes as Word32 values (each holding one byte, 0-255).
 *                 Length must be known at circuit construction time.
 * @return         Hash output as 8 Word32 values (256 bits total).
 *
 * Handles padding internally: appends 1-bit, zeros, 64-bit length.
 */
std::array<Word32, 8> sha256_full(R1CS& cs,
                                    const std::vector<Word32>& message_bytes);

/**
 * Double SHA256: SHA256(SHA256(data)).
 * Used by CTV (BIP-119) and Bitcoin-style hashing.
 *
 * @param cs       Constraint system
 * @param message  Input bytes as Word32 values.
 * @return         Double-hash output as 8 Word32 values.
 */
std::array<Word32, 8> double_sha256(R1CS& cs,
                                      const std::vector<Word32>& message_bytes);

} // namespace zkvm
} // namespace zk
} // namespace dinero
