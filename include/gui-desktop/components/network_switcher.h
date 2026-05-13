#pragma once

#include <QWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QMessageBox>

class NetworkSwitcher : public QWidget {
    Q_OBJECT

public:
    enum Network {
        REGTEST,
        TESTNET,
        MAINNET
    };

    explicit NetworkSwitcher(QWidget* parent = nullptr);
    
    // Network management
    void setCurrentNetwork(Network network);
    Network getCurrentNetwork() const { return m_currentNetwork; }
    QString getCurrentNetworkString() const;
    
    // Port configuration
    int getRpcPort() const;
    int getP2pPort() const;
    int getWebSocketPort() const;
    
    // Static port helpers for any network
    static int getRpcPortForNetwork(Network network);
    static int getP2pPortForNetwork(Network network);
    static int getWebSocketPortForNetwork(Network network);
    
    // Data directory paths
    QString getNetworkDataDir() const;
    QString getNetworkName() const;
    
    // UI state
    void setEnabled(bool enabled);
    void setSwitchInProgress(bool inProgress);

signals:
    void networkChangeRequested(Network newNetwork);
    void networkSwitched(Network network);

private slots:
    void onNetworkSelectionChanged();
    void onSwitchButtonClicked();
    void onSwitchProgressUpdate();

private:
    // UI Components
    QHBoxLayout* m_mainLayout;
    QLabel* m_networkIcon;
    QLabel* m_networkLabel;
    QComboBox* m_networkCombo;
    QPushButton* m_switchButton;
    QProgressBar* m_switchProgress;
    QLabel* m_statusLabel;
    
    // State
    Network m_currentNetwork = REGTEST;
    Network m_pendingNetwork = REGTEST;
    bool m_switchInProgress = false;
    QTimer* m_progressTimer;
    int m_progressValue = 0;
    
    // Setup methods
    void setupUI();
    void setupConnections();
    void populateNetworkCombo();
    
    // Network helpers
    QString getNetworkDisplayName(Network network) const;
    QString getNetworkIcon(Network network) const;
    QColor getNetworkColor(Network network) const;
    QString getNetworkDescription(Network network) const;
    
    // Port configuration (instance methods)
    int getRpcPortForCurrentNetwork() const;
    int getP2pPortForCurrentNetwork() const; 
    int getWebSocketPortForCurrentNetwork() const;
    
    // UI updates
    void updateNetworkDisplay();
    void updateSwitchButton();
    void showConfirmationDialog();
    
    // Network validation
    bool isNetworkSwitchSafe(Network fromNetwork, Network toNetwork) const;
    QString getNetworkSwitchWarning(Network fromNetwork, Network toNetwork) const;
};
