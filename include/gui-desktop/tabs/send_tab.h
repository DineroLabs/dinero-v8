#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QTextEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QCheckBox>
#include <QProgressBar>
#include <QSlider>
#include <QTimer>
#include <QStandardItemModel>
#include <QTableView>
#include "gui-desktop/utils/rpc_client.h"

/**
 * SendTab - Professional PSBT-based transaction sending
 * 
 * Features:
 * - Complete PSBT workflow (create → fund → sign → submit)
 * - Address validation (validateaddress)
 * - Fee estimation and slider
 * - Multiple output support
 * - RBF (Replace-by-Fee) option
 * - Mainnet safety guard (allow_mainnet checkbox)
 * - Transaction preview before signing
 * - Real-time fee calculation
 * - Professional error handling
 */
class SendTab : public QWidget {
    Q_OBJECT

public:
    enum SendState {
        Ready,
        CreatingPSBT,
        FundingPSBT,
        SigningPSBT,
        SubmittingPSBT,
        Completed,
        Error
    };

    explicit SendTab(QWidget *parent = nullptr);
    
    void setRpcClient(RpcClient *client);
    void resetForm();

public slots:
    void onConnectionChanged(bool connected);

private slots:
    // UI interactions
    void validateRecipientAddress();
    void calculateFee();
    void addRecipient();
    void removeRecipient();
    void onFeeSliderChanged(int value);
    void onCustomFeeChanged();
    void previewTransaction();
    void sendTransaction();
    void clearForm();
    
    // RPC callbacks
    void onAddressValidated(const QJsonObject &validation);
    void onPSBTCreated(const QString &psbt, const QString &txid);
    void onPSBTFunded(const QString &psbt, qint64 feeUna);
    void onPSBTSigned(const QString &psbt, bool complete);
    void onPSBTSubmitted(const QString &txid);
    void onRpcError(const QString &error);

private:
    void setupUI();
    void setupRecipientSection();
    void setupFeeSection();
    void setupOptionsSection();
    void setupPreviewSection();
    void setupControlSection();
    
    void updateSendState(SendState state);
    void updateFeeEstimate();
    void updatePreview();
    void validateForm();
    bool isFormValid() const;
    
    QString formatDIN(qint64 una) const;
    qint64 parseDIN(const QString &dinString) const;
    QString getNetworkType() const;

    // UI Components
    QVBoxLayout *m_mainLayout;
    
    // Recipient section
    QGroupBox *m_recipientGroup;
    QTableView *m_recipientTable;
    QStandardItemModel *m_recipientModel;
    QPushButton *m_addRecipientButton;
    QPushButton *m_removeRecipientButton;
    QLineEdit *m_addressEdit;
    QDoubleSpinBox *m_amountSpin;
    QLineEdit *m_labelEdit;
    QLabel *m_addressValidationLabel;
    
    // Fee section
    QGroupBox *m_feeGroup;
    QSlider *m_feeSlider;
    QLabel *m_feeRateLabel;
    QLabel *m_feeAmountLabel;
    QLabel *m_totalAmountLabel;
    QCheckBox *m_customFeeCheck;
    QDoubleSpinBox *m_customFeeSpin;
    QComboBox *m_feeTypeCombo;
    
    // Options section
    QGroupBox *m_optionsGroup;
    QCheckBox *m_rbfCheck;
    QCheckBox *m_allowMainnetCheck;
    QLabel *m_mainnetWarningLabel;
    
    // Preview section
    QGroupBox *m_previewGroup;
    QTextEdit *m_previewText;
    QPushButton *m_previewButton;
    
    // Control section
    QGroupBox *m_controlGroup;
    QPushButton *m_sendButton;
    QPushButton *m_clearButton;
    QProgressBar *m_progressBar;
    QLabel *m_statusLabel;
    QLabel *m_resultLabel;
    
    // Backend
    RpcClient *m_rpcClient;
    QTimer *m_validationTimer;
    
    // State
    bool m_isConnected;
    SendState m_currentState;
    QString m_currentPSBT;
    QString m_currentTxId;
    qint64 m_estimatedFee;
    qint64 m_totalAmount;
    bool m_isMainnet;
    
    // Helper methods
    void showToast(const QString &message);
    
    // Transaction data
    struct Recipient {
        QString address;
        qint64 amountUna;
        QString label;
        bool isValid;
    };
    QList<Recipient> m_recipients;
};

/**
 * FeeEstimator - Helper class for fee estimation
 */
class FeeEstimator : public QObject {
    Q_OBJECT
    
public:
    enum FeeLevel {
        Slow = 1,      // ~1 hour
        Normal = 6,    // ~10 minutes  
        Fast = 12      // ~5 minutes
    };
    
    explicit FeeEstimator(QObject *parent = nullptr);
    
    void setRpcClient(RpcClient *client);
    void estimateFee(FeeLevel level, std::function<void(double)> callback);
    
    static QString feeLevelToString(FeeLevel level);
    static double calculateFeeForSize(int vsize, double feeRate);
    
private:
    RpcClient *m_rpcClient;
};

/**
 * TransactionPreview - Widget for previewing transaction details
 */
class TransactionPreview : public QWidget {
    Q_OBJECT
    
public:
    explicit TransactionPreview(QWidget *parent = nullptr);
    
    void setTransactionData(const QList<QPair<QString, qint64>> &outputs,
                           qint64 fee, bool rbf, bool mainnet);
    void clear();
    
private:
    void updatePreview();
    QString formatDIN(qint64 una) const;
    
    QTextEdit *m_previewText;
    QList<QPair<QString, qint64>> m_outputs;
    qint64 m_fee;
    bool m_rbf;
    bool m_mainnet;
};
