#pragma once
#include <vector>
#include <cstddef>
#include <stdexcept>

#if defined(__APPLE__)
  #include <stdlib.h> // arc4random_buf
#elif defined(_WIN32)
  #include <windows.h>
  #include <bcrypt.h>
  #pragma comment(lib, "bcrypt.lib")
#else
  #include <sys/random.h> // getrandom
  #include <errno.h>
  #include <string.h>
  #include <unistd.h>
#endif

/**
 * Generate cryptographically secure random bytes using OS CSPRNG (non-blocking)
 * 
 * Uses:
 * - macOS: arc4random_buf (Yarrow/CTR-DRBG)
 * - Windows: BCryptGenRandom (CNG system-preferred RNG)  
 * - Linux/BSD: getrandom (non-blocking once CRNG is ready)
 */
inline std::vector<unsigned char> secure_random_bytes(std::size_t n) {
    std::vector<unsigned char> out(n);

#if defined(__APPLE__)
    // arc4random_buf is non-blocking, uses Yarrow/CTR-DRBG
    arc4random_buf(out.data(), out.size());

#elif defined(_WIN32)
    // BCryptGenRandom (CNG), system-preferred RNG
    if (BCryptGenRandom(nullptr, out.data(), (ULONG)out.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("BCryptGenRandom failed");
    }

#else
    // Linux/BSD: getrandom is non-blocking once CRNG is ready
    ssize_t r = 0, off = 0;
    while (off < (ssize_t)out.size()) {
        r = getrandom(out.data() + off, out.size() - off, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("getrandom failed");
        }
        off += r;
    }
#endif
    return out;
}
