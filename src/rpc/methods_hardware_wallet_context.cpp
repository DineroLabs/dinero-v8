/**
 * Hardware Wallet RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file migrates hardware wallet RPC methods to context-aware pattern.
 * These methods provide PSBT import/export for air-gapped hardware wallets.
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "common/logger.h"

// Forward declarations from methods_hardware_wallet.cpp
// Note: These are currently static in the original file, so we'll need to make them extern
namespace din {
namespace rpc {
    extern din::Json exportpsbttofile_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json importpsbtfromfile_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json analyzepsbt_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json enumeratehwdevices_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json connecthwdevice_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json disconnecthwdevice_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json gethwdeviceinfo_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json gethwaddress_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json gethwaccountdescriptor_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json getmasterfingerprint_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json signpsbt_impl(const ExecutionContext& ctx, const din::Json& params);
}
}

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE HARDWARE WALLET RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

din::Json rpc_context_exportpsbttofile(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::exportpsbttofile_impl(ctx, params);
}

din::Json rpc_context_importpsbtfromfile(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::importpsbtfromfile_impl(ctx, params);
}

din::Json rpc_context_analyzepsbt(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::analyzepsbt_impl(ctx, params);
}

din::Json rpc_context_enumeratehwdevices(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::enumeratehwdevices_impl(ctx, params);
}

din::Json rpc_context_connecthwdevice(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::connecthwdevice_impl(ctx, params);
}

din::Json rpc_context_disconnecthwdevice(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::disconnecthwdevice_impl(ctx, params);
}

din::Json rpc_context_gethwdeviceinfo(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::gethwdeviceinfo_impl(ctx, params);
}

din::Json rpc_context_gethwaddress(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::gethwaddress_impl(ctx, params);
}

din::Json rpc_context_gethwaccountdescriptor(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::gethwaccountdescriptor_impl(ctx, params);
}

din::Json rpc_context_getmasterfingerprint(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::getmasterfingerprint_impl(ctx, params);
}

din::Json rpc_context_signpsbt(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::signpsbt_impl(ctx, params);
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

void registerHardwareWalletMethodsContext() {
    extern RpcRegistry g_rpcRegistry;

    g_rpcRegistry.registerHandler("hwallet.exportpsbttofile",
                                 rpc_context_exportpsbttofile,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("hwallet.importpsbtfromfile",
                                 rpc_context_importpsbtfromfile,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("hwallet.analyzepsbt",
                                 rpc_context_analyzepsbt,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("hwallet.enumeratehwdevices",
                                 rpc_context_enumeratehwdevices,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("hwallet.connecthwdevice",
                                 rpc_context_connecthwdevice,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("hwallet.disconnecthwdevice",
                                 rpc_context_disconnecthwdevice,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("hwallet.gethwdeviceinfo",
                                 rpc_context_gethwdeviceinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("hwallet.gethwaddress",
                                 rpc_context_gethwaddress,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("hwallet.gethwaccountdescriptor",
                                 rpc_context_gethwaccountdescriptor,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("hwallet.getmasterfingerprint",
                                 rpc_context_getmasterfingerprint,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("hwallet.signpsbt",
                                 rpc_context_signpsbt,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] Registered 11 hardware wallet context-aware methods");
}
