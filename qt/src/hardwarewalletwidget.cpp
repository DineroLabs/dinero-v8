#include "hardwarewalletwidget.h"
#include "rpcclient.h"
#include "QrUtil.h"
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QDateTime>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalBlocker>
#include <QStringList>
#include <QVariant>

namespace {

QString hwButtonStyle()
{
    return QStringLiteral(
        "QPushButton { min-height: 30px; padding: 6px 12px; "
        "background: #2b3037; color: #e6ebf1; border: 1px solid #3c434d; "
        "border-radius: 7px; font-weight: 600; } "
        "QPushButton:hover { background: #333942; } "
        "QPushButton:pressed { background: #262b31; } "
        "QPushButton:disabled { background: #21252a; color: #7f8893; "
        "border: 1px solid #30353d; }");
}

QString hwPanelStyle()
{
    return QStringLiteral(
        "QLabel { padding: 7px 10px; background: #1f2328; color: #d7dde5; "
        "border: 1px solid #353b44; border-radius: 6px; }");
}

QString hwSubtleLabelStyle()
{
    return QStringLiteral("color: #9aa4af; padding: 5px 10px; margin-bottom: 10px;");
}

bool hwPathUsesRetiredCoinType(const QString& path)
{
    const QString normalized = path.trimmed();
    return normalized.contains("/1447'/") ||
           normalized.contains("/1447h/") ||
           normalized.contains("/1447H/") ||
           normalized.endsWith("/1447'") ||
           normalized.endsWith("/1447h") ||
           normalized.endsWith("/1447H");
}

QString hwRetiredCoinTypeMessage()
{
    return QStringLiteral("coin_type 1447 is permanently retired. Use coin_type 1448 paths such as m/86'/1448'/0'/0/0.");
}

QString dineroQrLogoPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/../Resources/Dinero-Coin.png"),
        appDir + QStringLiteral("/Dinero-Coin.png"),
        appDir + QStringLiteral("/../Dinero-Coin.png"),
        QDir::currentPath() + QStringLiteral("/Dinero-Coin.png")
    };

    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    return QString();
}

QString demoQrPayload()
{
    return QStringLiteral(
        "DINERO DEMO QR ONLY\n"
        "This is not a transaction.\n"
        "A real Dinero PSBT QR will be created after a PSBT is loaded.");
}

} // namespace

HardwareWalletWidget::HardwareWalletWidget(RpcClient* rpc, QWidget* parent)
    : QWidget(parent), rpc_(rpc) {
    setupUI();

    // Connect to RPC signals
    connect(rpc_, &RpcClient::rpcResult, this, &HardwareWalletWidget::onRpcResult);
    connect(rpc_, &RpcClient::rpcError, this, &HardwareWalletWidget::onRpcError);
}

void HardwareWalletWidget::loadPsbtForSigning(const QString& psbt, const QString& contextMessage) {
    if (!psbtTextEdit_) {
        return;
    }

    suppressLinkedSendReset_ = true;
    psbtTextEdit_->setPlainText(psbt.trimmed());
    suppressLinkedSendReset_ = false;
    if (qrPsbtEdit_) {
        qrPsbtEdit_->setPlainText(psbt.trimmed());
    }
    linkedSendContext_ = true;
    pendingBroadcastHex_.clear();
    currentOperation_.clear();

    if (tabs_ && fileTab_) {
        tabs_->setCurrentWidget(fileTab_);
    }

    if (fileStatusLabel_) {
        fileStatusLabel_->setText(
            contextMessage.isEmpty()
                ? QStringLiteral("✅ PSBT loaded from Send tab")
                : contextMessage);
    }

    renderQrForCurrentPsbt();
    updateFileActionButtons();
}

bool HardwareWalletWidget::hasConnectedDirectSigner() const {
    return !connectedUsbDeviceId_.isEmpty() && connectedUsbCanSign_;
}

void HardwareWalletWidget::clearWalletState() {
    // Operational state signed under or imported into the previous
    // wallet must not survive into the next one. A finalized PSBT
    // belongs to the wallet whose keys produced it; allowing it to
    // sit in the broadcast-ready slot while the user switches
    // wallets would let one wallet's session broadcast another
    // wallet's transaction.
    pendingBroadcastHex_.clear();
    currentOperation_.clear();
    currentFilepath_.clear();
    linkedSendContext_ = false;

    if (psbtTextEdit_) {
        suppressLinkedSendReset_ = true;
        psbtTextEdit_->clear();
        suppressLinkedSendReset_ = false;
    }
    if (qrPsbtEdit_) {
        qrPsbtEdit_->clear();
    }
    resetQrDisplay();
    if (exportPathEdit_) {
        exportPathEdit_->clear();
    }
    if (importPathEdit_) {
        importPathEdit_->clear();
    }
    if (fileStatusLabel_) {
        fileStatusLabel_->clear();
    }
    if (fileProgressBar_) {
        fileProgressBar_->setVisible(false);
    }
    if (qrStatusLabel_) {
        qrStatusLabel_->clear();
    }

    // USB tab: descriptors exported from a hardware device were
    // imported into the previous wallet. Drop them so the user
    // re-exports/re-imports against the active wallet.
    hasUsbExportedDescriptors_ = false;
    exportedUsbReceiveDescriptor_.clear();
    exportedUsbChangeDescriptor_.clear();
    exportedUsbDescriptorPolicy_.clear();
    exportedUsbDescriptorAccount_ = 0;
    if (usbDescriptorResultEdit_) {
        usbDescriptorResultEdit_->clear();
    }
    if (usbAddressResultLabel_) {
        usbAddressResultLabel_->clear();
    }
    if (usbStatusLabel_) {
        usbStatusLabel_->clear();
    }
    if (usbLogEdit_) {
        usbLogEdit_->clear();
    }

    updateFileActionButtons();
    updateUsbAddressButtons();
    updateUsbDescriptorControls();
}

void HardwareWalletWidget::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);

    // Title
    auto* title = new QLabel("🔐 Hardware Wallet Integration");
    title->setStyleSheet("font-size: 18px; font-weight: bold; padding: 10px; color: #d6dde6;");
    mainLayout->addWidget(title);

    // Description
    auto* desc = new QLabel(
        "Sign transactions using hardware wallets (Coldcard, Ledger, Trezor, Keystone, etc.)\n"
        "PSBT here means Partially Signed Dinero Transaction. It is a binary signing container shown as Base64 for copy/paste, files, and QR transfer."
    );
    desc->setWordWrap(true);
    desc->setStyleSheet(hwSubtleLabelStyle());
    mainLayout->addWidget(desc);

    // Tab widget for different transport methods
    tabs_ = new QTabWidget;

    setupFileBasedTab();
    setupQRCodeTab();
    setupUSBTab();

    tabs_->addTab(fileTab_, "📁 File / SD Card");
    tabs_->addTab(qrTab_, "📷 QR Code");
    tabs_->addTab(usbTab_, "🔌 USB");

    mainLayout->addWidget(tabs_);

    setLayout(mainLayout);
}

void HardwareWalletWidget::setupFileBasedTab() {
    fileTab_ = new QWidget;
    auto* layout = new QVBoxLayout(fileTab_);

    // Info box
    auto* infoBox = new QGroupBox("📋 How it works");
    auto* infoLayout = new QVBoxLayout(infoBox);
    auto* info = new QLabel(
        "1. Create unsigned transaction in DineroCoin wallet\n"
        "2. Export the Partially Signed Dinero Transaction to file\n"
        "3. Sign Taproot inputs on your hardware wallet or use a connected USB session\n"
        "4. Import or finalize the signed Dinero PSBT\n"
        "5. Broadcast the extracted final transaction"
    );
    info->setWordWrap(true);
    infoLayout->addWidget(info);
    layout->addWidget(infoBox);

    // PSBT Input
    auto* psbtGroup = new QGroupBox("Partially Signed Dinero Transaction (PSBT)");
    auto* psbtLayout = new QVBoxLayout(psbtGroup);

    auto* psbtLabel = new QLabel("Dinero PSBT (Base64):");
    psbtTextEdit_ = new QTextEdit;
    psbtTextEdit_->setPlaceholderText("Paste Partially Signed Dinero Transaction here or create one from the Send tab...");
    psbtTextEdit_->setMaximumHeight(100);
    connect(psbtTextEdit_, &QTextEdit::textChanged, this, [this]() {
        if (!suppressLinkedSendReset_) {
            linkedSendContext_ = false;
        }
        pendingBroadcastHex_.clear();
        if (qrPsbtEdit_) {
            const QSignalBlocker block(qrPsbtEdit_);
            qrPsbtEdit_->setPlainText(psbtTextEdit_->toPlainText());
        }
        resetQrDisplay();
        updateFileActionButtons();
    });

    psbtLayout->addWidget(psbtLabel);
    psbtLayout->addWidget(psbtTextEdit_);
    layout->addWidget(psbtGroup);

    // Export Section
    auto* exportGroup = new QGroupBox("📤 Export to Hardware Wallet");
    auto* exportLayout = new QGridLayout(exportGroup);

    auto* exportLabel = new QLabel("Export Path:");
    exportPathEdit_ = new QLineEdit;
    exportPathEdit_->setPlaceholderText("/Volumes/COLDCARD/unsigned.psbt");

    auto* browseSaveBtn = new QPushButton("Browse...");
    browseSaveBtn->setStyleSheet(hwButtonStyle());
    connect(browseSaveBtn, &QPushButton::clicked, [this]() {
        QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/unsigned.psbt";
        QString filename = QFileDialog::getSaveFileName(
            this,
            "Export Dinero PSBT",
            defaultPath,
            "PSBT Files (*.psbt);;All Files (*)"
        );
        if (!filename.isEmpty()) {
            exportPathEdit_->setText(filename);
        }
    });

    exportBtn_ = new QPushButton("Export Dinero PSBT to File");
    exportBtn_->setStyleSheet(hwButtonStyle());
    connect(exportBtn_, &QPushButton::clicked, this, &HardwareWalletWidget::onExportPSBT);

    exportLayout->addWidget(exportLabel, 0, 0);
    exportLayout->addWidget(exportPathEdit_, 0, 1);
    exportLayout->addWidget(browseSaveBtn, 0, 2);
    exportLayout->addWidget(exportBtn_, 1, 0, 1, 3);

    layout->addWidget(exportGroup);

    // Import Section
    auto* importGroup = new QGroupBox("📥 Import from Hardware Wallet");
    auto* importLayout = new QGridLayout(importGroup);

    auto* importLabel = new QLabel("Import Path:");
    importPathEdit_ = new QLineEdit;
    importPathEdit_->setPlaceholderText("/Volumes/COLDCARD/signed.psbt");

    auto* browseOpenBtn = new QPushButton("Browse...");
    browseOpenBtn->setStyleSheet(hwButtonStyle());
    connect(browseOpenBtn, &QPushButton::clicked, [this]() {
        QString filename = QFileDialog::getOpenFileName(
            this,
            "Import Signed Dinero PSBT",
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
            "PSBT Files (*.psbt);;All Files (*)"
        );
        if (!filename.isEmpty()) {
            importPathEdit_->setText(filename);
        }
    });

    importBtn_ = new QPushButton("Import Signed Dinero PSBT");
    importBtn_->setStyleSheet(hwButtonStyle());
    connect(importBtn_, &QPushButton::clicked, this, &HardwareWalletWidget::onImportPSBT);

    importLayout->addWidget(importLabel, 0, 0);
    importLayout->addWidget(importPathEdit_, 0, 1);
    importLayout->addWidget(browseOpenBtn, 0, 2);
    importLayout->addWidget(importBtn_, 1, 0, 1, 3);

    layout->addWidget(importGroup);

    // Analyze Button
    analyzeBtn_ = new QPushButton("🔍 Analyze Dinero PSBT");
    analyzeBtn_->setStyleSheet(hwButtonStyle());
    connect(analyzeBtn_, &QPushButton::clicked, this, &HardwareWalletWidget::onAnalyzePSBT);
    layout->addWidget(analyzeBtn_);

    auto* actionLayout = new QHBoxLayout;

    usbSignBtn_ = new QPushButton("🔌 Sign via Connected USB");
    usbSignBtn_->setStyleSheet(hwButtonStyle());
    usbSignBtn_->setToolTip("Connect a USB device with direct PSBT-signing support in the USB tab, then sign the PSBT currently loaded here.");
    connect(usbSignBtn_, &QPushButton::clicked, this, &HardwareWalletWidget::onUsbSignPSBT);

    finalizeBtn_ = new QPushButton("✅ Finalize Dinero PSBT");
    finalizeBtn_->setStyleSheet(hwButtonStyle());
    finalizeBtn_->setToolTip("Attempt to finalize the current Partially Signed Dinero Transaction and extract a broadcastable transaction.");
    connect(finalizeBtn_, &QPushButton::clicked, this, &HardwareWalletWidget::onFinalizePSBT);

    broadcastBtn_ = new QPushButton("📡 Broadcast Transaction");
    broadcastBtn_->setStyleSheet(hwButtonStyle());
    broadcastBtn_->setToolTip("Broadcast the last finalized or imported transaction hex.");
    connect(broadcastBtn_, &QPushButton::clicked, this, &HardwareWalletWidget::onBroadcastTransaction);

    actionLayout->addWidget(usbSignBtn_);
    actionLayout->addWidget(finalizeBtn_);
    actionLayout->addWidget(broadcastBtn_);
    layout->addLayout(actionLayout);

    // Status
    fileStatusLabel_ = new QLabel("Ready");
    fileStatusLabel_->setStyleSheet(hwPanelStyle());
    layout->addWidget(fileStatusLabel_);

    fileProgressBar_ = new QProgressBar;
    fileProgressBar_->setVisible(false);
    layout->addWidget(fileProgressBar_);

    updateFileActionButtons();
    layout->addStretch();
}

void HardwareWalletWidget::setupQRCodeTab() {
    qrTab_ = new QWidget;
    auto* layout = new QVBoxLayout(qrTab_);

    // Info
    auto* infoBox = new QGroupBox("📋 QR Code Workflow");
    auto* infoLayout = new QVBoxLayout(infoBox);
    auto* info = new QLabel(
        "Use QR codes for air-gapped signing with devices like:\n"
        "• Keystone (formerly Cobo Vault)\n"
        "• Passport by Foundation Devices\n"
        "• AirGap Vault\n\n"
        "1. Display unsigned Dinero PSBT as QR code\n"
        "2. Scan with hardware wallet camera\n"
        "3. Sign on device\n"
        "4. Scan signed Dinero PSBT QR code back"
    );
    info->setWordWrap(true);
    infoLayout->addWidget(info);
    layout->addWidget(infoBox);

    // PSBT Input
    auto* psbtGroup = new QGroupBox("Partially Signed Dinero Transaction (PSBT)");
    auto* psbtLayout = new QVBoxLayout(psbtGroup);
    qrPsbtEdit_ = new QTextEdit;
    qrPsbtEdit_->setPlaceholderText("Paste Partially Signed Dinero Transaction here...");
    qrPsbtEdit_->setMaximumHeight(80);
    connect(qrPsbtEdit_, &QTextEdit::textChanged, this, [this]() {
        if (psbtTextEdit_) {
            const QSignalBlocker block(psbtTextEdit_);
            psbtTextEdit_->setPlainText(qrPsbtEdit_->toPlainText());
        }
        linkedSendContext_ = false;
        pendingBroadcastHex_.clear();
        resetQrDisplay();
        updateFileActionButtons();
    });
    psbtLayout->addWidget(qrPsbtEdit_);
    layout->addWidget(psbtGroup);

    // QR Display Area
    auto* displayGroup = new QGroupBox("📱 QR Code Display");
    auto* displayLayout = new QVBoxLayout(displayGroup);

    qrDisplayLabel_ = new QLabel;
    qrDisplayLabel_->setAlignment(Qt::AlignCenter);
    qrDisplayLabel_->setFixedSize(360, 360);
    qrDisplayLabel_->setStyleSheet(
        "border: 1px dashed #5d6570; background-color: white; color: #c8d0da;");
    qrDisplayLabel_->setText("QR code will appear here");

    auto* qrCenterLayout = new QHBoxLayout;
    qrCenterLayout->addStretch();
    qrCenterLayout->addWidget(qrDisplayLabel_);
    qrCenterLayout->addStretch();
    displayLayout->addLayout(qrCenterLayout);
    layout->addWidget(displayGroup);

    // Buttons
    auto* btnLayout = new QHBoxLayout;

    showQRBtn_ = new QPushButton("📤 Show QR Code");
    showQRBtn_->setStyleSheet(hwButtonStyle());
    connect(showQRBtn_, &QPushButton::clicked, [this]() {
        renderQrForCurrentPsbt();
    });

    scanQRBtn_ = new QPushButton("📥 Scan Signed QR");
    scanQRBtn_->setStyleSheet(hwButtonStyle());
    scanQRBtn_->setEnabled(false);
    scanQRBtn_->setToolTip("Camera scanning requires additional dependencies");

    btnLayout->addWidget(showQRBtn_);
    btnLayout->addWidget(scanQRBtn_);
    layout->addLayout(btnLayout);

    // Status
    qrStatusLabel_ = new QLabel("Ready");
    qrStatusLabel_->setStyleSheet(hwPanelStyle());
    layout->addWidget(qrStatusLabel_);

    qrSummaryLabel_ = new QLabel("Transaction summary will appear after a Dinero PSBT is analyzed.");
    qrSummaryLabel_->setWordWrap(true);
    qrSummaryLabel_->setStyleSheet(hwPanelStyle());
    layout->addWidget(qrSummaryLabel_);

    layout->addStretch();
    renderDemoQrDisplay();
}

void HardwareWalletWidget::resetQrDisplay() {
    const QString psbt = qrPsbtEdit_ ? qrPsbtEdit_->toPlainText().trimmed() : QString();
    if (psbt.isEmpty()) {
        renderDemoQrDisplay();
        return;
    }

    if (qrDisplayLabel_) {
        qrDisplayLabel_->clear();
        qrDisplayLabel_->setText("Analyze PSBT to show QR code");
    }
    if (qrStatusLabel_) {
        qrStatusLabel_->setText("PSBT loaded. Click Show QR Code to analyze and display the signing QR.");
        qrStatusLabel_->setStyleSheet(hwPanelStyle());
    }
    if (qrSummaryLabel_) {
        qrSummaryLabel_->setText("Transaction summary will appear after analysis.");
        qrSummaryLabel_->setStyleSheet(hwPanelStyle());
    }
}

void HardwareWalletWidget::renderDemoQrDisplay() {
    if (!qrDisplayLabel_) {
        return;
    }

    try {
        const QString logoPath = dineroQrLogoPath();
        const int qrSize = qMin(qrDisplayLabel_->width(), qrDisplayLabel_->height());
        const QImage qrImage = logoPath.isEmpty()
            ? QrUtil::makeQr(demoQrPayload(), qrSize, 4, /*ecLevel=*/3)
            : QrUtil::makeQrWithLogo(demoQrPayload(), logoPath, qrSize, 4);
        if (qrImage.isNull()) {
            qrDisplayLabel_->setText("Demo QR unavailable");
            return;
        }

        qrDisplayLabel_->setPixmap(QPixmap::fromImage(qrImage));
        if (qrStatusLabel_) {
            const QString logoNote = logoPath.isEmpty()
                ? QStringLiteral(" Logo file not found; demo generated without center logo.")
                : QString();
            qrStatusLabel_->setText(
                QStringLiteral("Demo QR only. A real signing QR appears after a Dinero PSBT is created or pasted.%1")
                    .arg(logoNote));
            qrStatusLabel_->setStyleSheet(hwPanelStyle());
        }
    } catch (const std::exception& e) {
        if (qrStatusLabel_) {
            qrStatusLabel_->setText(QString("Demo QR generation failed: %1").arg(e.what()));
        }
    }

    if (qrSummaryLabel_) {
        qrSummaryLabel_->setText("No transaction loaded. Scanning this demo QR reads a plain-text placeholder, not a spend request.");
        qrSummaryLabel_->setStyleSheet(hwPanelStyle());
    }
}

void HardwareWalletWidget::renderQrForCurrentPsbt() {
    if (!qrPsbtEdit_ || !qrDisplayLabel_) {
        return;
    }

    const QString psbt = qrPsbtEdit_->toPlainText().trimmed();
    if (psbt.isEmpty()) {
        renderDemoQrDisplay();
        return;
    }

    pendingQrRenderAfterAnalyze_ = true;
    currentOperation_ = "hwallet.analyzepsbt";
    if (qrStatusLabel_) {
        qrStatusLabel_->setText("Analyzing Dinero PSBT before QR display...");
        qrStatusLabel_->setStyleSheet(hwPanelStyle());
    }
    if (fileStatusLabel_) {
        fileStatusLabel_->setText("Analyzing Dinero PSBT before QR display...");
    }

    QJsonObject params;
    params["psbt"] = psbt;
    QJsonArray arr;
    arr.append(params);
    rpc_->call("hwallet.analyzepsbt", arr);
}

void HardwareWalletWidget::renderPsbtQrImage(const QString& psbt) {
    if (!qrDisplayLabel_) {
        return;
    }

    try {
        const QString logoPath = dineroQrLogoPath();
        const int qrSize = qMin(qrDisplayLabel_->width(), qrDisplayLabel_->height());
        const QImage qrImage = logoPath.isEmpty()
            ? QrUtil::makeQr(psbt, qrSize, 4, /*ecLevel=*/3)
            : QrUtil::makeQrWithLogo(psbt, logoPath, qrSize, 4);
        if (qrImage.isNull()) {
            showError("QR generation failed");
            return;
        }

        qrDisplayLabel_->setPixmap(QPixmap::fromImage(qrImage));
        if (qrStatusLabel_) {
            const int bytes = psbt.toUtf8().size();
            const QString logoNote = logoPath.isEmpty()
                ? QStringLiteral(" Logo file not found; generated without center logo.")
                : QString();
            qrStatusLabel_->setText(
                QStringLiteral("✅ Dinero PSBT QR generated (%1 bytes). Scan with your hardware wallet.%2")
                    .arg(bytes)
                    .arg(logoNote));
            qrStatusLabel_->setStyleSheet(hwPanelStyle());
        }
    } catch (const std::exception& e) {
        showError(QString("QR generation failed: %1").arg(e.what()));
    }
}

void HardwareWalletWidget::setupUSBTab() {
    usbTab_ = new QWidget;
    auto* layout = new QVBoxLayout(usbTab_);

    // Info
    auto* infoBox = new QGroupBox("🔌 USB Hardware Wallets");
    auto* infoLayout = new QVBoxLayout(infoBox);
    auto* info = new QLabel(
        "Current USB status:\n"
        "• Device detection is experimental and depends on backend USB support\n"
        "• Detection currently recognizes Ledger devices and Trezor devices when available\n"
        "• Ledger sessions support connect, fingerprint export, and direct PSBT signing\n"
        "• Trezor sessions can support connect, fingerprint export, address verification, watch-only descriptor export, and constrained BIP86 direct PSBT signing when the backend is built with ENABLE_TREZOR=ON\n"
        "• Trezor USB signing is limited to active-wallet BIP86 Taproot PSBTs that match the loaded descriptor set\n"
        "• File / SD Card and QR flows remain the fallback signing paths\n\n"
        "Click 'Detect Devices' to check whether this daemon build can see connected USB wallets."
    );
    info->setWordWrap(true);
    infoLayout->addWidget(info);
    layout->addWidget(infoBox);

    // Device Selection
    auto* deviceGroup = new QGroupBox("🔍 Device Detection");
    auto* deviceLayout = new QGridLayout(deviceGroup);

    auto* deviceLabel = new QLabel("Connected Devices:");
    deviceCombo_ = new QComboBox;
    deviceCombo_->addItem("Click 'Detect Devices' to scan");
    connect(deviceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateUsbConnectButton(); });

    detectBtn_ = new QPushButton("🔄 Detect Devices");
    detectBtn_->setStyleSheet(hwButtonStyle());
    connect(detectBtn_, &QPushButton::clicked, this, &HardwareWalletWidget::onDeviceDetect);

    connectBtn_ = new QPushButton("🔌 Connect");
    connectBtn_->setStyleSheet(hwButtonStyle());
    connectBtn_->setEnabled(false);
    connectBtn_->setToolTip("Open a supported USB device session after detection. Direct signing availability depends on the device family.");
    connect(connectBtn_, &QPushButton::clicked, this, &HardwareWalletWidget::onDeviceConnect);

    deviceLayout->addWidget(deviceLabel, 0, 0);
    deviceLayout->addWidget(deviceCombo_, 0, 1);
    deviceLayout->addWidget(detectBtn_, 1, 0);
    deviceLayout->addWidget(connectBtn_, 1, 1);

    layout->addWidget(deviceGroup);

    // Device Info
    defaultUsbDeviceInfoText_ =
        "USB detection only.\n"
        "Make sure your hardware wallet is:\n"
        "• Connected via USB\n"
        "• Unlocked (PIN entered)\n"
        "• On the correct coin app where required\n"
        "• Ready for USB inspection, fingerprint export, and where supported direct signing";
    deviceInfoLabel_ = new QLabel(defaultUsbDeviceInfoText_);
    deviceInfoLabel_->setWordWrap(true);
    deviceInfoLabel_->setStyleSheet(hwPanelStyle());
    layout->addWidget(deviceInfoLabel_);

    auto* addressGroup = new QGroupBox("🏷️ Address Verification");
    auto* addressLayout = new QGridLayout(addressGroup);

    auto* derivationLabel = new QLabel("Derivation Path:");
    usbDerivationPathEdit_ = new QLineEdit("m/86'/1448'/0'/0/0");
    usbDerivationPathEdit_->setPlaceholderText("m/86'/1448'/0'/0/0");
    connect(usbDerivationPathEdit_, &QLineEdit::textChanged, this, [this]() {
        updateUsbAddressButtons();
    });

    usbFetchAddressBtn_ = new QPushButton("📥 Get Address");
    usbFetchAddressBtn_->setStyleSheet(hwButtonStyle());
    usbFetchAddressBtn_->setToolTip("Fetch the address for the derivation path from the active USB session.");
    connect(usbFetchAddressBtn_, &QPushButton::clicked, this, &HardwareWalletWidget::onUsbFetchAddress);

    usbVerifyAddressBtn_ = new QPushButton("👁️ Verify on Device");
    usbVerifyAddressBtn_->setStyleSheet(hwButtonStyle());
    usbVerifyAddressBtn_->setToolTip("Ask the active device to show and confirm the address for this derivation path.");
    connect(usbVerifyAddressBtn_, &QPushButton::clicked, this, &HardwareWalletWidget::onUsbVerifyAddress);

    usbAddressResultLabel_ = new QLabel("No address fetched yet.");
    usbAddressResultLabel_->setWordWrap(true);
    usbAddressResultLabel_->setStyleSheet(hwPanelStyle());

    addressLayout->addWidget(derivationLabel, 0, 0);
    addressLayout->addWidget(usbDerivationPathEdit_, 0, 1, 1, 2);
    addressLayout->addWidget(usbFetchAddressBtn_, 1, 0, 1, 1);
    addressLayout->addWidget(usbVerifyAddressBtn_, 1, 1, 1, 2);
    addressLayout->addWidget(usbAddressResultLabel_, 2, 0, 1, 3);
    layout->addWidget(addressGroup);

    auto* descriptorGroup = new QGroupBox("🧾 Watch-Only Descriptor Export");
    auto* descriptorLayout = new QGridLayout(descriptorGroup);

    auto* accountPathLabel = new QLabel("Account Path:");
    usbAccountPathEdit_ = new QLineEdit("m/86'/1448'/0'");
    usbAccountPathEdit_->setPlaceholderText("m/86'/1448'/0'");
    connect(usbAccountPathEdit_, &QLineEdit::textChanged, this, [this]() {
        updateUsbDescriptorControls();
    });

    auto* policyLabel = new QLabel("Descriptor Policy:");
    usbDescriptorPolicyCombo_ = new QComboBox;
    usbDescriptorPolicyCombo_->addItem("BIP86 Taproot", "bip86");
    usbDescriptorPolicyCombo_->addItem("BIP84 Native SegWit", "bip84");
    connect(usbDescriptorPolicyCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateUsbDescriptorControls(); });

    usbExportDescriptorBtn_ = new QPushButton("🧾 Export Account Descriptors");
    usbExportDescriptorBtn_->setStyleSheet(hwButtonStyle());
    usbExportDescriptorBtn_->setToolTip("Export receive/change descriptors and the account xpub from the active USB session.");
    connect(usbExportDescriptorBtn_, &QPushButton::clicked, this, &HardwareWalletWidget::onUsbExportAccountDescriptor);

    usbImportDescriptorBtn_ = new QPushButton("📥 Import Into Active Wallet");
    usbImportDescriptorBtn_->setStyleSheet(hwButtonStyle());
    usbImportDescriptorBtn_->setToolTip("Import the exported receive/change descriptors into the currently loaded wallet as watch-only descriptors.");
    connect(usbImportDescriptorBtn_, &QPushButton::clicked, this, &HardwareWalletWidget::onUsbImportAccountDescriptor);

    usbDescriptorResultEdit_ = new QTextEdit;
    usbDescriptorResultEdit_->setReadOnly(true);
    usbDescriptorResultEdit_->setMaximumHeight(170);
    usbDescriptorResultEdit_->setPlaceholderText("Exported receive/change descriptors will appear here.");

    descriptorLayout->addWidget(accountPathLabel, 0, 0);
    descriptorLayout->addWidget(usbAccountPathEdit_, 0, 1, 1, 2);
    descriptorLayout->addWidget(policyLabel, 1, 0);
    descriptorLayout->addWidget(usbDescriptorPolicyCombo_, 1, 1, 1, 2);
    descriptorLayout->addWidget(usbExportDescriptorBtn_, 2, 0, 1, 3);
    descriptorLayout->addWidget(usbImportDescriptorBtn_, 3, 0, 1, 3);
    descriptorLayout->addWidget(usbDescriptorResultEdit_, 4, 0, 1, 3);
    layout->addWidget(descriptorGroup);

    // Status
    usbStatusLabel_ = new QLabel("USB capability unknown until detection runs");
    usbStatusLabel_->setStyleSheet(hwPanelStyle());
    layout->addWidget(usbStatusLabel_);

    // Log
    auto* logGroup = new QGroupBox("📝 Activity Log");
    auto* logLayout = new QVBoxLayout(logGroup);
    usbLogEdit_ = new QTextEdit;
    usbLogEdit_->setReadOnly(true);
    usbLogEdit_->setMaximumHeight(150);
    usbLogEdit_->append("ℹ️ USB detection depends on the daemon being built with ENABLE_HARDWARE_WALLETS=ON.");
    usbLogEdit_->append("ℹ️ Trezor production support is optional and requires a backend build with ENABLE_TREZOR=ON.");
    usbLogEdit_->append("ℹ️ Qt USB sessions follow backend capabilities. Ledger can sign directly; Trezor can now sign constrained BIP86 Taproot PSBTs alongside inspection, fingerprint/address export, and watch-only descriptor export.");
    usbLogEdit_->append("");
    usbLogEdit_->append("Click 'Detect Devices' to scan for connected wallets.");
    logLayout->addWidget(usbLogEdit_);
    layout->addWidget(logGroup);

    updateUsbAddressButtons();
    updateUsbDescriptorControls();
    layout->addStretch();
}

void HardwareWalletWidget::onExportPSBT() {
    QString psbt = psbtTextEdit_->toPlainText().trimmed();
    QString filepath = exportPathEdit_->text().trimmed();

    if (psbt.isEmpty()) {
        showError("Please enter a PSBT to export");
        return;
    }

    if (filepath.isEmpty()) {
        showError("Please specify export path");
        return;
    }

    fileStatusLabel_->setText("Exporting PSBT...");
    fileProgressBar_->setVisible(true);
    fileProgressBar_->setRange(0, 0);

    // Store current operation for result handler
    currentOperation_ = "hwallet.exportpsbttofile";
    currentFilepath_ = filepath;

    // Call RPC with QJsonObject
    QJsonObject params;
    params["psbt"] = psbt;
    params["filepath"] = filepath;

    // Convert to QJsonArray
    QJsonArray arr;
    arr.append(params);

    rpc_->call("hwallet.exportpsbttofile", arr);
}

void HardwareWalletWidget::onImportPSBT() {
    QString filepath = importPathEdit_->text().trimmed();

    if (filepath.isEmpty()) {
        showError("Please specify import path");
        return;
    }

    if (!QFileInfo::exists(filepath)) {
        showError("File does not exist: " + filepath);
        return;
    }

    fileStatusLabel_->setText("Importing PSBT...");
    fileProgressBar_->setVisible(true);
    fileProgressBar_->setRange(0, 0);

    currentOperation_ = "hwallet.importpsbtfromfile";

    QJsonObject params;
    params["filepath"] = filepath;

    QJsonArray arr;
    arr.append(params);

    rpc_->call("hwallet.importpsbtfromfile", arr);
}

void HardwareWalletWidget::onAnalyzePSBT() {
    QString psbt = psbtTextEdit_->toPlainText().trimmed();

    if (psbt.isEmpty()) {
        showError("Please enter a PSBT to analyze");
        return;
    }

    fileStatusLabel_->setText("Analyzing PSBT...");

    currentOperation_ = "hwallet.analyzepsbt";

    QJsonObject params;
    params["psbt"] = psbt;

    QJsonArray arr;
    arr.append(params);

    rpc_->call("hwallet.analyzepsbt", arr);
}

void HardwareWalletWidget::onUsbSignPSBT() {
    const QString psbt = psbtTextEdit_->toPlainText().trimmed();
    if (psbt.isEmpty()) {
        showError("Please enter or import a PSBT to sign.");
        return;
    }

    if (connectedUsbDeviceId_.isEmpty()) {
        showError("Connect a supported USB device in the USB tab first.");
        return;
    }

    if (!connectedUsbCanSign_) {
        showError("The active USB session does not support direct PSBT signing. Use File / SD Card or QR signing for this device.");
        return;
    }

    fileStatusLabel_->setText("Requesting signature from connected USB device...");
    fileProgressBar_->setVisible(true);
    fileProgressBar_->setRange(0, 0);
    currentOperation_ = "hwallet.signpsbt";

    QJsonObject params;
    params["psbt"] = psbt;
    QJsonArray arr;
    arr.append(params);
    rpc_->call("hwallet.signpsbt", arr);
}

void HardwareWalletWidget::onFinalizePSBT() {
    const QString psbt = psbtTextEdit_->toPlainText().trimmed();
    if (psbt.isEmpty()) {
        showError("Please enter or import a PSBT to finalize.");
        return;
    }

    fileStatusLabel_->setText("Finalizing PSBT...");
    fileProgressBar_->setVisible(true);
    fileProgressBar_->setRange(0, 0);
    currentOperation_ = "wallet.finalizepsbt";
    rpc_->call("wallet.finalizepsbt", QJsonArray{psbt, true});
}

void HardwareWalletWidget::onBroadcastTransaction() {
    if (pendingBroadcastHex_.isEmpty()) {
        showError("No finalized transaction is ready to broadcast yet.");
        return;
    }

    fileStatusLabel_->setText("Broadcasting final transaction...");
    fileProgressBar_->setVisible(true);
    fileProgressBar_->setRange(0, 0);
    currentOperation_ = "wallet.sendrawtransaction";
    rpc_->call("wallet.sendrawtransaction", QJsonArray{pendingBroadcastHex_});
}

void HardwareWalletWidget::onDeviceDetect() {
    currentOperation_ = "hwallet.enumeratehwdevices";
    QJsonArray params;  // No parameters required
    rpc_->call("hwallet.enumeratehwdevices", params);

    usbStatusLabel_->setText("🔍 Detecting USB hardware wallets...");
    usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] Scanning for USB devices (Ledger, Trezor, etc.)");

    deviceCombo_->clear();
    deviceCombo_->addItem("Scanning...");
    updateUsbConnectButton();
}

void HardwareWalletWidget::onDeviceConnect() {
    if (!connectedUsbDeviceId_.isEmpty()) {
        currentOperation_ = "hwallet.disconnecthwdevice";
        usbStatusLabel_->setText("🔌 Disconnecting active USB session...");
        usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] Disconnecting active USB session");
        rpc_->call("hwallet.disconnecthwdevice", QJsonArray{});
        return;
    }

    const QString deviceId = deviceCombo_->currentData(Qt::UserRole).toString();
    const bool interactiveSupported = deviceCombo_->currentData(Qt::UserRole + 1).toBool();
    const QString note = deviceCombo_->currentData(Qt::UserRole + 2).toString();
    if (deviceId.isEmpty()) {
        showError("Select a detected device first.");
        return;
    }
    if (!interactiveSupported) {
        showError(note.isEmpty() ? "This detected device is still USB detect-only in the current backend build." : note);
        return;
    }

    currentOperation_ = "hwallet.connecthwdevice";
    QJsonObject params;
    params["device_id"] = deviceId;
    QJsonArray arr;
    arr.append(params);
    usbStatusLabel_->setText("🔌 Opening USB session...");
    usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] Opening USB session for " + deviceCombo_->currentText());
    rpc_->call("hwallet.connecthwdevice", arr);
    updateUsbConnectButton();
}

void HardwareWalletWidget::onUsbFetchAddress() {
    if (connectedUsbDeviceId_.isEmpty()) {
        showError("Connect a supported USB device in the USB tab first.");
        return;
    }
    if (!connectedUsbCanGetAddress_) {
        showError("The active USB session does not support address retrieval.");
        return;
    }

    const QString derivationPath = usbDerivationPathEdit_->text().trimmed();
    if (derivationPath.isEmpty()) {
        showError("Enter a derivation path first.");
        return;
    }
    if (hwPathUsesRetiredCoinType(derivationPath)) {
        showError(hwRetiredCoinTypeMessage());
        return;
    }

    currentOperation_ = "hwallet.gethwaddress";
    usbStatusLabel_->setText("📥 Fetching address from active USB session...");
    usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] Fetching address for " + derivationPath);

    QJsonObject params;
    params["derivation_path"] = derivationPath;
    params["show_display"] = false;
    rpc_->call("hwallet.gethwaddress", QJsonArray{params});
}

void HardwareWalletWidget::onUsbVerifyAddress() {
    if (connectedUsbDeviceId_.isEmpty()) {
        showError("Connect a supported USB device in the USB tab first.");
        return;
    }
    if (!connectedUsbCanVerifyAddress_) {
        showError("The active USB session does not support on-device address verification.");
        return;
    }

    const QString derivationPath = usbDerivationPathEdit_->text().trimmed();
    if (derivationPath.isEmpty()) {
        showError("Enter a derivation path first.");
        return;
    }
    if (hwPathUsesRetiredCoinType(derivationPath)) {
        showError(hwRetiredCoinTypeMessage());
        return;
    }

    currentOperation_ = "hwallet.gethwaddress";
    usbStatusLabel_->setText("👁️ Waiting for device address confirmation...");
    usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] Requesting on-device address verification for " + derivationPath);

    QJsonObject params;
    params["derivation_path"] = derivationPath;
    params["show_display"] = true;
    rpc_->call("hwallet.gethwaddress", QJsonArray{params});
}

void HardwareWalletWidget::onUsbExportAccountDescriptor() {
    if (connectedUsbDeviceId_.isEmpty() || !connectedUsbCanExportAccountDescriptor_) {
        showError("Connect a supported USB device with descriptor export in the USB tab first.");
        return;
    }

    const QString derivationPath = usbAccountPathEdit_->text().trimmed();
    if (derivationPath.isEmpty()) {
        showError("Enter an account derivation path such as m/86'/1448'/0'.");
        return;
    }
    if (hwPathUsesRetiredCoinType(derivationPath)) {
        showError(hwRetiredCoinTypeMessage());
        return;
    }

    currentOperation_ = "hwallet.gethwaccountdescriptor";
    usbStatusLabel_->setText("🧾 Exporting account descriptors...");
    usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] Exporting account descriptors for " + derivationPath);

    QJsonObject params;
    params["derivation_path"] = derivationPath;
    params["policy"] = usbDescriptorPolicyCombo_->currentData().toString();
    rpc_->call("hwallet.gethwaccountdescriptor", QJsonArray{params});
}

void HardwareWalletWidget::onUsbImportAccountDescriptor() {
    if (!hasUsbExportedDescriptors_ || exportedUsbReceiveDescriptor_.isEmpty() || exportedUsbChangeDescriptor_.isEmpty()) {
        showError("Export account descriptors from the connected USB device first.");
        return;
    }

    if (QMessageBox::question(
            this,
            "Import Watch-Only Descriptors",
            "Import the exported receive/change descriptors into the currently loaded wallet as watch-only descriptors?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes) != QMessageBox::Yes) {
        return;
    }

    currentOperation_ = "wallet.importdescriptors";
    usbStatusLabel_->setText("📥 Importing watch-only descriptors into the active wallet...");
    usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] Importing exported watch-only descriptors into the active wallet");

    QJsonArray requests;

    QJsonObject receiveReq;
    receiveReq["desc"] = exportedUsbReceiveDescriptor_;
    receiveReq["active"] = false;
    receiveReq["internal"] = false;
    receiveReq["timestamp"] = "now";
    receiveReq["label"] = QString("Hardware wallet account %1 receive").arg(exportedUsbDescriptorAccount_);
    QJsonArray receiveRange;
    receiveRange.append(0);
    receiveRange.append(250);
    receiveReq["range"] = receiveRange;
    requests.append(receiveReq);

    QJsonObject changeReq;
    changeReq["desc"] = exportedUsbChangeDescriptor_;
    changeReq["active"] = false;
    changeReq["internal"] = true;
    changeReq["timestamp"] = "now";
    changeReq["label"] = QString("Hardware wallet account %1 change").arg(exportedUsbDescriptorAccount_);
    QJsonArray changeRange;
    changeRange.append(0);
    changeRange.append(250);
    changeReq["range"] = changeRange;
    requests.append(changeReq);

    QJsonObject params;
    params["requests"] = requests;
    rpc_->call("wallet.importdescriptors", QJsonArray{params});
}

void HardwareWalletWidget::onRpcResult(const QString& method, const QJsonValue& result) {
    // Only handle our methods
    if (method != currentOperation_) {
        return;
    }

    const QString handledOperation = currentOperation_;
    fileProgressBar_->setVisible(false);

    if (method == "hwallet.exportpsbttofile") {
        handleExportResult(result.toObject());
    } else if (method == "hwallet.importpsbtfromfile") {
        handleImportResult(result.toObject());
    } else if (method == "hwallet.analyzepsbt") {
        handleAnalyzeResult(result.toObject());
    } else if (method == "hwallet.signpsbt") {
        handleUsbSignResult(result.toObject());
    } else if (method == "wallet.finalizepsbt") {
        handleFinalizeResult(result.toObject());
    } else if (method == "wallet.sendrawtransaction") {
        handleBroadcastResult(result.toObject());
    } else if (method == "hwallet.enumeratehwdevices") {
        handleDeviceEnumerationResult(result.toObject());
    } else if (method == "hwallet.connecthwdevice") {
        handleDeviceConnectResult(result.toObject());
    } else if (method == "hwallet.disconnecthwdevice") {
        handleDeviceDisconnectResult(result.toObject());
    } else if (method == "hwallet.gethwaddress") {
        handleUsbAddressResult(result.toObject());
    } else if (method == "hwallet.gethwaccountdescriptor") {
        handleUsbDescriptorResult(result.toObject());
    } else if (method == "wallet.importdescriptors") {
        handleUsbImportDescriptorResult(result);
    }

    if (currentOperation_ == handledOperation) {
        currentOperation_.clear();
    }
}

void HardwareWalletWidget::onRpcError(const QString& method, int code, const QString& message) {
    if (method != currentOperation_) {
        return;
    }

    fileProgressBar_->setVisible(false);
    if (method == "hwallet.analyzepsbt" && pendingQrRenderAfterAnalyze_) {
        pendingQrRenderAfterAnalyze_ = false;
        if (qrStatusLabel_) {
            qrStatusLabel_->setText("❌ PSBT analysis failed; QR was not displayed.");
            qrStatusLabel_->setStyleSheet(hwPanelStyle());
        }
    }
    showError(QString("RPC Error (%1): %2").arg(code).arg(message));
    if (method == "hwallet.enumeratehwdevices" ||
        method == "hwallet.connecthwdevice" ||
        method == "hwallet.disconnecthwdevice" ||
        method == "hwallet.gethwaddress" ||
        method == "hwallet.gethwaccountdescriptor" ||
        method == "hwallet.signpsbt" ||
        method == "wallet.importdescriptors") {
        usbStatusLabel_->setText("❌ USB operation failed");
        usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] Error: " + message);
        if (method == "hwallet.signpsbt") {
            fileStatusLabel_->setText("❌ USB signing failed");
        }
        updateUsbConnectButton();
    } else {
        fileStatusLabel_->setText("❌ Operation failed");
        updateFileActionButtons();
    }
    currentOperation_.clear();
}

void HardwareWalletWidget::handleExportResult(const QJsonObject& result) {
    if (result.contains("result")) {
        auto res = result["result"].toObject();
        QString msg = QString("✅ PSBT exported successfully!\n\nFile: %1\nSize: %2 bytes")
            .arg(currentFilepath_)
            .arg(res["size_bytes"].toInteger());

        if (res.contains("instructions")) {
            msg += "\n\n" + res["instructions"].toString();
        }

        showSuccess(msg);
        fileStatusLabel_->setText("✅ Export complete");
    } else if (result.contains("error")) {
        auto err = result["error"].toObject();
        showError(err["message"].toString());
        fileStatusLabel_->setText("❌ Export failed");
    }
}

void HardwareWalletWidget::handleImportResult(const QJsonObject& result) {
    if (result.contains("result")) {
        applySignedPsbtResult(result["result"].toObject(), "Imported signed PSBT");
    } else if (result.contains("error")) {
        auto err = result["error"].toObject();
        showError(err["message"].toString());
        fileStatusLabel_->setText("❌ Import failed");
        updateFileActionButtons();
    }
}

void HardwareWalletWidget::handleAnalyzeResult(const QJsonObject& result) {
    if (result.contains("result")) {
        auto res = result["result"].toObject();
        const QString analysis = formatPsbtAnalysis(res);

        if (pendingQrRenderAfterAnalyze_) {
            pendingQrRenderAfterAnalyze_ = false;
            if (qrSummaryLabel_) {
                qrSummaryLabel_->setText(analysis);
                qrSummaryLabel_->setStyleSheet(hwPanelStyle());
            }
            renderPsbtQrImage(qrPsbtEdit_ ? qrPsbtEdit_->toPlainText().trimmed() : QString());
            fileStatusLabel_->setText("✅ Analysis complete; QR ready");
            return;
        }

        showSuccess(analysis);
        fileStatusLabel_->setText("✅ Analysis complete");
    } else if (result.contains("error")) {
        pendingQrRenderAfterAnalyze_ = false;
        auto err = result["error"].toObject();
        showError(err["message"].toString());
        fileStatusLabel_->setText("❌ Analysis failed");
        if (qrStatusLabel_) {
            qrStatusLabel_->setText("❌ PSBT analysis failed; QR was not displayed.");
            qrStatusLabel_->setStyleSheet(hwPanelStyle());
        }
    }
}

QString HardwareWalletWidget::formatPsbtAnalysis(const QJsonObject& res) const {
    QString analysis;
    analysis += "PSBT Analysis\n";
    analysis += QString("Inputs: %1\n").arg(res["num_inputs"].toInt());
    analysis += QString("Outputs: %1\n").arg(res["num_outputs"].toInt());

    const QString nextRole = res["next_role"].toString();
    if (!nextRole.isEmpty()) {
        analysis += QString("Next step: %1\n").arg(nextRole);
    }

    if (res.contains("estimated_fee") && !res["estimated_fee"].isNull()) {
        analysis += QString("Estimated fee: %1 una\n").arg(QString::number(res["estimated_fee"].toVariant().toULongLong()));
    }
    if (res.contains("estimated_feerate") && !res["estimated_feerate"].isNull()) {
        analysis += QString("Estimated fee rate: %1 una/vB\n").arg(res["estimated_feerate"].toDouble(), 0, 'f', 2);
    }
    if (res.contains("estimated_vsize") && !res["estimated_vsize"].isNull()) {
        analysis += QString("Estimated size: %1 vB\n").arg(QString::number(res["estimated_vsize"].toVariant().toULongLong()));
    }

    if (res.contains("inputs") && res["inputs"].isArray()) {
        auto inputs = res["inputs"].toArray();
        analysis += "\nInput Status:\n";
        for (int i = 0; i < inputs.size(); i++) {
            auto input = inputs[i].toObject();
            analysis += QString("  Input %1: ").arg(i);
            if (input["is_final"].toBool()) {
                analysis += "Finalized\n";
            } else if (input["has_sigs"].toBool()) {
                analysis += "Partially signed\n";
            } else {
                analysis += "Unsigned\n";
            }
        }
    }

    analysis += "\nVerify destination, amount, and fee on the hardware wallet before signing.";
    return analysis;
}

void HardwareWalletWidget::handleUsbSignResult(const QJsonObject& result) {
    if (result.contains("result")) {
        applySignedPsbtResult(result["result"].toObject(), "USB signing complete");
    } else if (result.contains("error")) {
        auto err = result["error"].toObject();
        showError(err["message"].toString());
        fileStatusLabel_->setText("❌ USB signing failed");
        updateFileActionButtons();
    }
}

void HardwareWalletWidget::handleUsbAddressResult(const QJsonObject& result) {
    if (!result.contains("result")) {
        return;
    }

    const auto res = result["result"].toObject();
    const QString derivationPath = res["derivation_path"].toString();
    const QString address = res["address"].toString();
    const QString publicKey = res["public_key"].toString();
    const bool verifiedOnDevice = res["verified_on_device"].toBool();

    if (address.isEmpty()) {
        usbStatusLabel_->setText("⚠️ No address returned");
        usbAddressResultLabel_->setText("No address returned.");
        return;
    }

    usbStatusLabel_->setText(verifiedOnDevice
        ? "✅ Address confirmed on device"
        : "✅ Address fetched from device session");
    usbAddressResultLabel_->setText(
        QString("Path: %1\nAddress: %2%3")
            .arg(derivationPath,
                 address,
                 publicKey.isEmpty() ? QString() : QString("\nPublic key: %1").arg(publicKey)));
    usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] "
        + (verifiedOnDevice ? "Verified address on device: " : "Fetched address: ")
        + address + " (" + derivationPath + ")");
}

void HardwareWalletWidget::handleUsbDescriptorResult(const QJsonObject& result) {
    if (!result.contains("result")) {
        return;
    }

    const auto res = result["result"].toObject();
    const QString derivationPath = res["derivation_path"].toString();
    const QString policy = res["policy"].toString();
    const QString fingerprint = res["master_fingerprint"].toString();
    const QString accountXpub = res["account_xpub"].toString();
    const QString receiveDescriptor = res["receive_descriptor"].toString();
    const QString receiveDescriptorWithChecksum = res["receive_descriptor_with_checksum"].toString(receiveDescriptor);
    const QString changeDescriptor = res["change_descriptor"].toString();
    const QString changeDescriptorWithChecksum = res["change_descriptor_with_checksum"].toString(changeDescriptor);

    exportedUsbReceiveDescriptor_ = receiveDescriptorWithChecksum;
    exportedUsbChangeDescriptor_ = changeDescriptorWithChecksum;
    exportedUsbDescriptorPolicy_ = policy;
    exportedUsbDescriptorAccount_ = res["account"].toInt();
    hasUsbExportedDescriptors_ = !exportedUsbReceiveDescriptor_.isEmpty() && !exportedUsbChangeDescriptor_.isEmpty();

    usbDescriptorResultEdit_->setPlainText(
        QString("Policy: %1\n"
                "Account path: %2\n"
                "Master fingerprint: %3\n"
                "Account xpub: %4\n\n"
                "Receive descriptor:\n%5\n\n"
                "Change descriptor:\n%6")
            .arg(policy, derivationPath, fingerprint, accountXpub, receiveDescriptorWithChecksum, changeDescriptorWithChecksum));

    usbStatusLabel_->setText("✅ Account descriptors exported");
    usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] Exported "
                        + policy + " descriptors for " + derivationPath);
    updateUsbDescriptorControls();
}

void HardwareWalletWidget::handleUsbImportDescriptorResult(const QJsonValue& result) {
    if (!result.isArray()) {
        usbStatusLabel_->setText("⚠️ Descriptor import returned an unexpected response");
        return;
    }

    const QJsonArray results = result.toArray();
    if (results.size() < 2) {
        usbStatusLabel_->setText("⚠️ Descriptor import returned incomplete results");
        return;
    }

    int successCount = 0;
    int derivedCount = 0;
    QStringList warnings;
    QStringList errors;

    for (const auto& entryValue : results) {
        const auto entry = entryValue.toObject();
        if (entry["success"].toBool()) {
            ++successCount;
            derivedCount += entry["addresses_derived"].toInt();
            if (!entry["warning"].toString().isEmpty()) {
                warnings << entry["warning"].toString();
            }
        } else {
            const auto err = entry["error"].toObject();
            errors << (err["message"].toString().isEmpty() ? QStringLiteral("Descriptor import failed") : err["message"].toString());
        }
    }

    if (!errors.isEmpty()) {
        usbStatusLabel_->setText("❌ Watch-only descriptor import failed");
        showError(errors.join("\n"));
        return;
    }

    usbStatusLabel_->setText("✅ Watch-only descriptors imported into the active wallet");
    usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] Imported "
                        + QString::number(successCount) + " descriptor(s) into the active wallet");

    QString message = QString("✅ Imported %1 descriptor(s) into the active wallet.\n\nDerived %2 addresses for watch-only tracking.")
        .arg(successCount)
        .arg(derivedCount);
    if (!warnings.isEmpty()) {
        message += "\n\nWarnings:\n" + warnings.join("\n");
    }
    showSuccess(message);
}

void HardwareWalletWidget::handleFinalizeResult(const QJsonObject& result) {
    if (result.contains("error")) {
        showError(result["error"].toString());
        fileStatusLabel_->setText("❌ Finalize failed");
        pendingBroadcastHex_.clear();
        updateFileActionButtons();
        return;
    }

    const QString finalizedPsbt = result["psbt"].toString();
    if (!finalizedPsbt.isEmpty()) {
        suppressLinkedSendReset_ = true;
        psbtTextEdit_->setPlainText(finalizedPsbt);
        suppressLinkedSendReset_ = false;
    }

    const bool complete = result["complete"].toBool();
    pendingBroadcastHex_.clear();
    QString message;
    if (complete && result.contains("hex")) {
        pendingBroadcastHex_ = result["hex"].toString();
        message = "✅ PSBT finalized successfully.\n\nThe transaction is ready to broadcast.";
        fileStatusLabel_->setText("✅ Finalized. Ready to broadcast");
    } else {
        message = "⚠️ PSBT is still incomplete after finalization.\n\nContinue signing or combine additional PSBTs.";
        fileStatusLabel_->setText("⚠️ Finalization incomplete");
    }

    showSuccess(message);
    updateFileActionButtons();
}

void HardwareWalletWidget::handleBroadcastResult(const QJsonObject& result) {
    if (result.contains("error")) {
        const auto err = result["error"].toObject();
        showError(err["message"].toString());
        fileStatusLabel_->setText("❌ Broadcast failed");
        updateFileActionButtons();
        return;
    }

    const QString txid = result["result"].toString();
    pendingBroadcastHex_.clear();
    fileStatusLabel_->setText(txid.isEmpty() ? "✅ Broadcast complete" : "✅ Broadcast complete: " + txid);
    showSuccess(txid.isEmpty()
        ? "✅ Transaction broadcast successfully."
        : QString("✅ Transaction broadcast successfully.\n\nTXID: %1").arg(txid));
    updateFileActionButtons();

    if (!txid.isEmpty()) {
        const bool linkedSendFlow = linkedSendContext_;
        linkedSendContext_ = false;
        Q_EMIT transactionBroadcasted(txid, linkedSendFlow);
    }
}

void HardwareWalletWidget::handleDeviceEnumerationResult(const QJsonObject& result) {
    if (result.contains("result")) {
        auto res = result["result"].toObject();
        int count = res["count"].toInt();
        auto devices = res["devices"].toArray();

        deviceCombo_->clear();

        if (count == 0) {
            deviceCombo_->addItem("No devices detected");
            usbStatusLabel_->setText("⚠️ No USB hardware wallets found");
            usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] No devices detected");
            usbLogEdit_->append("Make sure your hardware wallet is connected and unlocked");
        } else {
            usbStatusLabel_->setText(QString("✅ Found %1 device%2").arg(count).arg(count > 1 ? "s" : ""));
            usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + QString("] Found %1 device(s):").arg(count));
            if (!res["notes"].toString().isEmpty()) {
                usbLogEdit_->append(res["notes"].toString());
            }

            for (int i = 0; i < devices.size(); i++) {
                auto device = devices[i].toObject();
                QString manufacturer = device["manufacturer"].toString();
                QString model = device["model"].toString();
                QString deviceId = device["device_id"].toString();
                QString firmwareVersion = device["firmware_version"].toString();
                const bool interactiveSupported = device["interactive_usb_supported"].toBool();
                const auto supportedOps = device["supported_operations"].toObject();
                const bool signSupported = supportedOps["sign_psbt"].toBool();
                const bool verifyAddressSupported = supportedOps["verify_address"].toBool();
                const bool descriptorSupported = supportedOps["export_account_descriptor"].toBool();
                QString note = supportedOps["notes"].toString();

                // Add to combo box
                QString displayName = QString("%1 %2").arg(manufacturer, model);
                if (!firmwareVersion.isEmpty()) {
                    displayName += QString(" (v%1)").arg(firmwareVersion);
                }
                if (!interactiveSupported) {
                    displayName += " [detect-only]";
                } else if (descriptorSupported && !signSupported) {
                    displayName += verifyAddressSupported ? " [address+descriptor]" : " [descriptor-export]";
                } else if (verifyAddressSupported && !signSupported) {
                    displayName += " [address-verify]";
                } else if (!signSupported) {
                    displayName += " [inspect-only]";
                }
                deviceCombo_->addItem(displayName, deviceId);
                const int itemIndex = deviceCombo_->count() - 1;
                deviceCombo_->setItemData(itemIndex, deviceId, Qt::UserRole);
                deviceCombo_->setItemData(itemIndex, interactiveSupported, Qt::UserRole + 1);
                deviceCombo_->setItemData(itemIndex, note, Qt::UserRole + 2);

                // Log device details
                usbLogEdit_->append(QString("  - %1").arg(displayName));
                if (!deviceId.isEmpty()) {
                    usbLogEdit_->append(QString("    Device ID: %1").arg(deviceId));
                }
            }
        }

        updateUsbConnectButton();
    } else if (result.contains("error")) {
        auto err = result["error"].toObject();
        QString errorMsg = err["message"].toString();
        showError(errorMsg);
        usbStatusLabel_->setText("❌ Device detection failed");
        usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] Error: " + errorMsg);

        deviceCombo_->clear();
        deviceCombo_->addItem("Detection failed");
        updateUsbConnectButton();
    }
}

void HardwareWalletWidget::handleDeviceConnectResult(const QJsonObject& result) {
    if (!result.contains("result")) {
        return;
    }

    const auto res = result["result"].toObject();
    const auto device = res["device"].toObject();
    connectedUsbDeviceId_ = device["device_id"].toString();
    const auto supportedOps = device["supported_operations"].toObject();
    connectedUsbCanSign_ = supportedOps["sign_psbt"].toBool();
    connectedUsbCanGetAddress_ = supportedOps["get_address"].toBool();
    connectedUsbCanVerifyAddress_ = supportedOps["verify_address"].toBool();
    connectedUsbCanExportAccountDescriptor_ = supportedOps["export_account_descriptor"].toBool();

    const QString displayName = QString("%1 %2")
        .arg(device["manufacturer"].toString(), device["model"].toString());
    usbStatusLabel_->setText("✅ Connected to " + displayName);
    usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] Connected to " + displayName);
    if (!device["firmware_version"].toString().isEmpty()) {
        usbLogEdit_->append("    Firmware: " + device["firmware_version"].toString());
    }
    if (!res["master_fingerprint"].toString().isEmpty()) {
        usbLogEdit_->append("    Master fingerprint: " + res["master_fingerprint"].toString());
    }

    deviceInfoLabel_->setText(
        QString("Connected device:\n"
                "• %1\n"
                "• Type: %2\n"
                "• Transport: %3\n"
                "• Master fingerprint: %4\n"
                "• Address verification: %5\n"
                "• Descriptor export: %6\n"
                "• Notes: %7")
            .arg(displayName)
            .arg(device["type"].toString())
            .arg(device["transport"].toString())
            .arg(res["master_fingerprint"].toString().isEmpty() ? QStringLiteral("unavailable") : res["master_fingerprint"].toString())
            .arg(connectedUsbCanVerifyAddress_ ? QStringLiteral("available") : QStringLiteral("unavailable"))
            .arg(connectedUsbCanExportAccountDescriptor_ ? QStringLiteral("available") : QStringLiteral("unavailable"))
            .arg(res["notes"].toString()));

    updateUsbConnectButton();
    updateUsbAddressButtons();
    updateUsbDescriptorControls();
    updateFileActionButtons();
}

void HardwareWalletWidget::handleDeviceDisconnectResult(const QJsonObject& result) {
    Q_UNUSED(result);
    connectedUsbDeviceId_.clear();
    connectedUsbCanSign_ = false;
    connectedUsbCanGetAddress_ = false;
    connectedUsbCanVerifyAddress_ = false;
    connectedUsbCanExportAccountDescriptor_ = false;
    hasUsbExportedDescriptors_ = false;
    exportedUsbReceiveDescriptor_.clear();
    exportedUsbChangeDescriptor_.clear();
    exportedUsbDescriptorPolicy_.clear();
    exportedUsbDescriptorAccount_ = 0;
    usbStatusLabel_->setText("ℹ️ USB session closed");
    usbLogEdit_->append("[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "] USB session closed");
    deviceInfoLabel_->setText(defaultUsbDeviceInfoText_);
    usbAddressResultLabel_->setText("No address fetched yet.");
    usbDescriptorResultEdit_->clear();
    updateUsbConnectButton();
    updateUsbAddressButtons();
    updateUsbDescriptorControls();
    updateFileActionButtons();
}

void HardwareWalletWidget::updateUsbConnectButton() {
    if (!connectedUsbDeviceId_.isEmpty()) {
        connectBtn_->setEnabled(true);
        connectBtn_->setText("🔌 Disconnect");
        connectBtn_->setToolTip("Close the active USB hardware-wallet session.");
        return;
    }

    const bool interactiveSupported = deviceCombo_->currentData(Qt::UserRole + 1).toBool();
    const QString note = deviceCombo_->currentData(Qt::UserRole + 2).toString();
    connectBtn_->setText("🔌 Connect");
    connectBtn_->setEnabled(interactiveSupported);
    connectBtn_->setToolTip(
        interactiveSupported
            ? (note.isEmpty() ? "Open a USB session for device inspection and any supported signing operations." : note)
            : (note.isEmpty() ? "Select a detected device with interactive USB support to open a session." : note));
}

void HardwareWalletWidget::updateUsbAddressButtons() {
    const bool hasPath = usbDerivationPathEdit_ && !usbDerivationPathEdit_->text().trimmed().isEmpty();
    if (usbFetchAddressBtn_) {
        usbFetchAddressBtn_->setEnabled(!connectedUsbDeviceId_.isEmpty() && connectedUsbCanGetAddress_ && hasPath);
    }
    if (usbVerifyAddressBtn_) {
        usbVerifyAddressBtn_->setEnabled(!connectedUsbDeviceId_.isEmpty() && connectedUsbCanVerifyAddress_ && hasPath);
    }
}

void HardwareWalletWidget::updateUsbDescriptorControls() {
    const bool hasAccountPath = usbAccountPathEdit_ && !usbAccountPathEdit_->text().trimmed().isEmpty();
    if (usbExportDescriptorBtn_) {
        usbExportDescriptorBtn_->setEnabled(!connectedUsbDeviceId_.isEmpty() && connectedUsbCanExportAccountDescriptor_ && hasAccountPath);
    }
    if (usbImportDescriptorBtn_) {
        usbImportDescriptorBtn_->setEnabled(hasUsbExportedDescriptors_ && !exportedUsbReceiveDescriptor_.isEmpty() && !exportedUsbChangeDescriptor_.isEmpty());
    }
}

void HardwareWalletWidget::updateFileActionButtons() {
    const bool hasPsbt = !psbtTextEdit_->toPlainText().trimmed().isEmpty();
    usbSignBtn_->setEnabled(hasPsbt && !connectedUsbDeviceId_.isEmpty() && connectedUsbCanSign_);
    finalizeBtn_->setEnabled(hasPsbt);
    broadcastBtn_->setEnabled(!pendingBroadcastHex_.isEmpty());
}

void HardwareWalletWidget::applySignedPsbtResult(const QJsonObject& result, const QString& actionLabel) {
    const QString psbt = result["psbt"].toString();
    if (!psbt.isEmpty()) {
        suppressLinkedSendReset_ = true;
        psbtTextEdit_->setPlainText(psbt);
        suppressLinkedSendReset_ = false;
    }

    pendingBroadcastHex_.clear();
    const bool readyToBroadcast = result["ready_to_broadcast"].toBool() && !result["hex"].toString().isEmpty();
    QString message = "✅ " + actionLabel + ".";

    if (readyToBroadcast) {
        pendingBroadcastHex_ = result["hex"].toString();
        fileStatusLabel_->setText("✅ Signed Dinero PSBT ready to broadcast");
        if (!result["txid"].toString().isEmpty()) {
            message += "\n\nTXID: " + result["txid"].toString();
        }
        message += "\n\nThe transaction is ready to broadcast.";
    } else if (result["complete"].toBool()) {
        fileStatusLabel_->setText("✅ Signed Dinero PSBT imported");
        message += "\n\nThe Dinero PSBT is fully signed, but a final transaction hex is not available yet. Use Finalize Dinero PSBT.";
    } else {
        fileStatusLabel_->setText("⚠️ Dinero PSBT still needs more signatures");
        message += "\n\nThe Dinero PSBT is not fully signed yet.";
    }

    if (!result["master_fingerprint"].toString().isEmpty()) {
        message += "\n\nSigner fingerprint: " + result["master_fingerprint"].toString();
    }
    if (!result["instructions"].toString().isEmpty()) {
        message += "\n\n" + result["instructions"].toString();
    }

    showSuccess(message);
    updateFileActionButtons();
}

void HardwareWalletWidget::showError(const QString& message) {
    QMessageBox::critical(this, "Hardware Wallet Error", message, QMessageBox::Ok);
}

void HardwareWalletWidget::showSuccess(const QString& message) {
    QMessageBox::information(this, "Hardware Wallet", message, QMessageBox::Ok);
}
