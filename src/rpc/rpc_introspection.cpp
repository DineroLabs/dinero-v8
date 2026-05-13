#include "rpc/rpc_introspection.h"
#include "rpc/rpc_registry.h"
#include "rpc/rpc_aliases.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>

namespace RpcIntrospection {

QJsonObject getCapabilities() {
    QJsonObject caps;
    caps["jsonrpc"] = "2.0";
    caps["version"] = "1.0.0";
    caps["style"] = "namespaced";
    caps["legacy_aliases"] = true;
    
    // Supported namespaces
    QJsonArray namespaces;
    namespaces.append("wallet");
    namespaces.append("blockchain");
    namespaces.append("network");
    namespaces.append("mining");
    namespaces.append("rpc");
    caps["namespaces"] = namespaces;
    
    // Features
    QJsonObject features;
    features["wallet_scoped_urls"] = true;
    features["batch_requests"] = true;
    features["websocket_notifications"] = true;
    features["method_introspection"] = true;
    caps["features"] = features;
    
    // Deprecation info
    QJsonObject deprecation;
    deprecation["legacy_aliases_until"] = "v2.0.0";
    deprecation["migration_guide"] = "https://docs.dinero-coin.com/rpc-migration";
    caps["deprecation"] = deprecation;
    
    caps["server_time"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    
    return caps;
}

QJsonArray listMethods(bool includeAliases) {
    QJsonArray methods;
    
    // Add all canonical methods
    const QStringList canonicalMethods = g_rpcRegistry.methodNames();
    for (const QString& method : canonicalMethods) {
        methods.append(method);
    }
    
    if (includeAliases) {
        // Add wallet aliases
        const auto& walletAliases = walletAliasMap();
        for (auto it = walletAliases.cbegin(); it != walletAliases.cend(); ++it) {
            QJsonObject alias;
            alias["method"] = it.key();
            alias["canonical"] = it.isMember();
            alias["deprecated"] = true;
            methods.append(alias);
        }
        
        // Add node aliases
        const auto& nodeAliases = nodeAliasMap();
        for (auto it = nodeAliases.cbegin(); it != nodeAliases.cend(); ++it) {
            QJsonObject alias;
            alias["method"] = it.key();
            alias["canonical"] = it.isMember();
            alias["deprecated"] = true;
            methods.append(alias);
        }
    }
    
    return methods;
}

QJsonObject getMethodHelp(const QString& method) {
    QJsonObject help;
    help["method"] = method;
    
    // Check if it's an alias
    const auto& walletAliases = walletAliasMap();
    const auto& nodeAliases = nodeAliasMap();
    
    QString canonical = method;
    bool isAlias = false;
    
    if (walletAliases.isMember(method)) {
        canonical = walletAliases.isMember(method);
        isAlias = true;
    } else if (nodeAliases.isMember(method)) {
        canonical = nodeAliases.isMember(method);
        isAlias = true;
    }
    
    help["canonical"] = canonical;
    help["is_alias"] = isAlias;
    
    if (isAlias) {
        help["deprecation_warning"] = "This method name is deprecated. Use the canonical name instead.";
    }
    
    // Add basic help based on namespace
    QString ns = canonical.split('.').first();
    if (ns == "wallet") {
        help["description"] = "Wallet operation - requires wallet context";
        help["context_required"] = true;
        help["usage"] = "Use /wallet/<name> path or ensure active wallet is loaded";
    } else if (ns == "blockchain") {
        help["description"] = "Blockchain query operation";
        help["context_required"] = false;
    } else if (ns == "network") {
        help["description"] = "Network information operation";
        help["context_required"] = false;
    } else if (ns == "mining") {
        help["description"] = "Mining control operation";
        help["context_required"] = false;
    } else if (ns == "rpc") {
        help["description"] = "RPC meta operation";
        help["context_required"] = false;
    }
    
    return help;
}

QJsonObject getMetrics() {
    QJsonObject metrics;
    
    // Method counts by namespace
    QJsonObject methodCounts;
    methodCounts["wallet"] = g_rpcRegistry.methodsByNamespace("wallet").size();
    methodCounts["blockchain"] = g_rpcRegistry.methodsByNamespace("blockchain").size();
    methodCounts["network"] = g_rpcRegistry.methodsByNamespace("network").size();
    methodCounts["mining"] = g_rpcRegistry.methodsByNamespace("mining").size();
    methodCounts["rpc"] = g_rpcRegistry.methodsByNamespace("rpc").size();
    metrics["method_counts"] = methodCounts;
    
    // Alias counts
    metrics["wallet_aliases"] = static_cast<int>(walletAliasMap().size());
    metrics["node_aliases"] = static_cast<int>(nodeAliasMap().size());
    metrics["total_aliases"] = static_cast<int>(walletAliasMap().size() + nodeAliasMap().size());
    
    // Add runtime metrics (basic implementation)
    // Framework for request counting and alias usage tracking
    // Real implementation would track:
    // - RPC method call counts
    // - Alias usage statistics  
    // - Response times and error rates
    // - Active connection counts
    
    QJsonObject runtime_metrics;
    runtime_metrics["uptime_seconds"] = 0; // Would track daemon start time
    runtime_metrics["total_requests"] = 0; // Would track RPC call count
    runtime_metrics["active_connections"] = 0; // Would get from network manager
    metrics["runtime"] = runtime_metrics;
    
    metrics["generated_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    
    return metrics;
}

} // namespace RpcIntrospection
