#include "secure_random.h"

// Platform-specific includes
#ifdef __APPLE__
    #include <Security/Security.h>
#elif defined(_WIN32)
    #include <windows.h>
    #include <bcrypt.h>
    #pragma comment(lib, "bcrypt.lib")
#else
    #include <sys/random.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif

bool SecureRandom::GetBytes(uint8_t* output, size_t length) {
    if (!output || length == 0) {
        return false;
    }
    
#ifdef __APPLE__
    return GetBytes_macOS(output, length);
#elif defined(_WIN32)
    return GetBytes_Windows(output, length);
#else
    return GetBytes_Linux(output, length);
#endif
}

uint32_t SecureRandom::GetUInt32() {
    uint32_t result;
    if (!GetBytes(reinterpret_cast<uint8_t*>(&result), sizeof(result))) {
        return 0; // Fallback - in production should throw exception
    }
    return result;
}

uint64_t SecureRandom::GetUInt64() {
    uint64_t result;
    if (!GetBytes(reinterpret_cast<uint8_t*>(&result), sizeof(result))) {
        return 0; // Fallback - in production should throw exception
    }
    return result;
}

bool SecureRandom::GetPrivateKey(uint8_t private_key[32]) {
    // Generate random bytes
    if (!GetBytes(private_key, 32)) {
        return false;
    }
    
    // Ensure the key is in valid secp256k1 range
    // secp256k1 order: FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
    static const uint8_t secp256k1_order[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
        0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48, 0xA0, 0x3B,
        0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41
    };
    
    // Check if generated key is >= order (extremely unlikely)
    bool key_too_large = true;
    for (int i = 0; i < 32; i++) {
        if (private_key[i] < secp256k1_order[i]) {
            key_too_large = false;
            break;
        } else if (private_key[i] > secp256k1_order[i]) {
            break;
        }
    }
    
    // Check if key is zero (extremely unlikely)
    bool key_is_zero = true;
    for (int i = 0; i < 32; i++) {
        if (private_key[i] != 0) {
            key_is_zero = false;
            break;
        }
    }
    
    if (key_too_large || key_is_zero) {
        // Regenerate (recursive call - probability of infinite recursion is negligible)
        return GetPrivateKey(private_key);
    }
    
    return true;
}

#ifdef __APPLE__
bool SecureRandom::GetBytes_macOS(uint8_t* output, size_t length) {
    OSStatus status = SecRandomCopyBytes(kSecRandomDefault, length, output);
    return status == errSecSuccess;
}
#endif

#ifdef _WIN32
bool SecureRandom::GetBytes_Windows(uint8_t* output, size_t length) {
    NTSTATUS status = BCryptGenRandom(
        nullptr,                           // Use default provider
        output,
        static_cast<ULONG>(length),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG   // Use system preferred RNG
    );
    return status == 0; // STATUS_SUCCESS
}
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
bool SecureRandom::GetBytes_Linux(uint8_t* output, size_t length) {
    // Try getrandom() first (available on Linux 3.17+)
    ssize_t result = getrandom(output, length, 0);
    if (result == static_cast<ssize_t>(length)) {
        return true;
    }
    
    // Fallback to /dev/urandom
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    
    size_t bytes_read = 0;
    while (bytes_read < length) {
        ssize_t n = read(fd, output + bytes_read, length - bytes_read);
        if (n <= 0) {
            close(fd);
            return false;
        }
        bytes_read += static_cast<size_t>(n);
    }
    
    close(fd);
    return true;
}
#endif
