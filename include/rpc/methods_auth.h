#pragma once

namespace dinero {
namespace rpc {

/**
 * @brief Register authentication RPC methods
 *
 * Methods registered:
 * - auth.requesttoken: Generate new access token
 * - auth.refreshtoken: Manually refresh token lifetime
 * - auth.revoketoken: Revoke (delete) a token
 * - auth.whoami: Get current client info
 * - auth.stats: Get token statistics
 */
void registerAuthMethods();

} // namespace rpc
} // namespace dinero
