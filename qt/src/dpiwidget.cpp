#include "dpiwidget.h"
#include "rpcclient.h"
#include "QrUtil.h"
#include "scrollsupport.h"
#include "chromestyle.h"
#include "paycollectpolicy.h"

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

namespace {
constexpr int kFormStretch = 3;
constexpr int kCardStretch = 2;
constexpr int kGutter = 12;
const QString kCreateText = QStringLiteral("Create invoice");
const QString kVerifyText = QStringLiteral("Check payment");
const QString kReviewText = QStringLiteral("Review invoice");
const QString kPayText = QStringLiteral("Pay this invoice");
using PayCollectPolicy::Tone;
QString toneText(Tone tone, bool bold = false) {
    return QString("color: %1;%2").arg(PayCollectPolicy::toneColor(tone), bold ? " font-weight: bold;" : "");
}
}  // namespace

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
    connect(collectAmountEdit_, &QLineEdit::textChanged, this, &DpiWidget::updateHints);
    updateHints();
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

    if (detailsGroup_) detailsGroup_->setVisible(false);
    if (collectInvoicePlaceholder_) collectInvoicePlaceholder_->setVisible(true);
    if (verifyResultLabel_) verifyResultLabel_->setVisible(false);
    if (verifyDetailsEdit_) verifyDetailsEdit_->setVisible(false);
    if (payStatusLabel_) payStatusLabel_->setVisible(false);
    if (packageGroup_) packageGroup_->setVisible(false);
    updateHints();
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

    // Same 3:2 split and gutter as the Overview and Covenants tabs.
    auto* row = new QHBoxLayout;
    row->setSpacing(kGutter);

    // --- Request a payment ---
    auto* createGroup = new QGroupBox("Request a payment");
    createGroup->setObjectName("collectForm");
    createGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* createLayout = new QGridLayout(createGroup);

    createLayout->addWidget(new QLabel("Amount (DIN):"), 0, 0);
    collectAmountEdit_ = new QLineEdit;
    collectAmountEdit_->setObjectName("collectAmount");
    collectAmountEdit_->setPlaceholderText("e.g. 50.0");
    createLayout->addWidget(collectAmountEdit_, 0, 1);

    createLayout->addWidget(new QLabel("Memo:"), 1, 0);
    collectMemoEdit_ = new QLineEdit;
    collectMemoEdit_->setPlaceholderText("e.g. Order #12345");
    createLayout->addWidget(collectMemoEdit_, 1, 1);

    createLayout->addWidget(new QLabel("Expires in:"), 2, 0);
    collectExpiryCombo_ = new QComboBox;
    collectExpiryCombo_->addItem("5 minutes", 300);
    collectExpiryCombo_->addItem("15 minutes", 900);
    collectExpiryCombo_->addItem("1 hour", 3600);
    collectExpiryCombo_->addItem("24 hours", 86400);
    collectExpiryCombo_->setCurrentIndex(1);
    createLayout->addWidget(collectExpiryCombo_, 2, 1);

    createInvoiceBtn_ = new QPushButton(kCreateText);
    createInvoiceBtn_->setObjectName("collectCreate");
    createInvoiceBtn_->setStyleSheet(chromeButtonStyle());
    connect(createInvoiceBtn_, &QPushButton::clicked, this, &DpiWidget::onCreateInvoice);
    createLayout->addWidget(createInvoiceBtn_, 3, 0, 1, 2);

    collectCreateHint_ = makeHint("collectCreateHint");
    createLayout->addWidget(collectCreateHint_, 4, 0, 1, 2);
    createLayout->setRowStretch(5, 1);
    row->addWidget(createGroup, kFormStretch);

    // --- Your invoice: placeholder until one is created ---
    auto* invoiceCard = new QGroupBox("Your invoice");
    invoiceCard->setObjectName("collectInvoiceCard");
    invoiceCard->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* cardLayout = new QVBoxLayout(invoiceCard);

    collectInvoicePlaceholder_ = new QLabel("Your invoice and its QR code will appear here.\n"
                                            "Share it with the payer from the Dinero phone app "
                                            "(DineroDPI) or another Dinero wallet.");
    collectInvoicePlaceholder_->setObjectName("collectInvoicePlaceholder");
    collectInvoicePlaceholder_->setWordWrap(true);
    collectInvoicePlaceholder_->setAlignment(Qt::AlignCenter);
    collectInvoicePlaceholder_->setStyleSheet(
        "QLabel { color: #868e96; padding: 24px; border: 1px dashed #3d434d; border-radius: 8px; }");
    cardLayout->addWidget(collectInvoicePlaceholder_);

    detailsGroup_ = new QWidget;
    auto* detailsLayout = new QVBoxLayout(detailsGroup_);
    detailsLayout->setContentsMargins(0, 0, 0, 0);

    invoiceQrLabel_ = new QLabel;
    invoiceQrLabel_->setAlignment(Qt::AlignCenter);
    invoiceQrLabel_->setFixedSize(256, 256);
    detailsLayout->addWidget(invoiceQrLabel_, 0, Qt::AlignHCenter);

    auto* infoGrid = new QGridLayout;
    infoGrid->setColumnStretch(1, 1);
    infoGrid->addWidget(new QLabel("Invoice ID:"), 0, 0);
    invoiceIdLabel_ = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
    invoiceIdLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    invoiceIdLabel_->setWordWrap(true);
    infoGrid->addWidget(invoiceIdLabel_, 0, 1);

    infoGrid->addWidget(new QLabel("Pays to:"), 1, 0);
    invoiceDestLabel_ = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
    invoiceDestLabel_->setWordWrap(true);
    invoiceDestLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoGrid->addWidget(invoiceDestLabel_, 1, 1);

    infoGrid->addWidget(new QLabel("Amount:"), 2, 0);
    invoiceAmountLabel_ = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
    infoGrid->addWidget(invoiceAmountLabel_, 2, 1);

    infoGrid->addWidget(new QLabel("Expires:"), 3, 0);
    invoiceExpiryLabel_ = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
    infoGrid->addWidget(invoiceExpiryLabel_, 3, 1);
    detailsLayout->addLayout(infoGrid);

    invoiceTextEdit_ = new QTextEdit;
    invoiceTextEdit_->setReadOnly(true);
    invoiceTextEdit_->setMaximumHeight(50);
    detailsLayout->addWidget(invoiceTextEdit_);

    copyInvoiceBtn_ = new QPushButton("Copy invoice");
    copyInvoiceBtn_->setStyleSheet(chromeButtonStyle());
    copyInvoiceBtn_->setEnabled(false);
    connect(copyInvoiceBtn_, &QPushButton::clicked, this, &DpiWidget::onCopyInvoice);
    detailsLayout->addWidget(copyInvoiceBtn_);

    detailsGroup_->setVisible(false);  // hidden until an invoice is created
    cardLayout->addWidget(detailsGroup_);
    cardLayout->addStretch(1);
    row->addWidget(invoiceCard, kCardStretch);
    layout->addLayout(row);

    // --- Confirm you were paid ---
    auto* verifyGroup = new QGroupBox("Confirm you were paid");
    verifyGroup->setObjectName("collectVerifyGroup");
    auto* verifyLayout = new QVBoxLayout(verifyGroup);

    auto* pastePrompt = new QLabel("Paste the payment package the payer sent you:");
    pastePrompt->setObjectName("collectPastePrompt");
    pastePrompt->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    verifyLayout->addWidget(pastePrompt);
    packageInputEdit_ = new QTextEdit;
    packageInputEdit_->setMaximumHeight(60);
    packageInputEdit_->setPlaceholderText("Paste payment package here...");
    connect(packageInputEdit_, &QTextEdit::textChanged, this, &DpiWidget::updateHints);
    verifyLayout->addWidget(packageInputEdit_);

    verifyPackageBtn_ = new QPushButton(kVerifyText);
    verifyPackageBtn_->setObjectName("collectVerify");
    verifyPackageBtn_->setStyleSheet(chromeButtonStyle());
    connect(verifyPackageBtn_, &QPushButton::clicked, this, &DpiWidget::onVerifyPackage);
    verifyLayout->addWidget(verifyPackageBtn_);

    collectVerifyHint_ = makeHint("collectVerifyHint");
    verifyLayout->addWidget(collectVerifyHint_);

    verifyResultLabel_ = new QLabel;
    verifyResultLabel_->setObjectName("collectVerifyResult");
    verifyResultLabel_->setWordWrap(true);
    verifyResultLabel_->setTextFormat(Qt::RichText);
    verifyResultLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    verifyResultLabel_->setVisible(false);  // until there is a result
    verifyLayout->addWidget(verifyResultLabel_);

    verifyDetailsEdit_ = new QTextEdit;
    verifyDetailsEdit_->setObjectName("collectVerifyDetails");
    verifyDetailsEdit_->setReadOnly(true);
    verifyDetailsEdit_->setMaximumHeight(170);
    verifyDetailsEdit_->setVisible(false);
    verifyLayout->addWidget(verifyDetailsEdit_);

    collectTierBadge_ = new QLabel;
    collectTierBadge_->setTextFormat(Qt::RichText);
    collectTierBadge_->setWordWrap(true);
    collectTierBadge_->setVisible(false);
    verifyLayout->addWidget(collectTierBadge_);

    layout->addWidget(verifyGroup);
    layout->addStretch(1);  // spare height goes below the content, not into the boxes

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

    auto* row = new QHBoxLayout;
    row->setSpacing(kGutter);

    // --- Invoice Input ---
    auto* inputGroup = new QGroupBox("Pay an invoice");
    inputGroup->setObjectName("payForm");
    inputGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* inputLayout = new QVBoxLayout(inputGroup);

    auto* payPrompt = new QLabel("Paste or scan an invoice from the Dinero phone app (DineroDPI) "
                                 "or another Dinero wallet:");
    payPrompt->setWordWrap(true);
    payPrompt->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    inputLayout->addWidget(payPrompt);
    payInvoiceInputEdit_ = new QTextEdit;
    payInvoiceInputEdit_->setObjectName("payInvoiceInput");
    payInvoiceInputEdit_->setMaximumHeight(60);
    payInvoiceInputEdit_->setPlaceholderText("Paste invoice here...");
    inputLayout->addWidget(payInvoiceInputEdit_);

    decodeInvoiceBtn_ = new QPushButton(kReviewText);
    decodeInvoiceBtn_->setObjectName("payReview");
    decodeInvoiceBtn_->setStyleSheet(chromeButtonStyle());
    connect(decodeInvoiceBtn_, &QPushButton::clicked, this, &DpiWidget::onDecodeInvoice);
    inputLayout->addWidget(decodeInvoiceBtn_);

    payReviewHint_ = makeHint("payReviewHint");
    inputLayout->addWidget(payReviewHint_);
    inputLayout->addStretch(1);
    row->addWidget(inputGroup, kFormStretch);

    // --- Decoded Details ---
    auto* decodedGroup = new QGroupBox("Invoice details");
    decodedGroup->setObjectName("payDetails");
    decodedGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* decodedLayout = new QVBoxLayout(decodedGroup);
    auto* decodedGrid = new QGridLayout;
    decodedGrid->setColumnStretch(1, 1);

    decodedGrid->addWidget(new QLabel("Amount:"), 0, 0);
    decodedAmountLabel_ = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
    decodedGrid->addWidget(decodedAmountLabel_, 0, 1);

    decodedGrid->addWidget(new QLabel("Pays to:"), 1, 0);
    decodedDestLabel_ = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
    decodedDestLabel_->setWordWrap(true);
    decodedDestLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    decodedGrid->addWidget(decodedDestLabel_, 1, 1);

    decodedGrid->addWidget(new QLabel("Memo:"), 2, 0);
    decodedMemoLabel_ = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
    decodedGrid->addWidget(decodedMemoLabel_, 2, 1);

    decodedGrid->addWidget(new QLabel("Expires:"), 3, 0);
    decodedExpiryLabel_ = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
    decodedGrid->addWidget(decodedExpiryLabel_, 3, 1);
    decodedLayout->addLayout(decodedGrid);

    payInvoiceBtn_ = new QPushButton(kPayText);
    payInvoiceBtn_->setStyleSheet(chromeButtonStyle());
    payInvoiceBtn_->setEnabled(false);
    connect(payInvoiceBtn_, &QPushButton::clicked, this, &DpiWidget::onPayInvoice);
    decodedLayout->addWidget(payInvoiceBtn_);

    payStatusLabel_ = new QLabel;
    payStatusLabel_->setWordWrap(true);
    payStatusLabel_->setVisible(false);
    decodedLayout->addWidget(payStatusLabel_);

    payTierBadge_ = new QLabel;
    payTierBadge_->setTextFormat(Qt::RichText);
    payTierBadge_->setWordWrap(true);
    payTierBadge_->setVisible(false);
    decodedLayout->addWidget(payTierBadge_);
    decodedLayout->addStretch(1);
    row->addWidget(decodedGroup, kCardStretch);
    layout->addLayout(row);

    // --- Package Output (after paying) ---
    packageGroup_ = new QGroupBox("Payment package");
    packageGroup_->setObjectName("payPackageGroup");
    auto* packageLayout = new QVBoxLayout(packageGroup_);

    auto* sendPrompt = new QLabel("Send this to the person you paid, so they can confirm the payment:");
    sendPrompt->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    packageLayout->addWidget(sendPrompt);
    packageOutputEdit_ = new QTextEdit;
    packageOutputEdit_->setReadOnly(true);
    packageOutputEdit_->setMaximumHeight(60);
    packageLayout->addWidget(packageOutputEdit_);

    copyPackageBtn_ = new QPushButton("Copy package");
    copyPackageBtn_->setStyleSheet(chromeButtonStyle());
    copyPackageBtn_->setEnabled(false);
    connect(copyPackageBtn_, &QPushButton::clicked, this, &DpiWidget::onCopyPackage);
    packageLayout->addWidget(copyPackageBtn_);

    packageGroup_->setVisible(false);  // until a payment produced a package
    layout->addWidget(packageGroup_);
    connect(payInvoiceInputEdit_, &QTextEdit::textChanged, this, [this]() {
        payInvoiceBase64_.clear();
        payInvoiceBtn_->setEnabled(false);
        packageOutputEdit_->clear();
        copyPackageBtn_->setEnabled(false);
        packageGroup_->setVisible(false);
        payStatusLabel_->clear();
        payStatusLabel_->setVisible(false);
        updateHints();
    });
    layout->addStretch(1);

    scroll->setWidget(payWidget);
    ScrollSupport::enableForScrollArea(scroll, payWidget);
    tabs_->addTab(scroll, "Pay");
}

QLabel* DpiWidget::makeHint(const char* objectName) {
    auto* hint = new QLabel;
    hint->setObjectName(objectName);
    hint->setWordWrap(true);
    hint->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    hint->setStyleSheet(PayCollectPolicy::pillStyle(PayCollectPolicy::Tone::Warn));
    return hint;
}

// Say what is missing instead of leaving a button that fails with a dialog.
void DpiWidget::updateHints() {
    auto show = [](QPushButton* button, QLabel* hint, const QString& blocker, bool busy) {
        if (!button || !hint) return;
        if (!busy) button->setEnabled(blocker.isEmpty());
        hint->setText(blocker.isEmpty() ? QString() : "To continue: " + blocker);
        hint->setVisible(!blocker.isEmpty());
    };
    show(createInvoiceBtn_, collectCreateHint_,
         PayCollectPolicy::amountBlocker(collectAmountEdit_ ? collectAmountEdit_->text() : QString()),
         createInvoiceBtn_ && createInvoiceBtn_->text() != kCreateText);

    QString verifyBlocker;
    if (collectInvoiceBase64_.isEmpty()) verifyBlocker = "Create an invoice first";
    else if (packageInputEdit_ && packageInputEdit_->toPlainText().trimmed().isEmpty())
        verifyBlocker = "Paste the payment package";
    show(verifyPackageBtn_, collectVerifyHint_, verifyBlocker,
         verifyPackageBtn_ && verifyPackageBtn_->text() != kVerifyText);

    const bool noInvoice = !payInvoiceInputEdit_ || payInvoiceInputEdit_->toPlainText().trimmed().isEmpty();
    show(decodeInvoiceBtn_, payReviewHint_, noInvoice ? QString("Paste an invoice to review it") : QString(),
         decodeInvoiceBtn_ && decodeInvoiceBtn_->text() != kReviewText);
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
        copyInvoiceBtn_->setText("Copy invoice");
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
    payStatusLabel_->setStyleSheet(toneText(Tone::Neutral));
    payStatusLabel_->setVisible(true);
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
        copyPackageBtn_->setText("Copy package");
    });
}

// ============================================================================
// RPC Result Handler
// ============================================================================

void DpiWidget::onRpcResult(const QString& method, const QJsonValue& result) {
    if (method == "dpi.createinvoice") {
        createInvoiceBtn_->setText(kCreateText);
        updateHints();

        if (!result.isObject()) return;
        auto obj = result.toObject();
        if (obj.contains("error") && !obj["error"].toString().isEmpty()) {
            QMessageBox::warning(this, "Invoice Error", obj["error"].toString());
            return;
        }

        collectInvoiceBase64_ = obj["invoice"].toString();
        collectInvoicePlaceholder_->setVisible(false);
        detailsGroup_->setVisible(true);
        copyInvoiceBtn_->setEnabled(!collectInvoiceBase64_.isEmpty());
        updateHints();
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
        decodeInvoiceBtn_->setText(kReviewText);
        updateHints();

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
            decodedExpiryLabel_->setText("Expired");
            decodedExpiryLabel_->setStyleSheet(toneText(Tone::Bad, true));
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
        payInvoiceBtn_->setText(kPayText);

        if (!result.isObject()) return;
        auto obj = result.toObject();
        payStatusLabel_->setVisible(true);
        if (obj.contains("error") && !obj["error"].toString().isEmpty()) {
            payStatusLabel_->setText("Payment failed: " + obj["error"].toString());
            payStatusLabel_->setStyleSheet(toneText(Tone::Bad));
            return;
        }

        QString txid = obj["txid"].toString();
        QString packageB64 = obj["package"].toString();
        packageOutputEdit_->setText(packageB64);
        copyPackageBtn_->setEnabled(!packageB64.isEmpty());
        packageGroup_->setVisible(!packageB64.isEmpty());
        payStatusLabel_->setText(QString("Payment sent. TxID: %1").arg(txid));
        payStatusLabel_->setStyleSheet(toneText(Tone::Good, true));

        // Start tier tracking for this payment
        startTierTracking(txid);
        trackedPayTxid_ = txid;
        payTier_ = 1;
        payConfirmations_ = 0;
        updateTierBadge(payTierBadge_, payTier_, payConfirmations_);
        payTierBadge_->setVisible(true);
    }
    else if (method == "dpi.verifypackage") {
        verifyPackageBtn_->setText(kVerifyText);
        updateHints();

        if (!result.isObject()) return;
        auto obj = result.toObject();
        verifyResultLabel_->setVisible(true);
        if (obj.contains("error") && !obj["error"].toString().isEmpty()) {
            verifyResultLabel_->setText("Error: " + obj["error"].toString().toHtmlEscaped());
            verifyResultLabel_->setStyleSheet(toneText(Tone::Bad));
            return;
        }
        verifyResultLabel_->setStyleSheet(QString());

        QString tier = obj["tier"].toString();
        double risk = obj["risk_score"].toDouble();
        QJsonObject checks = obj["checks"].toObject();

        const auto verdict = PayCollectPolicy::verifyResult(tier);
        verifyResultLabel_->setToolTip(verdict.tooltip);

        // UTXO proof presence indicator
        bool hasProofs = obj.contains("utreexo_proofs") &&
                         obj["utreexo_proofs"].isObject();
        const PayCollectPolicy::Badge proofBadge = hasProofs
            ? PayCollectPolicy::Badge{"UTXO proofs checked", Tone::Good, {}}
            : PayCollectPolicy::Badge{"No UTXO proofs", Tone::Neutral, {}};
        verifyResultLabel_->setText(
            PayCollectPolicy::badgeHtml(verdict) + "&nbsp;&nbsp;" + PayCollectPolicy::badgeHtml(proofBadge) +
            QString("&nbsp;&nbsp;Risk score %1").arg(risk, 0, 'f', 2));

        // Verification checks
        auto checkLine = [](bool ok, const QString& label) -> QString {
            return QString::fromUtf8("%1 %2\n").arg(ok ? QString::fromUtf8("\xE2\x9C\x93") : QString::fromUtf8("\xE2\x9C\x97"), label);
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
        verifyDetailsEdit_->setVisible(true);

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
        createInvoiceBtn_->setText(kCreateText);
        updateHints();
        QMessageBox::warning(this, "Invoice Error", err);
    }
    else if (method == "dpi.decodeinvoice") {
        decodeInvoiceBtn_->setText(kReviewText);
        updateHints();
        payInvoiceBase64_.clear();
        QMessageBox::warning(this, "Decode Error", err);
    }
    else if (method == "dpi.payinvoice") {
        payInvoiceBtn_->setEnabled(true);
        payInvoiceBtn_->setText(kPayText);
        payStatusLabel_->setText("Error: " + err);
        payStatusLabel_->setStyleSheet(toneText(Tone::Bad));
        payStatusLabel_->setVisible(true);
    }
    else if (method == "dpi.verifypackage") {
        verifyPackageBtn_->setText(kVerifyText);
        updateHints();
        verifyResultLabel_->setText("Error: " + err.toHtmlEscaped());
        verifyResultLabel_->setStyleSheet(toneText(Tone::Bad));
        verifyResultLabel_->setVisible(true);
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
    const auto status = PayCollectPolicy::paymentStatus(tier, confirmations);
    badge->setText(PayCollectPolicy::badgeHtml(status));
    badge->setToolTip(status.tooltip);
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
        invoiceExpiryLabel_->setText("Expired");
        invoiceExpiryLabel_->setStyleSheet(toneText(Tone::Bad, true));
        countdownTimer_->stop();
    } else {
        int min = static_cast<int>(remaining / 60);
        int sec = static_cast<int>(remaining % 60);
        invoiceExpiryLabel_->setText(
            QString("%1:%2 remaining").arg(min, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0')));
        invoiceExpiryLabel_->setStyleSheet(toneText(Tone::Good));
    }
}
