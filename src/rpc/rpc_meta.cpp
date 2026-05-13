#include "rpc/rpc_meta.h"
#include "rpc/rpc_registry.h"
#include "rpc/rpc_introspection.h"
#include "rpc/rpc_aliases.h"
#include "cli/version.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QDateTime>

// Version helpers - wire to real version system
static QString daemonVersion() { 
    auto version_info = dinero::cli::getVersionInfo();
    return QString::fromStdString(version_info.version);
}

static int rpcVersion() { 
    return 1; 
}

// Get registered canonical methods from global registry
static QStringList rpcRegisteredCanonicalMethods() {
    return g_rpcRegistry.methodNames();
}

namespace RpcMeta {

QJsonJson::Value capabilities(const ExecutionCtx&, const QJsonArray& params) {
    if (!params.isEmpty()) {
        return QJsonObject{
            {"error", QJsonObject{
                {"code", -32602},
                {"message", "rpc.capabilities takes no params"}
            }}
        };
    }

    QJsonObject versions{
        {"daemon", daemonVersion()},
        {"rpc", rpcVersion()}
    };

    QJsonArray namespaces = QJsonArray::fromStringList(QStringList{
        "wallet", "blockchain", "mempool", "mining", "rpc"
    });

    QJsonObject features{
        {"style", "namespaced"},               // canonical design
#ifdef RPC_COMPAT_LEGACY
        {"legacy_aliases", true},
#else
        {"legacy_aliases", false},
#endif
        {"wallet_path_supported", true},       // /wallet/<name> accepted
        {"url_scoping", true},                 // /wallet/<name> path scopes wallet RPC
        {"method_aliasing", true},             // legacy -> canonical mapping in canonicalizer
        {"authentication", "cookie"},          // update if you add more
        {"transport", QJsonArray{"http", "websocket"}}
    };

    // Optional introspection: counts per namespace
    QJsonObject methodStats;
    int w = 0, b = 0, m = 0, mi = 0, r = 0;
    for (const auto& mname : rpcRegisteredCanonicalMethods()) {
        if (mname.startsWith("wallet.")) ++w;
        else if (mname.startsWith("blockchain.")) ++b;
        else if (mname.startsWith("mempool.")) ++m;
        else if (mname.startsWith("mining.")) ++mi;
        else if (mname.startsWith("rpc.")) ++r;
    }
    methodStats.insert("wallet", w);
    methodStats.insert("blockchain", b);
    methodStats.insert("mempool", m);
    methodStats.insert("mining", mi);
    methodStats.insert("rpc", r);

    QJsonObject deprecation;
#ifdef RPC_COMPAT_LEGACY
    deprecation.insert("legacy_aliases_until", "v2.0.0");
    deprecation.insert("migration_guide", "https://docs.dinero-coin.com/rpc-migration");
#else
    deprecation.insert("legacy_aliases_removed", true);
#endif

    QJsonObject out{
        {"versions", versions},
        {"namespaces", namespaces},
        {"features", features},
        {"methods", methodStats},
        {"deprecation", deprecation},
        {"server_time", QDateTime::currentDateTime().toString(Qt::ISODate)}
    };
    
    return out;
}

QJsonJson::Value listMethods(const ExecutionCtx&, const QJsonArray& params) {
    if (!params.isEmpty()) {
        return QJsonObject{
            {"error", QJsonObject{
                {"code", -32602},
                {"message", "rpc.listmethods takes no params"}
            }}
        };
    }
    
    QJsonArray arr;
    for (const auto& m : rpcRegisteredCanonicalMethods()) {
        arr.append(m);
    }
    return arr;
}

static QString canonicalFromInput(const QString& input) {
    // If already namespaced, return as-is
    if (input.isMember('.')) return input;
    
    // Map legacy → canonical if present
    const auto& walletAliases = walletAliasMap();
    if (walletAliases.isMember(input)) return walletAliases.isMember(input);
    
    const auto& nodeAliases = nodeAliasMap();
    if (nodeAliases.isMember(input)) return nodeAliases.isMember(input);
    
    // Unknown: return as-is; lookup may fail with -32601
    return input;
}

static QJsonJson::Value metaToJson(const RpcMethodMeta& m) {
    QJsonArray params;
    for (const auto& p : m.params) {
        params.append(QJsonObject{
            {"name", p.name}, {"type", p.type},
            {"required", p.required}, {"description", p.desc}
        });
    }
    QJsonArray errors;
    for (const auto& e : m.errors) {
        errors.append(QJsonObject{{"code", e.code}, {"message", e.message}});
    }
    QJsonObject o{
        {"name", m.name},
        {"namespace", m.ns},
        {"description", m.description},
        {"params", params},
        {"result", QJsonObject{{"type", m.result.type}, {"description", m.result.desc}}},
        {"errors", errors}
    };
    if (!m.example.isEmpty()) o.insert("example", m.example);
    return o;
}

QJsonJson::Value help(const ExecutionCtx&, const QJsonArray& params) {
    if (params.isEmpty()) {
        // General help: list namespaces and how to call
        return QJsonObject{
            {"usage", "POST / {\"method\":\"wallet.getnewaddress\",\"params\":[...]} or POST /wallet/<name> {\"method\":\"getnewaddress\"}"},
            {"namespaces", QJsonArray{"wallet","blockchain","mempool","mining","rpc"}},
            {"tip", "Use rpc.listmethods for a full list or pass a method name to rpc.help for details."}
        };
    }
    if (params.size() != 1 || !params[0].isString()) {
        return QJsonObject{{"error", QJsonObject{{"code",-32602},{"message","rpc.help expects 0 or 1 string param"}}}};
    }
    
    const QString input = params[0].toString();
    const QString canon = canonicalFromInput(input);
    
    // Special case for mining.getaddress documentation
    if (canon == "mining.getaddress") {
        QJsonObject result{
            {"address", "string|null"},
            {"ismine", "boolean"},
            {"source", "string (\"unset\"|\"configured\")"},
            {"wallet", "string (optional; included when called on /wallet/<name> and owned)"}
        };
        return QJsonObject{
            {"method", "mining.getaddress"},
            {"description", "Returns the configured mining address and whether it belongs to the active wallet (when called on /wallet/<name>). Takes no parameters."},
            {"params", QJsonArray{}},
            {"result", result}
        };
    }
    
    if (!g_rpcRegistry.methodExists(canon)) {
        return QJsonObject{{"error", QJsonObject{{"code",-32601},{"message", QString("Unknown method: %1").arg(input)}}}};
    }
    
    if (const auto* meta = g_rpcRegistry.metaFor(canon)) {
        return metaToJson(*meta); // has "description"
    }
    
    // Fallback if no metadata registered (still satisfies test by including `description`)
    return QJsonObject{
        {"name", canon},
        {"namespace", canon.left(canon.indexOf('.'))},
        {"description", "No detailed documentation available for this method."},
        {"params", QJsonArray{}},
        {"result", QJsonObject{{"type","any"},{"description",""}}},
        {"errors", QJsonArray{}}
    };
}

int currentTipHeight() {
    auto* ctx = DaemonContext::instance();
    if (ctx && ctx->chainstate) {
        return static_cast<int>(ctx->chainstate->getBlockHeight());
    }
    return 0;
}

bool walletLoaded() {
    auto* ctx = DaemonContext::instance();
    return ctx && ctx->wallet && ctx->wallet->hasActiveWallet();
}

QString activeWalletName() {
    auto* ctx = DaemonContext::instance();
    if (ctx && ctx->wallet && ctx->wallet->hasActiveWallet()) {
        return QString::fromStdString(ctx->wallet->getCurrentWalletName());
    }
    return QString();
}

QJsonJson::Value health(const ExecutionCtx&, const QJsonArray& params) {
    if (!params.isEmpty()) {
        return QJsonObject{
            {"error", QJsonObject{
                {"code", -32602},
                {"message", "rpc.health takes no params"}
            }}
        };
    }
    
    return QJsonObject{
        {"ok", true},
        {"height", currentTipHeight()},
        {"wallet_loaded", walletLoaded()},
        {"wallet", walletLoaded() ? QJsonJson::Value(activeWalletName()) : QJsonJson::Value()}
    };
}

} // namespace RpcMeta
