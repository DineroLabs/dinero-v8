/**
 * Descriptor RPC Methods - Context-Aware Wrappers
 *
 * This file provides context-aware wrappers for descriptor RPC methods.
 * Follows the Week 2 pattern used by hardware wallet RPCs.
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"

// Forward declarations from methods_descriptor.cpp
namespace din {
namespace rpc {
    extern din::Json importdescriptor_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json listdescriptors_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json setactivedescriptor_impl(const ExecutionContext& ctx, const din::Json& params);
}
}

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE DESCRIPTOR RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

din::Json rpc_context_importdescriptor(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::importdescriptor_impl(ctx, params);
}

din::Json rpc_context_listdescriptors_v2(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::listdescriptors_impl(ctx, params);
}

din::Json rpc_context_setactivedescriptor(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::setactivedescriptor_impl(ctx, params);
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION
// ═══════════════════════════════════════════════════════════════

void registerDescriptorMethodsContext() {
    extern RpcRegistry g_rpcRegistry;

    g_rpcRegistry.registerHandler("descriptor.import",
                                 rpc_context_importdescriptor,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("descriptor.list",
                                 rpc_context_listdescriptors_v2,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("descriptor.setactive",
                                 rpc_context_setactivedescriptor,
                                 RegisterMode::Overwrite,
                                 "context-aware");
}
