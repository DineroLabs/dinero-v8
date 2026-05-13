#pragma once
#include <QWidget>
#include <QTimer>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;
class QCheckBox;
class QSpinBox;         // Added for thread control
class QSoundEffect;
class SparklineWidget;
class RpcClient;

class MiningPanel : public QWidget {
    Q_OBJECT
public:
    explicit MiningPanel(RpcClient* rpc, QWidget* parent = nullptr);
    
    void setActiveWallet(const QString& walletName);  // Set active wallet context
    QString getActiveWallet() const { return activeWallet_; }

private slots:
    void refreshOnce();                 // polls getmininginfo
    void pollEventsOnce();              // polls mining.events
    void onStart();
    void onStop();
    void onSetPayout();
    void onUseCurrentWallet();
    void onUseCurrentWalletStep2();     // async continuation
    void verifyPayoutAddress();         // verify payout belongs to current wallet
    void verifyPayoutAddressFallback(const QString& address);  // fallback validation
    void processMiningInfo(const QJsonObject& r);  // process mining info result
    void processMiningEvents(const QJsonArray& arr);  // process mining events result

private:
    RpcClient* rpc_;
    QString activeWallet_;  // Track active wallet for consistency
    QTimer poll_;
    QTimer pollLog_;
    qint64 lastEventId_ = 0;

    // UI
    QLabel*  lblStatus_;
    QLabel*  lblHashrate_;
    QLabel*  lblBits_;
    QLabel*  lblBlocks_;
    QLabel*  lblLastTime_;
    QLabel*  lblResolved_;
    QLabel*  lblVerification_;  // NEW: Payout verification status
    QLineEdit* edPayout_;
    QPushButton* btnStart_;
    QPushButton* btnStop_;
    QPushButton* btnSet_;
    QPushButton* btnUseWallet_;
    QSpinBox* spinThreads_;     // CPU thread control
    SparklineWidget* spark_;
    
    // Mining log
    QTextEdit* log_;              // Changed from QPlainTextEdit for colored lines
    QCheckBox* cbShowHashrate_;
    QCheckBox* cbAutoscroll_;
    QCheckBox* cbSound_;          // NEW: Sound on block found
    
    QSoundEffect* ding_ = nullptr; // NEW: Sound effect
};
