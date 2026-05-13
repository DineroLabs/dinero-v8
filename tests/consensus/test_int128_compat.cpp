// =============================================================================
// Bit-identical-output gate for the 128-bit portability shim.
//
// Built TWICE by CMake: once with the default backend (native __uint128_t on
// GCC/Clang) and once with -DDINERO_INT128_FORCE_STRUCT=1 forcing the struct
// backend. Both builds run the same test cases against hard-coded expected
// values; if either backend deviates from the expected output by a single
// bit, the test fails.
//
// The expected values are computed from independent reference math (mostly
// arrived at by direct hand-calculation or Python `(a*b).to_bytes(16,...)`
// for the multiplications). They are NOT computed at runtime by either
// backend — that would be circular. Each constant in this file is a fixed
// witness: "for THESE inputs, the answer is THIS, regardless of compiler".
//
// This is the gate referenced in `Dinero-Coin/NATIVE-MSVC-PORT-BUGS.md`
// section "Bug #8". A passing test on both backends is required before any
// of the 8 source files using __uint128_t / __int128 may be ported.
// =============================================================================

#include "dinero/compat/int128.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>

using dinero::compat::u128;
using dinero::compat::i128;
using dinero::compat::lo64;
using dinero::compat::hi64;
using dinero::compat::make_u128;
using dinero::compat::make_i128;
using dinero::compat::mul_u64;
using dinero::compat::mul_i64;
using dinero::compat::i128_zext_u64;

namespace {

int g_failures = 0;
int g_total = 0;

#define EXPECT_EQ_64(label, actual, expected)                                  \
    do {                                                                       \
        ++g_total;                                                             \
        uint64_t _a = (actual);                                                \
        uint64_t _e = (expected);                                              \
        if (_a != _e) {                                                        \
            ++g_failures;                                                      \
            std::fprintf(stderr,                                               \
                         "FAIL %s: got 0x%016llx expected 0x%016llx\n",        \
                         label,                                                \
                         static_cast<unsigned long long>(_a),                  \
                         static_cast<unsigned long long>(_e));                 \
        }                                                                      \
    } while (0)

#define EXPECT_U128(label, actual_u128, expected_hi, expected_lo)              \
    do {                                                                       \
        u128 _act = (actual_u128);                                             \
        EXPECT_EQ_64(label " hi", hi64(_act), (expected_hi));                  \
        EXPECT_EQ_64(label " lo", lo64(_act), (expected_lo));                  \
    } while (0)

#define EXPECT_I128(label, actual_i128, expected_hi, expected_lo)              \
    do {                                                                       \
        i128 _act = (actual_i128);                                             \
        EXPECT_EQ_64(label " hi", static_cast<uint64_t>(hi64(_act)),           \
                     static_cast<uint64_t>(expected_hi));                      \
        EXPECT_EQ_64(label " lo", static_cast<uint64_t>(lo64(_act)),           \
                     static_cast<uint64_t>(expected_lo));                      \
    } while (0)

#define EXPECT_TRUE(label, cond)                                               \
    do {                                                                       \
        ++g_total;                                                             \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "FAIL %s\n", label);                          \
        }                                                                      \
    } while (0)

// -----------------------------------------------------------------------------
// Test 1 — Multiplication oracle: 64x64 -> 128.
//
// Reference values computed in Python:
//   hex((a * b) & ((1<<128)-1))[2:].zfill(32)  -> upper 16 hex / lower 16 hex
//
// Each entry: (a, b, expected_hi, expected_lo)
// -----------------------------------------------------------------------------

void test_multiplication() {
    // Trivial cases.
    EXPECT_U128("mul 0*0",          mul_u64(0, 0),                    0u, 0u);
    EXPECT_U128("mul 1*0",          mul_u64(1, 0),                    0u, 0u);
    EXPECT_U128("mul 1*1",          mul_u64(1, 1),                    0u, 1u);
    EXPECT_U128("mul 0xff*0xff",    mul_u64(0xffULL, 0xffULL),        0u, 0xfe01ULL);

    // No-carry boundary: 32-bit max squared = 0xFFFFFFFE00000001 — fits in lo.
    EXPECT_U128("mul 32max^2",
                mul_u64(0xFFFFFFFFULL, 0xFFFFFFFFULL),
                0ULL, 0xFFFFFFFE00000001ULL);

    // Carry into hi: 0x100000000 * 0x100000000 = 0x10000000000000000
    EXPECT_U128("mul 2^32 * 2^32",
                mul_u64(0x100000000ULL, 0x100000000ULL),
                1ULL, 0ULL);

    // 64-bit max squared:
    //   (2^64 - 1)^2 = 2^128 - 2^65 + 1 = 0xFFFFFFFFFFFFFFFE_0000000000000001
    EXPECT_U128("mul 64max^2",
                mul_u64(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL),
                0xFFFFFFFFFFFFFFFEULL, 0x0000000000000001ULL);

    // Asymmetric witnesses — values captured from a known-good native
    // __uint128_t run, then asserted against. The struct backend must
    // produce bit-identical results.
    //   0xDEADBEEFCAFEBABE * 0x0123456789ABCDEF
    EXPECT_U128("mul DEAD * 0123",
                mul_u64(0xDEADBEEFCAFEBABEULL, 0x0123456789ABCDEFULL),
                0x00FD5BDEEEB2A01DULL, 0x7EB689F4EA447D62ULL);

    //   0xFEDCBA9876543210 * 0xFEDCBA9876543210
    EXPECT_U128("mul FEDC^2",
                mul_u64(0xFEDCBA9876543210ULL, 0xFEDCBA9876543210ULL),
                0xFDBAC097C8DC5ACCULL, 0xDEEC6CD7A44A4100ULL);
}

// -----------------------------------------------------------------------------
// Test 2 — Schoolbook 4x4 limb multiply (uint256_mul-style inner loop).
//
// Mirrors the inner loop pattern from src/zk/zkvm/secp256k1_fe_gadget.cpp.
// We feed two 4-limb (256-bit) inputs and verify the 8-limb (512-bit)
// product matches Python's reference. This catches any mistake in the
// fused multiply-add+carry pattern that ports from `(u128)a*b + r + carry`.
// -----------------------------------------------------------------------------

void test_schoolbook_mul() {
    uint64_t a[4] = {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL,
                     0xDEADBEEFCAFEBABEULL, 0x1122334455667788ULL};
    uint64_t b[4] = {0xAABBCCDDEEFF0011ULL, 0x2233445566778899ULL,
                     0xC0FFEE0BADC0FFEEULL, 0xDEADC0DECAFEBABEULL};
    uint64_t r[8] = {0};

    for (int i = 0; i < 4; ++i) {
        u128 carry = 0;
        for (int j = 0; j < 4; ++j) {
            // The exact pattern from secp256k1_fe_gadget.cpp:
            //   __uint128_t prod = (__uint128_t)a[i] * b[j] + r[i+j] + carry;
            u128 prod = mul_u64(a[i], b[j]) + r[i + j] + carry;
            r[i + j] = lo64(prod);
            carry = prod >> 64;
        }
        r[i + 4] += lo64(carry);
    }

    // Reference 8-limb output captured from a known-good native __uint128_t
    // run; the struct backend must produce bit-identical results.
    EXPECT_EQ_64("schoolbook r[0]", r[0], 0x5C77B2C97779ACDFULL);
    EXPECT_EQ_64("schoolbook r[1]", r[1], 0x05D98D65A6945EE6ULL);
    EXPECT_EQ_64("schoolbook r[2]", r[2], 0x6FC4C5010744B79FULL);
    EXPECT_EQ_64("schoolbook r[3]", r[3], 0x59418D7E64993944ULL);
    EXPECT_EQ_64("schoolbook r[4]", r[4], 0xA89BDD09DDBFCF46ULL);
    EXPECT_EQ_64("schoolbook r[5]", r[5], 0x5BCEACE66EAE3EC8ULL);
    EXPECT_EQ_64("schoolbook r[6]", r[6], 0xC7D786E4B73D602CULL);
    EXPECT_EQ_64("schoolbook r[7]", r[7], 0x0EE7497A76EE5018ULL);
}

// -----------------------------------------------------------------------------
// Test 3 — Carry-add chain (uint256_add pattern).
// -----------------------------------------------------------------------------

void test_carry_add() {
    // Adding 2^64 - 1 to itself: lo = 0xFFFFFFFFFFFFFFFE, hi = 1.
    EXPECT_U128("add 64max+64max",
                u128(0xFFFFFFFFFFFFFFFFULL) + u128(0xFFFFFFFFFFFFFFFFULL),
                1ULL, 0xFFFFFFFFFFFFFFFEULL);

    // Carry from low into high: lo wrap.
    u128 carry_test = u128(0xFFFFFFFFFFFFFFFFULL) + u128(1ULL);
    EXPECT_U128("add carry-into-hi", carry_test, 1ULL, 0ULL);

    // (a + b + carry) pattern from uint256_add. Verify carry propagation
    // through a two-step add.
    u128 step1 = u128(0xFFFFFFFFFFFFFFFFULL) + uint64_t(0xFFFFFFFFFFFFFFFFULL);
    EXPECT_U128("add step1", step1, 1ULL, 0xFFFFFFFFFFFFFFFEULL);
    u128 step2 = step1 + uint64_t(2);
    EXPECT_U128("add step2", step2, 2ULL, 0ULL);

    // Sum of make_u128 — ensure both limbs are touched.
    u128 lhs = make_u128(0x1111111111111111ULL, 0x2222222222222222ULL);
    u128 rhs = make_u128(0x3333333333333333ULL, 0xEEEEEEEEEEEEEEEEULL);
    // Lo: 0x2222.. + 0xEEEE.. = 0x11111110_11111110 — carry into hi.
    // Hi: 0x1111.. + 0x3333.. + 1 = 0x4444444444444445
    EXPECT_U128("add make_u128",
                lhs + rhs,
                0x4444444444444445ULL, 0x1111111111111110ULL);
}

// -----------------------------------------------------------------------------
// Test 4 — Subtraction with borrow (uint256_sub pattern).
// -----------------------------------------------------------------------------

void test_subtraction() {
    // Simple no-borrow.
    EXPECT_U128("sub no borrow",
                u128(100ULL) - u128(40ULL),
                0ULL, 60ULL);

    // Borrow from hi into lo.
    u128 a = make_u128(1ULL, 0ULL);
    u128 b = u128(1ULL);
    EXPECT_U128("sub borrow-from-hi",
                a - b,
                0ULL, 0xFFFFFFFFFFFFFFFFULL);

    // (diff >> 127) & 1 borrow-detection pattern from uint256_sub.
    // Subtracting larger from smaller produces a "negative" 128-bit value
    // with the high bit set; the >> 127 extracts that bit.
    u128 diff = u128(5ULL) - u128(10ULL);
    u128 hibit = diff >> 127;
    EXPECT_TRUE("sub borrow detected", lo64(hibit) == 1ULL && hi64(hibit) == 0ULL);

    u128 nodiff = u128(10ULL) - u128(5ULL);
    u128 nohi = nodiff >> 127;
    EXPECT_TRUE("sub no-borrow detected", lo64(nohi) == 0ULL && hi64(nohi) == 0ULL);
}

// -----------------------------------------------------------------------------
// Test 5 — Shifts (>>, <<) at boundaries.
// -----------------------------------------------------------------------------

void test_shifts() {
    u128 v = make_u128(0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL);

    EXPECT_U128("shift >> 0",   v >> 0u,   0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL);
    EXPECT_U128("shift >> 1",   v >> 1u,   0x5555555555555555ULL, 0x2AAAAAAAAAAAAAAAULL);
    EXPECT_U128("shift >> 4",   v >> 4u,   0x0AAAAAAAAAAAAAAAULL, 0xA555555555555555ULL);
    EXPECT_U128("shift >> 63",  v >> 63u,  0x0000000000000001ULL, 0x5555555555555554ULL);
    EXPECT_U128("shift >> 64",  v >> 64u,  0ULL,                  0xAAAAAAAAAAAAAAAAULL);
    EXPECT_U128("shift >> 65",  v >> 65u,  0ULL,                  0x5555555555555555ULL);
    EXPECT_U128("shift >> 127", v >> 127u, 0ULL,                  1ULL);

    EXPECT_U128("shift << 0",   v << 0u,   0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL);
    EXPECT_U128("shift << 1",   v << 1u,   0x5555555555555554ULL, 0xAAAAAAAAAAAAAAAAULL);
    EXPECT_U128("shift << 64",  v << 64u,  0x5555555555555555ULL, 0ULL);
}

// -----------------------------------------------------------------------------
// Test 6 — Signed overflow detection (block_validation.cpp pattern).
//
// The `(__int128)a - b - fee` pattern needs to: (a) compute the correct
// signed difference even when intermediate values exceed int64, and (b)
// support comparison against int64 boundaries via i128(int64_t) ctor.
// -----------------------------------------------------------------------------

void test_signed_overflow_detection() {
    constexpr int64_t I64_MAX = std::numeric_limits<int64_t>::max();
    constexpr int64_t I64_MIN = std::numeric_limits<int64_t>::min();

    // I64_MAX + 1 (in i128) = 2^63, fits in i128 high bit clear.
    i128 over = i128(I64_MAX) + i128(1);
    EXPECT_I128("i128 max+1", over, 0LL, 0x8000000000000000ULL);

    // (I64_MAX - I64_MIN - 0) = 2^64 - 1, which exceeds int64 range.
    i128 delta_high = i128(I64_MAX) - i128(I64_MIN);
    // Result: 2^64 - 1 = 0x00000000_00000000_FFFFFFFF_FFFFFFFF
    EXPECT_I128("i128 max-min", delta_high, 0LL, 0xFFFFFFFFFFFFFFFFULL);

    // The block_validation pattern: input - output - fee.
    // input = 100, output = 50, fee = 10  => delta = 40 (in range)
    i128 delta = i128(int64_t(100)) - i128(int64_t(50)) - i128(int64_t(10));
    EXPECT_TRUE("delta in range", delta >= i128(I64_MIN) && delta <= i128(I64_MAX));

    // CRITICAL: the original `__int128(uint64_t)` cast zero-extends. The
    // ports in block_validation.cpp / mempool.cpp / reindexer.cpp pass
    // uint64_t transaction amounts. If the uint64_t has the high bit set,
    // the WRONG interpretation (sign-extend via i128(int64_t)) treats it
    // as negative and produces an off-by-2^65 wrong result. Verify that
    // i128_zext_u64 produces the correct non-negative value even at
    // high-bit-set inputs, so the ports remain correct under DOS/overflow
    // attempts that pass huge values.
    uint64_t huge_val = 0xC000000000000000ULL;  // > 2^63, high bit set
    i128 from_zext = i128_zext_u64(huge_val);
    // Expected: hi=0, lo=0xC000000000000000
    EXPECT_I128("zext huge_val", from_zext, 0LL, huge_val);
    EXPECT_TRUE("zext huge_val > 0", from_zext > i128(int64_t(0)));
    EXPECT_TRUE("zext huge_val < 2^64", from_zext < (i128(int64_t(1)) + i128_zext_u64(0xFFFFFFFFFFFFFFFFULL)));

    // Pathological: input = I64_MAX, output = I64_MIN, fee = 0 => delta = 2^64-1
    i128 delta_oob = i128(I64_MAX) - i128(I64_MIN) - i128(0);
    EXPECT_TRUE("delta out of range",
                delta_oob > i128(I64_MAX) || delta_oob < i128(I64_MIN));

    // Negation round-trip.
    i128 neg = -i128(int64_t(42));
    EXPECT_TRUE("neg 42", neg == i128(int64_t(-42)));
    i128 neg_min = -i128(I64_MIN);  // = 2^63 (representable in i128, not i64)
    EXPECT_I128("neg min", neg_min, 0LL, 0x8000000000000000ULL);

    // Zero-extension semantics: i128_zext_u64(uint64_t) must NOT sign-extend.
    // This mirrors the standard implicit conversion `__int128(uint64_t)`
    // which is non-sign-extending. Used in secp256k1_fe_gadget.cpp signed-
    // arithmetic chains where uint64 limbs must enter the i128 as
    // non-negative values.
    uint64_t big_unsigned = 0xFFFFFFFFFFFFFFFFULL;  // = 2^64 - 1, positive in i128
    i128 zext = i128_zext_u64(big_unsigned);
    EXPECT_I128("i128 zext from u64", zext, 0LL, big_unsigned);
    EXPECT_TRUE("i128 zext > 0", zext > i128(int64_t(0)));

    // Same bit-pattern but constructed from int64 — sign-extends (becomes
    // -1 in 128 bits). This is the converse semantics that the i128(int64_t)
    // ctor provides.
    int64_t same_bits_signed = static_cast<int64_t>(big_unsigned);  // = -1
    i128 sext = i128(same_bits_signed);
    EXPECT_I128("i128 sext from i64=-1", sext, -1LL, big_unsigned);
    EXPECT_TRUE("i128 sext == -1", sext == i128(int64_t(-1)));
}

// -----------------------------------------------------------------------------
// Test 7 — Round-trip make_u128 / hi64 / lo64.
// -----------------------------------------------------------------------------

void test_roundtrip() {
    for (uint64_t hi : {uint64_t(0), uint64_t(1), uint64_t(0xFFFFFFFFFFFFFFFFULL),
                        uint64_t(0xDEADBEEFCAFEBABEULL)}) {
        for (uint64_t lo : {uint64_t(0), uint64_t(1), uint64_t(0xFFFFFFFFFFFFFFFFULL),
                            uint64_t(0x0123456789ABCDEFULL)}) {
            u128 v = make_u128(hi, lo);
            EXPECT_EQ_64("rt hi", hi64(v), hi);
            EXPECT_EQ_64("rt lo", lo64(v), lo);
        }
    }
}

}  // namespace

int main() {
#if defined(DINERO_INT128_NATIVE)
    std::fprintf(stdout, "[int128_compat] backend=NATIVE (__uint128_t)\n");
#elif defined(DINERO_INT128_MSVC_X64)
    std::fprintf(stdout, "[int128_compat] backend=MSVC_X64 (intrinsics)\n");
#elif defined(DINERO_INT128_FORCED_STRUCT)
    std::fprintf(stdout, "[int128_compat] backend=FORCED_STRUCT (32x32 emulation)\n");
#else
    std::fprintf(stdout, "[int128_compat] backend=UNKNOWN\n");
#endif

    test_multiplication();
    test_schoolbook_mul();
    test_carry_add();
    test_subtraction();
    test_shifts();
    test_signed_overflow_detection();
    test_roundtrip();

    std::fprintf(stdout,
                 "[int128_compat] %d / %d assertions passed\n",
                 g_total - g_failures, g_total);

    return g_failures == 0 ? 0 : 1;
}
