#pragma once

#include <QString>
#include <QMap>
#include <QTimer>
#include <functional>
#include <sqlite3.h>

namespace dinero {
namespace gui {

/**
 * SQLite error code mapping and retry logic for GUI applications
 * Maps technical SQLite errors to user-friendly messages and handles retries
 */
class SQLiteErrorMapper {
public:
    // Error types
    enum class ErrorType {
        BUSY,
        LOCKED,
        CORRUPTION,
        PERMISSION,
        DISK_FULL,
        IO_ERROR,
        TIMEOUT,
        UNKNOWN
    };

    // User-friendly error messages
    struct ErrorInfo {
        ErrorType type;
        QString userMessage;
        QString technicalDetails;
        bool isRetryable;
        int retryDelayMs;
    };

    static ErrorInfo mapErrorCode(int sqliteErrorCode);
    static ErrorInfo mapExtendedErrorCode(int extendedErrorCode);

    // Retry logic
    static bool shouldRetry(ErrorType errorType);
    static int getRetryDelay(ErrorType errorType, int attempt);

    // Error handling with retry
    template<typename Func>
    static bool executeWithRetry(Func operation, const QString& operationName,
                                int maxRetries = 3, std::function<void(const ErrorInfo&)> onError = nullptr);

    // Database contention detection
    static bool isDatabaseContention(int sqliteErrorCode);
    static QString getContentionMessage(int retryCount, int maxRetries);

private:
    static QMap<int, ErrorInfo> initializeErrorMap();
    static QMap<int, ErrorInfo> m_errorMap;
};

// Template implementation for retry logic
template<typename Func>
bool SQLiteErrorMapper::executeWithRetry(Func operation, const QString& operationName,
                                        int maxRetries, std::function<void(const ErrorInfo&)> onError) {
    int attempt = 0;

    while (attempt < maxRetries) {
        try {
            return operation();
        } catch (const std::exception& e) {
            // Convert exception to error info
            QString errorMessage = e.what();
            ErrorInfo errorInfo;
            errorInfo.type = ErrorType::UNKNOWN;
            errorInfo.userMessage = "Database operation failed";
            errorInfo.technicalDetails = errorMessage;
            errorInfo.isRetryable = false;
            // errorCode field will be set by the caller

            // Try to determine error type from error message
            if (errorMessage.contains("SQLITE_BUSY") || errorMessage.contains("busy")) {
                errorInfo = mapErrorCode(SQLITE_BUSY);
            } else if (errorMessage.contains("SQLITE_LOCKED") || errorMessage.contains("locked")) {
                errorInfo = mapErrorCode(SQLITE_LOCKED);
            }

            if (onError) {
                onError(errorInfo);
            }

            if (!errorInfo.isRetryable || attempt >= maxRetries - 1) {
                return false;
            }

            int delay = getRetryDelay(errorInfo.type, attempt);
            QTimer::singleShot(delay, []() { /* retry logic would go here */ });
            attempt++;

            if (onError) {
                onError(errorInfo);
            }
        }
    }

    return false;
}

} // namespace gui
} // namespace dinero
