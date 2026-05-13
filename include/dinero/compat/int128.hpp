#pragma once

/**
 * @brief 128-bit integer portability shim (GCC/Clang/MSVC).
 *
 * The Dinero codebase has historically used GCC's `__uint128_t` / `__int128`
 * extensions for 64x64->128 multiplication, carry-aware addition/subtraction,
 * and signed overflow detection. MSVC does not provide a native 128-bit
 * integer type, which blocks the native MSVC build of dinerod.
 *
 * This header provides:
 *   dinero::compat::u128 — unsigned 128-bit integer with operator overloads
 *   dinero::compat::i128 — signed 128-bit integer with operator overloads
 *
 * On GCC/Clang (where __SIZEOF_INT128__ is defined) the types are aliases for
 * the native compiler builtins — zero overhead, codegen identical to direct
 * use of `__uint128_t` / `__int128`.
 *
 * On MSVC x64 the types are POD structs of two 64-bit limbs with operators
 * implemented via Microsoft compiler intrinsics: `_umul128`, `_mul128`,
 * `_addcarry_u64`, `_subborrow_u64`, `__shiftright128`, `__shiftleft128`.
 *
 * The two backends MUST produce bit-identical results. This is verified by
 * `tests/consensus/test_int128_compat.cpp`, which builds twice (once natively
 * on GCC/Clang, once with `DINERO_INT128_FORCE_STRUCT=1` forcing the struct
 * backend even on GCC) and compares hashed outputs.
 *
 * The struct-backend layer is modelled on the existing portability shim in
 * `third_party/secp256k1-zkp/src/int128_struct_impl.h`, mirroring its proven
 * implementations of the underlying 64x64->128 primitive operations.
 */

#include <cstdint>
#include <type_traits>

// Backend selection. On targets where __SIZEOF_INT128__ is available (GCC,
// Clang, ICC) we default to the native types. The DINERO_INT128_FORCE_STRUCT
// override exists so the struct backend can be exercised on a Linux/GCC host
// for the bit-identical-output test gate without needing real MSVC. This is
// the same testing trick secp256k1-zkp uses internally.
#if defined(__SIZEOF_INT128__) && !defined(DINERO_INT128_FORCE_STRUCT)
#  define DINERO_INT128_NATIVE 1
#elif defined(_MSC_VER) && defined(_M_X64)
#  define DINERO_INT128_MSVC_X64 1
#  include <intrin.h>
#elif defined(DINERO_INT128_FORCE_STRUCT)
// Force-struct backend on a non-MSVC compiler. Used only for cross-validation
// testing of the struct path against the native path on the same machine. The
// 64x64->128 multiply uses the 32x32->64 emulation from secp256k1-zkp.
#  define DINERO_INT128_FORCED_STRUCT 1
#else
#  error "dinero/compat/int128.hpp: no 128-bit integer backend available. Need __SIZEOF_INT128__ (GCC/Clang) or _MSC_VER+_M_X64 (MSVC x64)."
#endif

#if defined(DINERO_INT128_MSVC_X64)
#  define DINERO_INT128_INLINE __forceinline
#else
#  define DINERO_INT128_INLINE inline
#endif

namespace dinero {
namespace compat {

#if defined(DINERO_INT128_NATIVE)

// =============================================================================
// Native backend: aliases for the GCC/Clang builtins.
// =============================================================================

using u128 = unsigned __int128;
using i128 = __int128;

DINERO_INT128_INLINE constexpr uint64_t lo64(u128 x) noexcept {
    return static_cast<uint64_t>(x);
}
DINERO_INT128_INLINE constexpr uint64_t hi64(u128 x) noexcept {
    return static_cast<uint64_t>(x >> 64);
}
DINERO_INT128_INLINE constexpr int64_t lo64(i128 x) noexcept {
    return static_cast<int64_t>(static_cast<uint64_t>(x));
}
DINERO_INT128_INLINE constexpr int64_t hi64(i128 x) noexcept {
    return static_cast<int64_t>(x >> 64);
}
DINERO_INT128_INLINE constexpr u128 make_u128(uint64_t hi, uint64_t lo) noexcept {
    return (static_cast<u128>(hi) << 64) | static_cast<u128>(lo);
}
DINERO_INT128_INLINE constexpr i128 make_i128(int64_t hi, uint64_t lo) noexcept {
    return (static_cast<i128>(static_cast<uint64_t>(hi)) << 64) |
           static_cast<i128>(lo);
}

// Free function: explicit 64x64 -> 128 multiply, useful when call-sites want
// to be unambiguous about precision. `(u128)a * b` does the same thing on the
// native path; this helper exists so the struct backend's `(u128)a * b` can
// dispatch through the same name. (See struct backend below.)
DINERO_INT128_INLINE u128 mul_u64(uint64_t a, uint64_t b) noexcept {
    return static_cast<u128>(a) * static_cast<u128>(b);
}

DINERO_INT128_INLINE i128 mul_i64(int64_t a, int64_t b) noexcept {
    return static_cast<i128>(a) * static_cast<i128>(b);
}

#else  // struct backend (MSVC x64 OR DINERO_INT128_FORCED_STRUCT on GCC)

// =============================================================================
// Struct backend: POD types with explicit operator overloads.
//
// 64x64->128 primitive: dispatched to MSVC intrinsics on MSVC, or to the
// 32x32->64 emulation (mirrored from secp256k1-zkp) on the forced-struct
// build that runs on GCC for testing.
// =============================================================================

namespace detail {

DINERO_INT128_INLINE uint64_t umul128(uint64_t a, uint64_t b, uint64_t* hi) noexcept {
#  if defined(DINERO_INT128_MSVC_X64)
    return _umul128(a, b, hi);
#  else
    // Mirror of secp256k1_umul128 from int128_struct_impl.h.
    uint64_t ll = static_cast<uint64_t>(static_cast<uint32_t>(a)) * static_cast<uint32_t>(b);
    uint64_t lh = static_cast<uint32_t>(a) * (b >> 32);
    uint64_t hl = (a >> 32) * static_cast<uint32_t>(b);
    uint64_t hh = (a >> 32) * (b >> 32);
    uint64_t mid34 = (ll >> 32) + static_cast<uint32_t>(lh) + static_cast<uint32_t>(hl);
    *hi = hh + (lh >> 32) + (hl >> 32) + (mid34 >> 32);
    return (mid34 << 32) + static_cast<uint32_t>(ll);
#  endif
}

DINERO_INT128_INLINE int64_t mul128(int64_t a, int64_t b, int64_t* hi) noexcept {
#  if defined(DINERO_INT128_MSVC_X64)
    return _mul128(a, b, hi);
#  else
    // Mirror of secp256k1_mul128 from int128_struct_impl.h.
    uint64_t ll = static_cast<uint64_t>(static_cast<uint32_t>(a)) * static_cast<uint32_t>(b);
    int64_t  lh = static_cast<uint32_t>(a) * (b >> 32);
    int64_t  hl = (a >> 32) * static_cast<uint32_t>(b);
    int64_t  hh = (a >> 32) * (b >> 32);
    uint64_t mid34 = (ll >> 32) + static_cast<uint32_t>(lh) + static_cast<uint32_t>(hl);
    *hi = hh + (lh >> 32) + (hl >> 32) + (mid34 >> 32);
    return static_cast<int64_t>((mid34 << 32) + static_cast<uint32_t>(ll));
#  endif
}

}  // namespace detail

// Forward decls — i128 references u128's lo64.
struct u128;
struct i128;

struct u128 {
    uint64_t lo;
    uint64_t hi;

    constexpr u128() noexcept : lo(0), hi(0) {}
    // Implicit ctor from uint64_t — required so `u128 carry = 0;` and
    // `(u128)a + b + carry` work the same as on the native backend.
    constexpr u128(uint64_t v) noexcept : lo(v), hi(0) {}
    constexpr u128(uint64_t hi_, uint64_t lo_) noexcept : lo(lo_), hi(hi_) {}

    explicit constexpr operator uint64_t() const noexcept { return lo; }
    explicit constexpr operator bool() const noexcept { return lo != 0 || hi != 0; }
};

struct i128 {
    uint64_t lo;
    int64_t  hi;

    constexpr i128() noexcept : lo(0), hi(0) {}
    constexpr i128(int64_t v) noexcept
        : lo(static_cast<uint64_t>(v)), hi(v >> 63) {}
    constexpr i128(int64_t hi_, uint64_t lo_) noexcept : lo(lo_), hi(hi_) {}

    explicit constexpr operator int64_t() const noexcept {
        return static_cast<int64_t>(lo);
    }
    explicit constexpr operator uint64_t() const noexcept { return lo; }
    explicit constexpr operator bool() const noexcept { return lo != 0 || hi != 0; }
};

// ---------- u128 helpers / free functions ----------

DINERO_INT128_INLINE constexpr uint64_t lo64(u128 x) noexcept { return x.lo; }
DINERO_INT128_INLINE constexpr uint64_t hi64(u128 x) noexcept { return x.hi; }
DINERO_INT128_INLINE constexpr u128 make_u128(uint64_t hi, uint64_t lo) noexcept {
    return u128{hi, lo};
}

DINERO_INT128_INLINE u128 mul_u64(uint64_t a, uint64_t b) noexcept {
    u128 r;
    r.lo = detail::umul128(a, b, &r.hi);
    return r;
}

// ---------- u128 arithmetic ----------

DINERO_INT128_INLINE u128 operator+(u128 a, u128 b) noexcept {
#  if defined(DINERO_INT128_MSVC_X64)
    u128 r;
    unsigned char c = _addcarry_u64(0, a.lo, b.lo, &r.lo);
    _addcarry_u64(c, a.hi, b.hi, &r.hi);
    return r;
#  else
    u128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo ? 1 : 0);
    return r;
#  endif
}

DINERO_INT128_INLINE u128 operator+(u128 a, uint64_t b) noexcept {
    return a + u128(b);
}
DINERO_INT128_INLINE u128 operator+(uint64_t a, u128 b) noexcept {
    return u128(a) + b;
}

DINERO_INT128_INLINE u128& operator+=(u128& a, u128 b) noexcept {
    a = a + b;
    return a;
}
DINERO_INT128_INLINE u128& operator+=(u128& a, uint64_t b) noexcept {
    a = a + b;
    return a;
}

DINERO_INT128_INLINE u128 operator-(u128 a, u128 b) noexcept {
#  if defined(DINERO_INT128_MSVC_X64)
    u128 r;
    unsigned char c = _subborrow_u64(0, a.lo, b.lo, &r.lo);
    _subborrow_u64(c, a.hi, b.hi, &r.hi);
    return r;
#  else
    u128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo ? 1 : 0);
    return r;
#  endif
}

DINERO_INT128_INLINE u128 operator-(u128 a, uint64_t b) noexcept {
    return a - u128(b);
}

DINERO_INT128_INLINE u128& operator-=(u128& a, u128 b) noexcept {
    a = a - b;
    return a;
}
DINERO_INT128_INLINE u128& operator-=(u128& a, uint64_t b) noexcept {
    a = a - b;
    return a;
}

// 64x64 -> 128 multiplication, when both operands are uint64_t. Native code
// like `(u128)a * b` becomes ambiguous on the struct backend (no implicit
// conversion from u128 back to uint64_t), so call-sites use the explicit
// helper or the operator below which takes one already-promoted operand.
DINERO_INT128_INLINE u128 operator*(u128 a, uint64_t b) noexcept {
#  if defined(DINERO_INT128_MSVC_X64)
    u128 r;
    r.lo = _umul128(a.lo, b, &r.hi);
    r.hi += a.hi * b;
    return r;
#  else
    uint64_t hi_partial;
    uint64_t lo_partial = detail::umul128(a.lo, b, &hi_partial);
    u128 r;
    r.lo = lo_partial;
    r.hi = hi_partial + a.hi * b;
    return r;
#  endif
}
DINERO_INT128_INLINE u128 operator*(uint64_t a, u128 b) noexcept { return b * a; }

// ---------- u128 shifts ----------

DINERO_INT128_INLINE u128 operator>>(u128 a, unsigned n) noexcept {
    if (n == 0) return a;
    if (n >= 128) return u128{};
    if (n >= 64) {
        return u128{0, a.hi >> (n - 64)};
    }
#  if defined(DINERO_INT128_MSVC_X64)
    u128 r;
    r.lo = __shiftright128(a.lo, a.hi, static_cast<unsigned char>(n));
    r.hi = a.hi >> n;
    return r;
#  else
    return u128{a.hi >> n, (a.hi << (64 - n)) | (a.lo >> n)};
#  endif
}

DINERO_INT128_INLINE u128 operator<<(u128 a, unsigned n) noexcept {
    if (n == 0) return a;
    if (n >= 128) return u128{};
    if (n >= 64) {
        return u128{a.lo << (n - 64), 0};
    }
#  if defined(DINERO_INT128_MSVC_X64)
    u128 r;
    r.lo = a.lo << n;
    r.hi = __shiftleft128(a.lo, a.hi, static_cast<unsigned char>(n));
    return r;
#  else
    return u128{(a.hi << n) | (a.lo >> (64 - n)), a.lo << n};
#  endif
}

DINERO_INT128_INLINE u128& operator>>=(u128& a, unsigned n) noexcept {
    a = a >> n;
    return a;
}
DINERO_INT128_INLINE u128& operator<<=(u128& a, unsigned n) noexcept {
    a = a << n;
    return a;
}

// ---------- u128 bitwise ----------

DINERO_INT128_INLINE u128 operator&(u128 a, u128 b) noexcept {
    return u128{a.hi & b.hi, a.lo & b.lo};
}
DINERO_INT128_INLINE u128 operator&(u128 a, uint64_t b) noexcept {
    return u128{0, a.lo & b};
}
DINERO_INT128_INLINE u128 operator|(u128 a, u128 b) noexcept {
    return u128{a.hi | b.hi, a.lo | b.lo};
}
DINERO_INT128_INLINE u128 operator^(u128 a, u128 b) noexcept {
    return u128{a.hi ^ b.hi, a.lo ^ b.lo};
}

// ---------- u128 comparisons ----------

DINERO_INT128_INLINE bool operator==(u128 a, u128 b) noexcept {
    return a.hi == b.hi && a.lo == b.lo;
}
DINERO_INT128_INLINE bool operator!=(u128 a, u128 b) noexcept { return !(a == b); }
DINERO_INT128_INLINE bool operator<(u128 a, u128 b) noexcept {
    return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
}
DINERO_INT128_INLINE bool operator>(u128 a, u128 b) noexcept { return b < a; }
DINERO_INT128_INLINE bool operator<=(u128 a, u128 b) noexcept { return !(a > b); }
DINERO_INT128_INLINE bool operator>=(u128 a, u128 b) noexcept { return !(a < b); }

// ---------- i128 helpers ----------

DINERO_INT128_INLINE constexpr int64_t lo64(i128 x) noexcept {
    return static_cast<int64_t>(x.lo);
}
DINERO_INT128_INLINE constexpr int64_t hi64(i128 x) noexcept { return x.hi; }
DINERO_INT128_INLINE constexpr i128 make_i128(int64_t hi, uint64_t lo) noexcept {
    return i128{hi, lo};
}

// ---------- i128 arithmetic (signed) ----------

DINERO_INT128_INLINE i128 operator+(i128 a, i128 b) noexcept {
    uint64_t r_lo = a.lo + b.lo;
    int64_t  r_hi = a.hi + b.hi + (r_lo < a.lo ? 1 : 0);
    return i128{r_hi, r_lo};
}
DINERO_INT128_INLINE i128 operator-(i128 a, i128 b) noexcept {
    uint64_t r_lo = a.lo - b.lo;
    int64_t  r_hi = a.hi - b.hi - (a.lo < b.lo ? 1 : 0);
    return i128{r_hi, r_lo};
}
DINERO_INT128_INLINE i128 operator-(i128 a) noexcept {
    // Two's-complement negation: ~a + 1.
    uint64_t r_lo = ~a.lo + 1;
    int64_t  r_hi = ~a.hi + (r_lo == 0 ? 1 : 0);
    return i128{r_hi, r_lo};
}
DINERO_INT128_INLINE i128& operator+=(i128& a, i128 b) noexcept { a = a + b; return a; }
DINERO_INT128_INLINE i128& operator-=(i128& a, i128 b) noexcept { a = a - b; return a; }

// Signed 64x64 -> 128 multiplication.
DINERO_INT128_INLINE i128 mul_i64(int64_t a, int64_t b) noexcept {
    int64_t hi_part;
    uint64_t lo_part = static_cast<uint64_t>(detail::mul128(a, b, &hi_part));
    return i128{hi_part, lo_part};
}

// ---------- i128 shifts (arithmetic right shift) ----------

DINERO_INT128_INLINE i128 operator>>(i128 a, unsigned n) noexcept {
    if (n == 0) return a;
    if (n >= 64) {
        // Arithmetic shift propagates sign bit through hi, then into lo.
        int64_t sign_extended = a.hi >> 63;  // 0 or -1
        if (n >= 128) return i128{sign_extended, static_cast<uint64_t>(sign_extended)};
        return i128{sign_extended, static_cast<uint64_t>(a.hi >> (n - 64))};
    }
    uint64_t r_lo = (static_cast<uint64_t>(a.hi) << (64 - n)) | (a.lo >> n);
    int64_t  r_hi = a.hi >> n;  // arithmetic shift on signed type
    return i128{r_hi, r_lo};
}

// ---------- i128 comparisons ----------

DINERO_INT128_INLINE bool operator==(i128 a, i128 b) noexcept {
    return a.hi == b.hi && a.lo == b.lo;
}
DINERO_INT128_INLINE bool operator!=(i128 a, i128 b) noexcept { return !(a == b); }
DINERO_INT128_INLINE bool operator<(i128 a, i128 b) noexcept {
    return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
}
DINERO_INT128_INLINE bool operator>(i128 a, i128 b) noexcept { return b < a; }
DINERO_INT128_INLINE bool operator<=(i128 a, i128 b) noexcept { return !(a > b); }
DINERO_INT128_INLINE bool operator>=(i128 a, i128 b) noexcept { return !(a < b); }

#endif  // DINERO_INT128_NATIVE / struct backend

// Zero-extending conversion uint64_t -> i128. Mirrors the standard implicit
// `__int128(uint64_t)` conversion on native, which is non-sign-extending (the
// uint64_t source is non-negative, so the i128 result is too). Provided as a
// free helper to avoid an i128(uint64_t) constructor — that would create
// ambiguity with the i128(int64_t) ctor when call-sites pass an `int` literal.
//
// Use this anywhere the original code did `(__int128_t)uint64_t_value` and
// expected the high 64 bits of the result to be zero.
inline constexpr i128 i128_zext_u64(uint64_t v) noexcept {
    return make_i128(int64_t(0), v);
}

}  // namespace compat
}  // namespace dinero

#undef DINERO_INT128_INLINE
