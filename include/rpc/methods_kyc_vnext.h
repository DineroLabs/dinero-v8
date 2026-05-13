/**
 * KYC RPC Methods - vNext Architecture
 * Header for registration function
 */

#pragma once

namespace din {
namespace rpc {

/**
 * Register KYC and payment method RPC handlers
 * Must be called AFTER KYCManager is initialized
 */
void registerKYCMethodsVNext();

} // namespace rpc
} // namespace din
