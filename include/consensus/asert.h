#pragma once

#include "consensus/chainwork.h"
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

    const int64_t height_delta =
        static_cast<int64_t>(in.target_height) - static_cast<int64_t>(in.anchor.height);
    const int64_t time_delta = in.reference_time - in.anchor.time;
    const int64_t ideal_time = height_delta * in.params.target_spacing_secs;
    const int64_t excess_time = time_delta - ideal_time;

    if (excess_time == 0) {
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

        if (k > 0) {
            target <<= static_cast<unsigned int>(k);
        } else if (k < 0) {
            target >>= static_cast<unsigned int>(-k);
        }

        if (r != 0) {
            const uint64_t kRadix16 = 65536;
            const uint64_t kCoeff1 = 195766423245049ull;
            const uint64_t kCoeff2 = 971821376ull;
            const uint64_t kCoeff3 = 5127ull;
            const uint64_t kRounding = (1ull << 47);

            const uint64_t frac = static_cast<uint64_t>(
                (r * static_cast<int64_t>(kRadix16)) / in.params.half_life_secs
            );
            const uint64_t frac_squared = asert_detail::mul64_rshift(frac, frac, 16);
            const uint64_t frac_cubed = asert_detail::mul64_rshift(frac_squared, frac, 16);
            const uint64_t polynomial_sum =
                (kCoeff1 * frac) +
                (kCoeff2 * frac_squared) +
                (kCoeff3 * frac_cubed) +
                kRounding;
            const uint64_t factor = kRadix16 + (polynomial_sum >> 48);

            target *= factor;
            target >>= 16;
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

    const uint32_t result = target.GetCompact();

    if (dbg_out) {
        dbg_out->clamped_target = target;
        dbg_out->result_bits = result;
    }

    return result;
}

} // namespace dinero
