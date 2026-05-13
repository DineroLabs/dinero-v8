// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Scalar field arithmetic on secp256k1's group order.
 *
 * Thin wrapper over secp256k1's public API for scalar operations.
 * All operations are modulo the group order n:
 *   n = FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
 *
 * Used throughout the ZKVM for R1CS constraint systems, IPA proofs,
 * and Nova folding — the entire ZK Tapscript proving stack.
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include <secp256k1.h>

namespace dinero {
namespace zk {
namespace zkvm {

class Scalar {
public:
    static constexpr size_t SIZE = 32;
    using Bytes = std::array<uint8_t, SIZE>;

    // Constructors
    Scalar() { data_.fill(0); }
    explicit Scalar(const Bytes& bytes) : data_(bytes) {}
    explicit Scalar(const uint8_t* bytes) { std::memcpy(data_.data(), bytes, SIZE); }
    explicit Scalar(uint64_t v);

    // Named constructors
    static Scalar zero() { return Scalar(); }
    static Scalar one() { return Scalar(uint64_t(1)); }
    static Scalar random(secp256k1_context* ctx);
    static Scalar from_hash(const uint8_t hash[32], secp256k1_context* ctx);

    // Arithmetic (all mod group order n)
    Scalar operator+(const Scalar& other) const;
    Scalar operator-(const Scalar& other) const;
    Scalar operator*(const Scalar& other) const;
    Scalar operator-() const;

    Scalar& operator+=(const Scalar& other);
    Scalar& operator-=(const Scalar& other);
    Scalar& operator*=(const Scalar& other);

    // Multiplicative inverse via Fermat's little theorem: a^{n-2} mod n
    Scalar inverse(secp256k1_context* ctx) const;

    // Comparison
    bool operator==(const Scalar& other) const { return data_ == other.data_; }
    bool operator!=(const Scalar& other) const { return data_ != other.data_; }
    bool is_zero() const;

    // Serialization (big-endian, as secp256k1 expects)
    const Bytes& bytes() const { return data_; }
    const uint8_t* data() const { return data_.data(); }
    uint8_t* data() { return data_.data(); }

private:
    Bytes data_; // Big-endian 256-bit scalar
};

/**
 * Compressed elliptic curve point on secp256k1.
 * Used for Pedersen commitments in IPA and vector commitments.
 */
class Point {
public:
    static constexpr size_t COMPRESSED_SIZE = 33;
    using Compressed = std::array<uint8_t, COMPRESSED_SIZE>;

    // Default constructor creates the identity element (point at infinity).
    // secp256k1's public API cannot represent identity, so we track it
    // with an explicit flag and short-circuit all arithmetic accordingly.
    Point() : is_identity_(true) { std::memset(&pk_, 0, sizeof(pk_)); }
    explicit Point(const secp256k1_pubkey& pk) : pk_(pk), is_identity_(false) {}

    // Named constructor for identity (alias for default)
    static Point identity() { return Point(); }

    // Check if this point is the identity element (point at infinity)
    bool is_identity() const { return is_identity_; }

    // Generator point G (base point of secp256k1)
    static Point generator(secp256k1_context* ctx);

    // Create point from scalar: P = s * G
    // Returns identity when s is zero.
    static Point from_scalar(const Scalar& s, secp256k1_context* ctx);

    // Parse from compressed bytes.
    // 33 zero bytes are parsed as the identity element.
    static bool parse(const uint8_t* data, size_t len, Point& out, secp256k1_context* ctx);

    // Serialize to compressed bytes.
    // Identity serializes as 33 zero bytes.
    bool serialize(Compressed& out, secp256k1_context* ctx) const;

    // Point arithmetic — identity-safe.
    //   identity + P = P,  P + identity = P
    //   identity - P = -P, P - identity = P
    Point operator+(const Point& other) const;
    Point operator-(const Point& other) const;

    // Scalar multiplication: result = scalar * this
    //   0 * P = identity,  s * identity = identity
    Point operator*(const Scalar& s) const;

    // Multi-scalar multiplication: sum(scalars[i] * points[i])
    // Skips identity points and zero scalars.
    static Point multi_scalar_mul(
        const std::vector<Scalar>& scalars,
        const std::vector<Point>& points,
        secp256k1_context* ctx
    );

    const secp256k1_pubkey& raw() const { return pk_; }
    secp256k1_pubkey& raw() { return pk_; }

    // Equality comparison (compares identity flag and raw pubkey bytes)
    bool operator==(const Point& other) const {
        if (is_identity_ != other.is_identity_) return false;
        if (is_identity_) return true; // Both identity
        return std::memcmp(&pk_, &other.pk_, sizeof(pk_)) == 0;
    }
    bool operator!=(const Point& other) const { return !(*this == other); }

private:
    secp256k1_pubkey pk_;
    bool is_identity_ = false;
};

// Convenience: scalar * point (commutative notation)
inline Point operator*(const Scalar& s, const Point& p) { return p * s; }

} // namespace zkvm
} // namespace zk
} // namespace dinero
