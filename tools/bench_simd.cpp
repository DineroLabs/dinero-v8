// Benchmark SIMD SHA-256 implementations
// Compares scalar vs SIMD performance

#include "crypto/sha256.h"
#include "crypto/sha256_simd.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <cstring>

using namespace std;
using namespace dinero::crypto;

// Benchmark function
double bench_sha256d(int iterations, SIMDLevel level) {
    // Sample 128-byte block header (Dinero BlockHeader v1)
    uint8_t header[128];
    memset(header, 0, 128);

    // Add some data to make it realistic
    const char* msg = "Dinero mining benchmark test";
    memcpy(header, msg, strlen(msg));

    uint8_t hash[32];

    auto start = chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        // Simulate mining: change nonce at offset 112 (BlockHeader v1)
        header[112] = i & 0xff;
        header[113] = (i >> 8) & 0xff;
        header[114] = (i >> 16) & 0xff;
        header[115] = (i >> 24) & 0xff;

        if (level == SIMDLevel::None) {
            // Scalar double-SHA256
            CSHA256 h1;
            h1.Write(header, 128);
            uint8_t tmp[32];
            h1.Finalize(tmp);

            CSHA256 h2;
            h2.Write(tmp, 32);
            h2.Finalize(hash);
        } else {
            // SIMD double-SHA256
            SHA256d_BlockHeader(header, hash, level);
        }
    }
    
    auto end = chrono::high_resolution_clock::now();
    double elapsed = chrono::duration<double>(end - start).count();
    
    return (double)iterations / elapsed / 1000000.0;  // MH/s
}

int main() {
    cout << "=== Dinero SHA-256 SIMD Benchmark ===\n\n";
    
    // Detect best SIMD
    SIMDContext ctx;
    cout << "Detected SIMD: " << ctx.name() << "\n\n";
    
    // Benchmark parameters
    const int warmup = 10000;
    const int iterations = 100000;
    
    cout << "Warming up... " << flush;
    bench_sha256d(warmup, SIMDLevel::None);
    cout << "done\n\n";
    
    cout << "Running benchmarks (" << iterations << " iterations each)...\n\n";
    
    // Benchmark scalar
    cout << "1. Scalar (baseline)... " << flush;
    double scalar_mhs = bench_sha256d(iterations, SIMDLevel::None);
    cout << fixed << setprecision(2) << scalar_mhs << " MH/s\n";
    
    // Benchmark detected SIMD
    cout << "2. " << ctx.name() << "... " << flush;
    double simd_mhs = bench_sha256d(iterations, ctx.level());
    cout << fixed << setprecision(2) << simd_mhs << " MH/s\n";
    
    // Calculate speedup
    double speedup = simd_mhs / scalar_mhs;
    
    cout << "\n=== Results ===\n";
    cout << "Scalar:    " << fixed << setprecision(2) << scalar_mhs << " MH/s\n";
    cout << "SIMD:      " << fixed << setprecision(2) << simd_mhs << " MH/s (" << ctx.name() << ")\n";
    cout << "Speedup:   " << fixed << setprecision(2) << speedup << "x\n";
    
    if (speedup > 1.1) {
        cout << "\n✅ SIMD optimization is ACTIVE and providing " 
             << fixed << setprecision(1) << ((speedup - 1.0) * 100) << "% speedup!\n";
    } else {
        cout << "\n⚠️  SIMD optimization not yet providing significant speedup.\n";
        cout << "   (Implementation may be falling back to scalar)\n";
    }
    
    return 0;
}

