#include "sha256_simd.h"
#include "crypto/sha256.h"  // Fallback scalar implementation
#include <cstring>

// Platform-specific CPU detection
#if defined(__x86_64__) || defined(_M_X64)
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <cpuid.h>
    #endif
    #define DINERO_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #if defined(__APPLE__)
        #include <sys/sysctl.h>
    #elif defined(__linux__)
        #include <sys/auxv.h>
        #include <asm/hwcap.h>
    #endif
    #define DINERO_ARM64 1
#endif

namespace dinero {
namespace crypto {

// Forward declarations for SIMD implementations
#if defined(DINERO_X86_64)
    extern void SHA256d_AVX2(const uint8_t* data, size_t blocks, uint8_t* out);
    extern void SHA256d_SSSE3(const uint8_t* data, size_t blocks, uint8_t* out);
    extern void SHA256d_SHA_NI(const uint8_t* data, size_t blocks, uint8_t* out);
#elif defined(DINERO_ARM64)
    extern void SHA256d_NEON(const uint8_t* data, size_t blocks, uint8_t* out);
    
    // Real ARM SHA hardware implementation (from Bitcoin Core)
    #if defined(__ARM_FEATURE_CRYPTO)
        extern void SHA256d_ARM_SHA_Real(const uint8_t* data, size_t blocks, uint8_t* out);
        
        inline void SHA256d_ARM_SHA(const uint8_t* data, size_t blocks, uint8_t* out) {
            SHA256d_ARM_SHA_Real(data, blocks, out);  // Use hardware SHA!
        }
    #else
        // Fallback if no crypto extensions
        inline void SHA256d_ARM_SHA(const uint8_t* data, size_t blocks, uint8_t* out) {
            SHA256d_NEON(data, blocks, out);
        }
    #endif
#endif

// CPU feature detection
static bool CheckCPUFeature_x86(uint32_t leaf, uint32_t subleaf, uint32_t reg, uint32_t bit) {
#if defined(DINERO_X86_64)
    uint32_t eax, ebx, ecx, edx;
    #if defined(_MSC_VER)
        int cpu_info[4];
        __cpuidex(cpu_info, leaf, subleaf);
        eax = cpu_info[0];
        ebx = cpu_info[1];
        ecx = cpu_info[2];
        edx = cpu_info[3];
        uint32_t* regs[4] = {&eax, &ebx, &ecx, &edx};
        return (*regs[reg] & (1u << bit)) != 0;
    #else
        if (__get_cpuid_count(leaf, subleaf, &eax, &ebx, &ecx, &edx)) {
            uint32_t* regs[4] = {&eax, &ebx, &ecx, &edx};
            return (*regs[reg] & (1u << bit)) != 0;
        }
    #endif
#endif
    return false;
}

SIMDLevel DetectBestSIMD() {
#if defined(DINERO_X86_64)
    // Check for Intel SHA Extensions (best)
    // CPUID.07H:EBX.SHA[bit 29]
    if (CheckCPUFeature_x86(7, 0, 1, 29)) {
        return SIMDLevel::SHA_NI;
    }
    
    // Check for AVX2 (very good)
    // CPUID.07H:EBX.AVX2[bit 5]
    if (CheckCPUFeature_x86(7, 0, 1, 5)) {
        return SIMDLevel::AVX2;
    }
    
    // Check for SSSE3 (good)
    // CPUID.01H:ECX.SSSE3[bit 9]
    if (CheckCPUFeature_x86(1, 0, 2, 9)) {
        return SIMDLevel::SSSE3;
    }
    
    return SIMDLevel::None;
    
#elif defined(DINERO_ARM64)
    #if defined(__APPLE__)
        // macOS: Check for ARM Crypto extensions via sysctl
        int hasArmCrypto = 0;
        size_t len = sizeof(hasArmCrypto);
        if (sysctlbyname("hw.optional.arm.FEAT_SHA256", &hasArmCrypto, &len, NULL, 0) == 0 && hasArmCrypto) {
            return SIMDLevel::ARM_SHA;
        }
        
        // All modern Apple Silicon has NEON
        return SIMDLevel::NEON;
        
    #elif defined(__linux__)
        // Linux: Check via getauxval
        unsigned long hwcap = getauxval(AT_HWCAP);
        if (hwcap & HWCAP_SHA2) {
            return SIMDLevel::ARM_SHA;
        }
        if (hwcap & HWCAP_ASIMD) {  // Advanced SIMD = NEON
            return SIMDLevel::NEON;
        }
    #endif
    
    // Assume NEON on ARMv8 (it's mandatory)
    return SIMDLevel::NEON;
    
#else
    return SIMDLevel::None;
#endif
}

const char* SIMDLevelName(SIMDLevel level) {
    switch (level) {
        case SIMDLevel::None:    return "Scalar";
        case SIMDLevel::SSE2:    return "SSE2 (4-way)";
        case SIMDLevel::SSSE3:   return "SSSE3 (4-way)";
        case SIMDLevel::AVX2:    return "AVX2 (8-way)";
        case SIMDLevel::SHA_NI:  return "SHA-NI (hw)";
        case SIMDLevel::NEON:    return "NEON (4-way)";
        case SIMDLevel::ARM_SHA: return "ARM SHA (hw)";
        default:                 return "Unknown";
    }
}

// Fallback: use existing scalar SHA-256
static void DoubleSHA256_Scalar(const uint8_t* data, size_t len, uint8_t out32[32]) {
    CSHA256 h1;
    h1.Write(data, len);
    uint8_t tmp[32];
    h1.Finalize(tmp);
    
    CSHA256 h2;
    h2.Write(tmp, 32);
    h2.Finalize(out32);
}

void DoubleSHA256_SIMD(const uint8_t* data, size_t len, uint8_t out32[32], SIMDLevel level) {
    // Auto-detect if not specified
    if (level == SIMDLevel::None) {
        static SIMDLevel detected = DetectBestSIMD();
        level = detected;
    }
    
    // For arbitrary-length data, we need to use scalar path
    // (SIMD versions are optimized for fixed-size block headers)
    DoubleSHA256_Scalar(data, len, out32);
}

void SHA256_SIMD(const uint8_t* data, size_t len, uint8_t out32[32], SIMDLevel level) {
    if (level == SIMDLevel::None) {
        static SIMDLevel detected = DetectBestSIMD();
        level = detected;
    }
    
    // Single SHA-256 (use scalar for now)
    CSHA256 h;
    h.Write(data, len);
    h.Finalize(out32);
}

void SHA256d_BlockHeader(const uint8_t header80[80], uint8_t out32[32], SIMDLevel level) {
    // Auto-detect if not specified
    if (level == SIMDLevel::None) {
        static SIMDLevel detected = DetectBestSIMD();
        level = detected;
    }
    
    // This is the HOT PATH for mining
    // We'll implement optimized versions for each SIMD level
    
#if defined(DINERO_X86_64)
    switch (level) {
        case SIMDLevel::SHA_NI:
            // SHA256d_SHA_NI(header80, 1, out32);
            // return;
            break;  // Fall through to scalar for now
        case SIMDLevel::AVX2:
            // SHA256d_AVX2(header80, 1, out32);
            // return;
            break;  // Fall through to scalar for now
        case SIMDLevel::SSSE3:
            // SHA256d_SSSE3(header80, 1, out32);
            // return;
            break;  // Fall through to scalar for now
        default:
            break;
    }
#elif defined(DINERO_ARM64)
    switch (level) {
        case SIMDLevel::ARM_SHA:
            SHA256d_ARM_SHA(header80, 1, out32);
            return;
        case SIMDLevel::NEON:
            SHA256d_NEON(header80, 1, out32);
            return;
        default:
            break;
    }
#endif
    
    // Fallback: scalar double-SHA256
    DoubleSHA256_Scalar(header80, 80, out32);
}

// RAII context
SIMDContext::SIMDContext() : level_(DetectBestSIMD()) {
    // Auto-detect at construction
}

} // namespace crypto
} // namespace dinero

