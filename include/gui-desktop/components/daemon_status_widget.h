#pragma once
#include <QWidget>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QTimer>
#include <unordered_map>
#include "gui-desktop/utils/net_defaults.h"
#include "gui-desktop/utils/daemon_supervisor.h"

/**
 * DaemonStatusWidget - Shows status of all network daemons with active connection indicator
 * 
 * Visual Layout:
 * ┌─────────────────────────────────────────┐
 * │ Network Daemons:                        │
 * │ ● Mainnet (20998)  ★ Testnet (20988)   │
 * │ ○ Regtest (20978)                       │
 * └─────────────────────────────────────────┘
 * 
 * Legend:
 * ● = Healthy daemon
 * ○ = Stopped daemon  
 * ★ = Active connection (current network)
 */
class DaemonStatusWidget : public QWidget {
    Q_OBJECT

public:
    explicit DaemonStatusWidget(QWidget* parent = nullptr);
    
    // Set the daemon supervisor to monitor
    void setDaemonSupervisor(DaemonSupervisor* supervisor);
    
    // Set which network is currently active (shows star)
    void setActiveNetwork(Network network);
    
    // Manual refresh
    void refreshStatus();

signals:
    void networkSwitchRequested(Network network);

public slots:
    void onDaemonStatusChanged(Network network, DaemonSupervisor::DaemonStatus status);
    void onNetworkSwitched(Network network);

private slots:
    void onRefreshTimer();
    void onDaemonClicked(Network network);

private:
    struct NetworkStatus {
        QLabel* statusIcon;      // ● ○ ★
        QLabel* networkLabel;    // "Mainnet"
        QLabel* portLabel;       // "(20998)"
        QPushButton* actionBtn;  // "Start" / "Stop" / "Switch"
        DaemonSupervisor::DaemonStatus status;
        bool isActive;
    };

    void setupUI();
    void setupConnections();
    void updateNetworkDisplay(Network network);
    QString getStatusIcon(Network network) const;
    QString getStatusColor(DaemonSupervisor::DaemonStatus status) const;
    void updateActionButton(Network network);

    // UI components
    QVBoxLayout* m_mainLayout;
    QLabel* m_titleLabel;
    QWidget* m_statusContainer;
    QHBoxLayout* m_statusLayout;
    
    // Network status tracking
    std::unordered_map<Network, NetworkStatus> m_networkStatus;
    Network m_activeNetwork{Network::Regtest};
    
    // Backend connection
    DaemonSupervisor* m_daemonSupervisor{nullptr};
    QTimer* m_refreshTimer;
    
    // Configuration
    static constexpr int REFRESH_INTERVAL_MS = 5000;  // 5 seconds
};
