#pragma once

#include <QString>
#include <QList>
#include <QJsonObject>
#include <functional>
#include <memory>

namespace dinero {
namespace gui {

/**
 * Database migration framework for GUI applications
 * Handles schema upgrades with backup and rollback capabilities
 */
class DatabaseMigrator {
public:
    // Migration operation result
    struct MigrationResult {
        bool success;
        QString message;
        QString backupPath;
        int fromVersion;
        int toVersion;
    };

    // Migration step
    struct MigrationStep {
        int fromVersion;
        int toVersion;
        QString description;
        std::function<bool()> migrate;
        std::function<bool()> rollback;
    };

    DatabaseMigrator();
    ~DatabaseMigrator();

    // Add migration steps
    void addMigration(int fromVersion, int toVersion,
                     const QString& description,
                     std::function<bool()> migrate,
                     std::function<bool()> rollback = nullptr);

    // Perform migrations
    MigrationResult migrate(const QString& databasePath,
                           int currentVersion,
                           int targetVersion,
                           bool createBackup = true);

    // Get available migrations
    QList<MigrationStep> getAvailableMigrations(int currentVersion, int targetVersion);

    // Utility functions
    static QString createBackupName(const QString& databasePath, int fromVersion, int toVersion);
    static bool createBackup(const QString& sourcePath, const QString& backupPath);
    static bool restoreBackup(const QString& backupPath, const QString& targetPath);

private:
    QList<MigrationStep> m_migrations;
    QString m_backupDirectory;
};

// Migration registry - global instance for managing migrations
class MigrationRegistry {
public:
    static MigrationRegistry& instance();

    void registerMigration(int fromVersion, int toVersion,
                          const QString& description,
                          std::function<bool()> migrate,
                          std::function<bool()> rollback = nullptr);

    DatabaseMigrator::MigrationResult runMigrations(const QString& databasePath,
                                                    int currentVersion,
                                                    int targetVersion);

private:
    MigrationRegistry();
    std::unique_ptr<DatabaseMigrator> m_migrator;
};

} // namespace gui
} // namespace dinero
