#pragma once

#include "consensus/chainwork.h"
#include "consensus/block_timing.h"
#include "consensus/pow_compact.h"
#include "dinero/compat/int128.hpp"
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace dinero {

struct AsertAnchor {
    int32_t height{0};
    int64_t time{0};
    uint32_t bits{0};
};

struct AsertParams {
    int64_t target_spacing_secs{0};
    int64_t half_life_secs{0};
    arith_uint256 pow_limit{};
    uint32_t sixty_second_activation_height{UINT32_MAX};
};

struct AsertInput {
    int32_t target_height{0};
    int64_t reference_time{0};
    AsertAnchor anchor{};
    AsertParams params{};
};

struct ComputedAsertDebug {
    int32_t target_height{0};
    int64_t reference_time{0};
    int32_t anchor_height{0};
    int64_t anchor_time{0};
    uint32_t anchor_bits{0};
    arith_uint256 anchor_target{};
    arith_uint256 raw_target{};
    arith_uint256 clamped_target{};
    uint32_t result_bits{0};
};

inline std::string FormatComputedAsertDebug(const ComputedAsertDebug& dbg) {
    std::ostringstream oss;
    oss << "target_height=" << dbg.target_height
        << " reference_time=" << dbg.reference_time
        << " anchor_height=" << dbg.anchor_height
        << " anchor_time=" << dbg.anchor_time
        << " anchor_bits=0x" << std::hex << std::setw(8) << std::setfill('0') << dbg.anchor_bits
        << " result_bits=0x" << std::setw(8) << dbg.result_bits
        << std::dec
        << " anchor_target=" << dbg.anchor_target.GetHex()
        << " raw_target=" << dbg.raw_target.GetHex()
        << " clamped_target=" << dbg.clamped_target.GetHex();
    return oss.str();
}

namespace asert_detail {

inline uint64_t mul64_rshift(uint64_t a, uint64_t b, unsigned shift) {
    // (a * b) >> shift, taking the low 64 bits of the shifted product.
    // Single cross-platform implementation via the shim — replaces the
    // prior manual GCC/__uint128_t vs MSVC/_umul128 fork. The shim handles
    // shift values 0..127 correctly; the previous MSVC branch was UB for
    // shift == 64 (`lo >> 64` is undefined).
    return dinero::compat::lo64(
        dinero::compat::mul_u64(a, b) >> shift);
}

// Post-upgrade encoding is byte-normalized and rounds down. GetCompact's
// historical bit-normalization remains untouched for replay below activation.
inline uint32_t EncodeTimingUpgradeTarget(const arith_uint256& target) {
    std::array<uint8_t, 32> bytes{};
    for (int i = 0; i < 32; ++i)
        bytes[31 - i] = static_cast<uint8_t>(target.GetWord(i / 8) >> (8 * (i % 8)));
    return BitsFromTargetBE(bytes);
}

// Keep the carry above bit 255 until after the fixed-point division. Throwing
// it away before >>16 can wrap a large valid target into arbitrarily hard work.
inline void MultiplyFixedPointSaturating(arith_uint256& target, uint64_t factor,
                                        const arith_uint256& limit) {
    uint64_t carry = 0;
    for (int i = 0; i < 4; ++i) {
        const auto product = dinero::compat::mul_u64(target.GetWord(i), factor) + carry;
        target.SetWord(i, dinero::compat::lo64(product));
        carry = dinero::compat::hi64(product);
    }
    if (carry >= 65536) {
        target = limit;
        return;
    }
    target >>= 16;
    target.SetWord(3, target.GetWord(3) | (carry << 48));
}

} // namespace asert_detail

inline uint32_t ComputeAsertBits(const AsertInput& in, ComputedAsertDebug* dbg_out = nullptr) {
    if (in.target_height == in.anchor.height) {
        if (dbg_out) {
            dbg_out->target_height = in.target_height;
            dbg_out->reference_time = in.reference_time;
            dbg_out->anchor_height = in.anchor.height;
            dbg_out->anchor_time = in.anchor.time;
            dbg_out->anchor_bits = in.anchor.bits;
            dbg_out->anchor_target.SetCompact(in.anchor.bits);
            dbg_out->raw_target = dbg_out->anchor_target;
            dbg_out->clamped_target = dbg_out->anchor_target;
            dbg_out->result_bits = in.anchor.bits;
        }
        return in.anchor.bits;
    }

    arith_uint256 target;
    target.SetCompact(in.anchor.bits);

    if (dbg_out) {
        dbg_out->target_height = in.target_height;
        dbg_out->reference_time = in.reference_time;
        dbg_out->anchor_height = in.anchor.height;
        dbg_out->anchor_time = in.anchor.time;
        dbg_out->anchor_bits = in.anchor.bits;
        dbg_out->anchor_target = target;
    }

    const int64_t time_delta = in.reference_time - in.anchor.time;
    const int64_t ideal_time = consensus::ExpectedBlockElapsed(
        in.anchor.height, in.target_height, in.params.target_spacing_secs,
        in.params.sixty_second_activation_height);
    const int64_t excess_time = time_delta - ideal_time;
    const bool timing_upgrade = in.target_height >= 0 && consensus::SixtySecondActive(
        static_cast<uint32_t>(in.target_height), in.params.sixty_second_activation_height);

    if (excess_time == 0 && !timing_upgrade) {
        if (dbg_out) {
            dbg_out->raw_target = target;
            dbg_out->clamped_target = target;
            dbg_out->result_bits = in.anchor.bits;
        }
        return in.anchor.bits;
    }

    if (in.params.half_life_secs > 0) {
        int64_t k = excess_time / in.params.half_life_secs;
        int64_t r = excess_time % in.params.half_life_secs;
        if (r < 0) {
            k -= 1;
            r += in.params.half_life_secs;
        }

        if (k > 32) k = 32;
        if (k < -32) k = -32;

        bool saturated = false;
        if (k > 0) {
            auto shift_limit = in.params.pow_limit;
            shift_limit >>= static_cast<unsigned int>(k);
            if (timing_upgrade && target > shift_limit) {
                // The fractional factor is >= 1, so this cap is final. Check
                // before shifting: a 256-bit overflow cannot be capped later.
                target = in.params.pow_limit;
                saturated = true;
            } else {
                target <<= static_cast<unsigned int>(k);
            }
        } else if (k < 0) {
            target >>= static_cast<unsigned int>(-k);
        }

        if (r != 0 && !saturated) {
            const uint64_t kRadix16 = 65536;
            const uint64_t kCoeff1 = 195766423245049ull;
            const uint64_t kCoeff2 = 971821376ull;
            const uint64_t kCoeff3 = 5127ull;
            const uint64_t kRounding = (1ull << 47);

            const uint64_t frac = static_cast<uint64_t>(
                (r * static_cast<int64_t>(kRadix16)) / in.params.half_life_secs
            );
            // These coefficients multiply the full Q16 integer powers. The
            // legacy extra >>16 on each power made the curve discontinuous
            // at whole half-lives; preserve it only for historical blocks.
            const uint64_t frac_squared = timing_upgrade
                ? frac * frac : asert_detail::mul64_rshift(frac, frac, 16);
            const uint64_t frac_cubed = timing_upgrade
                ? frac_squared * frac : asert_detail::mul64_rshift(frac_squared, frac, 16);
            const uint64_t polynomial_sum =
                (kCoeff1 * frac) +
                (kCoeff2 * frac_squared) +
                (kCoeff3 * frac_cubed) +
                kRounding;
            const uint64_t factor = kRadix16 + (polynomial_sum >> 48);

            if (timing_upgrade) {
                asert_detail::MultiplyFixedPointSaturating(target, factor, in.params.pow_limit);
            } else {
                target *= factor;
                target >>= 16;
            }
        }
    }

    if (dbg_out) {
        dbg_out->raw_target = target;
    }

    if (target > in.params.pow_limit) {
        target = in.params.pow_limit;
    }
    if (target.IsZero()) {
        target = arith_uint256::One();
    }

    const uint32_t result = timing_upgrade
        ? asert_detail::EncodeTimingUpgradeTarget(target) : target.GetCompact();

    if (dbg_out) {
        dbg_out->clamped_target = target;
        dbg_out->result_bits = result;
    }

    return result;
}

} // namespace dinero
