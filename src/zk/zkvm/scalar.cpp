// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/scalar.h"
#include "zk/zkvm/secp256k1_msm_cpu.h"
#include "crypto/evp_secp256k1.h"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <cassert>
#include <algorithm>

namespace dinero {
namespace zk {
namespace zkvm {

namespace {

secp256k1_context* default_ctx() {
    return dinero::crypto::GetSecp256k1ContextSignVerify();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Scalar constructors
// ---------------------------------------------------------------------------

Scalar::Scalar(uint64_t v) {
    data_.fill(0);
    // Big-endian: value goes in the last 8 bytes
    for (int i = 0; i < 8; ++i) {
        data_[31 - i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

Scalar Scalar::random(secp256k1_context* ctx) {
    Scalar result;
    for (int attempts = 0; attempts < 100; ++attempts) {
        if (RAND_bytes(result.data_.data(), SIZE) != 1) continue;
        if (secp256k1_ec_seckey_verify(ctx, result.data_.data())) return result;
    }
    // Should never happen
    assert(false && "Failed to generate random scalar after 100 attempts");
    return Scalar::zero();
}

Scalar Scalar::from_hash(const uint8_t hash[32], secp256k1_context* ctx) {
    Scalar result;
    std::memcpy(result.data_.data(), hash, 32);
    // Reduce: re-hash until in valid scalar range [1, n-1]
    // (same approach as CLSAG tagged_hash)
    while (!secp256k1_ec_seckey_verify(ctx, result.data_.data())) {
        uint8_t tmp[32];
        ::SHA256(result.data_.data(), 32, tmp);
        std::memcpy(result.data_.data(), tmp, 32);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

Scalar Scalar::operator+(const Scalar& other) const {
    // secp256k1_ec_seckey_tweak_add rejects zero keys — handle explicitly
    if (is_zero()) return other;
    if (other.is_zero()) return *this;

    Scalar result;
    std::memcpy(result.data_.data(), data_.data(), SIZE);
    // secp256k1_ec_seckey_tweak_add: key = key + tweak mod n
    // Returns 0 if result is zero mod n (which is valid for field arithmetic)
    int ok = secp256k1_ec_seckey_tweak_add(default_ctx(), result.data_.data(), other.data_.data());
    if (!ok) {
        result.data_.fill(0);
    }
    return result;
}

Scalar Scalar::operator-(const Scalar& other) const {
    return *this + (-other);
}

Scalar Scalar::operator*(const Scalar& other) const {
    // Handle zero specially — secp256k1_ec_seckey_tweak_mul rejects zero
    if (is_zero() || other.is_zero()) return Scalar::zero();

    Scalar result;
    std::memcpy(result.data_.data(), data_.data(), SIZE);
    int ok = secp256k1_ec_seckey_tweak_mul(default_ctx(), result.data_.data(), other.data_.data());
    if (!ok) {
        result.data_.fill(0);
    }
    return result;
}

Scalar Scalar::operator-() const {
    if (is_zero()) return Scalar::zero();
    Scalar result;
    std::memcpy(result.data_.data(), data_.data(), SIZE);
    secp256k1_ec_seckey_negate(default_ctx(), result.data_.data());
    return result;
}

Scalar& Scalar::operator+=(const Scalar& other) {
    *this = *this + other;
    return *this;
}

Scalar& Scalar::operator-=(const Scalar& other) {
    *this = *this - other;
    return *this;
}

Scalar& Scalar::operator*=(const Scalar& other) {
    *this = *this * other;
    return *this;
}

Scalar Scalar::inverse(secp256k1_context* ctx) const {
    if (is_zero()) return Scalar::zero(); // Undefined, but don't crash

    // Fermat's little theorem: a^{-1} = a^{n-2} mod n
    // n-2 = FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD036413F
    //
    // We compute this via binary exponentiation using secp256k1's scalar
    // multiplication. The exponent n-2 is 256 bits.
    //
    // n = FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
    // n-2 in big-endian bytes:
    static const uint8_t N_MINUS_2[32] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
        0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48, 0xA0, 0x3B,
        0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x3F
    };

    // Binary exponentiation: result = a^(n-2) mod n
    Scalar result = Scalar::one();
    Scalar base = *this;

    for (int i = 255; i >= 0; --i) {
        // Square
        result = result * result;
        // Multiply if bit is set
        int byte_idx = (255 - i) / 8;
        int bit_idx = 7 - ((255 - i) % 8);
        if ((N_MINUS_2[byte_idx] >> bit_idx) & 1) {
            result = result * base;
        }
    }

    return result;
}

bool Scalar::is_zero() const {
    for (auto b : data_) {
        if (b != 0) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Point operations
// ---------------------------------------------------------------------------

Point Point::generator(secp256k1_context* ctx) {
    // G = 1 * G (create public key from secret key = 1)
    return from_scalar(Scalar::one(), ctx);
}

Point Point::from_scalar(const Scalar& s, secp256k1_context* ctx) {
    // 0 * G = identity (point at infinity)
    if (s.is_zero()) return Point::identity();

    Point result;
    if (secp256k1_ec_pubkey_create(ctx, &result.pk_, s.data())) {
        result.is_identity_ = false;
        return result;
    }
    // secp256k1_ec_pubkey_create returns 0 only for zero scalar,
    // which we handled above. Treat any other failure as identity.
    return Point::identity();
}

bool Point::parse(const uint8_t* data, size_t len, Point& out, secp256k1_context* ctx) {
    // 33 zero bytes -> identity element
    if (len == COMPRESSED_SIZE) {
        bool all_zero = true;
        for (size_t i = 0; i < COMPRESSED_SIZE; ++i) {
            if (data[i] != 0) { all_zero = false; break; }
        }
        if (all_zero) {
            out = Point::identity();
            return true;
        }
    }
    if (secp256k1_ec_pubkey_parse(ctx, &out.pk_, data, len) == 1) {
        out.is_identity_ = false;
        return true;
    }
    return false;
}

bool Point::serialize(Compressed& out, secp256k1_context* ctx) const {
    if (is_identity_) {
        out.fill(0);
        return true;
    }
    size_t len = COMPRESSED_SIZE;
    return secp256k1_ec_pubkey_serialize(ctx, out.data(), &len, &pk_,
                                          SECP256K1_EC_COMPRESSED) == 1;
}

Point Point::operator+(const Point& other) const {
    // identity + P = P, P + identity = P, identity + identity = identity
    if (is_identity_) return other;
    if (other.is_identity_) return *this;

    Point result;
    const secp256k1_pubkey* pts[2] = {&pk_, &other.pk_};
    if (secp256k1_ec_pubkey_combine(default_ctx(), &result.pk_, pts, 2) != 1) {
        // P + (-P) = identity
        return Point::identity();
    }
    result.is_identity_ = false;
    return result;
}

Point Point::operator-(const Point& other) const {
    // P - identity = P, identity - P = -P, identity - identity = identity
    if (other.is_identity_) return *this;
    if (is_identity_) {
        // -P: negate the other point
        secp256k1_pubkey neg = other.pk_;
        secp256k1_ec_pubkey_negate(default_ctx(), &neg);
        return Point(neg);
    }

    secp256k1_pubkey neg = other.pk_;
    secp256k1_ec_pubkey_negate(default_ctx(), &neg);
    Point neg_pt(neg);
    return *this + neg_pt;
}

Point Point::operator*(const Scalar& s) const {
    // 0 * P = identity, s * identity = identity
    if (s.is_zero() || is_identity_) return Point::identity();

    Point result;
    result.pk_ = pk_;
    if (secp256k1_ec_pubkey_tweak_mul(default_ctx(), &result.pk_, s.data()) != 1) {
        return Point::identity();
    }
    result.is_identity_ = false;
    return result;
}

Point Point::multi_scalar_mul(
    const std::vector<Scalar>& scalars,
    const std::vector<Point>& points,
    secp256k1_context* ctx
) {
    assert(scalars.size() == points.size());
    assert(!scalars.empty());

    // Filter out zero scalars and identity points — both contribute identity,
    // which secp256k1's public API cannot represent.
    std::vector<Scalar> active_scalars;
    std::vector<Point>  active_points;
    active_scalars.reserve(scalars.size());
    active_points.reserve(points.size());

    for (size_t i = 0; i < scalars.size(); ++i) {
        if (scalars[i].is_zero() || points[i].is_identity()) continue;
        active_scalars.push_back(scalars[i]);
        active_points.push_back(points[i]);
    }

    if (active_scalars.empty()) return Point::identity();

    // For large inputs use parallel chunked Pippenger (each chunk fits in L3).
    if (active_scalars.size() >= PARALLEL_MSM_THRESHOLD) {
        return msm_pippenger_cpu_parallel(active_scalars, active_points, ctx);
    }

    // For medium inputs use single-threaded Pippenger.
    if (active_scalars.size() >= PIPPENGER_CPU_THRESHOLD) {
        return msm_pippenger_cpu(active_scalars, active_points, ctx);
    }

    // Fallback: sequential scalar mults (optimal for small n).
    Point result = Point::identity();
    for (size_t i = 0; i < active_scalars.size(); ++i) {
        Point term = active_points[i] * active_scalars[i];
        if (term.is_identity()) continue;
        result = result + term;
    }
    return result;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
