// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * CPU Multi-Scalar Multiplication via secp256k1_ecmult_multi_var
 *
 * Calls dinero_secp256k1_msm() from secp256k1_msm_shim.c, which is compiled
 * as part of the secp256k1-zkp library and has access to the internal
 * secp256k1_ecmult_multi_var function (Pippenger/Strauss MSM).
 *
 * Algorithm selection (inside secp256k1-zkp):
 *   n < 88:  Strauss wNAF   (~3-5x faster than sequential scalar mults)
 *   n >= 88: Pippenger c=8  (~6-15x faster than sequential scalar mults)
 *
 * This replaces the naive O(n × 256) sequential loop with O(n + 2^c × rounds)
 * native field arithmetic — no API call overhead per EC operation.
 */

#include "zk/zkvm/secp256k1_msm_cpu.h"
#include <cassert>
#include <cstring>
#include <thread>
#include <vector>

/* dinero_secp256k1_msm is compiled into the secp256k1 library from
 * secp256k1_msm_shim.c via CMake target_sources. */
extern "C" {
int dinero_secp256k1_msm(
    const secp256k1_context* ctx,
    const unsigned char (*scalars_bytes)[32],
    const secp256k1_pubkey* points,
    size_t n,
    secp256k1_pubkey* result
);
}

namespace dinero {
namespace zk {
namespace zkvm {

Point msm_pippenger_cpu(
    const std::vector<Scalar>& scalars,
    const std::vector<Point>& points,
    secp256k1_context* ctx
) {
    const size_t n = scalars.size();
    assert(n > 0 && n == points.size());

    /* Build contiguous arrays of raw scalar bytes and pubkeys for the C shim. */
    std::vector<std::array<unsigned char, 32>> sc_bytes(n);
    std::vector<secp256k1_pubkey> pk_arr(n);

    for (size_t i = 0; i < n; ++i) {
        std::memcpy(sc_bytes[i].data(), scalars[i].data(), 32);
        pk_arr[i] = points[i].raw();
    }

    secp256k1_pubkey result_pk;
    int ok = dinero_secp256k1_msm(
        ctx,
        reinterpret_cast<const unsigned char (*)[32]>(sc_bytes.data()),
        pk_arr.data(),
        n,
        &result_pk
    );

    if (!ok) {
        // Pippenger failed or returned identity — fall back to sequential.
        // This shouldn't happen for non-trivial inputs but handles OOM gracefully.
        Point result = Point::identity();
        for (size_t i = 0; i < n; ++i) {
            Point term = points[i] * scalars[i];
            if (!term.is_identity()) {
                result = result + term;
            }
        }
        return result;
    }
    return Point(result_pk);
}

Point msm_pippenger_cpu_parallel(
    const std::vector<Scalar>& scalars,
    const std::vector<Point>& points,
    secp256k1_context* ctx
) {
    const size_t n = scalars.size();
    assert(n > 0 && n == points.size());

    // Determine thread count.  Each chunk handles ~32K points so the bucket
    // array + point data fits in per-core L2 cache (~4 MB on Apple M-series).
    // Points: 32K × 66 bytes = 2 MB.  Scratch: 32K × 192 bytes = 6 MB → L3.
    // With hw=16 threads and n=1M: 16 × 64K-point chunks run concurrently.
    static const size_t hw = std::thread::hardware_concurrency();
    const size_t target_chunk = 8 * 1024;  // 8K points per thread
    size_t nthreads = (n + target_chunk - 1) / target_chunk;
    if (nthreads > hw) nthreads = hw;
    if (nthreads < 1) nthreads = 1;

    if (nthreads <= 1) {
        return msm_pippenger_cpu(scalars, points, ctx);
    }

    // Build contiguous raw arrays once — shared across all threads (read-only).
    std::vector<std::array<unsigned char, 32>> sc_bytes(n);
    std::vector<secp256k1_pubkey> pk_arr(n);
    for (size_t i = 0; i < n; ++i) {
        std::memcpy(sc_bytes[i].data(), scalars[i].data(), 32);
        pk_arr[i] = points[i].raw();
    }

    const size_t chunk = (n + nthreads - 1) / nthreads;
    std::vector<Point> partial(nthreads, Point::identity());

    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (size_t t = 0; t < nthreads; ++t) {
        const size_t begin = t * chunk;
        const size_t end   = std::min(begin + chunk, n);
        if (begin >= n) break;
        threads.emplace_back([&sc_bytes, &pk_arr, ctx, begin, end, t, &partial]() {
            secp256k1_pubkey partial_pk{};
            int ok = dinero_secp256k1_msm(
                ctx,
                reinterpret_cast<const unsigned char (*)[32]>(sc_bytes.data() + begin),
                pk_arr.data() + begin,
                end - begin,
                &partial_pk
            );
            if (ok) partial[t] = Point(partial_pk);
        });
    }
    for (auto& thr : threads) thr.join();

    // Combine partial sums.
    Point result = Point::identity();
    for (const auto& p : partial) {
        if (!p.is_identity()) result = result + p;
    }
    return result;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
