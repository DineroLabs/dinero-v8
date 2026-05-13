// KYC Provider Abstraction Layer
// wallet-core/ffi/kyc_provider.h

#pragma once

#include "wallet_ffi.h"
#include <string>
#include <memory>

namespace dinero {
namespace kyc {

/**
 * KYC Provider Types
 */
enum class ProviderType {
    MOCK = 0,      // Mock provider for testing
    OPENKYC = 1,   // Self-hosted OpenKYC
    SUMSUB = 2,    // Commercial Sumsub
    ONFIDO = 3,    // Commercial Onfido
    CUSTOM = 99    // Custom provider
};

/**
 * Abstract base class for KYC providers
 * 
 * All KYC providers must implement this interface.
 * This allows swapping providers without changing FFI or mobile app code.
 */
class KYCProvider {
public:
    virtual ~KYCProvider() = default;
    
    /**
     * Get provider name
     */
    virtual const char* GetName() const = 0;
    
    /**
     * Get provider type
     */
    virtual ProviderType GetType() const = 0;
    
    /**
     * Initialize provider (called once at startup)
     * @param config Configuration string (JSON, TOML, or key=value)
     * @return true on success, false on failure
     */
    virtual bool Initialize(const std::string& config) = 0;
    
    /**
     * Start KYC verification
     * @param user_id User identifier (wallet address or internal ID)
     * @param level Verification level ("basic", "advanced", etc.)
     * @param country ISO country code (e.g., "US", "GB")
     * @param verification_url_out Output verification URL
     * @return 0 on success, non-zero on error
     */
    virtual int StartVerification(
        const std::string& user_id,
        const std::string& level,
        const std::string& country,
        std::string& verification_url_out
    ) = 0;
    
    /**
     * Get KYC verification status
     * @param user_id User identifier
     * @param session_id Session/verification ID (optional, provider-specific)
     * @param status_out Output status structure
     * @return 0 on success, non-zero on error
     */
    virtual int GetStatus(
        const std::string& user_id,
        const std::string& session_id,
        FFI_KYCStatus& status_out
    ) = 0;
    
    /**
     * Check if provider is available/configured
     */
    virtual bool IsAvailable() const = 0;
    
    /**
     * Cleanup (called on shutdown)
     */
    virtual void Cleanup() {}
};

/**
 * Provider factory
 * Creates provider instances based on type
 */
class KYCProviderFactory {
public:
    static std::unique_ptr<KYCProvider> Create(ProviderType type);
    static std::unique_ptr<KYCProvider> CreateFromString(const std::string& type_str);
};

/**
 * Global provider registry
 * Manages active provider instance
 */
class KYCProviderRegistry {
public:
    /**
     * Set active provider
     */
    static void SetProvider(std::unique_ptr<KYCProvider> provider);
    
    /**
     * Get active provider
     */
    static KYCProvider* GetProvider();
    
    /**
     * Check if provider is set
     */
    static bool HasProvider();
    
    /**
     * Reset provider (for testing)
     */
    static void Reset();
};

} // namespace kyc
} // namespace dinero

