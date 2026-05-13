#pragma once

#include <QMainWindow>
#include <QTabWidget>
#include <QStatusBar>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QPointer>
#include <memory>
#include <QLockFile>
#include <atomic>
#include "gui-desktop/utils/net_defaults.h"
#include "gui-desktop/utils/daemon_supervisor.h"

enum class DnrNetwork; // Forward declaration

class RpcClient;
class AppEvents;
class StatusTab;
class WalletTab;
class SendTab;
class MiningTab;
class ExplorerTab;
class DaemonLauncher;
class ImportKeyDialog;
class WalletVaultDialog;
class NetworkSwitcher;
class DaemonStatusWidget;
class MiningController;
struct ImportRequest;

/**
 * MainWindow - Core application window for Dinero Desktop
 * 
 * Architecture:
 * - 5 main tabs with specific responsibilities
 * - Direct RPC integration (no CLI fallbacks)
 * - Real-time updates via WebSocket events
 * - Professional error handling
 * - System tray integration
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void setRpcClient(RpcClient *client);

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void onConnectionStatusChanged(bool connected);
    void onBlockchainSynced(int height, int connections);
    void onNewBlockReceived(const QString &hash, int height);
    void onWalletTransactionReceived(const QString &txid, double amount);
    void onMiningUpdateReceived(bool active, double hashrate);
    void onDaemonStatusChanged();
    void onDaemonStarted();
    void onDaemonStopped();
    void onDaemonFailed(const QString& error);
    void updateNetworkStatus();
    
    void showAbout();
    void showSettings();
    void showImportKeyDialog();
    void showWalletVaultDialog();
    void showDaemonManager();
    void startDaemon();
    void stopDaemon();
    void restartDaemon();
    void toggleVisibility();
    void exitApplication();
    
    // Proper Qt pattern for import dialogs
    void openImportKeyDialog();
    void startImportFromDialog();
    void onImportFinished(bool success, const QString& result);
    void onImportError(const QString& msg);
    
    // Network switching (AppContext integration)
    void onNetworkChanged(DnrNetwork network);
    void onDaemonStatusChanged(bool isRunning, bool isHealthy);
    void refreshControllersForNetwork(DnrNetwork network);
    void onNetworkChangeRequested(int networkId);
    void onNetworkSwitched(int networkId);
    void switchNetwork(Network target);
    void rollbackNetworkSwitch();  // Rollback on switch failure
    
    // Multi-daemon management
    void ensureNetworkDaemon(Network network);
    void refreshDaemonStatus();
    bool waitUntilPortFree(int port, int timeoutMs);  // Port safety check
    

private:
    void setupUI();
    void setupMenuBar();
    void setupStatusBar();
    void setupNetworkSwitcher();
    void setupSystemTray();
    void setupRealTimeEvents();
    void setupConnections();
    void setupDaemonLauncher();
    void setupMiningController();
    
    void updateConnectionStatus();
    void updateSyncProgress();
    void setUiBusy(bool busy);
    void refreshWallet();
    
    // UI Components
    QTabWidget *m_tabWidget;
    QStatusBar *m_statusBar;
    
    // Dialog management (proper Qt pattern)
    QPointer<ImportKeyDialog> m_importDlg;
    QLabel *m_connectionLabel;
    QLabel *m_syncLabel;
    QLabel *m_networkLabel;
    QLabel *m_daemonStatusLabel;
    QProgressBar *m_syncProgress;
    NetworkSwitcher *m_networkSwitcher;
    DaemonStatusWidget *m_daemonStatusWidget;
    
    // System Tray
    QSystemTrayIcon *m_trayIcon;
    QMenu *m_trayMenu;
    QAction *m_showAction;
    QAction *m_hideAction;
    QAction *m_exitAction;
    
    // Tabs (use raw pointers to avoid forward declaration issues)
    StatusTab *m_statusTab;
    WalletTab *m_walletTab;
    SendTab *m_sendTab;
    MiningTab *m_miningTab;
    ExplorerTab *m_explorerTab;
    
    // Backend
    RpcClient *m_rpcClient;
    AppEvents *m_appEvents;
    std::shared_ptr<MiningController> m_miningController;
    DaemonLauncher *m_daemonLauncher;  // Legacy - will be replaced by supervisor
    DaemonSupervisor *m_daemonSupervisor;
    QTimer *m_updateTimer;
    
    // Network switching state
    Network m_currentNetwork{Network::Regtest};
    std::unique_ptr<QLockFile> m_networkLock;
    std::atomic<bool> m_switchInProgress{false};  // Debounce network switches
    
    
    // State
    bool m_isConnected;
    bool m_isSynced;
    int m_currentHeight;
    int m_connectionCount;
    QString m_network;
};

/**
 * Application-wide notification system
 * Handles toast notifications and system tray messages
 */
class NotificationManager : public QObject {
    Q_OBJECT
    
public:
    static NotificationManager* instance();
    
    void showInfo(const QString &title, const QString &message);
    void showWarning(const QString &title, const QString &message);
    void showError(const QString &title, const QString &message);
    void showSuccess(const QString &title, const QString &message);
    
private:
    explicit NotificationManager(QObject *parent = nullptr);
    static NotificationManager *s_instance;
};
