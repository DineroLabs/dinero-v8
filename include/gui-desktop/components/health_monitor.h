#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;
class RpcClient;

/**
 * Health Monitor Widget
 * Shows daemon health status and provides access to diagnostics
 */
class HealthMonitor : public QWidget {
    Q_OBJECT

public:
    enum class HealthStatus {
        Healthy,
        Warning,
        Error,
        Offline
    };

    explicit HealthMonitor(QWidget* parent = nullptr);
    
    void setRpcClient(RpcClient* rpcClient);
    bool isHealthy() const;

signals:
    void healthStatusChanged(bool healthy);
    void diagnosticsRequested();

private slots:
    void checkHealth();
    void showDiagnostics();

private:
    void setupUI();
    void setupTimer();
    void updateStatus(HealthStatus status, const QString& message);
    
    RpcClient* m_rpcClient = nullptr;
    QTimer* m_healthTimer = nullptr;
    QLabel* m_statusIndicator = nullptr;
    QLabel* m_statusLabel = nullptr;
    QPushButton* m_diagnosticsBtn = nullptr;
    HealthStatus m_currentStatus = HealthStatus::Offline;
};
