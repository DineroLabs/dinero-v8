// pow_daa_log.hpp
// Tiny DAA log formatter with ASERT trace fields
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <sstream>
#include <iomanip>

struct DAAStats {
    // ASERT internals (optional – fill if available)
    int64_t excessTime = 0;   // (prevMTP - anchorMTP) - blocksSinceAnchor * T
    int64_t kInteger   = 0;   // floor(excessTime / halfLifeSec)
    int64_t fracQ16    = 0;   // fractional exponent in Q16, optional
};

inline const char* PhaseName(bool isPhase1) { return isPhase1 ? "P1" : "P2-ASERT"; }

inline std::string FormatDAA(uint32_t height,
                             uint32_t nextBits,
                             uint32_t prevBits,
                             bool     isPhase1,
                             int64_t  prevMTP,
                             int64_t  prevPrevMTP,
                             int64_t  anchorMTP,
                             uint32_t powLimitBits,
                             const DAAStats* stats /*nullable*/)
{
    std::ostringstream oss;

    if (isPhase1) {
        oss << "[DAA] h=" << height << " phase=" << PhaseName(true)
            << " next=0x" << std::hex << std::setw(8) << std::setfill('0') << nextBits
            << " prev=0x" << std::hex << std::setw(8) << std::setfill('0') << prevBits
            << std::dec
            << " mtp=" << anchorMTP
            << " prevMTP=" << prevMTP
            << " prevPrevMTP=" << prevPrevMTP
            << " powLimit=0x" << std::hex << std::setw(8) << std::setfill('0') << powLimitBits;

        // Detect rescue block (min diff) vs normal fixed diff
        if (nextBits == powLimitBits) {
            oss << "\n[DAA]  phase1 RESCUE(min-diff) triggered (stall ≥ threshold)";
        }
        return oss.str();
    }

    oss << "[DAA] h=" << height << " phase=" << PhaseName(false)
        << " next=0x" << std::hex << std::setw(8) << std::setfill('0') << nextBits
        << " prev=0x" << std::hex << std::setw(8) << std::setfill('0') << prevBits
        << std::dec
        << " mtp(prev)=" << prevMTP
        << " mtp(anchor)=" << anchorMTP
        << " powLimit=0x" << std::hex << std::setw(8) << std::setfill('0') << powLimitBits;

    if (stats) {
        oss << std::dec
            << " excess=" << stats->excessTime
            << " k=" << stats->kInteger
            << " fracQ16=" << stats->fracQ16;
    }

    return oss.str();
}
