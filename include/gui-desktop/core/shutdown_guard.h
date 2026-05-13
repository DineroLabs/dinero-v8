#pragma once

#include <QObject>
#include <QTimer>
#include <QApplication>
#include "gui-desktop/core/db_provider.h"

namespace dinero {
namespace gui {

/**
 * Shutdown guard for GUI applications
 * Ensures proper WAL checkpoint on app exit
 */
class ShutdownGuard : public QObject {
    Q_OBJECT

public:
    explicit ShutdownGuard(DbProvider* dbProvider, QObject* parent = nullptr);
    ~ShutdownGuard();

    // Manual shutdown trigger
    void requestShutdown();
    
    // Check if shutdown is in progress
    bool isShuttingDown() const { return m_shuttingDown; }

signals:
    void shutdownStarted();
    void shutdownCompleted();
    void checkpointCompleted(qint64 walSize);

private slots:
    void performShutdown();
    void onCheckpointCompleted(qint64 walSize);

private:
    DbProvider* m_dbProvider;
    bool m_shuttingDown;
    QTimer* m_shutdownTimer;
    
    static constexpr int SHUTDOWN_TIMEOUT_MS = 5000; // 5 seconds
};

} // namespace gui
} // namespace dinero
