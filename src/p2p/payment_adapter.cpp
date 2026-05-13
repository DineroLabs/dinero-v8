/**
 * Payment Adapter Implementation
 */

#include "p2p/payment_adapter.h"
#include <regex>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace din {
namespace p2p {

// ═══════════════════════════════════════════════════════════════
// JSON CONVERSION
// ═══════════════════════════════════════════════════════════════

Json PaymentMethodInfo::toJson() const {
    Json j;
    j["id"] = id;
    j["display_name"] = display_name;
    j["icon_emoji"] = icon_emoji;
    j["region"] = static_cast<int>(region);
    j["typical_settlement_minutes"] = typical_settlement_minutes;
    j["requires_phone"] = requires_phone;
    j["requires_email"] = requires_email;
    j["requires_account_name"] = requires_account_name;
    j["supports_instant"] = supports_instant;
    j["handle_placeholder"] = handle_placeholder;
    j["supported_currencies"] = ::Json::arrayValue;
    for (const auto& currency : supported_currencies) {
        j["supported_currencies"].append(currency);
    }
    return j;
}

PaymentMethodInfo PaymentMethodInfo::fromJson(const Json& j) {
    PaymentMethodInfo info;
    info.id = j["id"].asString();
    info.display_name = j["display_name"].asString();
    info.icon_emoji = j["icon_emoji"].asString();
    info.region = static_cast<PaymentRegion>(j["region"].asInt());
    info.typical_settlement_minutes = j["typical_settlement_minutes"].asInt();
    info.requires_phone = j["requires_phone"].asBool();
    info.requires_email = j["requires_email"].asBool();
    info.requires_account_name = j["requires_account_name"].asBool();
    info.supports_instant = j["supports_instant"].asBool();
    info.handle_placeholder = j["handle_placeholder"].asString();

    info.supported_currencies.clear();
    if (j.isMember("supported_currencies")) {
        for (const auto& currency : j["supported_currencies"]) {
            info.supported_currencies.push_back(currency.asString());
        }
    }
    return info;
}

Json PaymentHandle::toJson() const {
    Json j;
    j["method_id"] = method_id;
    j["encrypted_handle"] = encrypted_handle;
    j["handle_hash"] = handle_hash;
    j["display_hint"] = display_hint;
    j["verified"] = verified;
    j["added_at"] = static_cast<Json::Int64>(added_at);
    j["last_used_at"] = static_cast<Json::Int64>(last_used_at);
    return j;
}

PaymentHandle PaymentHandle::fromJson(const Json& j) {
    PaymentHandle handle;
    handle.method_id = j["method_id"].asString();
    handle.encrypted_handle = j["encrypted_handle"].asString();
    handle.handle_hash = j["handle_hash"].asString();
    handle.display_hint = j["display_hint"].asString();
    handle.verified = j["verified"].asBool();
    handle.added_at = j["added_at"].asInt64();
    handle.last_used_at = j.isMember("last_used_at") ? j["last_used_at"].asInt64() : 0;
    return handle;
}

// ═══════════════════════════════════════════════════════════════
// ZELLE ADAPTER (US BANK-TO-BANK)
// ═══════════════════════════════════════════════════════════════

PaymentMethodInfo ZelleAdapter::getInfo() const {
    return {
        "zelle",
        "Zelle",
        "💵",
        PaymentRegion::US_CANADA,
        5,  // ~5 minutes typical
        true,  // requires phone or email
        true,
        false,
        true,  // instant
        "Phone number or email",
        {"USD"}
    };
}

bool ZelleAdapter::validateHandle(const std::string& handle) const {
    // Accept phone number or email
    std::regex phone_regex(R"(^\+?1?[2-9]\d{9}$)");  // US phone
    std::regex email_regex(R"(^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$)");

    return std::regex_match(handle, phone_regex) || std::regex_match(handle, email_regex);
}

std::string ZelleAdapter::createDisplayHint(const std::string& handle) const {
    if (handle.find('@') != std::string::npos) {
        // Email: show first 2 chars and domain
        auto at_pos = handle.find('@');
        return handle.substr(0, 2) + "***@" + handle.substr(at_pos + 1);
    } else {
        // Phone: show last 4 digits
        std::string digits_only;
        for (char c : handle) {
            if (std::isdigit(c)) digits_only += c;
        }
        if (digits_only.length() >= 4) {
            return "***-***-" + digits_only.substr(digits_only.length() - 4);
        }
        return "***-***-****";
    }
}

std::string ZelleAdapter::generateInstructions(
    const std::string& handle,
    double amount,
    const std::string& currency,
    const std::string& reference
) const {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "📱 Send payment via Zelle:\n\n";
    ss << "Recipient: " << handle << "\n";
    ss << "Amount: $" << amount << " " << currency << "\n";
    ss << "Payment Note: \"" << reference << "\"\n\n";
    ss << "⚠️ IMPORTANT:\n";
    ss << "1. Open your banking app\n";
    ss << "2. Go to Zelle/Send Money\n";
    ss << "3. Enter recipient exactly as shown\n";
    ss << "4. Include the payment note for verification\n";
    ss << "5. Send payment and save confirmation\n";
    ss << "6. Return here and click 'I Have Sent Payment'\n\n";
    ss << "⏱️ Typical delivery: 1-5 minutes";
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════
// CASH APP ADAPTER
// ═══════════════════════════════════════════════════════════════

PaymentMethodInfo CashAppAdapter::getInfo() const {
    return {
        "cashapp",
        "Cash App",
        "💸",
        PaymentRegion::US_CANADA,
        1,  // Instant
        true,
        false,
        false,
        true,
        "$cashtag or phone",
        {"USD"}
    };
}

bool CashAppAdapter::validateHandle(const std::string& handle) const {
    // Accept $cashtag or phone number
    std::regex cashtag_regex(R"(^\$[a-zA-Z0-9_]{1,20}$)");
    std::regex phone_regex(R"(^\+?1?[2-9]\d{9}$)");

    return std::regex_match(handle, cashtag_regex) || std::regex_match(handle, phone_regex);
}

std::string CashAppAdapter::createDisplayHint(const std::string& handle) const {
    if (handle[0] == '$') {
        // Cashtag: show first 3 chars
        if (handle.length() > 5) {
            return handle.substr(0, 3) + "***" + handle.substr(handle.length() - 2);
        }
        return handle;
    } else {
        // Phone
        std::string digits_only;
        for (char c : handle) {
            if (std::isdigit(c)) digits_only += c;
        }
        if (digits_only.length() >= 4) {
            return "***-***-" + digits_only.substr(digits_only.length() - 4);
        }
        return "***-***-****";
    }
}

std::string CashAppAdapter::generateInstructions(
    const std::string& handle,
    double amount,
    const std::string& currency,
    const std::string& reference
) const {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "💸 Send payment via Cash App:\n\n";
    ss << "Recipient: " << handle << "\n";
    ss << "Amount: $" << amount << " " << currency << "\n";
    ss << "Note: \"" << reference << "\"\n\n";
    ss << "⚠️ STEPS:\n";
    ss << "1. Open Cash App\n";
    ss << "2. Tap 'Send' ($ icon)\n";
    ss << "3. Enter " << handle << "\n";
    ss << "4. Enter $" << amount << "\n";
    ss << "5. Add note: " << reference << "\n";
    ss << "6. Tap 'Pay' and confirm\n";
    ss << "7. Screenshot the confirmation\n";
    ss << "8. Return here and upload proof\n\n";
    ss << "⚡ Instant delivery";
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════
// VENMO ADAPTER
// ═══════════════════════════════════════════════════════════════

PaymentMethodInfo VenmoAdapter::getInfo() const {
    return {
        "venmo",
        "Venmo",
        "💳",
        PaymentRegion::US_CANADA,
        1,
        true,
        false,
        false,
        true,
        "@username or phone",
        {"USD"}
    };
}

bool VenmoAdapter::validateHandle(const std::string& handle) const {
    std::regex username_regex(R"(^@?[a-zA-Z0-9_-]{1,30}$)");
    std::regex phone_regex(R"(^\+?1?[2-9]\d{9}$)");

    return std::regex_match(handle, username_regex) || std::regex_match(handle, phone_regex);
}

std::string VenmoAdapter::createDisplayHint(const std::string& handle) const {
    if (handle[0] == '@') {
        if (handle.length() > 5) {
            return handle.substr(0, 3) + "***" + handle.substr(handle.length() - 2);
        }
        return handle;
    } else {
        std::string digits_only;
        for (char c : handle) {
            if (std::isdigit(c)) digits_only += c;
        }
        if (digits_only.length() >= 4) {
            return "***-***-" + digits_only.substr(digits_only.length() - 4);
        }
        return "***-***-****";
    }
}

std::string VenmoAdapter::generateInstructions(
    const std::string& handle,
    double amount,
    const std::string& currency,
    const std::string& reference
) const {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "💳 Send payment via Venmo:\n\n";
    ss << "Recipient: " << handle << "\n";
    ss << "Amount: $" << amount << " " << currency << "\n";
    ss << "Note: \"" << reference << "\"\n\n";
    ss << "⚠️ STEPS:\n";
    ss << "1. Open Venmo app\n";
    ss << "2. Tap 'Pay or Request'\n";
    ss << "3. Search for " << handle << "\n";
    ss << "4. Enter $" << amount << "\n";
    ss << "5. Add note: " << reference << "\n";
    ss << "6. Set to 'Private'\n";
    ss << "7. Tap 'Pay' and confirm\n";
    ss << "8. Screenshot confirmation\n\n";
    ss << "⚡ Instant delivery";
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════
// APPLE PAY ADAPTER
// ═══════════════════════════════════════════════════════════════

PaymentMethodInfo ApplePayAdapter::getInfo() const {
    return {
        "applepay",
        "Apple Pay (iMessage)",
        "🍎",
        PaymentRegion::US_CANADA,
        1,
        true,
        false,
        false,
        true,
        "Phone number or email",
        {"USD"}
    };
}

bool ApplePayAdapter::validateHandle(const std::string& handle) const {
    std::regex phone_regex(R"(^\+?1?[2-9]\d{9}$)");
    std::regex email_regex(R"(^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$)");

    return std::regex_match(handle, phone_regex) || std::regex_match(handle, email_regex);
}

std::string ApplePayAdapter::createDisplayHint(const std::string& handle) const {
    if (handle.find('@') != std::string::npos) {
        auto at_pos = handle.find('@');
        return handle.substr(0, 2) + "***@" + handle.substr(at_pos + 1);
    } else {
        std::string digits_only;
        for (char c : handle) {
            if (std::isdigit(c)) digits_only += c;
        }
        if (digits_only.length() >= 4) {
            return "***-***-" + digits_only.substr(digits_only.length() - 4);
        }
        return "***-***-****";
    }
}

std::string ApplePayAdapter::generateInstructions(
    const std::string& handle,
    double amount,
    const std::string& currency,
    const std::string& reference
) const {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "🍎 Send payment via Apple Pay:\n\n";
    ss << "Recipient: " << handle << "\n";
    ss << "Amount: $" << amount << " " << currency << "\n";
    ss << "Note: \"" << reference << "\"\n\n";
    ss << "⚠️ STEPS:\n";
    ss << "1. Open Messages (iMessage)\n";
    ss << "2. Start conversation with " << handle << "\n";
    ss << "3. Tap Apple Pay button\n";
    ss << "4. Enter $" << amount << "\n";
    ss << "5. Tap 'Pay' and authenticate\n";
    ss << "6. Screenshot confirmation\n\n";
    ss << "⚡ Instant delivery\n";
    ss << "📱 Note: Recipient must have Apple Pay Cash enabled";
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════
// GOOGLE PAY ADAPTER
// ═══════════════════════════════════════════════════════════════

PaymentMethodInfo GooglePayAdapter::getInfo() const {
    return {
        "googlepay",
        "Google Pay",
        "🔵",
        PaymentRegion::US_CANADA,
        1,
        true,
        true,
        false,
        true,
        "Phone or email",
        {"USD"}
    };
}

bool GooglePayAdapter::validateHandle(const std::string& handle) const {
    std::regex phone_regex(R"(^\+?1?[2-9]\d{9}$)");
    std::regex email_regex(R"(^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$)");

    return std::regex_match(handle, phone_regex) || std::regex_match(handle, email_regex);
}

std::string GooglePayAdapter::createDisplayHint(const std::string& handle) const {
    if (handle.find('@') != std::string::npos) {
        auto at_pos = handle.find('@');
        return handle.substr(0, 2) + "***@" + handle.substr(at_pos + 1);
    } else {
        std::string digits_only;
        for (char c : handle) {
            if (std::isdigit(c)) digits_only += c;
        }
        if (digits_only.length() >= 4) {
            return "***-***-" + digits_only.substr(digits_only.length() - 4);
        }
        return "***-***-****";
    }
}

std::string GooglePayAdapter::generateInstructions(
    const std::string& handle,
    double amount,
    const std::string& currency,
    const std::string& reference
) const {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "🔵 Send payment via Google Pay:\n\n";
    ss << "Recipient: " << handle << "\n";
    ss << "Amount: $" << amount << " " << currency << "\n";
    ss << "Note: \"" << reference << "\"\n\n";
    ss << "⚠️ STEPS:\n";
    ss << "1. Open Google Pay app\n";
    ss << "2. Tap 'New Payment'\n";
    ss << "3. Select 'Send money to friends'\n";
    ss << "4. Enter " << handle << "\n";
    ss << "5. Enter $" << amount << "\n";
    ss << "6. Add memo: " << reference << "\n";
    ss << "7. Tap 'Send' and confirm\n";
    ss << "8. Screenshot confirmation\n\n";
    ss << "⚡ Instant delivery";
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════
// INTERAC ADAPTER (CANADA)
// ═══════════════════════════════════════════════════════════════

PaymentMethodInfo InteracAdapter::getInfo() const {
    return {
        "interac",
        "Interac e-Transfer",
        "🇨🇦",
        PaymentRegion::US_CANADA,
        30,  // ~30 minutes typical
        false,
        true,
        false,
        false,
        "Email address",
        {"CAD"}
    };
}

bool InteracAdapter::validateHandle(const std::string& handle) const {
    std::regex email_regex(R"(^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$)");
    return std::regex_match(handle, email_regex);
}

std::string InteracAdapter::createDisplayHint(const std::string& handle) const {
    auto at_pos = handle.find('@');
    return handle.substr(0, 2) + "***@" + handle.substr(at_pos + 1);
}

std::string InteracAdapter::generateInstructions(
    const std::string& handle,
    double amount,
    const std::string& currency,
    const std::string& reference
) const {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "🇨🇦 Send payment via Interac e-Transfer:\n\n";
    ss << "Recipient Email: " << handle << "\n";
    ss << "Amount: $" << amount << " " << currency << "\n";
    ss << "Security Question Answer: " << reference << "\n\n";
    ss << "⚠️ STEPS:\n";
    ss << "1. Log into your Canadian bank's online banking\n";
    ss << "2. Go to 'Interac e-Transfer' or 'Send Money'\n";
    ss << "3. Add recipient: " << handle << "\n";
    ss << "4. Enter amount: $" << amount << "\n";
    ss << "5. Set security question: 'Trade reference?'\n";
    ss << "6. Set answer: " << reference << "\n";
    ss << "7. Send transfer and save confirmation\n";
    ss << "8. Return here with confirmation number\n\n";
    ss << "⏱️ Typical delivery: 30 minutes";
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════
// BANK TRANSFER ADAPTER (GENERIC)
// ═══════════════════════════════════════════════════════════════

PaymentMethodInfo BankTransferAdapter::getInfo() const {
    return {
        "banktransfer",
        "Bank Transfer",
        "🏦",
        PaymentRegion::GLOBAL,
        1440,  // 24 hours
        false,
        false,
        true,
        false,
        "Account details",
        {"USD", "EUR", "GBP", "CAD"}
    };
}

bool BankTransferAdapter::validateHandle(const std::string& handle) const {
    // Accept any non-empty string (bank details vary by country)
    return !handle.empty() && handle.length() >= 10;
}

std::string BankTransferAdapter::createDisplayHint(const std::string& handle) const {
    if (handle.length() > 10) {
        return handle.substr(0, 4) + "***..." + handle.substr(handle.length() - 4);
    }
    return "Bank account ***";
}

std::string BankTransferAdapter::generateInstructions(
    const std::string& handle,
    double amount,
    const std::string& currency,
    const std::string& reference
) const {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "🏦 Send payment via Bank Transfer:\n\n";
    ss << "Bank Details:\n" << handle << "\n\n";
    ss << "Amount: $" << amount << " " << currency << "\n";
    ss << "Reference: " << reference << "\n\n";
    ss << "⚠️ IMPORTANT:\n";
    ss << "1. Log into your online banking\n";
    ss << "2. Initiate wire/ACH transfer\n";
    ss << "3. Enter recipient bank details exactly as shown\n";
    ss << "4. Include reference code in payment notes\n";
    ss << "5. Submit transfer\n";
    ss << "6. Save bank's confirmation receipt\n";
    ss << "7. Upload receipt as proof of payment\n\n";
    ss << "⏱️ Settlement: 1-3 business days\n";
    ss << "💰 May incur bank fees";
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════
// PAYMENT ADAPTER REGISTRY
// ═══════════════════════════════════════════════════════════════

std::unique_ptr<PaymentAdapterRegistry> PaymentAdapterRegistry::instance_;

PaymentAdapterRegistry& PaymentAdapterRegistry::instance() {
    if (!instance_) {
        instance_.reset(new PaymentAdapterRegistry());
    }
    return *instance_;
}

PaymentAdapterRegistry::PaymentAdapterRegistry() {
    registerDefaultAdapters();
}

void PaymentAdapterRegistry::registerDefaultAdapters() {
    registerAdapter("zelle", std::make_unique<ZelleAdapter>());
    registerAdapter("cashapp", std::make_unique<CashAppAdapter>());
    registerAdapter("venmo", std::make_unique<VenmoAdapter>());
    registerAdapter("applepay", std::make_unique<ApplePayAdapter>());
    registerAdapter("googlepay", std::make_unique<GooglePayAdapter>());
    registerAdapter("interac", std::make_unique<InteracAdapter>());
    registerAdapter("banktransfer", std::make_unique<BankTransferAdapter>());
}

void PaymentAdapterRegistry::registerAdapter(const std::string& method_id, std::unique_ptr<PaymentAdapter> adapter) {
    adapters_[method_id] = std::move(adapter);
}

PaymentAdapter* PaymentAdapterRegistry::getAdapter(const std::string& method_id) const {
    auto it = adapters_.find(method_id);
    return (it != adapters_.end()) ? it->second.get() : nullptr;
}

std::vector<PaymentMethodInfo> PaymentAdapterRegistry::listMethods() const {
    std::vector<PaymentMethodInfo> methods;
    for (const auto& [id, adapter] : adapters_) {
        methods.push_back(adapter->getInfo());
    }
    return methods;
}

std::vector<PaymentMethodInfo> PaymentAdapterRegistry::listMethodsByRegion(PaymentRegion region) const {
    std::vector<PaymentMethodInfo> methods;
    for (const auto& [id, adapter] : adapters_) {
        auto info = adapter->getInfo();
        if (info.region == region || info.region == PaymentRegion::GLOBAL) {
            methods.push_back(info);
        }
    }
    return methods;
}

bool PaymentAdapterRegistry::validateHandle(const std::string& method_id, const std::string& handle) const {
    auto adapter = getAdapter(method_id);
    return adapter ? adapter->validateHandle(handle) : false;
}

std::string PaymentAdapterRegistry::createDisplayHint(const std::string& method_id, const std::string& handle) const {
    auto adapter = getAdapter(method_id);
    return adapter ? adapter->createDisplayHint(handle) : "***";
}

std::string PaymentAdapterRegistry::generateInstructions(
    const std::string& method_id,
    const std::string& handle,
    double amount,
    const std::string& currency,
    const std::string& reference
) const {
    auto adapter = getAdapter(method_id);
    return adapter ? adapter->generateInstructions(handle, amount, currency, reference) : "";
}

} // namespace p2p
} // namespace din
