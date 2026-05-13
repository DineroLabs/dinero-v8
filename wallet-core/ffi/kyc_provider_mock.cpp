// Mock KYC Provider (for testing)
// wallet-core/ffi/kyc_provider_mock.cpp

#include "kyc_provider.h"
#include <cstring>
#include <map>
#include <ctime>

namespace dinero {
namespace kyc {

class MockKYCProvider : public KYCProvider {
private:
    struct VerificationSession {
        std::string user_id;
        std::string level;
        std::string country;
        std::string status;  // "pending", "approved", "rejected"
        int64_t created_at;
        int64_t verified_at;
    };
    
    std::map<std::string, VerificationSession> sessions_;
    bool initialized_;
    
public:
    MockKYCProvider() : initialized_(false) {}
    
    const char* GetName() const override {
        return "MockKYC";
    }
    
    ProviderType GetType() const override {
        return ProviderType::MOCK;
    }
    
    bool Initialize(const std::string& config) override {
        // Mock provider doesn't need config
        initialized_ = true;
        return true;
    }
    
    int StartVerification(
        const std::string& user_id,
        const std::string& level,
        const std::string& country,
        std::string& verification_url_out
    ) override {
        if (!initialized_) {
            return -1;
        }
        
        // Generate session ID
        std::string session_id = "mock_session_" + std::to_string(std::time(nullptr));
        
        // Create session
        VerificationSession session;
        session.user_id = user_id;
        session.level = level;
        session.country = country;
        session.status = "pending";
        session.created_at = std::time(nullptr);
        session.verified_at = 0;
        
        sessions_[session_id] = session;
        
        // Generate mock verification URL
        verification_url_out = "mock://kyc-verify/" + session_id;
        
        return 0;
    }
    
    int GetStatus(
        const std::string& user_id,
        const std::string& session_id,
        FFI_KYCStatus& status_out
    ) override {
        if (!initialized_) {
            return -1;
        }
        
        // Find session
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            // Check by user_id if session_id empty
            if (session_id.empty()) {
                for (const auto& pair : sessions_) {
                    if (pair.second.user_id == user_id) {
                        const auto& session = pair.second;
                        
                        memset(&status_out, 0, sizeof(FFI_KYCStatus));
                        status_out.is_verified = (session.status == "approved");
                        
                        strncpy(status_out.verification_level, session.level.c_str(), 31);
                        status_out.verification_level[31] = '\0';
                        
                        strncpy(status_out.provider, "MockKYC", 31);
                        status_out.provider[31] = '\0';
                        
                        status_out.verified_at = session.verified_at;
                        status_out.expires_at = session.created_at + (365 * 24 * 60 * 60); // 1 year
                        
                        strncpy(status_out.country, session.country.c_str(), 2);
                        status_out.country[2] = '\0';
                        
                        return 0;
                    }
                }
            }
            
            // Not found
            memset(&status_out, 0, sizeof(FFI_KYCStatus));
            status_out.is_verified = false;
            strncpy(status_out.verification_level, "none", 31);
            return -1;
        }
        
        const auto& session = it->second;
        
        // Simulate verification (auto-approve after 5 seconds)
        if (session.status == "pending" && 
            (std::time(nullptr) - session.created_at) > 5) {
            sessions_[session_id].status = "approved";
            sessions_[session_id].verified_at = std::time(nullptr);
        }
        
        // Populate status
        memset(&status_out, 0, sizeof(FFI_KYCStatus));
        status_out.is_verified = (sessions_[session_id].status == "approved");
        
        strncpy(status_out.verification_level, session.level.c_str(), 31);
        status_out.verification_level[31] = '\0';
        
        strncpy(status_out.provider, "MockKYC", 31);
        status_out.provider[31] = '\0';
        
        status_out.verified_at = sessions_[session_id].verified_at;
        status_out.expires_at = session.created_at + (365 * 24 * 60 * 60); // 1 year
        
        strncpy(status_out.country, session.country.c_str(), 2);
        status_out.country[2] = '\0';
        
        return 0;
    }
    
    bool IsAvailable() const override {
        return initialized_;
    }
    
    void Cleanup() override {
        sessions_.clear();
        initialized_ = false;
    }
};

} // namespace kyc
} // namespace dinero

