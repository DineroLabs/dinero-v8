#pragma once
#include <string>
#include <filesystem>

namespace dinero::rpc_auth {

namespace fs = std::filesystem;

/**
 * Cookie authentication for RPC server
 * 
 * Policy: if rpcauth present → use that; else cookie mode:
 * - On start: if .cookie exists, read it; otherwise generate and write it (0600)
 * - Keep username fixed as __cookie__ and password = 32 random bytes hex
 * - On shutdown: delete it
 */

/**
 * Get the cookie file path for a given datadir
 */
fs::path CookiePath(const fs::path& datadir);

/**
 * Read cookie credentials from file
 * @param path Path to cookie file
 * @param user Output username (should be "__cookie__")
 * @param pass Output password (32-byte hex string)
 * @return true if cookie was read successfully
 */
bool ReadCookie(const fs::path& path, std::string& user, std::string& pass);

/**
 * Write new cookie credentials to file
 * @param path Path to cookie file
 * @param user Output username (will be set to "__cookie__")
 * @param pass Output password (will be set to 32 random bytes hex)
 * @return true if cookie was written successfully
 */
bool WriteCookie(const fs::path& path, std::string& user, std::string& pass);

/**
 * Delete cookie file
 * @param path Path to cookie file
 */
void DeleteCookie(const fs::path& path);

/**
 * Check if cookie authentication should be used
 * @param datadir Path to data directory
 * @return true if cookie auth should be used (no rpcauth in config)
 */
bool ShouldUseCookieAuth(const fs::path& datadir);

} // namespace dinero::rpc_auth
