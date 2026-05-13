// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/schnorr_gadget.h"
#include "crypto/sha256.h"
#include <openssl/sha.h>
#include <cassert>
#include <cstring>

namespace dinero {
namespace zk {
namespace zkvm {

// ---------------------------------------------------------------------------
// BIP-340 tagged hash constants
// ---------------------------------------------------------------------------

// SHA256("BIP0340/challenge") — precomputed tag hash (32 bytes)
static const uint8_t* bip340_challenge_tag() {
    static uint8_t tag[32] = {0};
    static bool computed = false;
    if (!computed) {
        const char* tag_str = "BIP0340/challenge";
        SHA256(reinterpret_cast<const uint8_t*>(tag_str),
               std::strlen(tag_str), tag);
        computed = true;
    }
    return tag;
}

// ---------------------------------------------------------------------------
// FieldElement → 32 big-endian Word32 bytes
// ---------------------------------------------------------------------------

std::vector<Word32> fe_to_bytes(R1CS& cs, const FieldElement& fe,
                                 const std::string& label) {
    // Each limb is 64 bits = 8 bytes. Total: 32 bytes.
    // Output: big-endian (byte 0 = MSB of limbs[3]).
    //
    // We need to decompose each limb into 8 bytes, constrained.
    // Use the existing limb value and extract bytes via bit decomposition.

    std::vector<Word32> bytes(32);

    for (int l = 3; l >= 0; --l) {
        Scalar limb_val = cs.get_value(fe.limbs[l]);
        // Extract limb as uint64
        const uint8_t* lb = limb_val.data();
        uint64_t val64 = 0;
        for (int b = 0; b < 8; ++b)
            val64 = (val64 << 8) | lb[24 + b];

        // 8 bytes from this limb, big-endian within the limb
        int base_byte = (3 - l) * 8; // l=3 → bytes 0..7, l=0 → bytes 24..31
        for (int b = 0; b < 8; ++b) {
            uint8_t byte_val = (val64 >> (56 - 8 * b)) & 0xFF;
            bytes[base_byte + b] = word32_alloc(cs, byte_val,
                label + "_l" + std::to_string(l) + "b" + std::to_string(b));
        }

        // Constrain: limb == sum(byte[i] * 256^(7-i)) for the 8 bytes
        // i.e., limb = byte[0]*2^56 + byte[1]*2^48 + ... + byte[7]*2^0
        LinearCombination pack;
        Scalar pow256 = Scalar(uint64_t(1));
        for (int b = 7; b >= 0; --b) {
            Variable byte_packed = word32_pack(cs, bytes[base_byte + b],
                label + "_pk" + std::to_string(l) + std::to_string(b));
            pack = pack + LinearCombination(pow256, byte_packed);
            pow256 = pow256 * Scalar(uint64_t(256));
        }
        cs.constrain(LinearCombination(fe.limbs[l]), LinearCombination(VAR_ONE),
                     pack, label + "_eq" + std::to_string(l));
    }

    return bytes;
}

// ---------------------------------------------------------------------------
// BIP-340 challenge hash midstate
// ---------------------------------------------------------------------------

// SHA256 midstate after compressing tag(32) || tag(32).
// Eliminates one full sha256_compress call (~30K constraints) and the
// 64 × word32_alloc for the constant tag bytes (~2K constraints).
static const std::array<uint32_t, 8>& bip340_challenge_midstate() {
    static const std::array<uint32_t, 8> kMidstate = [] {
        std::array<uint32_t, 8> ms{};
        const uint8_t* tag = bip340_challenge_tag();
        dinero::crypto::CSHA256 hasher;
        hasher.Write(tag, 32);
        hasher.Write(tag, 32);
        hasher.GetMidstate(ms.data());
        return ms;
    }();
    return kMidstate;
}

// ---------------------------------------------------------------------------
// BIP-340 challenge hash
// ---------------------------------------------------------------------------

std::array<Word32, 8> bip340_challenge_hash(
    R1CS& cs,
    const std::vector<Word32>& R_x_bytes,
    const std::vector<Word32>& P_x_bytes,
    const std::vector<Word32>& msg_bytes,
    const std::string& label)
{
    assert(R_x_bytes.size() == 32);
    assert(P_x_bytes.size() == 32);
    assert(msg_bytes.size() == 32);

    // Total message is: tag(32) || tag(32) || R_x(32) || P_x(32) || msg(32) = 160 bytes.
    // The first 64 bytes (tag || tag) are compile-time constants.
    // Use the precomputed midstate to skip block 1 compression entirely.

    // Block 2: R_x || P_x (64 private bytes)
    std::vector<Word32> block2;
    block2.reserve(64);
    block2.insert(block2.end(), R_x_bytes.begin(), R_x_bytes.end());
    block2.insert(block2.end(), P_x_bytes.begin(), P_x_bytes.end());

    // Block 3: msg (32 private bytes) + NIST padding for 160-byte message (1280 bits)
    std::vector<Word32> block3;
    block3.reserve(64);
    block3.insert(block3.end(), msg_bytes.begin(), msg_bytes.end());
    {
        // Padding: 0x80 byte, zeros, then 8-byte big-endian bit length
        Word32 pad80;
        for (unsigned b = 0; b < 32; ++b)
            pad80.bits[b] = (b < 8 && ((0x80u >> b) & 1u)) ? cs.const_one()
                                                             : cs.const_zero();
        block3.push_back(pad80);
        while (block3.size() < 56) {
            Word32 zero_byte;
            for (unsigned b = 0; b < 32; ++b) zero_byte.bits[b] = cs.const_zero();
            block3.push_back(zero_byte);
        }
        constexpr uint64_t kBitLen = 160u * 8u; // 1280 = 0x500
        for (int i = 7; i >= 0; --i) {
            uint8_t v = (kBitLen >> (i * 8)) & 0xFFu;
            Word32 len_byte;
            for (unsigned b = 0; b < 32; ++b)
                len_byte.bits[b] = (b < 8 && ((v >> b) & 1u)) ? cs.const_one()
                                                               : cs.const_zero();
            block3.push_back(len_byte);
        }
    }

    SHA256State state = sha256_state_from_midstate(cs, bip340_challenge_midstate());
    state = sha256_compress(cs, state, sha256_pack_block(cs, block2));
    state = sha256_compress(cs, state, sha256_pack_block(cs, block3));
    return state.h;
}

// ---------------------------------------------------------------------------
// Hash output → scalar bits
// ---------------------------------------------------------------------------

std::vector<Variable> hash_to_scalar_bits(const std::array<Word32, 8>& hash) {
    // hash[0] = most significant 32 bits, hash[7] = least significant
    // We need LSB-first bit ordering.
    // hash[7].bits[0] = bit 0 (LSB), hash[0].bits[31] = bit 255 (MSB)
    std::vector<Variable> bits(256);
    for (int w = 0; w < 8; ++w) {
        // Word w contributes bits [w*32 .. w*32+31] in the REVERSED word order
        // hash[7] = lowest word, hash[0] = highest word
        int word_idx = 7 - w; // w=0 → hash[7] (LSB word)
        for (int b = 0; b < 32; ++b) {
            bits[w * 32 + b] = hash[word_idx].bits[b];
        }
    }
    return bits;
}

// ---------------------------------------------------------------------------
// X-only point lift (BIP-340: even Y)
// ---------------------------------------------------------------------------

ECPoint bip340_lift_x(R1CS& cs, const FieldElement& x,
                       const std::string& label) {
    // Prover computes y = sqrt(x^3 + 7) mod p, choosing even y.
    Uint256 x_val;
    for (int i = 0; i < 4; ++i) {
        Scalar s = cs.get_value(x.limbs[i]);
        const uint8_t* bytes = s.data();
        x_val.limbs[i] = 0;
        for (int b = 0; b < 8; ++b)
            x_val.limbs[i] = (x_val.limbs[i] << 8) | bytes[24 + b];
    }

    // Compute x^3 + 7 mod p
    Uint256 x2 = uint256_mul_mod_p(x_val, x_val);
    Uint256 x3 = uint256_mul_mod_p(x2, x_val);
    Uint256 seven(uint64_t(7));
    Uint256 rhs = uint256_add_mod_p(x3, seven);

    // Compute y = sqrt(rhs) mod p
    // For p ≡ 3 (mod 4): y = rhs^((p+1)/4) mod p
    // secp256k1: p ≡ 3 (mod 4), so this works.
    // (p+1)/4 = (p+1) >> 2
    //
    // p+1 = FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC30
    // (p+1)/4 = 3FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFBFFFFF0C
    uint8_t exp[32] = {
        0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xBF, 0xFF, 0xFF, 0x0C
    };

    // y = rhs^exp mod p (via square-and-multiply)
    Uint256 y_val(uint64_t(1));
    Uint256 base = rhs;
    for (int i = 0; i < 256; ++i) {
        int byte_idx = 31 - (i / 8);
        int bit_idx = i % 8;
        if ((exp[byte_idx] >> bit_idx) & 1) {
            y_val = uint256_mul_mod_p(y_val, base);
        }
        base = uint256_mul_mod_p(base, base);
    }

    // Ensure even Y: if y is odd, use p - y
    bool y_is_odd = (y_val.limbs[0] & 1) != 0;
    if (y_is_odd) {
        y_val = uint256_sub_mod_p(uint256_p(), y_val);
    }

    // Allocate y as FieldElement
    FieldElement y_fe = fe_alloc_uint256(cs, y_val, label + "_y");

    // Constrain: y^2 == x^3 + 7 (on-curve check)
    FieldElement y2 = fe_mul(cs, y_fe, y_fe, label + "_y2");
    FieldElement x2_fe = fe_mul(cs, x, x, label + "_x2");
    FieldElement x3_fe = fe_mul(cs, x2_fe, x, label + "_x3");
    FieldElement seven_fe = fe_constant(cs, Uint256(uint64_t(7)), label + "_7");
    FieldElement rhs_fe = fe_add(cs, x3_fe, seven_fe, label + "_rhs");

    // y^2 must equal rhs
    for (int i = 0; i < 4; ++i) {
        gadgets::assert_equal(cs, y2.limbs[i], rhs_fe.limbs[i],
                              label + "_oc" + std::to_string(i));
    }

    // Constrain even parity: y's bit 0 must be 0.
    // The LSB of y is the LSB of y_fe.limbs[0].
    // We already have the limb decomposed in fe_alloc_uint256 (range check).
    // But we need explicit access to bit 0. Extract it:
    Scalar y_limb0_val = cs.get_value(y_fe.limbs[0]);
    const uint8_t* y0_bytes = y_limb0_val.data();
    uint64_t y0_int = 0;
    for (int b = 0; b < 8; ++b) y0_int = (y0_int << 8) | y0_bytes[24 + b];
    uint64_t lsb = y0_int & 1;

    Variable y_lsb = cs.alloc(Scalar(lsb));
    gadgets::enforce_boolean(cs, y_lsb, label + "_lsb_bool");

    // Constrain: y_fe.limbs[0] = 2 * half + y_lsb  (decompose LSB)
    Scalar half_val = Scalar(y0_int >> 1);
    Variable half = cs.alloc(half_val);
    // y_limb0 = 2 * half + lsb
    cs.constrain(
        LinearCombination(y_fe.limbs[0]),
        LinearCombination(VAR_ONE),
        LinearCombination(Scalar(uint64_t(2)), half) + LinearCombination(y_lsb),
        label + "_lsb_dec");

    // Range check half (63 bits, since limb is 64 bits and we removed 1 bit)
    range_check_limb(cs, half, 63, label + "_half_rc");

    // Assert y_lsb == 0 (even parity)
    gadgets::assert_zero(cs, y_lsb, label + "_even");

    ECPoint result;
    result.x = x;
    result.y = y_fe;
    result.is_identity = gadgets::constant(cs, Scalar::zero(), label + "_noinf");
    return result;
}

// ---------------------------------------------------------------------------
// Full BIP-340 Schnorr verification
// ---------------------------------------------------------------------------

Variable bip340_verify(R1CS& cs,
                        const std::vector<Variable>& s_bits,
                        const FieldElement& R_x,
                        const FieldElement& P_x,
                        const std::vector<Word32>& msg_bytes,
                        const std::string& label)
{
    assert(s_bits.size() == 256);
    assert(msg_bytes.size() == 32);

    // Step 1: Lift R_x and P_x to full points (even Y)
    ECPoint R = bip340_lift_x(cs, R_x, label + "_lR");
    ECPoint P = bip340_lift_x(cs, P_x, label + "_lP");

    // Step 2: Compute challenge e = tagged_hash(R_x || P_x || m)
    std::vector<Word32> R_x_bytes = fe_to_bytes(cs, R_x, label + "_Rxb");
    std::vector<Word32> P_x_bytes = fe_to_bytes(cs, P_x, label + "_Pxb");

    std::array<Word32, 8> e_hash = bip340_challenge_hash(
        cs, R_x_bytes, P_x_bytes, msg_bytes, label + "_ch");

    // Convert hash to 256 scalar bits (LSB first)
    std::vector<Variable> e_bits = hash_to_scalar_bits(e_hash);

    // Step 3: Compute s*G (fixed-base)
    ECPoint sG = ec_scalar_mul_gen(cs, s_bits, label + "_sG");

    // Step 4: Compute e*P (variable-base)
    ECPoint eP = ec_scalar_mul(cs, e_bits, P, label + "_eP");

    // Step 5: Compute R + e*P
    ECPoint R_plus_eP = ec_add_complete(cs, R, eP, label + "_ReP");

    // Step 6: Check s*G == R + e*P
    Variable result = ec_equal(cs, sG, R_plus_eP, label + "_eq");

    return result;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
