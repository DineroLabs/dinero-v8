/**
 * Marketplace RPC method implementations.
 *
 * The marketplace subsystem is compiled in RPC wiring, but the runtime backend
 * is intentionally disabled in this build. Methods fail closed with explicit
 * machine-readable errors instead of returning fabricated data.
 */

#include "rpc/rpc_method_builder.h"
#include "p2p/marketplace_manager.h"
#include <json/json.h>

namespace din {
namespace rpc {

namespace {
din::Json MarketplaceUnavailable() {
    din::Json result;
    result["error"]["code"] = -32601;
    result["error"]["message"] = "Marketplace RPC is unavailable in this build";
    result["available"] = false;
    return result;
}
}  // namespace

din::Json market_createoffer_impl(const ExecutionContext& ctx, const din::Json& params) {
    return MarketplaceUnavailable();
}

din::Json market_canceloffer_impl(const ExecutionContext& ctx, const din::Json& params) {
    return MarketplaceUnavailable();
}

din::Json market_updateoffer_impl(const ExecutionContext& ctx, const din::Json& params) {
    return MarketplaceUnavailable();
}

din::Json market_listoffers_impl(const ExecutionContext& ctx, const din::Json& params) {
    return MarketplaceUnavailable();
}

din::Json market_getoffer_impl(const ExecutionContext& ctx, const din::Json& params) {
    return MarketplaceUnavailable();
}

din::Json market_search_impl(const ExecutionContext& ctx, const din::Json& params) {
    return MarketplaceUnavailable();
}

din::Json market_acceptoffer_impl(const ExecutionContext& ctx, const din::Json& params) {
    return MarketplaceUnavailable();
}

din::Json market_completetrade_impl(const ExecutionContext& ctx, const din::Json& params) {
    return MarketplaceUnavailable();
}

din::Json market_disputetrade_impl(const ExecutionContext& ctx, const din::Json& params) {
    return MarketplaceUnavailable();
}

din::Json market_getreputation_impl(const ExecutionContext& ctx, const din::Json& params) {
    return MarketplaceUnavailable();
}

din::Json market_myoffers_impl(const ExecutionContext& ctx, const din::Json& params) {
    return MarketplaceUnavailable();
}

din::Json market_mytrades_impl(const ExecutionContext& ctx, const din::Json& params) {
    return MarketplaceUnavailable();
}

} // namespace rpc
} // namespace din
