#include "wallet_ffi.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string make_temp_datadir() {
    namespace fs = std::filesystem;
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("dinero_ffi_concurrency_" + std::to_string(ts));
    fs::create_directories(dir);
    return dir.string();
}

void free_if_set(char* ptr) {
    if (ptr) {
        dinero_wallet_free_string(ptr);
    }
}

} // namespace

int main() {
    const std::string datadir = make_temp_datadir();

    if (dinero_wallet_init(datadir.c_str()) != 0) {
        std::cerr << "wallet init failed" << std::endl;
        return 1;
    }

    char* mnemonic = nullptr;
    if (dinero_wallet_create(&mnemonic) != 0) {
        std::cerr << "wallet create failed" << std::endl;
        return 1;
    }
    free_if_set(mnemonic);

    constexpr int kThreads = 8;
    constexpr int kItersPerThread = 250;

    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            int local_success_addrs = 0;
            for (int i = 0; i < kItersPerThread; ++i) {
                if (failed.load(std::memory_order_relaxed)) {
                    return;
                }

                (void)dinero_wallet_is_encrypted();
                (void)dinero_wallet_is_locked();
                (void)dinero_wallet_get_balance();

                FFI_SyncProgress progress{};
                const int sync_rc = dinero_wallet_get_sync_progress(&progress);
                if (sync_rc == 0 && progress.status_message) {
                    dinero_wallet_free_string(progress.status_message);
                    progress.status_message = nullptr;
                }

                if ((i % 10) == 0) {
                    char* addr = nullptr;
                    const int rc = dinero_wallet_get_new_address("ffi-concurrency", &addr);
                    if (rc == 0 && addr) {
                        ++local_success_addrs;
                    }
                    free_if_set(addr);
                }
            }

            if (local_success_addrs == 0) {
                failed.store(true, std::memory_order_relaxed);
            }
        });
    }

    for (auto& th : workers) {
        th.join();
    }

    if (failed.load(std::memory_order_relaxed)) {
        std::cerr << "ffi concurrency stress failed" << std::endl;
        return 1;
    }

    std::cout << "ffi concurrency stress passed" << std::endl;
    return 0;
}
