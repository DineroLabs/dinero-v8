// SPDX-License-Identifier: MIT
// Dinero - Authentication Security Header

#pragma once
#include <string>

namespace AuthSecurity {

/**
 * Check if cookie file has secure permissions (0600)
 * @param cookie_path Path to cookie file
 * @return true if permissions are secure, false otherwise
 */
bool check_cookie_permissions(const std::string& cookie_path);

/**
 * Get human-readable error message for HTTP authentication failures
 * @param http_code HTTP status code
 * @param response_body HTTP response body (truncated)
 * @return Descriptive error message
 */
std::string get_auth_error_message(int http_code, const std::string& response_body);

/**
 * Validate cookie file content format
 * @param cookie_content Raw cookie content
 * @return true if format is valid, false otherwise
 */
bool validate_cookie_content(const std::string& cookie_content);

/**
 * Securely read cookie file with validation
 * @param cookie_path Path to cookie file
 * @return Cookie content if valid
 * @throws std::runtime_error on validation failure
 */
std::string read_cookie_file_secure(const std::string& cookie_path);

/**
 * Ensure cookie file has secure permissions, fix if needed
 * @param cookie_path Path to cookie file
 * @return true if secure or successfully fixed, false otherwise
 */
bool ensure_cookie_security(const std::string& cookie_path);

} // namespace AuthSecurity
