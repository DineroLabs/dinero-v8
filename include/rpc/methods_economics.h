#pragma once

namespace din {
namespace rpc {

// Register economics & telemetry RPC methods in vNext (Pure DSL)
void registerEconomicsMethodsVNext();

} // namespace rpc
} // namespace din

// Legacy registration (deprecated)
void registerEconomicsRPC();
