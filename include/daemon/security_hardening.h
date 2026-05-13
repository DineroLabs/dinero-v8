#pragma once

#include <string>

namespace dinero {

/**
 * Ensures cookie file has secure permissions (0600 - owner read/write only)
 * @param cookie_path Path to the RPC authentication cookie file
 * @return true if permissions are secure or were successfully fixed
 */
bool EnsureCookiePermissions(const std::string& cookie_path);

/**
 * Validates that server is binding to loopback addresses only
 * @param bind_address The address the server is binding to
 * @param port The port number for logging
 * @return true if binding to loopback, false if external (logs security warning)
 */
bool ValidateLoopbackBinding(const std::string& bind_address, int port);

/**
 * Validates mining address file on daemon startup
 * Removes invalid addresses and logs security issues
 * @param datadir Path to daemon data directory
 * @return true if validation passed or no mining address configured
 */
bool ValidateMiningAddressOnStartup(const std::string& datadir);

/**
 * Scrubs authentication information from HTTP headers for safe logging
 * @param headers Raw HTTP headers string
 * @return Headers with Authorization and Cookie values redacted
 */
std::string ScrubAuthFromHeaders(const std::string& headers);

} // namespace dinero
