#pragma once
#include "consensus/asert.h"
#include <cstdint>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace dinero {

/**
 * Backwards-compatible wrapper around the explicit ASERT input contract.
 */
inline uint32_t CalculateASERT(
    uint32_t currentHeight,
    uint32_t anchorHeight,
    int64_t currentTime,
    int64_t anchorTime,
    uint32_t anchorBits,
    const AsertParams& params)
{
    return ComputeAsertBits(AsertInput{
        static_cast<int32_t>(currentHeight),
        currentTime,
        AsertAnchor{
            static_cast<int32_t>(anchorHeight),
            anchorTime,
            anchorBits,
        },
        params,
    });
}

} // namespace dinero
