#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include "gui-desktop/utils/database_migrator.h"

namespace dinero {
namespace gui {

/**
 * Wallet database migrations
 * Manages schema evolution for wallet databases
 */
class WalletMigrations : public QObject {
    Q_OBJECT

public:
    explicit WalletMigrations(QObject* parent = nullptr);
    ~WalletMigrations() = default;

    // Initialize migrations
    void initialize();

    // Migration operations
    DatabaseMigrator::MigrationResult migrateWallet(const QString& databasePath,
                                                    int currentVersion,
                                                    int targetVersion);

private:
    // Migration methods (static for use as function pointers)
    static bool migrateFromV0ToV1();
    static bool migrateFromV1ToV2();
    static bool migrateFromV2ToV3();

    // Rollback methods
    static bool rollbackV1ToV0();
    static bool rollbackV2ToV1();
    static bool rollbackV3ToV2();

    // Database helpers
    static bool createInitialTables();
    static bool addTransactionIndex();
    static bool addAddressBookTable();
    static bool addMetadataTable();
    static bool updateIndexes();

    // Migration descriptions
    static const QStringList MIGRATION_DESCRIPTIONS;
};

// Global migration initialization
void initializeWalletMigrations();

} // namespace gui
} // namespace dinero
