#pragma once

#include <QObject>
#include <QTimer>
#include <QLabel>
#include <QStatusBar>
#include "gui-desktop/core/app_context.h"
#include "gui-desktop/core/db_provider.h"
#include "gui-desktop/core/background_jobs.h"
#include "gui-desktop/core/shutdown_guard.h"
#include "gui-desktop/dialogs/troubleshooting_dialog.h"
#include "gui-desktop/components/database_status_widget.h"

namespace dinero {
namespace gui {

/**
 * Example integration of RAII + WAL stack with desktop GUI
 * Shows how to use DbProvider, BackgroundJobs, and ShutdownGuard
 */
class DatabaseIntegrationExample : public QObject {
    Q_OBJECT

public:
    explicit DatabaseIntegrationExample(QObject* parent = nullptr);
    ~DatabaseIntegrationExample() = default;

    // Setup integration with main window
    void setupWithMainWindow(QStatusBar* statusBar);
    
    // Database operations using the new stack
    void refreshWalletData();
    void performDatabaseMaintenance();
    void showTroubleshootingDialog();

signals:
    void walletDataRefreshed(const QString& balance);
    void operationCompleted(const QString& message);
    void operationFailed(const QString& error);

private slots:
    void onDatabaseInitialized(bool success);
    void onIntegrityCheckCompleted(bool passed, const QString& error);
    void onMigrationCompleted(bool success, const QString& message);
    void onCheckpointCompleted(qint64 walSize);
    void onBusyRetryOccurred(int retryCount);
    void updateStatusBar();

private:
    void setupStatusBarLabels();
    void performWalletQuery();
    void performMaintenanceTask();
    
    // Core components
    AppContext* m_appContext;
    DbProvider* m_dbProvider;
    BackgroundJobs* m_backgroundJobs;
    ShutdownGuard* m_shutdownGuard;
    TroubleshootingDialog* m_troubleshootingDialog;
    DatabaseStatusWidget* m_statusWidget;
    
    // UI components
    QStatusBar* m_statusBar;
    QLabel* m_dbStatusLabel;
    QLabel* m_walSizeLabel;
    QLabel* m_busyRetryLabel;
    QLabel* m_lastCheckpointLabel;
    QTimer* m_statusUpdateTimer;
    
    // State
    bool m_integrationReady;
};

} // namespace gui
} // namespace dinero
