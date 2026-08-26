// Standalone GPU backend validation. Replicates the three-phase test
// from dinero-miner: permissive target (kernel launches, atomicAdd),
// CPU-vs-GPU hash correctness on a tightened target, then a hashrate
// bench against an unsatisfiable target.
//
// Built when a CUDA, Metal, or OpenCL backend is enabled.
// Exits 0 on success and prints a "[self-test] PASS" line at the end.

#if defined(MINER_ENABLE_CUDA) || defined(MINER_ENABLE_METAL) || defined(MINER_ENABLE_OPENCL)

#include "solo_miner/gpu_backend.h"
#include "solo_miner/hash_engine.h"
#include "solo_miner/types.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

using namespace dinero::solo;

namespace {

void writeU32LE(uint8_t* dest, uint32_t val) {
    dest[0] = val & 0xFF;
    dest[1] = (val >> 8) & 0xFF;
    dest[2] = (val >> 16) & 0xFF;
    dest[3] = (val >> 24) & 0xFF;
}

void writeU64LE(uint8_t* dest, uint64_t val) {
    for (int i = 0; i < 8; i++) dest[i] = (val >> (i * 8)) & 0xFF;
}

std::string hexOf(const uint8_t* p, size_t n) {
    std::stringstream ss;
    for (size_t i = 0; i < n; i++) {
        ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(p[i]);
    }
    return ss.str();
}

} // namespace

int main(int argc, char* argv[]) {
    using std::cout;
    using std::cerr;
    using std::endl;

    int bench_secs = 10;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--bench" && i + 1 < argc) {
            bench_secs = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            cout << "Usage: " << argv[0] << " [--bench <seconds>]\n"
                 << "\n"
                 << "Validates the GPU mining backend: permissive-target launch test,\n"
                 << "CPU-vs-GPU hash correctness, and hashrate bench (default 10s).\n";
            return 0;
        }
    }

    cout << "[self-test] backend=gpu bench_secs=" << bench_secs << "\n";

    auto backend = CreateGpuBackend();
    if (!backend) {
        cerr << "[self-test] FAIL: CreateGpuBackend() returned nullptr; "
             << "see backend errors above" << endl;
        return 1;
    }
    cout << "[self-test] backend=" << backend->backendName()
         << " device=\"" << backend->deviceName()
         << "\" arch=" << backend->computeArch() << "\n";

    // Fixed test header. Non-zero version/timestamp/nbits so we don't hit
    // any all-zero-input quirks. Bytes 4..100 (prev_hash, merkle_root,
    // utreexo_root) stay zero; bytes 112..128 (nonce + reserved) are
    // written per-thread by the kernel and start zero per consensus.
    uint8_t header[128] = {0};
    writeU32LE(&header[0], 1);                // version
    writeU64LE(&header[100], 1715000000ULL);  // timestamp
    writeU32LE(&header[108], 0x1d00ffff);     // nbits

    // === Test 1: permissive target — every thread should win ===
    // Use a non-block-multiple batch so the test also proves the kernel
    // ignores rounded-up excess threads (i.e. tid >= batch_size returns
    // early without writing a result slot).
    constexpr uint32_t TEST1_BATCH = 257;
    uint8_t permissive[32];
    std::memset(permissive, 0xFF, sizeof(permissive));
    auto out1 = backend->dispatch(header, permissive, 0, TEST1_BATCH);
    if (out1.total_count != TEST1_BATCH) {
        cerr << "[self-test] FAIL test1: kernel reported total_count="
             << out1.total_count << " for batch=" << TEST1_BATCH
             << " against permissive target — every thread should satisfy. "
             << "Hash computation or atomicAdd is broken." << endl;
        return 1;
    }
    if (out1.found_count != GPU_RESULT_CAPACITY) {
        cerr << "[self-test] FAIL test1: found_count=" << out1.found_count
             << ", expected min(total, capacity)=" << GPU_RESULT_CAPACITY << endl;
        return 1;
    }
    for (uint32_t i = 0; i < out1.found_count; i++) {
        if (out1.nonces[i] >= TEST1_BATCH) {
            cerr << "[self-test] FAIL test1: nonce[" << i << "]=" << out1.nonces[i]
                 << " outside batch [0, " << TEST1_BATCH << ")" << endl;
            return 1;
        }
    }
    cout << "[self-test] OK test1 permissive: total=" << out1.total_count
         << " (kept " << out1.found_count << " of " << GPU_RESULT_CAPACITY
         << "), elapsed_ms=" << std::fixed << std::setprecision(3)
         << out1.elapsed_ms << "\n";

    // === Test 2: CPU-vs-GPU hash correctness on a tightened target ===
    // Hash 4096 nonces on the CPU, sort by hash, use sorted_hashes[1] as
    // the target so exactly one nonce (the smallest-hash one) satisfies
    // the strict less-than. The GPU must report the same nonce.
    constexpr uint32_t TEST2_BATCH = 4096;
    struct Pair {
        Hash256 hash;
        uint32_t nonce;
    };
    std::vector<Pair> hashes(TEST2_BATCH);
    for (uint32_t n = 0; n < TEST2_BATCH; n++) {
        uint8_t local_header[128];
        std::memcpy(local_header, header, 128);
        writeU32LE(&local_header[112], n);
        hashes[n].hash = HashEngine::hashHeader(local_header);
        hashes[n].nonce = n;
    }
    std::sort(hashes.begin(), hashes.end(), [](const Pair& a, const Pair& b) {
        return std::memcmp(a.hash.data(), b.hash.data(), 32) < 0;
    });
    const Pair& winner = hashes[0];
    const uint8_t* target = hashes[1].hash.data();  // strict less-than

    cout << "[self-test] cpu winner: nonce=" << winner.nonce
         << " hash=" << hexOf(winner.hash.data(), 32) << "\n";
    cout << "[self-test] target (cpu second-smallest): "
         << hexOf(target, 32) << "\n";

    auto out2 = backend->dispatch(header, target, 0, TEST2_BATCH);
    if (out2.total_count != 1 || out2.found_count != 1) {
        cerr << "[self-test] FAIL test2: gpu total_count=" << out2.total_count
             << " found_count=" << out2.found_count
             << "; tightened target should yield exactly one winner (nonce "
             << winner.nonce << ")" << endl;
        return 1;
    }
    if (out2.nonces[0] != winner.nonce) {
        // Hash the GPU's reported nonce on the CPU so the diagnostic
        // shows what the GPU actually computed vs what we expected.
        uint8_t local_header[128];
        std::memcpy(local_header, header, 128);
        writeU32LE(&local_header[112], out2.nonces[0]);
        Hash256 gpu_hash = HashEngine::hashHeader(local_header);
        cerr << "[self-test] FAIL test2: gpu nonce=" << out2.nonces[0]
             << " (hash " << hexOf(gpu_hash.data(), 32)
             << "), cpu expected nonce=" << winner.nonce
             << " (hash " << hexOf(winner.hash.data(), 32)
             << "). Likely hash-comparison or word-order divergence." << endl;
        return 1;
    }
    cout << "[self-test] OK test2 hash-correctness: gpu nonce="
         << out2.nonces[0] << " matches cpu, dispatch_ms="
         << std::fixed << std::setprecision(3) << out2.elapsed_ms << "\n";

    // === Hashrate bench: target=[0;32] is unsatisfiable ===
    // Every thread must fail the compare so the kernel sweeps the full
    // batch with no early-out from the result-array path. Wall-clock /
    // hashes gives raw throughput.
    uint8_t zero_target[32] = {0};
    constexpr uint32_t BENCH_BATCH = 1u << 20;
    cout << "[self-test] bench start: batch=" << BENCH_BATCH
         << " window=" << bench_secs << "s\n";

    auto bench_start = std::chrono::steady_clock::now();
    uint64_t total_hashes = 0;
    uint64_t dispatches = 0;
    uint32_t nonce_start = 0;
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - bench_start).count()
            >= bench_secs) {
            break;
        }
        auto outcome = backend->dispatch(header, zero_target, nonce_start, BENCH_BATCH);
        if (outcome.total_count != 0) {
            cerr << "[self-test] FAIL bench: gpu reported total_count="
                 << outcome.total_count << " with target=0 (impossible). "
                 << "First nonce reported: "
                 << (outcome.found_count > 0 ? outcome.nonces[0] : 0)
                 << " — kernel hash_meets_target is broken." << endl;
            return 1;
        }
        total_hashes += BENCH_BATCH;
        nonce_start += BENCH_BATCH;
        dispatches++;
    }
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - bench_start).count();
    double mhs = (total_hashes / elapsed) / 1e6;
    double dispatch_ms = (elapsed * 1000.0) / (dispatches > 0 ? dispatches : 1);
    cout << "[self-test] bench result: "
         << std::fixed << std::setprecision(2) << mhs << " MH/s ("
         << total_hashes << " hashes / "
         << std::setprecision(2) << elapsed << "s, "
         << dispatches << " dispatches, "
         << std::setprecision(3) << dispatch_ms << " ms/dispatch)\n";

    cout << "[self-test] PASS" << endl;
    return 0;
}

#else  // no GPU backend

#include <iostream>

int main() {
    std::cerr << "gpu_self_test was built without GPU backend support. "
              << "Configure with -DMINER_ENABLE_CUDA=ON or "
              << "-DMINER_ENABLE_METAL=ON or -DMINER_ENABLE_OPENCL=ON to enable."
              << std::endl;
    return 2;
}

#endif
