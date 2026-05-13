// Integration additions for MainWindow
// Add these to the existing MainWindow class

#include "gui-desktop/components/dev_pane.h"
#include "gui-desktop/components/health_monitor.h"
#include "gui-desktop/dialogs/about_dialog.h"
#include "gui-desktop/dialogs/diagnostics_dialog.h"

// Add to MainWindow constructor after existing UI setup:
void MainWindow::integrateNewComponents() {
    // Add health monitor to status bar
    m_healthMonitor = new HealthMonitor(this);
    m_healthMonitor->setRpcClient(m_rpcClient);
    statusBar()->addPermanentWidget(m_healthMonitor);
    
    // Connect health monitor signals
    connect(m_healthMonitor, &HealthMonitor::healthStatusChanged, 
            this, &MainWindow::onHealthStatusChanged);
    connect(m_healthMonitor, &HealthMonitor::diagnosticsRequested,
            this, &MainWindow::showDiagnostics);
    
    // Add dev pane to tab widget (will be hidden unless regtest)
    m_devPane = new DevPane(this);
    m_devPane->setRpcClient(m_rpcClient);
    m_tabWidget->addTab(m_devPane, "🔧 Dev Tools");
    
    // Connect dev pane signals
    connect(m_devPane, &DevPane::blockMined, 
            this, &MainWindow::onBlockMined);
    
    // Update Help menu with About dialog
    if (m_helpMenu) {
        m_helpMenu->addSeparator();
        auto* aboutAction = m_helpMenu->addAction("About Dinero Desktop");
        connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
    }
}

// Add these slot implementations:
void MainWindow::onHealthStatusChanged(bool healthy) {
    // Gray out certain tabs if daemon is offline
    if (!healthy) {
        // Disable blockchain-dependent tabs
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            if (m_tabWidget->tabText(i).contains("Blocks") || 
                m_tabWidget->tabText(i).contains("Mempool")) {
                m_tabWidget->setTabEnabled(i, false);
                m_tabWidget->setTabToolTip(i, "Daemon offline - tab disabled");
            }
        }
    } else {
        // Re-enable all tabs
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            m_tabWidget->setTabEnabled(i, true);
            m_tabWidget->setTabToolTip(i, "");
        }
    }
}

void MainWindow::onBlockMined(const QString& blockHash) {
    // Show toast notification for mined block
    statusBar()->showMessage(QString("Block mined: %1").arg(blockHash.left(8) + "..."), 5000);
    
    // Refresh blockchain data
    updateSyncProgress();
}

void MainWindow::showAbout() {
    AboutDialog dialog(this);
    dialog.setRpcClient(m_rpcClient);
    dialog.exec();
}

void MainWindow::showDiagnostics() {
    DiagnosticsDialog dialog(this);
    dialog.setRpcClient(m_rpcClient);
    dialog.exec();
}

// Add these member variables to MainWindow.h:
private:
    HealthMonitor* m_healthMonitor = nullptr;
    DevPane* m_devPane = nullptr;
