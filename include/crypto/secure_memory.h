/**
 * Phase E.2.2: Secure Memory Management
 *
 * Cross-platform memory locking to prevent sensitive data from being swapped to disk.
 *
 * Platform Support:
 * - Unix/Linux/macOS: Uses mlock() / munlock()
 * - Windows: Uses VirtualLock() / VirtualUnlock()
 *
 * Security Note:
 * - Failure to lock memory is non-fatal (degrades security but allows operation)
 * - Unix systems may require CAP_IPC_LOCK capability or RLIMIT_MEMLOCK adjustment
 * - Locked memory counts against system/process limits
 */

#ifndef DINERO_CRYPTO_SECURE_MEMORY_H
#define DINERO_CRYPTO_SECURE_MEMORY_H

#include <cstddef>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <unistd.h>
#endif

namespace dinero {
namespace crypto {

/**
 * Get system page size for memory alignment
 */
inline size_t getPageSize() {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

/**
 * Lock memory pages to prevent swapping to disk
 *
 * @param addr Memory address to lock
 * @param len Length of memory region in bytes
 * @return true if successful, false otherwise
 */
inline bool lockMemory(void* addr, size_t len) {
    if (addr == nullptr || len == 0) {
        return false;
    }

#ifdef _WIN32
    // Windows: VirtualLock
    return VirtualLock(addr, len) != 0;
#else
    // Unix/Linux/macOS: mlock
    return mlock(addr, len) == 0;
#endif
}

/**
 * Unlock previously locked memory pages
 *
 * @param addr Memory address to unlock
 * @param len Length of memory region in bytes
 * @return true if successful, false otherwise
 */
inline bool unlockMemory(void* addr, size_t len) {
    if (addr == nullptr || len == 0) {
        return false;
    }

#ifdef _WIN32
    // Windows: VirtualUnlock
    return VirtualUnlock(addr, len) != 0;
#else
    // Unix/Linux/macOS: munlock
    return munlock(addr, len) == 0;
#endif
}

/**
 * RAII wrapper for locked memory
 *
 * Automatically unlocks memory when object goes out of scope
 */
class LockedMemory {
public:
    /**
     * Lock memory region
     *
     * @param addr Memory address to lock
     * @param len Length in bytes
     */
    LockedMemory(void* addr, size_t len)
        : addr_(addr), len_(len), locked_(false) {
        if (addr != nullptr && len > 0) {
            locked_ = lockMemory(addr, len);
        }
    }

    /**
     * Destructor - automatically unlocks memory
     */
    ~LockedMemory() {
        if (locked_ && addr_ != nullptr && len_ > 0) {
            unlockMemory(addr_, len_);
        }
    }

    // Disable copy
    LockedMemory(const LockedMemory&) = delete;
    LockedMemory& operator=(const LockedMemory&) = delete;

    // Enable move
    LockedMemory(LockedMemory&& other) noexcept
        : addr_(other.addr_), len_(other.len_), locked_(other.locked_) {
        other.addr_ = nullptr;
        other.len_ = 0;
        other.locked_ = false;
    }

    LockedMemory& operator=(LockedMemory&& other) noexcept {
        if (this != &other) {
            // Unlock current memory
            if (locked_ && addr_ != nullptr && len_ > 0) {
                unlockMemory(addr_, len_);
            }

            // Move from other
            addr_ = other.addr_;
            len_ = other.len_;
            locked_ = other.locked_;

            // Invalidate other
            other.addr_ = nullptr;
            other.len_ = 0;
            other.locked_ = false;
        }
        return *this;
    }

    /**
     * Check if memory is successfully locked
     */
    bool isLocked() const { return locked_; }

private:
    void* addr_;
    size_t len_;
    bool locked_;
};

} // namespace crypto
} // namespace dinero

#endif // DINERO_CRYPTO_SECURE_MEMORY_H
