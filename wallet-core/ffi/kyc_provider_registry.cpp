// KYC Provider Registry Implementation
// wallet-core/ffi/kyc_provider_registry.cpp

#include "kyc_provider.h"
#include "kyc_provider_mock.cpp"
#include "kyc_provider_openkyc.cpp"
#include <mutex>

namespace dinero {
namespace kyc {

// Global provider instance (thread-safe)
static std::mutex g_provider_mutex;
static std::unique_ptr<KYCProvider> g_active_provider;

void KYCProviderRegistry::SetProvider(std::unique_ptr<KYCProvider> provider) {
    std::lock_guard<std::mutex> lock(g_provider_mutex);
    g_active_provider = std::move(provider);
}

KYCProvider* KYCProviderRegistry::GetProvider() {
    std::lock_guard<std::mutex> lock(g_provider_mutex);
    
    // If no provider set, return nullptr
    if (!g_active_provider) {
        return nullptr;
    }
    
    return g_active_provider.get();
}

bool KYCProviderRegistry::HasProvider() {
    std::lock_guard<std::mutex> lock(g_provider_mutex);
    return g_active_provider != nullptr;
}

void KYCProviderRegistry::Reset() {
    std::lock_guard<std::mutex> lock(g_provider_mutex);
    if (g_active_provider) {
        g_active_provider->Cleanup();
    }
    g_active_provider.reset();
}

std::unique_ptr<KYCProvider> KYCProviderFactory::Create(ProviderType type) {
    switch (type) {
        case ProviderType::MOCK:
            return std::make_unique<MockKYCProvider>();
        
        case ProviderType::OPENKYC:
            return std::make_unique<OpenKYCProvider>();
        
        case ProviderType::SUMSUB:
            // TODO: Implement Sumsub provider
            // return std::make_unique<SumsubProvider>();
            return nullptr;
        
        case ProviderType::ONFIDO:
            // TODO: Implement Onfido provider
            // return std::make_unique<OnfidoProvider>();
            return nullptr;
        
        default:
            return nullptr;
    }
}

std::unique_ptr<KYCProvider> KYCProviderFactory::CreateFromString(const std::string& type_str) {
    if (type_str == "mock" || type_str == "MockKYC") {
        return Create(ProviderType::MOCK);
    } else if (type_str == "openkyc" || type_str == "OpenKYC") {
        return Create(ProviderType::OPENKYC);
    } else if (type_str == "sumsub" || type_str == "Sumsub") {
        return Create(ProviderType::SUMSUB);
    } else if (type_str == "onfido" || type_str == "Onfido") {
        return Create(ProviderType::ONFIDO);
    }
    
    return nullptr;
}

} // namespace kyc
} // namespace dinero
