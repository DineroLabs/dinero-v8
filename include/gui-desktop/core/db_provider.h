#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>
#include <sqlite3.h>
#include <mutex>
#include <atomic>
#include "gui-desktop/utils/sqlite_error_mapper.h"

namespace dinero {
namespace gui {

// Database types with different safety/performance trade-offs
enum class DatabaseType {
    WALLET,        // Highest safety: FULL sync, strict FK, no mmap
    BLOCKCHAIN,    // High safety: NORMAL sync, FK on, selective mmap
    MEMPOOL,       // Medium safety: NORMAL sync, FK off, mmap enabled
    ANALYTICS,     // Performance: NORMAL sync, FK off, mmap + query_only
    MAINTENANCE_RO // Read-only maintenance connections
};

// Fault injection for chaos testing
struct FaultConfig {
    int seed = 0;              // Random seed for reproducible faults
    double rate = 0.0;         // Fault probability (0.0 = no faults, 1.0 = all faults)
    bool busyInjection = false; // Inject SQLITE_BUSY randomly
    bool slowIo = false;       // Slow down I/O operations
    bool shortReads = false;   // Return short reads randomly
    int slowIoMs = 100;        // Milliseconds to delay I/O
};

static FaultConfig getFaultConfig() {
    FaultConfig config;

    // Parse DINERO_FAULTS environment variable: "seed=123,rate=0.1,busy,slowio=50"
    QString faultsEnv = qEnvironmentVariable("DINERO_FAULTS");
    if (!faultsEnv.isEmpty()) {
        QStringList parts = faultsEnv.split(',');
        for (const QString& part : parts) {
            QStringList kv = part.trimmed().split('=');
            if (kv.size() == 2) {
                QString key = kv[0].toLower();
                QString value = kv[1];

                if (key == "seed") {
                    config.seed = value.toInt();
                } else if (key == "rate") {
                    config.rate = value.toDouble();
                } else if (key == "slowio") {
                    config.slowIoMs = value.toInt();
                    config.slowIo = true;
                }
            } else {
                QString flag = part.trimmed().toLower();
                if (flag == "busy") config.busyInjection = true;
                else if (flag == "shortreads") config.shortReads = true;
                else if (flag == "slowio") config.slowIo = true;
            }
        }

        // Fault injection enabled (logged via qInfo when faults are actually used)
    }

    return config;
}

// Per-database PRAGMA configuration matrix
struct DatabaseConfig {
    QString journalMode;
    QString synchronous;
    bool foreignKeys;
    bool trustedSchema;
    int busyTimeoutMs;
    QString tempStore;
    int walAutocheckpoint;
    int mmapSize;
    bool queryOnly;
    QString cacheSize;
    int pageSize;  // 0 = default
};

/**
 * Get PRAGMA configuration for specific database type
 */
static DatabaseConfig getDatabaseConfig(DatabaseType type) {
    switch (type) {
        case DatabaseType::WALLET:
            return {
                "WAL", "FULL", true, false, 120000, "MEMORY",
                0, 0, false, "-2000", 4096  // Conservative for wallet safety
            };

        case DatabaseType::BLOCKCHAIN:
            return {
                "WAL", "NORMAL", true, false, 30000, "MEMORY",
                100, 268435456, false, "-8000", 4096  // Balanced for blockchain
            };

        case DatabaseType::MEMPOOL:
            return {
                "WAL", "NORMAL", false, false, 10000, "MEMORY",
                50, 268435456, false, "-4000", 4096  // Performance for mempool
            };

        case DatabaseType::ANALYTICS:
            return {
                "WAL", "NORMAL", false, false, 5000, "MEMORY",
                25, 268435456, true, "-2000", 4096  // Query-only for analytics
            };

        case DatabaseType::MAINTENANCE_RO:
            return {
                "WAL", "NORMAL", false, false, 5000, "MEMORY",
                0, 268435456, true, "-1000", 4096  // Read-only maintenance
            };

        default:
            return getDatabaseConfig(DatabaseType::ANALYTICS);  // Safe fallback
    }
}

/**
 * Centralized SQLite database provider for GUI applications
 * Implements RAII + WAL stack with proper error handling and contention management
 */
class DbProvider : public QObject {
    Q_OBJECT

public:
    explicit DbProvider(QObject* parent = nullptr);
    ~DbProvider();

#ifdef QT_DEBUG
    // Debug-only re-entrancy prevention
    struct LockMark {
        std::atomic<std::thread::id> owner{};
        void onLock()   { owner.store(std::this_thread::get_id(), std::memory_order_relaxed); }
        void onUnlock() { owner.store(std::thread::id{},          std::memory_order_relaxed); }
        bool heldByThisThread() const { return owner.load() == std::this_thread::get_id(); }
    };
    LockMark m_lockMark;
    void ASSERT_NOT_LOCKED_HERE() { Q_ASSERT(!m_lockMark.heldByThisThread()); }
#endif

    // Helper for queued connections (thread-safe signals)
    template<typename... Args>
    static auto qconnect(Args&&... args) {
        return QObject::connect(std::forward<Args>(args)..., Qt::QueuedConnection);
    }

    // Database management
    bool initialize(const QString& dataDir, const QString& network = "mainnet");
    void safeInitialize(const QString& dataDir, const QString& network = "mainnet");
    void shutdown();
    void safeShutdown();

    // Database configuration
    void applyDatabaseConfig(sqlite3* db, DatabaseType type);
    bool hasEnoughDiskSpace(const QString& path, qint64 requiredBytes);

    // Support Bundle for debugging
    QString createSupportBundle(const QString& outputDir = QString()) const;
    QString createSupportBundleWithResolvedPaths(const QString& outputDir, const QString& dataDir, const QString& dbPath) const;

    // Lock detection for headless mode
    static bool probeLockedBySQLite(const QString& dbPath);
    
    // Database access
    sqlite3* getDatabase() const { return m_database; }
    bool isConnected() const { return m_database != nullptr; }
    
    // Network management
    void switchNetwork(const QString& network);
    QString currentNetwork() const { return m_currentNetwork; }
    QString dataDir() const { return m_dataDir; }
    
    // Health monitoring
    bool performIntegrityCheck();
    std::pair<bool, QString> safePerformIntegrityCheck();
    bool performIntegrityCheckReadOnly();
    int getBusyRetryCount() const { return m_busyRetryCount.load(); }
    void resetBusyRetryCount() { m_busyRetryCount.store(0); }

    // WAL management
    bool checkpointWAL();
    bool checkpointWALPassive();
    bool checkpointWALOnShutdown();
    qint64 getWALSize() const;
    int getWALFrames() const;
    QString getLastError() const;

    // Writer tracking for safe checkpointing
    void incrementWriterCount();
    void decrementWriterCount();
    int getWriterCount() const;

    // Maintenance mode for safe operations
    void enterMaintenanceMode();
    void exitMaintenanceMode();
    bool isInMaintenanceMode() const;
    bool isDatabaseLocked(const QString& dbPath) const;
    bool isDatabaseLockedBySQLite(const QString& dbPath) const;
    
    // Migration support
    int getUserVersion() const;
    bool setUserVersion(int version);
    bool backupDatabase(const QString& backupPath) const;

signals:
    void databaseOpened(const QString& path);
    void databaseClosed();
    void networkChanged(const QString& network);
    void integrityCheckFailed(const QString& error);
    void busyRetryOccurred(int retryCount);
    void checkpointCompleted(qint64 walSize);
    void databaseError(const QString& userMessage, const QString& technicalDetails, bool isRetryable);
    void contentionDetected(int retryCount, int maxRetries);

private slots:
    void performPeriodicCheckpoint();

private:
    bool openDatabase(const QString& dbPath);
    void closeDatabase();
    bool applyPragmas();
    QString getDatabasePath(const QString& network) const;
public:
    QString getCurrentDatabasePath() const;
    
    // Database state
    sqlite3* m_database{nullptr};
    QString m_dataDir;
    QString m_currentDbPath;
    QString m_currentNetwork;
    std::atomic<int> m_busyRetryCount{0};
    std::atomic<int> m_writerCount{0};
    std::atomic<bool> m_maintenanceMode{false};
    QString m_lastError;

    // WAL checkpoint timer
    QTimer* m_checkpointTimer{nullptr};

    // Thread safety
    mutable std::mutex m_databaseMutex;
    mutable std::mutex m_maintenanceMutex;
    
    // Configuration
    static constexpr int BUSY_TIMEOUT_MS = 5000;
    static constexpr int CHECKPOINT_INTERVAL_MS = 30000; // 30 seconds
};

} // namespace gui
} // namespace dinero
