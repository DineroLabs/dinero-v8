#pragma once
#include <QLoggingCategory>

// Production logging categories
Q_DECLARE_LOGGING_CATEGORY(lcAuth)
Q_DECLARE_LOGGING_CATEGORY(lcRPC)
Q_DECLARE_LOGGING_CATEGORY(lcUI)
Q_DECLARE_LOGGING_CATEGORY(lcDaemon)
Q_DECLARE_LOGGING_CATEGORY(lcNetwork)

/**
 * @brief Initialize production logging configuration
 * 
 * Sets up logging rules to hide Qt noise and show only
 * relevant Dinero application logs.
 */
void initializeProductionLogging();

/**
 * @brief Get current log level configuration as string
 * @return Human-readable log configuration
 */
QString getLogConfiguration();
