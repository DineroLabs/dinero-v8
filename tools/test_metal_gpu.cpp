// Quick standalone test of Metal GPU mining backend
#include <cstdio>
#include <chrono>
#include "mining/gpu/compute_backend.h"
#include "mining/gpu/gpu_device_manager.h"

int main() {
    fprintf(stderr, "=== Metal GPU Mining Test ===\n");

    // Step 1: Detect devices
    dinero::gpu::GPUDeviceManager dm;
    auto devices = dm.detectAllDevices();
    fprintf(stderr, "Detected %zu GPU devices\n", devices.size());
    for (auto& d : devices) {
        fprintf(stderr, "  [%u] %s (%s, %zu MB)\n",
                d.device_id, d.name.c_str(),
                dinero::gpu::backendToString(d.backend).c_str(),
                d.global_memory_mb);
    }

    auto best = dm.getBestAvailableBackend();
    fprintf(stderr, "Best backend: %s\n", dinero::gpu::backendToString(best).c_str());
    if (best == dinero::gpu::BackendType::NONE) {
        fprintf(stderr, "FAIL: No GPU backend available\n");
        return 1;
    }

    // Step 2: Create backend
    auto backend = dinero::gpu::createBackend(best);
    if (!backend) {
        fprintf(stderr, "FAIL: createBackend returned nullptr\n");
        return 1;
    }
    fprintf(stderr, "Backend created OK\n");

    // Step 3: Init device
    uint32_t dev_id = 0;
    for (auto& d : devices) {
        if (d.backend == best) { dev_id = d.device_id; break; }
    }
    if (!backend->initDevice(dev_id)) {
        fprintf(stderr, "FAIL: initDevice(%u) failed\n", dev_id);
        return 1;
    }
    fprintf(stderr, "initDevice OK\n");

    // Step 4: Compile kernel
    if (!backend->compileKernel("")) {
        fprintf(stderr, "FAIL: compileKernel failed\n");
        return 1;
    }
    fprintf(stderr, "compileKernel OK\n");

    // Step 5: Mine a test batch
    dinero::gpu::WorkPackage work;
    // Fill header with dummy data
    for (int i = 0; i < 32; i++) work.header[i] = 0x12345678 + i;
    // Set easy target (all 0xFF = any hash passes)
    for (int i = 0; i < 8; i++) work.target[i] = 0xFFFFFFFF;
    work.nonce_start = 0;
    work.nonce_end = 0xFFFF;  // Small batch for testing
    work.backend = best;

    dinero::gpu::MiningResult result;
    auto t0 = std::chrono::high_resolution_clock::now();
    bool ok = backend->mine(work, result);
    auto t1 = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    fprintf(stderr, "mine() returned: %s\n", ok ? "true" : "false");
    fprintf(stderr, "  found: %s\n", result.found ? "true" : "false");
    fprintf(stderr, "  nonce: 0x%08x\n", result.nonce);
    fprintf(stderr, "  hashes_tried: %llu\n", result.hashes_tried);
    fprintf(stderr, "  elapsed: %lld us\n", (long long)us);
    if (us > 0 && result.hashes_tried > 0) {
        double mhs = (double)result.hashes_tried / us;
        fprintf(stderr, "  hashrate: %.2f MH/s\n", mhs);
    }

    // Step 6: Mine a real batch (16M nonces) with impossible target
    fprintf(stderr, "\n--- Large batch test (16M nonces, impossible target) ---\n");
    for (int i = 0; i < 8; i++) work.target[i] = 0;
    work.target[0] = 0x00000001;  // Very hard target
    work.nonce_start = 0x80000000;
    work.nonce_end = 0x80FFFFFF;  // 16M nonces

    t0 = std::chrono::high_resolution_clock::now();
    ok = backend->mine(work, result);
    t1 = std::chrono::high_resolution_clock::now();
    us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    fprintf(stderr, "mine() returned: %s\n", ok ? "true" : "false");
    fprintf(stderr, "  found: %s\n", result.found ? "true" : "false");
    fprintf(stderr, "  hashes_tried: %llu\n", result.hashes_tried);
    fprintf(stderr, "  elapsed: %lld us (%.2f ms)\n", (long long)us, us / 1000.0);
    if (us > 0 && result.hashes_tried > 0) {
        double mhs = (double)result.hashes_tried / us;
        fprintf(stderr, "  hashrate: %.2f MH/s\n", mhs);
    }

    backend->stop();
    fprintf(stderr, "\n=== DONE ===\n");
    return 0;
}
