#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include "json/json.h"

namespace dinero {

/**
 * Log Redactor - prevents sensitive data from appearing in logs
 * 
 * Usage:
 *   Json::Value request = ...;
 *   auto safe = LogRedactor::RedactSensitive(request);
 *   g_logger.info("RPC request: " + safe);
 */
class LogRedactor {
public:
    // Redact sensitive fields from JSON for safe logging
    static std::string RedactSensitive(const Json::Value& json);
    
    // Redact sensitive fields from raw JSON string
    static std::string RedactSensitive(const std::string& json_str);
    
    // Add custom sensitive field names
    static void AddSensitiveField(const std::string& field_name);
    
private:
    static Json::Value RedactJsonValue(const Json::Value& value);
    static const std::unordered_set<std::string>& GetSensitiveFields();
    
    // Default sensitive field names
    static const std::unordered_set<std::string> DEFAULT_SENSITIVE_FIELDS;
};

// RAII helper for secure string handling
class SecureString {
public:
    explicit SecureString(size_t size);
    explicit SecureString(const std::string& str);
    ~SecureString();
    
    // No copy/move to prevent accidental duplication
    SecureString(const SecureString&) = delete;
    SecureString& operator=(const SecureString&) = delete;
    SecureString(SecureString&&) = delete;
    SecureString& operator=(SecureString&&) = delete;
    
    char* data() { return data_; }
    const char* data() const { return data_; }
    size_t size() const { return size_; }
    
    std::string str() const { return std::string(data_, size_); }
    
private:
    char* data_;
    size_t size_;
};

} // namespace dinero
