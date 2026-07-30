#pragma once

#include "din_json.h"
#include "rpc/rpc_registry.h"

namespace dinero::rpc {

enum class WalletCcvRpcGate {
  Disabled,
  Enabled,
  RefusedNonRegtest,
};

WalletCcvRpcGate EvaluateWalletCcvRpcGate(bool requested, bool isRegtest);
WalletCcvRpcGate EvaluateWalletCcvRpcGateForSelectedChain(bool requested);

din::Json HandleWalletCcvCreateOutput(const ExecutionContext &ctx,
                                      const din::Json &params);

din::Json HandleWalletCcvBuildTransition(const ExecutionContext &ctx,
                                         const din::Json &params);

bool RegisterWalletCcvMethods();

} // namespace dinero::rpc
