#pragma once

#include <QWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QProgressBar>
#include <QFileDialog>
#include <QJsonObject>
#include <QJsonValue>
#include "rpcclient.h"

class HardwareWalletWidget : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY(HardwareWalletWidget)

public:
    explicit HardwareWalletWidget(RpcClient* rpc, QWidget* parent = nullptr);
    void loadPsbtForSigning(const QString& psbt, const QString& contextMessage = QString());
    bool hasConnectedDirectSigner() const;

public Q_SLOTS:
    // Drop wallet-bound operational state on wallet switch.
    // Anything signed/finalized under the previous wallet must NOT
    // remain queued for broadcast under the newly active wallet,
    // and exported descriptors are tied to the device path used
    // to import them into a specific wallet.
    void clearWalletState();

private Q_SLOTS:
    void onExportPSBT();
    void onImportPSBT();
    void onAnalyzePSBT();
    void onUsbSignPSBT();
    void onFinalizePSBT();
    void onBroadcastTransaction();
    void onDeviceDetect();
    void onDeviceConnect();
    void onUsbFetchAddress();
    void onUsbVerifyAddress();
    void onUsbExportAccountDescriptor();
    void onUsbImportAccountDescriptor();

    // RPC result handlers
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);

Q_SIGNALS:
    void transactionBroadcasted(const QString& txid, bool linkedSendFlow);

private:
    void setupUI();
    void setupFileBasedTab();
    void setupQRCodeTab();
    void setupUSBTab();
    void showError(const QString& message);
    void showSuccess(const QString& message);

    // Result handlers
    void handleExportResult(const QJsonObject& result);
    void handleImportResult(const QJsonObject& result);
    void handleAnalyzeResult(const QJsonObject& result);
    void handleDeviceEnumerationResult(const QJsonObject& result);
    void handleDeviceConnectResult(const QJsonObject& result);
    void handleDeviceDisconnectResult(const QJsonObject& result);
    void handleUsbSignResult(const QJsonObject& result);
    void handleUsbAddressResult(const QJsonObject& result);
    void handleUsbDescriptorResult(const QJsonObject& result);
    void handleUsbImportDescriptorResult(const QJsonValue& result);
    void handleFinalizeResult(const QJsonObject& result);
    void handleBroadcastResult(const QJsonObject& result);
    void updateUsbConnectButton();
    void updateUsbAddressButtons();
    void updateUsbDescriptorControls();
    void updateFileActionButtons();
    void resetQrDisplay();
    void renderDemoQrDisplay();
    void renderQrForCurrentPsbt();
    void renderPsbtQrImage(const QString& psbt);
    QString formatPsbtAnalysis(const QJsonObject& result) const;
    void applySignedPsbtResult(const QJsonObject& result, const QString& actionLabel);

    RpcClient* rpc_;
    QString currentOperation_;
    QString currentFilepath_;
    QString defaultUsbDeviceInfoText_;
    QString connectedUsbDeviceId_;
    QString pendingBroadcastHex_;
    bool connectedUsbCanSign_ = false;
    bool connectedUsbCanGetAddress_ = false;
    bool connectedUsbCanVerifyAddress_ = false;
    bool connectedUsbCanExportAccountDescriptor_ = false;
    bool linkedSendContext_ = false;
    bool suppressLinkedSendReset_ = false;
    bool pendingQrRenderAfterAnalyze_ = false;
    bool hasUsbExportedDescriptors_ = false;
    QString exportedUsbReceiveDescriptor_;
    QString exportedUsbChangeDescriptor_;
    QString exportedUsbDescriptorPolicy_;
    int exportedUsbDescriptorAccount_ = 0;

    // File-Based Tab (Coldcard)
    QWidget* fileTab_;
    QLineEdit* exportPathEdit_;
    QLineEdit* importPathEdit_;
    QTextEdit* psbtTextEdit_;
    QPushButton* exportBtn_;
    QPushButton* importBtn_;
    QPushButton* analyzeBtn_;
    QPushButton* usbSignBtn_;
    QPushButton* finalizeBtn_;
    QPushButton* broadcastBtn_;
    QLabel* fileStatusLabel_;
    QProgressBar* fileProgressBar_;

    // QR Code Tab (Keystone, Passport)
    QWidget* qrTab_;
    QLabel* qrDisplayLabel_;
    QPushButton* showQRBtn_;
    QPushButton* scanQRBtn_;
    QTextEdit* qrPsbtEdit_;
    QLabel* qrSummaryLabel_;
    QLabel* qrStatusLabel_;

    // USB Tab (Ledger, Trezor)
    QWidget* usbTab_;
    QComboBox* deviceCombo_;
    QPushButton* detectBtn_;
    QPushButton* connectBtn_;
    QLineEdit* usbDerivationPathEdit_;
    QPushButton* usbFetchAddressBtn_;
    QPushButton* usbVerifyAddressBtn_;
    QLabel* usbAddressResultLabel_;
    QLineEdit* usbAccountPathEdit_;
    QComboBox* usbDescriptorPolicyCombo_;
    QPushButton* usbExportDescriptorBtn_;
    QPushButton* usbImportDescriptorBtn_;
    QTextEdit* usbDescriptorResultEdit_;
    QLabel* deviceInfoLabel_;
    QLabel* usbStatusLabel_;
    QTextEdit* usbLogEdit_;

    // Tab widget
    QTabWidget* tabs_;
};
