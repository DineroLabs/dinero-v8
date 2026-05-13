#pragma once

#include <QWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QMenu>
#include <QAction>
#include "gui-desktop/core/db_provider.h"

namespace dinero {
namespace gui {

/**
 * Database status widget for GUI applications
 * Displays real-time database health metrics in the status bar
 */
class DatabaseStatusWidget : public QWidget {
    Q_OBJECT

public:
    explicit DatabaseStatusWidget(DbProvider* dbProvider, QWidget* parent = nullptr);
    ~DatabaseStatusWidget() = default;

    // Update display
    void updateStatus();

    // Get current values for external use
    QString getCurrentStatus() const;
    int getWALSize() const;
    int getBusyRetryCount() const;
    QString getLastCheckpointTime() const;

public slots:
    // Manual refresh
    void refresh();

    // Network change handling
    void onNetworkChanged(const QString& network);

signals:
    void statusClicked();
    void showTroubleshootingDialog();

private slots:
    void onCheckpointCompleted(qint64 walSize);
    void onDatabaseError(const QString& userMessage, const QString& technicalDetails, bool isRetryable);
    void onContentionDetected(int retryCount, int maxRetries);
    void onIntegrityCheckFailed(const QString& error);

private:
    void setupUI();
    void setupMenu();
    void updateDisplay();
    void updateTooltip();

    // UI components
    QHBoxLayout* m_layout;
    QLabel* m_statusLabel;
    QLabel* m_walSizeLabel;
    QLabel* m_retryLabel;
    QLabel* m_checkpointLabel;
    QProgressBar* m_contentionBar;

    // Data
    DbProvider* m_dbProvider;
    QTimer* m_updateTimer;

    // State
    QString m_currentNetwork;
    int m_walSize;
    int m_busyRetryCount;
    QString m_lastCheckpointTime;
    QString m_currentStatus;
    int m_contentionRetries;
    int m_maxRetries;

    // Menu
    QMenu* m_contextMenu;
    QAction* m_refreshAction;
    QAction* m_troubleshootingAction;
    QAction* m_checkpointAction;

    // Constants
    static constexpr int UPDATE_INTERVAL_MS = 5000; // 5 seconds
    static constexpr int CRITICAL_WAL_SIZE = 10 * 1024 * 1024; // 10MB
};

} // namespace gui
} // namespace dinero
