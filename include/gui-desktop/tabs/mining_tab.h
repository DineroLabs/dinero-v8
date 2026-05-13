#pragma once
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QLineEdit>
#include <QTextEdit>
#include <QTimer>
#include <QDateTime>
#include <QJsonObject>
#include <QSlider>
#include <QComboBox>
#include <QCheckBox>
#include <memory>

class RpcClient;
class MiningController;

class MiningTab : public QWidget {
    Q_OBJECT
    
public:
    explicit MiningTab(QWidget *parent = nullptr);
    void setRpcClient(RpcClient *client);
    void setMiningController(std::shared_ptr<MiningController> controller);

public slots:
    void onMiningUpdateReceived(bool active, double hashrate);
    void onConnectionChanged(bool connected);

private slots:
    void startMining();
    void stopMining();
    void updateMiningStatus();
    void onMiningStatusReceived(const QJsonObject &status);
    
    // New mining address functionality
    void generateMiningAddress();
    void onMiningAddressGenerated(const QString& address, const QString& label);
    void loadCurrentMiningAddress();
    void onCurrentMiningAddressLoaded(const QString& address);
    void updateMiningEarnings();
    
    // MiningController integration
    void onMiningStarted();
    void onMiningStopped(const QString& reason);
    void onMiningStatusUpdated();
    void onMiningError(const QString& error);
    
    // UI control slots
    void onThreadsChanged();
    void onThrottleChanged();
    void onLowPowerModeChanged();

private:
    void setupUI();
    void logActivity(const QString &message);
    
    // UI Components
    QLineEdit *m_addressLineEdit;
    QPushButton *m_generateAddressBtn;
    QComboBox *m_threadComboBox;
    QSlider *m_throttleSlider;
    QLabel *m_throttleLabel;
    QCheckBox *m_lowPowerModeCheckBox;
    QCheckBox *m_iUnderstandCheckBox;
    QPushButton *m_startButton;
    QPushButton *m_stopButton;
    QLabel *m_statusLabel;
    QLabel *m_hashrateLabel;
    QLabel *m_activeThreadsLabel;
    QLabel *m_currentHeightLabel;
    QLabel *m_blocksFoundLabel;
    QTextEdit *m_activityLog;
    
    // New reward and phase info components
    QLabel *m_currentRewardLabel;
    QLabel *m_phaseInfoLabel;
    QLabel *m_totalMinedLabel;
    QLabel *m_nextHalvingLabel;
    
    // Backend
    RpcClient *m_rpcClient;
    std::shared_ptr<MiningController> m_miningController;
    QTimer *m_updateTimer;
    
    // State
    bool m_isMining;
    double m_currentHashrate;
    int m_currentThreads;
};
