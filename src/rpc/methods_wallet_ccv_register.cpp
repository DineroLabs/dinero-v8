#include "rpc/methods_wallet_ccv.h"

namespace dinero::rpc {

bool RegisterWalletCcvMethods() {
  constexpr const char *CREATE_METHOD = "wallet.ccv.createoutput";
  constexpr const char *TRANSITION_METHOD = "wallet.ccv.buildtransition";

  // Registration happens during single-threaded startup. Check both names first
  // so a collision cannot leave only half of this versioned API installed.
  if (g_rpcRegistry.has(CREATE_METHOD) ||
      g_rpcRegistry.has(TRANSITION_METHOD)) {
    return false;
  }

  const bool createRegistered = g_rpcRegistry.registerHandler(
      CREATE_METHOD, HandleWalletCcvCreateOutput, "wallet-ccv-v1");
  const bool transitionRegistered = g_rpcRegistry.registerHandler(
      TRANSITION_METHOD, HandleWalletCcvBuildTransition, "wallet-ccv-v1");
  return createRegistered && transitionRegistered;
}

} // namespace dinero::rpc
