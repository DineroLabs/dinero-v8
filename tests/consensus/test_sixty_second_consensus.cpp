#include "consensus/asert.h"
#include "consensus/subsidy.h"
#include "consensus/stateless_verification.h"
#include <cstdint>
#include <iostream>

namespace {
int failures = 0;
void Equal(uint64_t actual, uint64_t expected, const char* label) {
    if (actual != expected) {
        std::cerr << "FAIL " << label << ": " << actual << " != " << expected << '\n';
        ++failures;
    }
}
}

int main() {
    using dinero::ConsensusSubsidy;
    constexpr uint32_t activation = 1000;
    const auto reward = [](uint32_t h, uint32_t a) {
        const auto full = ConsensusSubsidy::GetBlockSubsidy(h, a).GetUna();
        Equal(dinero::consensus::GetBlockSubsidy(h, a), full, "full/stateless subsidy parity");
        return full;
    };
    Equal(reward(0, activation), 0, "genesis");
    Equal(reward(activation - 1, activation), 10'000'000'000ULL, "old initial reward");
    Equal(reward(activation, activation), 10'000'000'000ULL, "no cadence reward cut");
    Equal(reward(1'314'000, activation), 10'000'000'000ULL, "last initial reward");
    Equal(reward(1'314'001, activation), 5'000'000'000ULL, "unchanged first halving height");
    Equal(reward(9'198'000, activation), 156'250'000ULL, "last epoch six reward");
    Equal(reward(9'198'001, activation), 78'125'000ULL, "additional 0.78125 epoch");
    Equal(reward(10'512'000, activation), 78'125'000ULL, "last pre-tail reward");
    Equal(reward(10'512'001, activation), 50'000'000ULL, "new tail");
    Equal(reward(UINT32_MAX, activation), 50'000'000ULL, "large-height new tail");
    Equal(reward(UINT32_MAX, UINT32_MAX), 100'000'000ULL, "disabled sentinel never activates");
    Equal(reward(9'198'001, 9'198'002), 100'000'000ULL, "historical tail unchanged");
    Equal(reward(9'198'002, 9'198'002), 78'125'000ULL, "late activation in epoch seven");
    Equal(reward(10'512'001, 10'512'002), 100'000'000ULL, "historical late tail");
    Equal(reward(10'512'002, 10'512'002), 50'000'000ULL, "late tail activation");
    Equal(ConsensusSubsidy::GetPoWIssuedAtHeight(10'512'000, activation),
          26'177'343'750'000'000ULL, "independent pre-tail total");
    Equal(ConsensusSubsidy::GetPoWIssuedAtHeight(9'198'003, 9'198'003),
          26'074'687'778'125'000ULL, "cumulative history keeps two old tail blocks");
    Equal(ConsensusSubsidy::GetTotalIssuedAtHeight(10'512'001, activation),
          26'177'353'800'000'000ULL, "total includes fixed genesis burn and first tail");

    dinero::AsertInput input;
    input.anchor = {0, 1'000'000, 0x1c100000};
    input.params.target_spacing_secs = 120;
    input.params.half_life_secs = 43'200;
    input.params.pow_limit.SetCompact(0x1d31ffce);
    input.params.sixty_second_activation_height = activation;
    // Independent literal elapsed times: 999 old intervals, then 60 seconds
    // per new interval. Exact half-life offsets avoid a circular math oracle.
    for (const auto [height, elapsed] : {std::pair{999,119880}, {1000,119940},
                                       {1001,120000}, {1719,163080}}) {
        input.target_height = height;
        input.reference_time = input.anchor.time + elapsed;
        Equal(dinero::ComputeAsertBits(input), 0x1c100000, "piecewise ideal cadence");
        input.reference_time += 43'200;
        dinero::ComputedAsertDebug debug;
        // Preserve historical encoding below A. At/above A the compact target
        // must retain its byte magnitude and round down, including at the limit.
        Equal(dinero::ComputeAsertBits(input, &debug), height < activation ? 0x1d008000 : 0x1c200000, "height-selected slow compact encoding");
        dinero::arith_uint256 expected;
        expected.SetCompact(0x1c200000);
        Equal(debug.clamped_target == expected, true, "one half-life slow raw target");
        input.reference_time -= 86'400;
        Equal(dinero::ComputeAsertBits(input, &debug), height < activation ? 0x1d008000 : 0x1c080000, "height-selected fast compact encoding");
        expected.SetCompact(0x1c080000);
        Equal(debug.clamped_target == expected, true, "one half-life fast raw target");
    }
    input.target_height = 1719;
    input.reference_time = input.anchor.time + 206280;
    input.params.sixty_second_activation_height = UINT32_MAX;
    Equal(dinero::ComputeAsertBits(input), 0x1c100000, "disabled historical ASERT");
    input.params.sixty_second_activation_height = activation;
    Equal(dinero::ComputeAsertBits(input), 0x1c200000, "old cadence changes expected bits");
    input.anchor = {1005,1'000'000,0x1c100000};
    input.target_height = 1015;
    input.reference_time = 1'000'600;
    Equal(dinero::ComputeAsertBits(input), 0x1c100000, "anchor within new era");
    input.anchor = {0,1'000'000,0x1c100000};
    input.params.sixty_second_activation_height = 1;
    input.target_height = 1;
    input.reference_time = 1'000'060;
    Equal(dinero::ComputeAsertBits(input), 0x1c100000, "activation at first block");
    // Encoding must not undo the cap. Include mantissas that would overflow
    // a 256-bit shift before the old code reached its clamp.
    input.target_height = 1000;
    input.params.sixty_second_activation_height = 1000;
    for (auto anchor_bits : {0x1d31ffceu, 0x1d200000u, 0x207fffffu}) {
        input.anchor.bits = anchor_bits;
        input.params.pow_limit.SetCompact(anchor_bits);
        for (int64_t excess : {-1'500'001, -86'400, -43'200, -1, 0, 1, 43'200, 1'500'001}) {
            input.reference_time = input.anchor.time + 119940 + excess;
            dinero::ComputedAsertDebug debug;
            dinero::arith_uint256 decoded;
            decoded.SetCompact(dinero::ComputeAsertBits(input, &debug));
            Equal(decoded.IsZero(), false, "nonzero encoded target");
            Equal(decoded <= input.params.pow_limit, true, "encoded target respects PoW limit");
            Equal(decoded <= debug.clamped_target, true, "encoding never increases target");
            if (excess >= 0)
                Equal(decoded == input.params.pow_limit, true, "saturation cannot wrap into harder work");
        }
    }
    if (failures) return 1;
    std::cout << "PASS sixty-second monetary and ASERT boundary vectors\n";
}
