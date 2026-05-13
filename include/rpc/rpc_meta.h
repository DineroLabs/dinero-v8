#pragma once
#ifdef QT_CORE_LIB
#include <QJsonJson::Value>
#else
#include "compat/jsoncpp_compat.h"
#endif
#ifdef QT_CORE_LIB
#include <QJsonArray>
#include <QString>
#else
#include <string>
#endif

struct ExecutionCtx {
#ifdef QT_CORE_LIB
    QString wallet;   // may be empty
    QString user;     // authenticated user
    QString cookie;   // auth cookie (redacted in logs)
#else
    std::string wallet;   // may be empty
    std::string user;     // authenticated user
    std::string cookie;   // auth cookie (redacted in logs)
#endif
    // Add other fields as needed
};

namespace RpcMeta {
    /**
     * @brief Get server capabilities and configuration
     * @param ctx Execution context (unused for capabilities)
     * @param params Parameters (should be empty)
     * @return JSON object with server capabilities
     */
#ifdef QT_CORE_LIB
    QJsonJson::Value capabilities(const ExecutionCtx& ctx, const QJsonArray& params);
#else
    Json::Value capabilities(const ExecutionCtx& ctx, const Json::Value& params);
#endif

    /**
     * @brief List all available RPC methods
     * @param ctx Execution context (unused for method listing)
     * @param params Parameters (should be empty)
     * @return JSON array of method names
     */
#ifdef QT_CORE_LIB
    QJsonJson::Value listMethods(const ExecutionCtx& ctx, const QJsonArray& params);
#else
    Json::Value listMethods(const ExecutionCtx& ctx, const Json::Value& params);
#endif

    /**
     * @brief Get help for RPC methods
     * @param ctx Execution context (unused for help)
     * @param params Parameters (method name or empty for all)
     * @return JSON object with help information
     */
#ifdef QT_CORE_LIB
    QJsonJson::Value help(const ExecutionCtx& ctx, const QJsonArray& params);
#else
    Json::Value help(const ExecutionCtx& ctx, const Json::Value& params);
#endif

    /**
     * @brief Get server metrics and statistics
     * @param ctx Execution context (unused for metrics)
     * @param params Parameters (should be empty)
     * @return JSON object with server metrics
     */
#ifdef QT_CORE_LIB
    QJsonJson::Value metrics(const ExecutionCtx& ctx, const QJsonArray& params);
#else
    Json::Value metrics(const ExecutionCtx& ctx, const Json::Value& params);
#endif

    /**
     * @brief Get server health status
     * @param ctx Execution context (unused for health)
     * @param params Parameters (should be empty)
     * @return JSON object with health status
     */
#ifdef QT_CORE_LIB
    QJsonJson::Value health(const ExecutionCtx& ctx, const QJsonArray& params);
#else
    Json::Value health(const ExecutionCtx& ctx, const Json::Value& params);
#endif

} // namespace RpcMeta
