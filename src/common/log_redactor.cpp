#include "common/log_redactor.h"
#include <openssl/crypto.h>
#include <json/writer.h>
#include <cstring>

namespace dinero {

const std::unordered_set<std::string> LogRedactor::DEFAULT_SENSITIVE_FIELDS = {
    "passphrase", "password", "private_key", "privkey", "key", "seed", "mnemonic",
    "ct", "ciphertext", "iv", "tag", "salt", "auth_tag", "encrypted_key",
    "wif", "hex_key", "master_key", "xprv", "cookie", "token", "bearer",
    "authorization", "secret", "entropy"
};

std::string LogRedactor::RedactSensitive(const Json::Value& json) {
    Json::Value redacted = RedactJsonValue(json);
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, redacted);
}

std::string LogRedactor::RedactSensitive(const std::string& json_str) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(json_str);
    
    if (!Json::parseFromStream(builder, stream, &root, &errs)) {
        // If parsing fails, redact common patterns in raw string
        std::string result = json_str;
        for (const auto& field : GetSensitiveFields()) {
            size_t pos = 0;
            std::string pattern = "\"" + field + "\":";
            while ((pos = result.find(pattern, pos)) != std::string::npos) {
                size_t start = pos + pattern.length();
                size_t end = result.find_first_of(",}", start);
                if (end != std::string::npos) {
                    result.replace(start, end - start, "\"**REDACTED**\"");
                    pos = start + 13; // length of "**REDACTED**"
                } else {
                    break;
                }
            }
        }
        return result;
    }
    
    return RedactSensitive(root);
}

void LogRedactor::AddSensitiveField(const std::string& field_name) {
    // Note: This modifies a const set, which is not thread-safe
    // In production, consider using a thread-safe container
    const_cast<std::unordered_set<std::string>&>(GetSensitiveFields()).insert(field_name);
}

Json::Value LogRedactor::RedactJsonValue(const Json::Value& value) {
    if (value.isObject()) {
        Json::Value result(Json::objectValue);
        for (const auto& key : value.getMemberNames()) {
            if (GetSensitiveFields().count(key)) {
                result[key] = "**REDACTED**";
            } else {
                result[key] = RedactJsonValue(value[key]);
            }
        }
        return result;
    } else if (value.isArray()) {
        Json::Value result(Json::arrayValue);
        for (const auto& item : value) {
            result.append(RedactJsonValue(item));
        }
        return result;
    } else {
        return value; // Primitive values are returned as-is
    }
}

const std::unordered_set<std::string>& LogRedactor::GetSensitiveFields() {
    return DEFAULT_SENSITIVE_FIELDS;
}

// SecureString implementation
SecureString::SecureString(size_t size) : size_(size) {
    data_ = new char[size_];
    std::memset(data_, 0, size_);
}

SecureString::SecureString(const std::string& str) : size_(str.size()) {
    data_ = new char[size_];
    std::memcpy(data_, str.data(), size_);
}

SecureString::~SecureString() {
    if (data_) {
        OPENSSL_cleanse(data_, size_);
        delete[] data_;
        data_ = nullptr;
    }
}

} // namespace dinero
