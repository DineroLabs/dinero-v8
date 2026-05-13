#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include "gui-desktop/utils/daemon_supervisor.h"
#include "gui-desktop/core/db_provider.h"
#include "gui-desktop/core/background_jobs.h"
#include "gui-desktop/core/cli_exit_codes.h"

enum class DnrNetwork {
    Mainnet,
    Testnet,
    Regtest
};
Q_DECLARE_METATYPE(DnrNetwork)

/**
 * @brief Central application context - single source of truth for network state
 * 
 * This class manages the current network configuration and emits signals when
 * the network changes, ensuring all subsystems stay synchronized.
 */
class AppContext : public QObject {
    Q_OBJECT

public:
    enum class DnrNetwork {
        Mainnet,
        Testnet,
        Regtest
    };
    Q_ENUM(DnrNetwork)

    static AppContext& instance();
    
    // Current network state
    DnrNetwork currentNetwork() const { return m_currentNetwork; }
    QString networkString() const;
    QString hrp() const;
    
    // Network endpoints
    QUrl rpcEndpoint() const;
    QString dataDir() const;
    QString cookiePath() const;
    QString getDatabasePath() const;
    
    // Network switching
    void switchNetwork(DnrNetwork network);

    // Shutdown management
    void prepareShutdown();

    // Data directory override for headless mode
    static void setDataDirOverride(const QString& path);
    static QString getDataDirOverride();

    // Database access (public API)
    dinero::gui::DbProvider* dbProvider() const { return m_dbProvider; }
    dinero::gui::BackgroundJobs* backgroundJobs() const { return m_backgroundJobs; }

    // Database management (public API)
    bool initializeDatabase();
    void shutdownDatabase();

    // Clean public API for headless mode
    bool ensureDatabase();  // Ensures database is initialized
    dinero::gui::DbProvider& db();  // Safe accessor (throws on failure)

private:
    static QString s_dataDirOverride;

    // Network validation
    bool validateNetworkState();

    // Database path utilities
    QString getDatabasePathForNetwork(const QString& network) const;

    // Migration support
    bool performStartupIntegrityCheck();
    bool migrateDatabase();

signals:
    void networkChanged(DnrNetwork newNetwork);
    void networkValidated(bool isValid, const QString& actualChain);
    void daemonStatusChanged(bool isRunning, bool isHealthy);
    void databaseInitialized(bool success);
    void integrityCheckCompleted(bool passed, const QString& error);
    void migrationCompleted(bool success, const QString& message);

private slots:
    void onDaemonStarted();
    void onDaemonStopped();
    void onHealthCheckCompleted(bool healthy);
    void onDaemonSupervisorStatusChanged(Network network, DaemonSupervisor::DaemonStatus status);
    void onDatabaseNetworkChanged(const QString& network);
    void onIntegrityCheckFailed(const QString& error);

private:
    explicit AppContext(QObject* parent = nullptr);
    ~AppContext() = default;
    
    // Singleton - no copy/move
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;
    
    // Network configuration
    DnrNetwork m_currentNetwork = DnrNetwork::Regtest;
    bool m_daemonRunning = false;
    bool m_daemonHealthy = false;
    
    // Daemon management
    DaemonSupervisor* m_daemonSupervisor;
    
    // Database management
    dinero::gui::DbProvider* m_dbProvider;
    dinero::gui::BackgroundJobs* m_backgroundJobs;
    
    // Database state
    bool m_databaseInitialized = false;
    int m_currentDbVersion = 0;
    
    // Network-specific configuration
    struct NetworkConfig {
        QString name;
        QString hrp;
        QString dataSubdir;
        int rpcPort;
    };
    
    static const NetworkConfig& configFor(DnrNetwork network);
};
