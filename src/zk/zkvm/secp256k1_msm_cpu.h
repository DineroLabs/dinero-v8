// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * CPU Pippenger Multi-Scalar Multiplication
 *
 * Replaces the naive sequential loop in Point::multi_scalar_mul() with
 * Pippenger's bucket algorithm (c=8 window).
 *
 * Complexity:
 *   Naive:     n × ~256 EC ops = O(256n)
 *   Pippenger: n (bucket accumulation) + 255×32 (bucket sums) + 248 (doublings)
 *              = O(n + 8408) for c=8, 32 windows
 *
 * Practical speedup vs naive sequential loop:
 *   n=64:   ~3-5x
 *   n=256:  ~8-12x
 *   n=1024: ~15-25x
 *
 * Uses only the public secp256k1 API — no internal header access.
 * The secp256k1 library handles P+P (doubling) correctly via pubkey_combine.
 *
 * Minimum n to use Pippenger: PIPPENGER_CPU_THRESHOLD (= 64).
 * Below that the setup overhead outweighs the savings.
 */

#include "zk/zkvm/scalar.h"
#include <vector>
#include <secp256k1.h>

namespace dinero {
namespace zk {
namespace zkvm {

// Minimum number of points for Pippenger to win over sequential scalar mults.
static constexpr size_t PIPPENGER_CPU_THRESHOLD = 64;

/**
 * Pippenger multi-scalar multiplication: sum(scalars[i] * points[i]).
 *
 * Preconditions (caller must ensure):
 *   - scalars.size() == points.size()
 *   - All zero scalars and identity points are already filtered out
 *   - scalars.size() >= PIPPENGER_CPU_THRESHOLD
 *
 * @param scalars  Non-zero scalars (big-endian 32-byte)
 * @param points   Non-identity points
 * @param ctx      secp256k1 context (sign+verify capable)
 * @return         Sum of scalar[i] * point[i], or identity if result is infinity
 */
Point msm_pippenger_cpu(
    const std::vector<Scalar>& scalars,
    const std::vector<Point>& points,
    secp256k1_context* ctx
);

/**
 * Parallel chunked MSM: same semantics as msm_pippenger_cpu but splits the
 * work across hardware threads.  Each chunk is processed by secp256k1's
 * ecmult_multi_var (Pippenger) in a separate std::thread; partial sums are
 * combined at the end with EC point addition.
 *
 * Use when n >= PARALLEL_MSM_THRESHOLD.  Chunk size = 8K points
 * (empirically optimal on Apple M-series: plateaus at 8K–4K, 24% walltime
 * improvement vs single-threaded Pippenger for the 1M-point HMB proof).
 */
static constexpr size_t PARALLEL_MSM_THRESHOLD = 4096;

Point msm_pippenger_cpu_parallel(
    const std::vector<Scalar>& scalars,
    const std::vector<Point>& points,
    secp256k1_context* ctx
);

} // namespace zkvm
} // namespace zk
} // namespace dinero
