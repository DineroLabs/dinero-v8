#include "consensus/chainwork.h"
#include "consensus/target_helpers.h"
#include "dinero/compat/int128.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {

// === arith_uint256 Implementation ===

void arith_uint256::SetCompact(uint32_t compact) {
    // Convert compact difficulty to 256-bit target
    auto target_array = dinero::TargetFromBits(compact);
    
    // Convert 32-byte big-endian array to 4 uint64_t little-endian
    SetWord(0, 0); SetWord(1, 0); SetWord(2, 0); SetWord(3, 0);
    
    for (int i = 0; i < 32; ++i) {
        int word_idx = i / 8;
        int byte_idx = i % 8;
        SetWord(word_idx, GetWord(word_idx) | (uint64_t(target_array[31-i]) << (byte_idx * 8)));
    }
}

uint32_t arith_uint256::GetCompact() const {
    // Convert 256-bit target back to compact difficulty format
    
    // Find the most significant non-zero word
    int msw = -1;
    for (int i = 3; i >= 0; --i) {
        if (GetWord(i) != 0) {
            msw = i;
            break;
        }
    }
    
    if (msw == -1) {
        return 0; // Zero target
    }
    
    // Simple implementation: just use the exponent from word position
    // and the top 24 bits as mantissa
    uint64_t msword = GetWord(msw);
    
    // Find leading bit position
    int leading_bit = 63;
    while (leading_bit >= 0 && ((msword >> leading_bit) & 1) == 0) {
        leading_bit--;
    }
    
    if (leading_bit < 0) {
        return 0;
    }
    
    // Calculate total bit position
    int total_bits = msw * 64 + leading_bit + 1;
    
    // Convert to byte-based exponent (round up)
    int exponent = (total_bits + 7) / 8;
    
    // Extract mantissa (top 24 bits)
    uint32_t mantissa;
    if (leading_bit >= 23) {
        mantissa = (msword >> (leading_bit - 23)) & 0x00FFFFFF;
    } else {
        mantissa = (msword << (23 - leading_bit)) & 0x00FFFFFF;
        if (msw > 0 && leading_bit < 23) {
            // Add bits from next word if needed
            uint64_t lower = GetWord(msw - 1);
            mantissa |= (lower >> (64 - (23 - leading_bit))) & ((1 << (23 - leading_bit)) - 1);
        }
    }
    
    // Bitcoin compact format: bit 23 of mantissa is the sign bit.
    // For positive targets, if bit 23 is set, shift right and bump exponent.
    if (mantissa & 0x00800000u) {
        mantissa >>= 8;
        exponent++;
    }

    // If mantissa is too small, normalize upward
    if (mantissa != 0 && mantissa < 0x8000) {
        mantissa <<= 8;
        exponent--;
    }

    return (exponent << 24) | (mantissa & 0x007FFFFF);
}

arith_uint256 arith_uint256::operator+(const arith_uint256& other) const {
    arith_uint256 result;
    uint64_t carry = 0;
    
    for (int i = 0; i < 4; ++i) {
        uint64_t sum = GetWord(i) + other.GetWord(i) + carry;
        result.SetWord(i, sum);
        carry = (sum < GetWord(i) || (carry > 0 && sum == GetWord(i))) ? 1 : 0;
    }
    
    return result;
}

arith_uint256& arith_uint256::operator+=(const arith_uint256& other) {
    *this = *this + other;
    return *this;
}

arith_uint256& arith_uint256::operator*=(uint64_t multiplier) {
    using dinero::compat::u128;
    using dinero::compat::lo64;
    using dinero::compat::hi64;
    using dinero::compat::mul_u64;

    uint64_t carry = 0;
    for (int i = 0; i < 4; ++i) {
        u128 p = mul_u64(GetWord(i), multiplier) + carry;
        SetWord(i, lo64(p));
        carry = hi64(p);
    }
    return *this;
}

arith_uint256& arith_uint256::operator<<=(unsigned int shift) {
    if (shift >= 256) {
        *this = Zero();
        return *this;
    }
    
    while (shift >= 64) {
        // Shift by 64 bits (one word)
        SetWord(3, GetWord(2));
        SetWord(2, GetWord(1));
        SetWord(1, GetWord(0));
        SetWord(0, 0);
        shift -= 64;
    }
    
    if (shift > 0) {
        uint64_t carry = 0;
        for (int i = 0; i < 4; ++i) {
            uint64_t word = GetWord(i);
            SetWord(i, (word << shift) | carry);
            carry = word >> (64 - shift);
        }
    }
    
    return *this;
}

arith_uint256& arith_uint256::operator>>=(unsigned int shift) {
    if (shift >= 256) {
        *this = Zero();
        return *this;
    }
    
    while (shift >= 64) {
        // Shift by 64 bits (one word)
        SetWord(0, GetWord(1));
        SetWord(1, GetWord(2));
        SetWord(2, GetWord(3));
        SetWord(3, 0);
        shift -= 64;
    }
    
    if (shift > 0) {
        uint64_t carry = 0;
        for (int i = 3; i >= 0; --i) {
            uint64_t word = GetWord(i);
            SetWord(i, (word >> shift) | carry);
            carry = word << (64 - shift);
        }
    }
    
    return *this;
}

arith_uint256 arith_uint256::operator/(const arith_uint256& other) const {
    if (other.IsZero()) {
        return arith_uint256::Max(); // Division by zero returns max value
    }
    
    return DivideBy(other);
}

bool arith_uint256::operator==(const arith_uint256& other) const {
    return GetWord(0) == other.GetWord(0) && GetWord(1) == other.GetWord(1) && 
           GetWord(2) == other.GetWord(2) && GetWord(3) == other.GetWord(3);
}

bool arith_uint256::operator<(const arith_uint256& other) const {
    return IsLessThan(other);
}

bool arith_uint256::operator>(const arith_uint256& other) const {
    return other.IsLessThan(*this);
}

std::string arith_uint256::GetHex() const {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    
    // Output in big-endian order (most significant first)
    for (int i = 3; i >= 0; --i) {
        ss << std::setw(16) << GetWord(i);
    }
    
    return ss.str();
}

bool arith_uint256::IsZero() const {
    return GetWord(0) == 0 && GetWord(1) == 0 && GetWord(2) == 0 && GetWord(3) == 0;
}

arith_uint256 arith_uint256::Max() {
    arith_uint256 result;
    result.SetWord(0, ~uint64_t(0));
    result.SetWord(1, ~uint64_t(0));
    result.SetWord(2, ~uint64_t(0));
    result.SetWord(3, ~uint64_t(0));
    return result;
}

bool arith_uint256::IsLessThan(const arith_uint256& other) const {
    for (int i = 3; i >= 0; --i) {
        if (GetWord(i) < other.GetWord(i)) return true;
        if (GetWord(i) > other.GetWord(i)) return false;
    }
    return false; // Equal
}

arith_uint256 arith_uint256::DivideBy(const arith_uint256& divisor) const {
    if (divisor.IsZero()) return Max();
    if (IsZero()) return Zero();
    if (*this < divisor) return Zero();
    if (*this == divisor) return One();

    auto get_bit = [](const arith_uint256& value, int bit_index) -> bool {
        const int word = bit_index / 64;
        const int bit = bit_index % 64;
        return ((value.GetWord(word) >> bit) & uint64_t{1}) != 0;
    };

    auto set_bit = [](arith_uint256& value, int bit_index) {
        const int word = bit_index / 64;
        const int bit = bit_index % 64;
        value.SetWord(word, value.GetWord(word) | (uint64_t{1} << bit));
    };

    auto subtract_in_place = [](arith_uint256& lhs, const arith_uint256& rhs) {
        using dinero::compat::u128;
        using dinero::compat::lo64;
        uint64_t borrow = 0;
        for (int i = 0; i < 4; ++i) {
            const u128 minuend(lhs.GetWord(i));
            const u128 subtrahend = u128(rhs.GetWord(i)) + borrow;
            lhs.SetWord(i, lo64(minuend - subtrahend));
            borrow = (minuend < subtrahend) ? 1 : 0;
        }
    };

    arith_uint256 quotient = Zero();
    arith_uint256 remainder = Zero();

    for (int bit = 255; bit >= 0; --bit) {
        remainder <<= 1;
        if (get_bit(*this, bit)) {
            remainder.SetWord(0, remainder.GetWord(0) | uint64_t{1});
        }

        if (!(remainder < divisor)) {
            subtract_in_place(remainder, divisor);
            set_bit(quotient, bit);
        }
    }

    return quotient;
}

// === Global Functions ===

arith_uint256 GetBlockProof(uint32_t nBits) {
    arith_uint256 target;
    target.SetCompact(nBits);
    
    if (target.IsZero()) {
        return arith_uint256::Zero();
    }
    
    // Work = (2^256 - 1) / (target + 1) + 1
    arith_uint256 max_target = arith_uint256::Max();
    arith_uint256 target_plus_one = target + arith_uint256::One();
    
    return (max_target / target_plus_one) + arith_uint256::One();
}

std::string ChainworkToHex(const arith_uint256& chainwork) {
    return chainwork.GetHex();
}

arith_uint256 ChainworkFromHex(const std::string& hex) {
    arith_uint256 result;
    
    if (hex.length() != 64) {
        return result; // Return zero for invalid length
    }

    // A 64-char string that isn't all hex digits makes std::stoull throw
    // std::invalid_argument. Uncaught, that aborts LoadSnapshot's genesis→base
    // materialization (EnsureHeaderBranchIndexed → AddBlockIndex → UpdateChainwork)
    // before StartBackgroundValidation() runs, deadlocking a fresh snapshot node
    // (tip held at base, background validation never starts). Treat a malformed
    // chainwork string as zero work; EnsureHeaderBranchIndexed overwrites the
    // block index's chainwork with the authoritative header-index value right
    // after AddBlockIndex returns, so the zero fallback here is never persisted.
    for (char c : hex) {
        const bool is_hex = (c >= '0' && c <= '9') ||
                            (c >= 'a' && c <= 'f') ||
                            (c >= 'A' && c <= 'F');
        if (!is_hex) {
            return result; // Return zero for invalid hex
        }
    }

    // Parse hex string into 4 uint64_t values (big-endian input)
    for (int i = 0; i < 4; ++i) {
        std::string word_hex = hex.substr(i * 16, 16);
        uint64_t word_value = std::stoull(word_hex, nullptr, 16);
        result.SetWord(3-i, word_value); // Convert to little-endian storage
    }
    
    return result;
}

int CompareChainwork(const std::string& work_a, const std::string& work_b) {
    if (work_a.length() != 64 || work_b.length() != 64) {
        return 0; // Invalid comparison
    }
    
    arith_uint256 a = ChainworkFromHex(work_a);
    arith_uint256 b = ChainworkFromHex(work_b);
    
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

} // namespace dinero
