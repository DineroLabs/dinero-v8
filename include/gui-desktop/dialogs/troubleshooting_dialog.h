#pragma once

#include <QDialog>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QTimer>
#include "gui-desktop/core/db_provider.h"
#include "gui-desktop/core/background_jobs.h"

namespace dinero {
namespace gui {

/**
 * Database troubleshooting dialog for GUI applications
 * Provides integrity check, VACUUM, backup, and DB path viewer functionality
 */
class TroubleshootingDialog : public QDialog {
    Q_OBJECT

public:
    explicit TroubleshootingDialog(DbProvider* dbProvider, BackgroundJobs* backgroundJobs, 
                                 QWidget* parent = nullptr);
    ~TroubleshootingDialog();

private slots:
    void runIntegrityCheck();
    void runDeepIntegrityCheck();
    void runVacuum();
    void createBackup();
    void showDatabasePath();
    void openDatabaseFolder();
    void cancelOperation();
    void onIntegrityCheckCompleted(bool passed, const QString& error);
    void onVacuumCompleted(bool success, const QString& message);
    void onBackupCompleted(bool success, const QString& message);
    void onJobStarted(const QString& jobName);
    void onJobCompleted(const QString& jobName);
    void onJobFailed(const QString& jobName, const QString& error);

private:
    void setupUI();
    void updateStatus(const QString& message, bool isError = false);
    void clearStatus();
    void setButtonsEnabled(bool enabled);
    void updateDatabaseInfo();
    QString getBackupPath() const;
    
    // UI components
    QVBoxLayout* m_mainLayout;
    QGroupBox* m_actionsGroup;
    QGroupBox* m_statusGroup;
    QGroupBox* m_infoGroup;
    
    QPushButton* m_integrityButton;
    QPushButton* m_deepIntegrityButton;
    QPushButton* m_vacuumButton;
    QPushButton* m_backupButton;
    QPushButton* m_pathButton;
    QPushButton* m_folderButton;
    QPushButton* m_cancelButton;
    QPushButton* m_closeButton;
    
    QProgressBar* m_progressBar;
    QLabel* m_statusLabel;
    QTextEdit* m_infoText;
    
    // Core components
    DbProvider* m_dbProvider;
    BackgroundJobs* m_backgroundJobs;
    
    // State
    bool m_operationInProgress;
};

} // namespace gui
} // namespace dinero
