/**
 * KYC RPC Methods - vNext Architecture
 *
 * User verification and tier management
 */

#include "rpc/rpc_method_builder.h"
#include "p2p/kyc_manager.h"
#include "p2p/payment_adapter.h"
#include "common/logger.h"

namespace din {
namespace rpc {

namespace {

std::string ResolveUserPubkey(const Json& params, const std::string& fallback) {
    if (params.isObject() && params.isMember("user_pubkey") && params["user_pubkey"].isString()) {
        return params["user_pubkey"].asString();
    }
    return fallback;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════
// KYC RPC IMPLEMENTATIONS
// ═══════════════════════════════════════════════════════════════

Json kyc_getstatus_impl(const ExecutionContext& ctx, const Json& params) {
    const std::string user_pubkey = ResolveUserPubkey(params, "03test_user_pubkey");

    auto& kyc = din::p2p::KYCManager::instance();
    auto profile = kyc.getProfile(user_pubkey);

    Json result;
    result["user_pubkey"] = profile.user_pubkey;
    result["tier"] = static_cast<int>(profile.tier);

    // Tier name
    switch (profile.tier) {
        case din::p2p::KYCTier::UNVERIFIED:
            result["tier_name"] = "Unverified";
            break;
        case din::p2p::KYCTier::LIGHT_KYC:
            result["tier_name"] = "Light KYC";
            break;
        case din::p2p::KYCTier::FULL_KYC:
            result["tier_name"] = "Full KYC";
            break;
    }

    // Verification status
    result["email_verified"] = profile.email.has_value() &&
        profile.email->status == din::p2p::VerificationStatus::VERIFIED;
    result["phone_verified"] = profile.phone.has_value() &&
        profile.phone->status == din::p2p::VerificationStatus::VERIFIED;
    result["id_verified"] = profile.id_document.has_value() &&
        profile.id_document->status == din::p2p::VerificationStatus::VERIFIED;

    // Limits
    result["limits"] = profile.limits.toJson();

    // Current usage
    result["usage"]["volume_24h"] = profile.volume_24h;
    result["usage"]["volume_30d"] = profile.volume_30d;
    result["usage"]["active_trades"] = profile.active_trades_count;
    result["usage"]["remaining_daily"] = kyc.getRemainingDailyCapacity(user_pubkey);

    return result;
}

Json kyc_verifyemail_impl(const ExecutionContext& ctx, const Json& params) {
    std::string user_pubkey = ResolveUserPubkey(params, "03test_user");
    std::string email = params[0].asString();

    auto& kyc = din::p2p::KYCManager::instance();

    try {
        auto verification = kyc.sendEmailVerification(user_pubkey, email);

        Json result;
        result["success"] = true;
        result["email"] = email;
        result["code_sent"] = true;
        result["expires_in_minutes"] = 15;

        // Display hint (masked email)
        auto& payment_registry = din::p2p::PaymentAdapterRegistry::instance();
        result["display_hint"] = payment_registry.createDisplayHint("zelle", email);

        // For testing only - remove in production!
        #ifndef MAINNET_BUILD
        result["test_code"] = verification.verification_code;
        #endif

        return result;

    } catch (const std::exception& e) {
        Json error;
        error["success"] = false;
        error["error"] = e.what();
        return error;
    }
}

Json kyc_confirmcode_impl(const ExecutionContext& ctx, const Json& params) {
    std::string user_pubkey = ResolveUserPubkey(params, "03test_user");
    std::string code = params[0].asString();
    std::string verification_type = params.size() > 1 ? params[1].asString() : "email";

    auto& kyc = din::p2p::KYCManager::instance();

    bool verified = false;
    if (verification_type == "email") {
        verified = kyc.verifyEmail(user_pubkey, code);
    } else if (verification_type == "phone") {
        verified = kyc.verifyPhone(user_pubkey, code);
    }

    Json result;
    result["success"] = verified;

    if (verified) {
        result["message"] = verification_type + " verified successfully";

        // Check if tier upgraded
        auto profile = kyc.getProfile(user_pubkey);
        result["tier"] = static_cast<int>(profile.tier);
        result["tier_name"] = profile.tier == din::p2p::KYCTier::LIGHT_KYC ? "Light KYC" : "Unverified";
    } else {
        result["error"] = "Invalid or expired verification code";
    }

    return result;
}

Json kyc_verifyphone_impl(const ExecutionContext& ctx, const Json& params) {
    std::string user_pubkey = ResolveUserPubkey(params, "03test_user");
    std::string phone = params[0].asString();

    auto& kyc = din::p2p::KYCManager::instance();

    try {
        auto verification = kyc.sendPhoneVerification(user_pubkey, phone);

        Json result;
        result["success"] = true;
        result["phone"] = phone;
        result["code_sent"] = true;
        result["expires_in_minutes"] = 15;

        // Display hint (masked phone)
        auto& payment_registry = din::p2p::PaymentAdapterRegistry::instance();
        result["display_hint"] = payment_registry.createDisplayHint("zelle", phone);

        // For testing only
        #ifndef MAINNET_BUILD
        result["test_code"] = verification.verification_code;
        #endif

        return result;

    } catch (const std::exception& e) {
        Json error;
        error["success"] = false;
        error["error"] = e.what();
        return error;
    }
}

Json kyc_submitid_impl(const ExecutionContext& ctx, const Json& params) {
    std::string user_pubkey = ResolveUserPubkey(params, "03test_user");
    std::string document_type = params[0].asString();
    std::string document_data = params[1].asString();  // Base64 image
    std::string selfie_data = params[2].asString();    // Base64 selfie

    auto& kyc = din::p2p::KYCManager::instance();

    try {
        auto verification = kyc.submitIDVerification(
            user_pubkey,
            document_type,
            document_data,
            selfie_data
        );

        Json result;
        result["success"] = true;
        result["message"] = "ID verification submitted for review";
        result["status"] = "pending";
        result["submitted_at"] = static_cast<Json::Int64>(verification.submitted_at);

        return result;

    } catch (const std::exception& e) {
        Json error;
        error["success"] = false;
        error["error"] = e.what();
        return error;
    }
}

// ═══════════════════════════════════════════════════════════════
// PAYMENT METHODS
// ═══════════════════════════════════════════════════════════════

Json payment_listmethods_impl(const ExecutionContext& ctx, const Json& params) {
    auto& payment_registry = din::p2p::PaymentAdapterRegistry::instance();
    auto methods = payment_registry.listMethods();

    Json result;
    result["methods"] = ::Json::arrayValue;

    for (const auto& method : methods) {
        result["methods"].append(method.toJson());
    }

    return result;
}

Json payment_listbyregion_impl(const ExecutionContext& ctx, const Json& params) {
    std::string region_str = params[0].asString();

    din::p2p::PaymentRegion region = din::p2p::PaymentRegion::US_CANADA;
    if (region_str == "EU") region = din::p2p::PaymentRegion::EU;
    else if (region_str == "LATAM") region = din::p2p::PaymentRegion::LATAM;
    else if (region_str == "AFRICA") region = din::p2p::PaymentRegion::AFRICA;
    else if (region_str == "GLOBAL") region = din::p2p::PaymentRegion::GLOBAL;

    auto& payment_registry = din::p2p::PaymentAdapterRegistry::instance();
    auto methods = payment_registry.listMethodsByRegion(region);

    Json result;
    result["region"] = region_str;
    result["methods"] = ::Json::arrayValue;

    for (const auto& method : methods) {
        result["methods"].append(method.toJson());
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION
// ═══════════════════════════════════════════════════════════════

void registerKYCMethodsVNext() {
    // KYC Status & Management
    RPC_METHOD("kyc.getstatus", "kyc")
        .description("Get user's KYC tier, limits, and current usage")
        .result("object", "KYC profile with tier, limits, and usage statistics")
        .handler(kyc_getstatus_impl)
        .examples({
            "kyc.getstatus"
        });

    RPC_METHOD("kyc.verifyemail", "kyc")
        .description("Start email verification process")
        .param("email", "string", "Email address to verify", true)
        .result("object", "Verification code sent confirmation")
        .handler(kyc_verifyemail_impl)
        .examples({
            "kyc.verifyemail \"user@example.com\""
        });

    RPC_METHOD("kyc.verifyphone", "kyc")
        .description("Start phone verification process")
        .param("phone", "string", "Phone number to verify (E.164 format)", true)
        .result("object", "SMS code sent confirmation")
        .handler(kyc_verifyphone_impl)
        .examples({
            "kyc.verifyphone \"+15551234567\""
        });

    RPC_METHOD("kyc.confirmcode", "kyc")
        .description("Confirm email or phone verification with code")
        .param("code", "string", "6-digit verification code", true)
        .param("type", "string", "Verification type: 'email' or 'phone' (default: email)", false)
        .result("object", "Verification result and tier upgrade status")
        .handler(kyc_confirmcode_impl)
        .examples({
            "kyc.confirmcode \"123456\"",
            "kyc.confirmcode \"123456\" \"phone\""
        });

    RPC_METHOD("kyc.submitid", "kyc")
        .description("Submit ID documents for Full KYC verification")
        .param("document_type", "string", "Type: 'passport', 'drivers_license', 'national_id'", true)
        .param("document_data", "string", "Base64 encoded document image", true)
        .param("selfie_data", "string", "Base64 encoded selfie image", true)
        .result("object", "Submission confirmation")
        .handler(kyc_submitid_impl)
        .examples({
            "kyc.submitid \"passport\" \"<base64>\" \"<base64>\""
        });

    // Payment Methods
    RPC_METHOD("payment.listmethods", "payment")
        .description("List all available payment methods")
        .result("array", "Array of payment method details")
        .handler(payment_listmethods_impl)
        .examples({
            "payment.listmethods"
        });

    RPC_METHOD("payment.listbyregion", "payment")
        .description("List payment methods available in specific region")
        .param("region", "string", "Region: 'US_CANADA', 'EU', 'LATAM', 'AFRICA', 'GLOBAL'", true)
        .result("array", "Array of regional payment methods")
        .handler(payment_listbyregion_impl)
        .examples({
            "payment.listbyregion \"US_CANADA\""
        });

    dinero::g_logger.info("[KYC RPC] Registered 7 KYC and payment methods");
}

// NOTE: Auto-registration DISABLED to avoid static initialization issues
// Call registerKYCMethodsVNext() explicitly from main.cpp after initializing managers
// static auto _kyc_vnext_init = (din::rpc::registerKYCMethodsVNext(), 0);

} // namespace rpc
} // namespace din
