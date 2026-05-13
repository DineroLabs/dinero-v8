#include "daemon/execution_context.h"
#include <json/json.h>
#include <stdexcept>

namespace din::rpc {
namespace {

[[noreturn]] void ThrowSilentPaymentsUnavailable() {
  throw std::runtime_error(
      "Silent Payments RPC is unavailable: wallet backend integration is not wired in this build");
}

}  // namespace

// walletgetnewspaddress - Generate new Silent Payment address
Json::Value walletgetnewspaddress(ExecutionContext& ctx, const Json::Value& req) {
  (void)ctx;
  (void)req;
  ThrowSilentPaymentsUnavailable();
}

// sendtosilent - Send to Silent Payment address
Json::Value sendtosilent(ExecutionContext& ctx, const Json::Value& req) {
  (void)ctx;
  (void)req;
  ThrowSilentPaymentsUnavailable();
}

// silentpaymentscan - Scan for Silent Payment outputs
Json::Value silentpaymentscan(ExecutionContext& ctx, const Json::Value& req) {
  (void)ctx;
  (void)req;
  ThrowSilentPaymentsUnavailable();
}

}  // namespace din::rpc
