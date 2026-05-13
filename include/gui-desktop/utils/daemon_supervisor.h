#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QNetworkAccessManager>
#include <unordered_map>
#include <filesystem>
#include <QMetaType>
#include "net_defaults.h"

/**
 * DaemonSupervisor manages multiple concurrent daemons (one per network)
 * Eliminates stop/start complexity by keeping all daemons running and 
 * routing GUI connections to the appropriate daemon.
 */
class DaemonSupervisor : public QObject {
    Q_OBJECT

public:
    enum class DaemonStatus {
        Stopped,     // Not running
        Starting,    // Process launched, waiting for health
        Healthy,     // Running and responding to RPC
        Unhealthy    // Running but not responding
    };
    Q_ENUM(DaemonStatus)

    explicit DaemonSupervisor(QObject* parent = nullptr);
    ~DaemonSupervisor();

    // Core daemon management
    bool ensureRunning(Network network);           // Start daemon if not running
    bool isHealthy(Network network) const;         // Check if daemon is responsive
    DaemonStatus getStatus(Network network) const; // Get current status
    
    // Process management
    void shutdownAll();                            // Clean shutdown of all daemons
    void shutdownNetwork(Network network);         // Shutdown specific network
    
    // Health monitoring
    void startHealthMonitoring();                  // Begin periodic health checks
    void stopHealthMonitoring();                   // Stop health monitoring
    
    // Configuration
    void setBaseDataDir(const std::filesystem::path& baseDir);
    void setDaemonPath(const std::string& daemonPath);
    
    // Status queries
    std::vector<Network> getRunningNetworks() const;
    std::vector<Network> getHealthyNetworks() const;
    bool isAnyDaemonRunning() const;

signals:
    void daemonStatusChanged(Network network, DaemonStatus status);
    void daemonStarted(Network network);
    void daemonStopped(Network network);
    void healthCheckFailed(Network network, const QString& error);

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void performHealthCheck();

private:
    struct DaemonInfo {
        QProcess* process = nullptr;
        DaemonStatus status = DaemonStatus::Stopped;
        QString logPath;
        int restartAttempts = 0;
        qint64 lastStartTime = 0;
    };

    // Daemon management
    bool startDaemon(Network network);
    QStringList buildDaemonArgs(Network network) const;
    std::filesystem::path cookiePathFor(Network network) const;
    std::filesystem::path logPathFor(Network network) const;
    
    // Health checking
    bool checkDaemonHealth(Network network);
    bool tcpPortListening(int port) const;
    bool rpcHealthCheck(Network network);
    
    // Process utilities
    void cleanupProcess(Network network);
    bool waitForCookieFile(Network network, int timeoutMs = 15000);
    bool waitForRpcReady(Network network, int timeoutMs = 15000);

    // Member variables
    std::unordered_map<Network, DaemonInfo> m_daemons;
    QTimer* m_healthTimer;
    QNetworkAccessManager* m_networkManager;
    
    std::filesystem::path m_baseDataDir;
    std::string m_daemonPath;
    
    // Configuration
    static constexpr int HEALTH_CHECK_INTERVAL_MS = 10000;  // 10 seconds
    static constexpr int MAX_RESTART_ATTEMPTS = 3;
    static constexpr int MIN_RESTART_INTERVAL_MS = 5000;    // 5 seconds between restarts
};
