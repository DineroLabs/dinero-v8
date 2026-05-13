#pragma once
#ifdef QT_CORE_LIB
#include <QJsonObject>
#else
#include "compat/jsoncpp_compat.h"
#endif
#ifdef QT_CORE_LIB
#include <QJsonArray>
#include <QString>
#else
#include <string>
#endif

/**
 * @brief RPC introspection and capabilities handlers
 */
namespace RpcIntrospection {

/**
 * @brief Returns server capabilities and API information
 * @return JSON object with server capabilities
 */
#ifdef QT_CORE_LIB
QJsonObject getCapabilities();
#else
Json::Value getCapabilities();
#endif

/**
 * @brief Returns list of all available RPC methods
 * @param includeAliases Whether to include alias methods
 * @return JSON array of method names
 */
#ifdef QT_CORE_LIB
QJsonArray listMethods(bool includeAliases = false);
#else
Json::Value listMethods(bool includeAliases = false);
#endif

/**
 * @brief Get help information for a specific method
 * @param method Method name to get help for
 * @return JSON object with method help
 */
#ifdef QT_CORE_LIB
QJsonObject getMethodHelp(const QString& method);
#else
Json::Value getMethodHelp(const std::string& method);
#endif

/**
 * @brief Get server metrics and statistics
 * @return JSON object with server metrics
 */
#ifdef QT_CORE_LIB
QJsonObject getMetrics();
#else
Json::Value getMetrics();
#endif

} // namespace RpcIntrospection
