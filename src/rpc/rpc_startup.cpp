#include "rpc/rpc_startup.h"
#include "rpc/rpc_registry.h"
#include "rpc/rpc_meta.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#include <QDebug>
#include <stdexcept>

namespace RpcStartup {

void registerMetaHandlers() {
    using P = RpcParamMeta;
    using R = RpcResultMeta;
    using E = RpcErrorMeta;
    
    // Register RPC introspection handlers with metadata
    g_rpcRegistry.registerHandler("rpc.capabilities", 
        [](const ExecutionContext& ctx, const QJsonArray& params) {
            ExecutionCtx metaCtx{ctx.walletName, ctx.user, ctx.cookie};
            return RpcMeta::capabilities(metaCtx, params);
        },
        RpcMethodMeta{
            .name = "rpc.capabilities",
            .ns = "rpc",
            .description = "Returns supported namespaces and feature flags.",
            .params = {},
            .result = R{"object", "Capabilities"},
            .errors = {}
        });
    
    g_rpcRegistry.registerHandler("rpc.listmethods",
        [](const ExecutionContext& ctx, const QJsonArray& params) {
            ExecutionCtx metaCtx{ctx.walletName, ctx.user, ctx.cookie};
            return RpcMeta::listMethods(metaCtx, params);
        },
        RpcMethodMeta{
            .name = "rpc.listmethods",
            .ns = "rpc", 
            .description = "Lists canonical method names.",
            .params = {},
            .result = R{"array", "Method names"},
            .errors = {}
        });
    
    g_rpcRegistry.registerHandler("rpc.help",
        [](const ExecutionContext& ctx, const QJsonArray& params) {
            ExecutionCtx metaCtx{ctx.walletName, ctx.user, ctx.cookie};
            return RpcMeta::help(metaCtx, params);
        },
        RpcMethodMeta{
            .name = "rpc.help",
            .ns = "rpc",
            .description = "General or method-specific help text.",
            .params = {P{"method", "string", "Method name (optional)", false}},
            .result = R{"object", "Help document or general help"},
            .errors = {E{-32601, "Unknown method"}, E{-32602, "Invalid params"}}
        });
    
    g_rpcRegistry.registerHandler("rpc.health",
        [](const ExecutionContext& ctx, const QJsonArray& params) {
            if (!params.isEmpty()) {
                return QJsonValue(QJsonObject{
                    {"error", QJsonObject{
                        {"code", -32602},
                        {"message", "rpc.health takes no params"}
                    }}
                });
            }

            // Get real chain data from DaemonContext
            int height = 0;
            bool walletLoaded = false;
            QString walletName;

            if (ctx.daemon) {
                // Get chain height from ChainstateService
                if (ctx.daemon->chainstate) {
                    height = static_cast<int>(ctx.daemon->chainstate->getBlockHeight());
                }

                // Get wallet status from WalletService
                if (ctx.daemon->wallet) {
                    walletLoaded = ctx.daemon->wallet->hasActiveWallet();
                    if (walletLoaded) {
                        walletName = QString::fromStdString(ctx.daemon->wallet->getCurrentWalletName());
                    }
                }
            }

            return QJsonValue(QJsonObject{
                {"ok", true},
                {"height", height},
                {"wallet_loaded", walletLoaded},
                {"wallet", walletLoaded ? QJsonValue(walletName) : QJsonValue()}
            });
        },
        RpcMethodMeta{
            .name = "rpc.health",
            .ns = "rpc",
            .description = "Returns server health status including chain height and wallet state.",
            .params = {},
            .result = R{"object", "Health status"},
            .errors = {}
        });
    
    qDebug() << "Registered RPC meta handlers: rpc.capabilities, rpc.listmethods, rpc.help, rpc.health";
}

void initialize() {
    qInfo() << "Initializing RPC system...";
    
    // Register meta handlers first
    registerMetaHandlers();
    
    // Validate that all aliases point to registered handlers
    try {
        g_rpcRegistry.validateAliases();
    } catch (const std::exception& e) {
        qCritical() << "RPC alias validation failed:" << e.what();
        throw std::runtime_error("RPC system initialization failed");
    }
    
    // Log final statistics
    QStringList methods = g_rpcRegistry.methodNames();
    qInfo() << "RPC system initialized successfully with" << methods.size() << "methods";
    
    // Log method counts by namespace
    int walletMethods = g_rpcRegistry.methodsByNamespace("wallet").size();
    int blockchainMethods = g_rpcRegistry.methodsByNamespace("blockchain").size();
    int miningMethods = g_rpcRegistry.methodsByNamespace("mining").size();
    int rpcMethods = g_rpcRegistry.methodsByNamespace("rpc").size();
    
    qInfo() << "Method distribution: wallet=" << walletMethods 
            << " blockchain=" << blockchainMethods
            << " mining=" << miningMethods
            << " rpc=" << rpcMethods;

#ifdef RPC_COMPAT_LEGACY
    qInfo() << "RPC legacy alias compatibility: ENABLED";
#else
    qInfo() << "RPC legacy alias compatibility: DISABLED";
#endif
}

} // namespace RpcStartup
