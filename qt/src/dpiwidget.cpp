#include "dpiwidget.h"
#include "rpcclient.h"
#include "QrUtil.h"
#include "scrollsupport.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QClipboard>
#include <QApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QScrollArea>
#include <QCoreApplication>

DpiWidget::DpiWidget(RpcClient* rpc, QWidget* parent)
    : QWidget(parent)
    , rpc_(rpc)
{
    setupUI();

    countdownTimer_ = new QTimer(this);
    countdownTimer_->setInterval(1000);
    connect(countdownTimer_, &QTimer::timeout, this, &DpiWidget::onCountdownTick);

    tierPollTimer_ = new QTimer(this);
    tierPollTimer_->setInterval(3000);
    connect(tierPollTimer_, &QTimer::timeout, this, &DpiWidget::onTierPollTick);

    connect(rpc_, &RpcClient::rpcResult, this, &DpiWidget::onRpcResult);
    connect(rpc_, &RpcClient::rpcError, this, &DpiWidget::onRpcError);
}

void DpiWidget::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    tabs_ = new QTabWidget;
    setupCollectTab();
    setupPayTab();
    mainLayout->addWidget(tabs_);
}

void DpiWidget::clearWalletState() {
    if (countdownTimer_) countdownTimer_->stop();
    if (tierPollTimer_)  tierPollTimer_->stop();

    collectInvoiceBase64_.clear();
    payInvoiceBase64_.clear();
    invoiceExpiryTimestamp_ = 0;

    trackedPayTxid_.clear();
    trackedCollectTxid_.clear();
    payTier_ = 0;
    payConfirmations_ = 0;
    collectTier_ = 0;
    collectConfirmations_ = 0;

    if (collectAmountEdit_) collectAmountEdit_->clear();
    if (collectMemoEdit_)   collectMemoEdit_->clear();
    if (invoiceQrLabel_)    invoiceQrLabel_->clear();
    if (invoiceIdLabel_)    invoiceIdLabel_->clear();
    if (invoiceDestLabel_)  invoiceDestLabel_->clear();
    if (invoiceAmountLabel_) invoiceAmountLabel_->clear();
    if (invoiceExpiryLabel_) invoiceExpiryLabel_->clear();
    if (invoiceTextEdit_)   invoiceTextEdit_->clear();
    if (packageInputEdit_)  packageInputEdit_->clear();
    if (verifyResultLabel_) verifyResultLabel_->clear();
    if (verifyDetailsEdit_) verifyDetailsEdit_->clear();

    if (payInvoiceInputEdit_) payInvoiceInputEdit_->clear();
    if (decodedAmountLabel_)  decodedAmountLabel_->clear();
    if (decodedDestLabel_)    decodedDestLabel_->clear();
    if (decodedMemoLabel_)    decodedMemoLabel_->clear();
    if (decodedExpiryLabel_)  decodedExpiryLabel_->clear();
    if (payStatusLabel_)      payStatusLabel_->clear();
    if (packageOutputEdit_)   packageOutputEdit_->clear();

    if (payTierBadge_)     payTierBadge_->clear();
    if (collectTierBadge_) collectTierBadge_->clear();
}

// ============================================================================
// Collect tab — merchant creates invoices, verifies payments
// ============================================================================

void DpiWidget::setupCollectTab() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFocusPolicy(Qt::NoFocus);
    auto* collectWidget = new QWidget;
    auto* layout = new QVBoxLayout(collectWidget);

    // --- Create Invoice ---
    auto* createGroup = new QGroupBox("Create DPI Invoice");
    auto* createLayout = new QGridLayout(createGroup);

    createLayout->addWidget(new QLabel("Amount (DIN):"), 0, 0);
    collectAmountEdit_ = new QLineEdit;
    collectAmountEdit_->setPlaceholderText("e.g. 50.0");
    createLayout->addWidget(collectAmountEdit_, 0, 1);

    createLayout->addWidget(new QLabel("Memo:"), 1, 0);
    collectMemoEdit_ = new QLineEdit;
    collectMemoEdit_->setPlaceholderText("e.g. Order #12345");
    createLayout->addWidget(collectMemoEdit_, 1, 1);

    createLayout->addWidget(new QLabel("Expiry:"), 2, 0);
    collectExpiryCombo_ = new QComboBox;
    collectExpiryCombo_->addItem("5 minutes", 300);
    collectExpiryCombo_->addItem("15 minutes", 900);
    collectExpiryCombo_->addItem("1 hour", 3600);
    collectExpiryCombo_->addItem("24 hours", 86400);
    collectExpiryCombo_->setCurrentIndex(1);
    createLayout->addWidget(collectExpiryCombo_, 2, 1);

    createInvoiceBtn_ = new QPushButton("Create Collect Invoice");
    createInvoiceBtn_->setStyleSheet(
        "background-color: #4CAF50; color: white; padding: 8px; font-weight: bold;");
    connect(createInvoiceBtn_, &QPushButton::clicked, this, &DpiWidget::onCreateInvoice);
    createLayout->addWidget(createInvoiceBtn_, 3, 0, 1, 2);

    layout->addWidget(createGroup);

    // --- Invoice Details (hidden until invoice created) ---
    detailsGroup_ = new QGroupBox("Collect Invoice");
    auto* detailsLayout = new QVBoxLayout(detailsGroup_);

    // QR + info side by side
    auto* qrRow = new QHBoxLayout;

    invoiceQrLabel_ = new QLabel;
    invoiceQrLabel_->setAlignment(Qt::AlignCenter);
    invoiceQrLabel_->setFixedSize(256, 256);
    qrRow->addWidget(invoiceQrLabel_);

    auto* infoGrid = new QGridLayout;
    infoGrid->addWidget(new QLabel("Invoice ID:"), 0, 0);
    invoiceIdLabel_ = new QLabel("—");
    invoiceIdLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    invoiceIdLabel_->setWordWrap(true);
    infoGrid->addWidget(invoiceIdLabel_, 0, 1);

    infoGrid->addWidget(new QLabel("Destination:"), 1, 0);
    invoiceDestLabel_ = new QLabel("—");
    invoiceDestLabel_->setWordWrap(true);
    invoiceDestLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoGrid->addWidget(invoiceDestLabel_, 1, 1);

    infoGrid->addWidget(new QLabel("Amount:"), 2, 0);
    invoiceAmountLabel_ = new QLabel("—");
    infoGrid->addWidget(invoiceAmountLabel_, 2, 1);

    infoGrid->addWidget(new QLabel("Expires:"), 3, 0);
    invoiceExpiryLabel_ = new QLabel("—");
    infoGrid->addWidget(invoiceExpiryLabel_, 3, 1);

    auto* infoWidget = new QWidget;
    infoWidget->setLayout(infoGrid);
    qrRow->addWidget(infoWidget, 1);
    detailsLayout->addLayout(qrRow);

    invoiceTextEdit_ = new QTextEdit;
    invoiceTextEdit_->setReadOnly(true);
    invoiceTextEdit_->setMaximumHeight(50);
    invoiceTextEdit_->setPlaceholderText("Invoice data will appear here...");
    detailsLayout->addWidget(invoiceTextEdit_);

    copyInvoiceBtn_ = new QPushButton("Copy Invoice");
    copyInvoiceBtn_->setStyleSheet(
        "background-color: #2196F3; color: white; padding: 6px;");
    copyInvoiceBtn_->setEnabled(false);
    connect(copyInvoiceBtn_, &QPushButton::clicked, this, &DpiWidget::onCopyInvoice);
    detailsLayout->addWidget(copyInvoiceBtn_);

    detailsGroup_->setVisible(false);  // hidden until invoice created
    layout->addWidget(detailsGroup_);

    // --- Verify Payment ---
    auto* verifyGroup = new QGroupBox("Verify Payment Package");
    auto* verifyLayout = new QVBoxLayout(verifyGroup);

    verifyLayout->addWidget(new QLabel("Paste payment package from sender:"));
    packageInputEdit_ = new QTextEdit;
    packageInputEdit_->setMaximumHeight(60);
    packageInputEdit_->setPlaceholderText("Paste payment package here...");
    verifyLayout->addWidget(packageInputEdit_);

    verifyPackageBtn_ = new QPushButton("Verify Package");
    verifyPackageBtn_->setStyleSheet(
        "background-color: #FF9800; color: white; padding: 8px; font-weight: bold;");
    verifyPackageBtn_->setEnabled(false);
    connect(verifyPackageBtn_, &QPushButton::clicked, this, &DpiWidget::onVerifyPackage);
    verifyLayout->addWidget(verifyPackageBtn_);

    verifyResultLabel_ = new QLabel;
    verifyResultLabel_->setWordWrap(true);
    verifyResultLabel_->setTextFormat(Qt::RichText);
    verifyLayout->addWidget(verifyResultLabel_);

    verifyDetailsEdit_ = new QTextEdit;
    verifyDetailsEdit_->setReadOnly(true);
    verifyDetailsEdit_->setMaximumHeight(120);
    verifyLayout->addWidget(verifyDetailsEdit_);

    collectTierBadge_ = new QLabel;
    collectTierBadge_->setTextFormat(Qt::RichText);
    collectTierBadge_->setWordWrap(true);
    collectTierBadge_->setVisible(false);
    verifyLayout->addWidget(collectTierBadge_);

    layout->addWidget(verifyGroup);
    layout->addStretch();

    scroll->setWidget(collectWidget);
    ScrollSupport::enableForScrollArea(scroll, collectWidget);
    tabs_->addTab(scroll, "Collect");
}

// ============================================================================
// Pay tab — sender decodes invoice and pays
// ============================================================================

void DpiWidget::setupPayTab() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFocusPolicy(Qt::NoFocus);
    auto* payWidget = new QWidget;
    auto* layout = new QVBoxLayout(payWidget);

    // --- Invoice Input ---
    auto* inputGroup = new QGroupBox("Pay DPI Invoice");
    auto* inputLayout = new QVBoxLayout(inputGroup);

    inputLayout->addWidget(new QLabel("Paste or scan a DPI invoice from DineroDPI or Dinero-Qt:"));
    payInvoiceInputEdit_ = new QTextEdit;
    payInvoiceInputEdit_->setMaximumHeight(60);
    payInvoiceInputEdit_->setPlaceholderText("Paste invoice here...");
    inputLayout->addWidget(payInvoiceInputEdit_);

    decodeInvoiceBtn_ = new QPushButton("Review Invoice");
    decodeInvoiceBtn_->setStyleSheet(
        "background-color: #2196F3; color: white; padding: 8px;");
    connect(decodeInvoiceBtn_, &QPushButton::clicked, this, &DpiWidget::onDecodeInvoice);
    inputLayout->addWidget(decodeInvoiceBtn_);

    layout->addWidget(inputGroup);

    // --- Decoded Details ---
    auto* decodedGroup = new QGroupBox("Invoice Details");
    auto* decodedGrid = new QGridLayout(decodedGroup);

    decodedGrid->addWidget(new QLabel("Amount:"), 0, 0);
    decodedAmountLabel_ = new QLabel("—");
    decodedGrid->addWidget(decodedAmountLabel_, 0, 1);

    decodedGrid->addWidget(new QLabel("Destination:"), 1, 0);
    decodedDestLabel_ = new QLabel("—");
    decodedDestLabel_->setWordWrap(true);
    decodedDestLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    decodedGrid->addWidget(decodedDestLabel_, 1, 1);

    decodedGrid->addWidget(new QLabel("Memo:"), 2, 0);
    decodedMemoLabel_ = new QLabel("—");
    decodedGrid->addWidget(decodedMemoLabel_, 2, 1);

    decodedGrid->addWidget(new QLabel("Expires:"), 3, 0);
    decodedExpiryLabel_ = new QLabel("—");
    decodedGrid->addWidget(decodedExpiryLabel_, 3, 1);

    layout->addWidget(decodedGroup);

    // --- Pay Button ---
    payInvoiceBtn_ = new QPushButton("Pay Reviewed Invoice");
    payInvoiceBtn_->setStyleSheet(
        "background-color: #4CAF50; color: white; padding: 10px; font-weight: bold;");
    payInvoiceBtn_->setEnabled(false);
    connect(payInvoiceBtn_, &QPushButton::clicked, this, &DpiWidget::onPayInvoice);
    layout->addWidget(payInvoiceBtn_);

    payStatusLabel_ = new QLabel;
    payStatusLabel_->setWordWrap(true);
    layout->addWidget(payStatusLabel_);

    payTierBadge_ = new QLabel;
    payTierBadge_->setTextFormat(Qt::RichText);
    payTierBadge_->setWordWrap(true);
    payTierBadge_->setVisible(false);
    layout->addWidget(payTierBadge_);

    // --- Package Output ---
    auto* packageGroup = new QGroupBox("Payment Package");
    auto* packageLayout = new QVBoxLayout(packageGroup);

    packageLayout->addWidget(new QLabel("Send this to the merchant:"));
    packageOutputEdit_ = new QTextEdit;
    packageOutputEdit_->setReadOnly(true);
    packageOutputEdit_->setMaximumHeight(60);
    packageOutputEdit_->setPlaceholderText("Package will appear after payment...");
    packageLayout->addWidget(packageOutputEdit_);

    copyPackageBtn_ = new QPushButton("Copy Package");
    copyPackageBtn_->setStyleSheet(
        "background-color: #2196F3; color: white; padding: 6px;");
    copyPackageBtn_->setEnabled(false);
    connect(copyPackageBtn_, &QPushButton::clicked, this, &DpiWidget::onCopyPackage);
    packageLayout->addWidget(copyPackageBtn_);

    layout->addWidget(packageGroup);
    connect(payInvoiceInputEdit_, &QTextEdit::textChanged, this, [this]() {
        payInvoiceBase64_.clear();
        payInvoiceBtn_->setEnabled(false);
        packageOutputEdit_->clear();
        copyPackageBtn_->setEnabled(false);
        payStatusLabel_->clear();
    });
    layout->addStretch();

    scroll->setWidget(payWidget);
    ScrollSupport::enableForScrollArea(scroll, payWidget);
    tabs_->addTab(scroll, "Pay");
}

// ============================================================================
// Slot: Create Invoice
// ============================================================================

void DpiWidget::onCreateInvoice() {
    QString amountStr = collectAmountEdit_->text().trimmed();
    if (amountStr.isEmpty()) {
        QMessageBox::warning(this, "Input Required", "Please enter an amount.");
        return;
    }

    double amount = amountStr.toDouble();
    if (amount <= 0) {
        QMessageBox::warning(this, "Invalid Amount", "Amount must be greater than 0.");
        return;
    }

    QJsonObject params;
    params["amount"] = amount;
    params["memo"] = collectMemoEdit_->text().trimmed();
    params["expiry_seconds"] = collectExpiryCombo_->currentData().toInt();

    createInvoiceBtn_->setEnabled(false);
    createInvoiceBtn_->setText("Creating...");
    rpc_->callNamed("dpi.createinvoice", params);
}

// ============================================================================
// Slot: Copy Invoice
// ============================================================================

void DpiWidget::onCopyInvoice() {
    if (collectInvoiceBase64_.isEmpty()) return;
    QApplication::clipboard()->setText(collectInvoiceBase64_);
    copyInvoiceBtn_->setText("Copied!");
    QTimer::singleShot(1500, this, [this]() {
        copyInvoiceBtn_->setText("Copy Invoice");
    });
}

// ============================================================================
// Slot: Verify Package
// ============================================================================

void DpiWidget::onVerifyPackage() {
    QString packageB64 = packageInputEdit_->toPlainText().trimmed();
    if (packageB64.isEmpty()) {
        QMessageBox::warning(this, "Input Required", "Paste a payment package first.");
        return;
    }
    if (collectInvoiceBase64_.isEmpty()) {
        QMessageBox::warning(this, "No Invoice", "Create an invoice first.");
        return;
    }

    QJsonObject params;
    params["package"] = packageB64;
    params["invoice"] = collectInvoiceBase64_;

    verifyPackageBtn_->setEnabled(false);
    verifyPackageBtn_->setText("Verifying...");
    rpc_->callNamed("dpi.verifypackage", params);
}

// ============================================================================
// Slot: Decode Invoice (Pay tab)
// ============================================================================

void DpiWidget::onDecodeInvoice() {
    QString invoiceB64 = payInvoiceInputEdit_->toPlainText().trimmed();
    if (invoiceB64.isEmpty()) {
        QMessageBox::warning(this, "Input Required", "Paste an invoice first.");
        return;
    }

    payInvoiceBase64_ = invoiceB64;

    QJsonObject params;
    params["invoice"] = invoiceB64;

    decodeInvoiceBtn_->setEnabled(false);
    decodeInvoiceBtn_->setText("Decoding...");
    rpc_->callNamed("dpi.decodeinvoice", params);
}

// ============================================================================
// Slot: Pay Invoice
// ============================================================================

void DpiWidget::onPayInvoice() {
    if (payInvoiceBase64_.isEmpty()) return;

    QJsonObject params;
    params["invoice"] = payInvoiceBase64_;

    payInvoiceBtn_->setEnabled(false);
    payInvoiceBtn_->setText("Paying...");
    payStatusLabel_->setText("Processing payment...");
    payStatusLabel_->setStyleSheet("color: #aaa;");
    rpc_->callNamed("dpi.payinvoice", params);
}

// ============================================================================
// Slot: Copy Package
// ============================================================================

void DpiWidget::onCopyPackage() {
    QString pkg = packageOutputEdit_->toPlainText().trimmed();
    if (pkg.isEmpty()) return;
    QApplication::clipboard()->setText(pkg);
    copyPackageBtn_->setText("Copied!");
    QTimer::singleShot(1500, this, [this]() {
        copyPackageBtn_->setText("Copy Package");
    });
}

// ============================================================================
// RPC Result Handler
// ============================================================================

void DpiWidget::onRpcResult(const QString& method, const QJsonValue& result) {
    if (method == "dpi.createinvoice") {
        createInvoiceBtn_->setEnabled(true);
        createInvoiceBtn_->setText("Create Collect Invoice");

        if (!result.isObject()) return;
        auto obj = result.toObject();
        if (obj.contains("error") && !obj["error"].toString().isEmpty()) {
            QMessageBox::warning(this, "Invoice Error", obj["error"].toString());
            return;
        }

        collectInvoiceBase64_ = obj["invoice"].toString();
        detailsGroup_->setVisible(true);
        copyInvoiceBtn_->setEnabled(!collectInvoiceBase64_.isEmpty());
        verifyPackageBtn_->setEnabled(!collectInvoiceBase64_.isEmpty());
        invoiceIdLabel_->setText(obj["invoice_id"].toString());
        invoiceDestLabel_->setText(obj["destination"].toString());
        invoiceAmountLabel_->setText(
            QString("%1 DIN").arg(obj["amount_din"].toDouble(), 0, 'f', 8));
        invoiceTextEdit_->setText(collectInvoiceBase64_);

        // Start countdown
        qint64 ts = static_cast<qint64>(obj["timestamp"].toDouble());
        int expiry = obj["expiry_seconds"].toInt();
        invoiceExpiryTimestamp_ = ts + expiry;
        countdownTimer_->start();
        onCountdownTick();

        // QR code with Dinero logo overlay (generate at 256 for quality, scale to label)
        // QR contains raw invoice base64, matching DineroDPI invoice mode.
        QString logoPath = QCoreApplication::applicationDirPath()
                           + "/../Resources/Dinero-Coin.png";
        QImage qr = QrUtil::makeQrWithLogo(collectInvoiceBase64_, logoPath, 320, 4);
        if (!qr.isNull()) {
            invoiceQrLabel_->setPixmap(
                QPixmap::fromImage(qr).scaled(invoiceQrLabel_->size(),
                    Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
    else if (method == "dpi.decodeinvoice") {
        decodeInvoiceBtn_->setEnabled(true);
        decodeInvoiceBtn_->setText("Review Invoice");

        if (!result.isObject()) return;
        auto obj = result.toObject();
        if (obj.contains("error") && !obj["error"].toString().isEmpty()) {
            QMessageBox::warning(this, "Decode Error", obj["error"].toString());
            payInvoiceBase64_.clear();
            payInvoiceBtn_->setEnabled(false);
            return;
        }

        decodedAmountLabel_->setText(
            QString("%1 DIN").arg(obj["amount_din"].toDouble(), 0, 'f', 8));
        decodedDestLabel_->setText(obj["destination_address"].toString());
        decodedMemoLabel_->setText(
            obj["memo"].toString().isEmpty() ? "(none)" : obj["memo"].toString());

        bool expired = obj["expired"].toBool();
        if (expired) {
            decodedExpiryLabel_->setText("EXPIRED");
            decodedExpiryLabel_->setStyleSheet("color: #f44336; font-weight: bold;");
            payInvoiceBase64_.clear();
            payInvoiceBtn_->setEnabled(false);
        } else {
            int expiry = obj["expiry"].toInt();
            decodedExpiryLabel_->setText(QString("%1 seconds").arg(expiry));
            decodedExpiryLabel_->setStyleSheet("");
            payInvoiceBtn_->setEnabled(true);
        }
    }
    else if (method == "dpi.payinvoice") {
        payInvoiceBtn_->setEnabled(true);
        payInvoiceBtn_->setText("Pay Reviewed Invoice");

        if (!result.isObject()) return;
        auto obj = result.toObject();
        if (obj.contains("error") && !obj["error"].toString().isEmpty()) {
            payStatusLabel_->setText("Payment failed: " + obj["error"].toString());
            payStatusLabel_->setStyleSheet("color: #f44336;");
            return;
        }

        QString txid = obj["txid"].toString();
        QString packageB64 = obj["package"].toString();
        packageOutputEdit_->setText(packageB64);
        copyPackageBtn_->setEnabled(!packageB64.isEmpty());
        payStatusLabel_->setText(QString("Payment sent! TxID: %1").arg(txid));
        payStatusLabel_->setStyleSheet("color: #4CAF50; font-weight: bold;");

        // Start tier tracking for this payment
        startTierTracking(txid);
        trackedPayTxid_ = txid;
        payTier_ = 1;
        payConfirmations_ = 0;
        updateTierBadge(payTierBadge_, payTier_, payConfirmations_);
        payTierBadge_->setVisible(true);
    }
    else if (method == "dpi.verifypackage") {
        verifyPackageBtn_->setEnabled(true);
        verifyPackageBtn_->setText("Verify Package");

        if (!result.isObject()) return;
        auto obj = result.toObject();
        if (obj.contains("error") && !obj["error"].toString().isEmpty()) {
            verifyResultLabel_->setText("Error: " + obj["error"].toString());
            verifyResultLabel_->setStyleSheet("color: #f44336;");
            return;
        }

        QString tier = obj["tier"].toString();
        double risk = obj["risk_score"].toDouble();
        QJsonObject checks = obj["checks"].toObject();

        // Tier badge
        QString badge;
        if (tier == "T1") {
            badge = "<span style='background:#4CAF50; color:white; padding:4px 12px; "
                    "border-radius:4px; font-weight:bold; font-size:14px;'>"
                    "T1 — Verified</span>";
        } else {
            badge = "<span style='background:#FF9800; color:white; padding:4px 12px; "
                    "border-radius:4px; font-weight:bold; font-size:14px;'>"
                    "T0 — Unverified</span>";
        }

        // UTXO proof presence indicator
        bool hasProofs = obj.contains("utreexo_proofs") &&
                         obj["utreexo_proofs"].isObject();
        QString proofBadge;
        if (hasProofs) {
            proofBadge = "  <span style='background:#4CAF50; color:white; padding:2px 8px; "
                         "border-radius:3px; font-size:11px;'>UTXO Verified</span>";
        } else {
            proofBadge = "  <span style='background:#999; color:white; padding:2px 8px; "
                         "border-radius:3px; font-size:11px;'>No UTXO Proof</span>";
        }
        verifyResultLabel_->setText(
            badge + proofBadge + QString("  Risk: %1").arg(risk, 0, 'f', 2));

        // Verification checks
        auto checkLine = [](bool ok, const QString& label) -> QString {
            return QString("%1 %2\n").arg(ok ? "PASS" : "FAIL", label);
        };
        QString details;
        details += checkLine(checks["invoice_bound"].toBool(), "Invoice bound");
        details += checkLine(checks["output_match"].toBool(), "Output match");
        details += checkLine(checks["amount_match"].toBool(), "Amount match");
        details += checkLine(checks["attestation_valid"].toBool(), "Attestation valid");
        details += checkLine(checks["seen_in_mempool"].toBool(), "Seen in mempool");
        details += checkLine(!checks["conflicts_found"].toBool(), "No conflicts");
        details += checkLine(!checks["expired"].toBool(), "Not expired");
        details += checkLine(checks["utreexo_proofs_valid"].toBool(), "Utreexo UTXO proofs");

        // UTXO proof status summary
        if (hasProofs) {
            auto proofs = obj["utreexo_proofs"].toObject();
            bool hasAnchor = proofs.contains("anchor");
            int inputCount = proofs["inputs"].toArray().size();
            details += QString("\nUTXO Proofs: %1 input(s) verified%2\n")
                .arg(inputCount)
                .arg(hasAnchor ? " (anchored)" : "");
        } else {
            details += "\nUTXO Proofs: not available (bridge not running)\n";
        }
        verifyDetailsEdit_->setText(details);

        // Start tier tracking if we have a txid
        QString collectTxid = obj["txid"].toString();
        if (!collectTxid.isEmpty() && tier == "T1") {
            trackedCollectTxid_ = collectTxid;
            collectTier_ = 1;
            collectConfirmations_ = 0;
            startTierTracking(collectTxid);
            updateTierBadge(collectTierBadge_, collectTier_, collectConfirmations_);
            collectTierBadge_->setVisible(true);
        }
    }
    else if (method == "wallet.listtransactions") {
        // Tier progression polling response
        if (!result.isObject() && !result.isArray()) return;
        QJsonArray txList;
        if (result.isArray()) {
            txList = result.toArray();
        } else if (result.isObject()) {
            auto obj = result.toObject();
            if (obj.contains("transactions"))
                txList = obj["transactions"].toArray();
            else if (obj.contains("result"))
                txList = obj["result"].toArray();
        }

        // Build txid -> confirmations map
        QHash<QString, int> confMap;
        for (const auto& val : txList) {
            auto tx = val.toObject();
            QString txid = tx["txid"].toString();
            int conf = tx["confirmations"].toInt(0);
            if (!txid.isEmpty()) {
                confMap[txid] = conf;
            }
        }

        // Update pay tier (T1=verified+mempool, T2=1 conf, T3=6+ conf)
        if (!trackedPayTxid_.isEmpty() && confMap.contains(trackedPayTxid_)) {
            int conf = confMap[trackedPayTxid_];
            payConfirmations_ = conf;
            if (conf >= 6) payTier_ = 3;
            else if (conf >= 1) payTier_ = 2;
            else if (payTier_ < 1) payTier_ = 1;
            updateTierBadge(payTierBadge_, payTier_, payConfirmations_);
        } else if (!trackedPayTxid_.isEmpty() && payTier_ < 1) {
            // Not in wallet list yet — might still be propagating
        }

        // Update collect tier (same semantics)
        if (!trackedCollectTxid_.isEmpty() && confMap.contains(trackedCollectTxid_)) {
            int conf = confMap[trackedCollectTxid_];
            collectConfirmations_ = conf;
            if (conf >= 6) collectTier_ = 3;
            else if (conf >= 1) collectTier_ = 2;
            else if (collectTier_ < 1) collectTier_ = 1;
            updateTierBadge(collectTierBadge_, collectTier_, collectConfirmations_);
        }
    }
}

// ============================================================================
// RPC Error Handler
// ============================================================================

void DpiWidget::onRpcError(const QString& method, int code, const QString& message) {
    QString err = QString("%1 (code %2)").arg(message).arg(code);

    if (method == "dpi.createinvoice") {
        createInvoiceBtn_->setEnabled(true);
        createInvoiceBtn_->setText("Create Collect Invoice");
        QMessageBox::warning(this, "Invoice Error", err);
    }
    else if (method == "dpi.decodeinvoice") {
        decodeInvoiceBtn_->setEnabled(true);
        decodeInvoiceBtn_->setText("Review Invoice");
        payInvoiceBase64_.clear();
        QMessageBox::warning(this, "Decode Error", err);
    }
    else if (method == "dpi.payinvoice") {
        payInvoiceBtn_->setEnabled(true);
        payInvoiceBtn_->setText("Pay Reviewed Invoice");
        payStatusLabel_->setText("Error: " + err);
        payStatusLabel_->setStyleSheet("color: #f44336;");
    }
    else if (method == "dpi.verifypackage") {
        verifyPackageBtn_->setEnabled(true);
        verifyPackageBtn_->setText("Verify Package");
        verifyResultLabel_->setText("Error: " + err);
        verifyResultLabel_->setStyleSheet("color: #f44336;");
    }
}

// ============================================================================
// Tier Tracking
// ============================================================================

void DpiWidget::startTierTracking(const QString& txid) {
    Q_UNUSED(txid)
    if (!tierPollTimer_->isActive()) {
        tierPollTimer_->start();
    }
}

void DpiWidget::updateTierBadge(QLabel* badge, int tier, int confirmations) {
    QString color, label, confText;
    switch (tier) {
    case 0:
        color = "#f44336"; label = "T0 — Failed"; break;
    case 1:
        color = "#FF9800"; label = "T1 — Verified"; break;
    case 2:
        color = "#2196F3"; label = "T2 — Confirmed"; break;
    case 3:
        color = "#4CAF50"; label = "T3 — Finalized"; break;
    default:
        color = "#999"; label = "Unknown"; break;
    }
    confText = confirmations > 0 ? QString(" (%1 conf)").arg(confirmations) : "";
    badge->setText(
        QString("<span style='background:%1; color:white; padding:4px 12px; "
                "border-radius:4px; font-weight:bold;'>%2%3</span>")
            .arg(color, label, confText));
}

void DpiWidget::onTierPollTick() {
    // Poll wallet.listtransactions for confirmation updates
    bool anyActive = false;

    if (!trackedPayTxid_.isEmpty() && payTier_ < 3) {
        anyActive = true;
    } else if (!trackedPayTxid_.isEmpty() && payConfirmations_ < 6) {
        anyActive = true;
    }
    if (!trackedCollectTxid_.isEmpty() && collectTier_ < 3) {
        anyActive = true;
    } else if (!trackedCollectTxid_.isEmpty() && collectConfirmations_ < 6) {
        anyActive = true;
    }

    if (!anyActive) {
        tierPollTimer_->stop();
        return;
    }

    // Request wallet.listtransactions — the result comes back via onRpcResult
    QJsonObject params;
    rpc_->callNamed("wallet.listtransactions", params);
}

// ============================================================================
// Countdown Timer
// ============================================================================

void DpiWidget::onCountdownTick() {
    if (invoiceExpiryTimestamp_ == 0) return;

    qint64 remaining = invoiceExpiryTimestamp_ - QDateTime::currentSecsSinceEpoch();
    if (remaining <= 0) {
        invoiceExpiryLabel_->setText("EXPIRED");
        invoiceExpiryLabel_->setStyleSheet("color: #f44336; font-weight: bold;");
        countdownTimer_->stop();
    } else {
        int min = static_cast<int>(remaining / 60);
        int sec = static_cast<int>(remaining % 60);
        invoiceExpiryLabel_->setText(
            QString("%1:%2 remaining").arg(min, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0')));
        invoiceExpiryLabel_->setStyleSheet("color: #4CAF50;");
    }
}
