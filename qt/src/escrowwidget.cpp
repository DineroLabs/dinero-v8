#include "escrowwidget.h"
#include "rpcclient.h"
#include "websocketclient.h"
#include "QrUtil.h"
#include "scrollsupport.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QInputDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QScrollArea>

EscrowWidget::EscrowWidget(RpcClient* rpc, WebSocketClient* ws, QWidget* parent)
    : QWidget(parent)
    , rpc_(rpc)
    , ws_(ws)
    , currentBlockHeight_(0)
    , exportForRelease_(true)
{
    setupUi();
    setupConnections();

    // Initial data load
    onRefreshContracts();

    // Start auto-refresh
    refreshTimer_.start(REFRESH_INTERVAL_MS);
}

EscrowWidget::~EscrowWidget() = default;

void EscrowWidget::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    // ========== HEADER ==========
    auto* headerLayout = new QHBoxLayout();

    titleLabel_ = new QLabel("<h2>⚖️ Smart Contract Escrow</h2>");
    statsLabel_ = new QLabel("0 active contracts");
    statsLabel_->setStyleSheet("color: #666; font-size: 12px;");

    createButton_ = new QPushButton("➕ Create New Escrow");
    createButton_->setStyleSheet("font-weight: bold; padding: 8px 16px;");

    refreshButton_ = new QPushButton("🔄 Refresh");
    exportButton_ = new QPushButton("📥 Export CSV");

    headerLayout->addWidget(titleLabel_);
    headerLayout->addWidget(statsLabel_);
    headerLayout->addStretch();
    headerLayout->addWidget(createButton_);
    headerLayout->addWidget(refreshButton_);
    headerLayout->addWidget(exportButton_);

    mainLayout->addLayout(headerLayout);

    // ========== FILTER ==========
    auto* filterLayout = new QHBoxLayout();
    filterLabel_ = new QLabel("Filter:");
    filterCombo_ = new QComboBox();
    filterCombo_->addItem("All Contracts", QVariant::fromValue(StatusFilter::All));
    filterCombo_->addItem("⏳ Pending", QVariant::fromValue(StatusFilter::Pending));
    filterCombo_->addItem("🔒 Locked", QVariant::fromValue(StatusFilter::Locked));
    filterCombo_->addItem("✅ Released", QVariant::fromValue(StatusFilter::Released));
    filterCombo_->addItem("↩️  Refunded", QVariant::fromValue(StatusFilter::Refunded));
    filterCombo_->addItem("⏰ Expired", QVariant::fromValue(StatusFilter::Expired));

    filterLayout->addWidget(filterLabel_);
    filterLayout->addWidget(filterCombo_);
    filterLayout->addStretch();

    mainLayout->addLayout(filterLayout);

    // ========== CONTRACTS TABLE ==========
    contractsTable_ = new QTableView();
    tableModel_ = new QStandardItemModel(0, COL_COUNT, this);

    tableModel_->setHorizontalHeaderLabels({
        "Contract ID",
        "P2SH Address",
        "Amount (DIN)",
        "Status",
        "Buyer",
        "Seller",
        "Refund Block",
        "Created"
    });

    contractsTable_->setModel(tableModel_);
    contractsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    contractsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    contractsTable_->setAlternatingRowColors(true);
    contractsTable_->horizontalHeader()->setStretchLastSection(true);
    contractsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Column widths
    contractsTable_->setColumnWidth(COL_ID, 180);
    contractsTable_->setColumnWidth(COL_P2SH, 350);
    contractsTable_->setColumnWidth(COL_AMOUNT, 120);
    contractsTable_->setColumnWidth(COL_STATUS, 100);
    contractsTable_->setColumnWidth(COL_BUYER, 100);
    contractsTable_->setColumnWidth(COL_SELLER, 100);
    contractsTable_->setColumnWidth(COL_REFUND_HEIGHT, 120);

    mainLayout->addWidget(contractsTable_);

    // ========== DETAILS PANEL ==========
    detailsGroup_ = new QGroupBox("Selected Contract Details");
    auto* detailsLayout = new QGridLayout();

    contractIdLabel_ = new QLabel("-");
    p2shAddressLabel_ = new QLabel("-");
    amountLabel_ = new QLabel("-");
    statusLabel_ = new QLabel("-");
    buyerLabel_ = new QLabel("-");
    sellerLabel_ = new QLabel("-");
    mediatorLabel_ = new QLabel("-");
    lockTxidLabel_ = new QLabel("-");
    refundHeightLabel_ = new QLabel("-");
    currentHeightLabel_ = new QLabel("-");
    timeRemainingLabel_ = new QLabel("-");

    copyAddressButton_ = new QPushButton("📋 Copy P2SH");
    copyAddressButton_->setMaximumWidth(120);
    showQRButton_ = new QPushButton("📱 Show QR");
    showQRButton_->setMaximumWidth(120);
    showQRButton_->setEnabled(false);
    viewScriptButton_ = new QPushButton("📜 View Script");
    viewScriptButton_->setMaximumWidth(120);
    exportSighashButton_ = new QPushButton("💾 Export Sighash");
    exportSighashButton_->setMaximumWidth(140);
    exportSighashButton_->setEnabled(false);

    timelockProgress_ = new QProgressBar();
    timelockProgress_->setTextVisible(true);

    int row = 0;
    detailsLayout->addWidget(new QLabel("<b>Contract ID:</b>"), row, 0);
    detailsLayout->addWidget(contractIdLabel_, row++, 1, 1, 2);

    detailsLayout->addWidget(new QLabel("<b>P2SH Address:</b>"), row, 0);
    detailsLayout->addWidget(p2shAddressLabel_, row, 1);
    detailsLayout->addWidget(copyAddressButton_, row++, 2);

    // QR Code button row
    detailsLayout->addWidget(showQRButton_, row++, 2);

    detailsLayout->addWidget(new QLabel("<b>Amount:</b>"), row, 0);
    detailsLayout->addWidget(amountLabel_, row++, 1, 1, 2);

    detailsLayout->addWidget(new QLabel("<b>Status:</b>"), row, 0);
    detailsLayout->addWidget(statusLabel_, row++, 1, 1, 2);

    detailsLayout->addWidget(new QLabel("<b>Buyer:</b>"), row, 0);
    detailsLayout->addWidget(buyerLabel_, row++, 1, 1, 2);

    detailsLayout->addWidget(new QLabel("<b>Seller:</b>"), row, 0);
    detailsLayout->addWidget(sellerLabel_, row++, 1, 1, 2);

    detailsLayout->addWidget(new QLabel("<b>Mediator:</b>"), row, 0);
    detailsLayout->addWidget(mediatorLabel_, row++, 1, 1, 2);

    detailsLayout->addWidget(new QLabel("<b>Lock TXID:</b>"), row, 0);
    detailsLayout->addWidget(lockTxidLabel_, row++, 1, 1, 2);

    detailsLayout->addWidget(new QLabel("<b>Refund Height:</b>"), row, 0);
    detailsLayout->addWidget(refundHeightLabel_, row++, 1, 1, 2);

    detailsLayout->addWidget(new QLabel("<b>Current Height:</b>"), row, 0);
    detailsLayout->addWidget(currentHeightLabel_, row++, 1, 1, 2);

    detailsLayout->addWidget(new QLabel("<b>Time Remaining:</b>"), row, 0);
    detailsLayout->addWidget(timeRemainingLabel_, row++, 1, 1, 2);

    detailsLayout->addWidget(new QLabel("<b>Timelock Progress:</b>"), row, 0);
    detailsLayout->addWidget(timelockProgress_, row++, 1, 1, 2);

    detailsLayout->addWidget(viewScriptButton_, row, 1);
    detailsLayout->addWidget(exportSighashButton_, row++, 2);

    detailsGroup_->setLayout(detailsLayout);
    mainLayout->addWidget(detailsGroup_);

    // ========== ACTION BUTTONS ==========
    auto* actionLayout = new QHBoxLayout();

    releaseButton_ = new QPushButton("✅ Release Funds to Seller");
    releaseButton_->setStyleSheet("background-color: #4CAF50; color: white; font-weight: bold; padding: 10px;");
    releaseButton_->setEnabled(false);

    refundButton_ = new QPushButton("↩️ Refund to Buyer");
    refundButton_->setStyleSheet("background-color: #FF9800; color: white; font-weight: bold; padding: 10px;");
    refundButton_->setEnabled(false);

    viewDetailsButton_ = new QPushButton("🔍 View Full Details");
    viewDetailsButton_->setEnabled(false);

    importSigsButton_ = new QPushButton("📥 Import Signatures");
    importSigsButton_->setStyleSheet("background-color: #2196F3; color: white; font-weight: bold; padding: 10px;");
    importSigsButton_->setEnabled(false);

    actionLayout->addWidget(releaseButton_);
    actionLayout->addWidget(refundButton_);
    actionLayout->addWidget(importSigsButton_);
    actionLayout->addWidget(viewDetailsButton_);

    mainLayout->addLayout(actionLayout);

    // ========== EVENT LOG ==========
    auto* logLabel = new QLabel("<b>Event Log:</b>");
    eventLog_ = new QTextEdit();
    eventLog_->setReadOnly(true);
    eventLog_->setMaximumHeight(150);
    eventLog_->setStyleSheet("background-color: #f5f5f5; font-family: 'Courier New', monospace; font-size: 11px;");

    mainLayout->addWidget(logLabel);
    mainLayout->addWidget(eventLog_);

    appendLog("Escrow widget initialized. Ready to create contracts.");
}

void EscrowWidget::setupConnections()
{
    // Button connections
    connect(createButton_, &QPushButton::clicked, this, &EscrowWidget::onCreateEscrow);
    connect(refreshButton_, &QPushButton::clicked, this, &EscrowWidget::onRefreshContracts);
    connect(exportButton_, &QPushButton::clicked, this, &EscrowWidget::onExportCsv);
    connect(releaseButton_, &QPushButton::clicked, this, &EscrowWidget::onReleaseSelected);
    connect(refundButton_, &QPushButton::clicked, this, &EscrowWidget::onRefundSelected);
    connect(viewDetailsButton_, &QPushButton::clicked, this, &EscrowWidget::onViewDetails);
    connect(copyAddressButton_, &QPushButton::clicked, this, &EscrowWidget::onCopyAddress);
    connect(showQRButton_, &QPushButton::clicked, this, &EscrowWidget::onShowQRCode);
    connect(viewScriptButton_, &QPushButton::clicked, this, &EscrowWidget::onViewScript);
    connect(exportSighashButton_, &QPushButton::clicked, this, &EscrowWidget::onExportSighash);
    connect(importSigsButton_, &QPushButton::clicked, this, &EscrowWidget::onImportSignatures);

    // Table selection
    connect(contractsTable_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &EscrowWidget::onContractSelected);

    // Filter
    connect(filterCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EscrowWidget::onRefreshContracts);

    // Auto-refresh timer
    connect(&refreshTimer_, &QTimer::timeout, this, &EscrowWidget::onRefreshContracts);

    // RPC callbacks
    connect(rpc_, &RpcClient::rpcResult, this, &EscrowWidget::onRpcResult);
    connect(rpc_, &RpcClient::rpcError, this, &EscrowWidget::onRpcError);

    // WebSocket events
    if (ws_) {
        connect(ws_, &WebSocketClient::subscriptionEvent, this, &EscrowWidget::onWebSocketEvent);
        ws_->subscribe("contract_update");
        ws_->subscribe("new_block");
    }
}

void EscrowWidget::onRefreshContracts()
{
    callRpc("contract.list", QJsonArray{QJsonObject{}});
    callRpc("blockchain.getblockcount", QJsonArray{});
}

void EscrowWidget::onCreateEscrow()
{
    showCreateDialog();
}

void EscrowWidget::onReleaseSelected()
{
    if (selectedContractId_.isEmpty()) {
        QMessageBox::warning(this, "No Selection", "Please select a contract first.");
        return;
    }

    auto reply = QMessageBox::question(
        this,
        "Release Funds",
        QString("Release funds to seller for contract %1?\n\n"
                "This requires both buyer and seller signatures.\n"
                "Have you verified the transaction details?")
            .arg(selectedContractId_),
        QMessageBox::Yes | QMessageBox::No
    );

    if (reply == QMessageBox::Yes) {
        QJsonObject params{{"contract_id", selectedContractId_}, {"tx_type", "release"}};
        callRpc("contract.getsighash", QJsonArray{params});
        appendLog(QString("Requesting release signing package for contract %1. "
                          "No transaction will be broadcast without real imported signatures.")
                      .arg(selectedContractId_));
    }
}

void EscrowWidget::onRefundSelected()
{
    if (selectedContractId_.isEmpty()) {
        QMessageBox::warning(this, "No Selection", "Please select a contract first.");
        return;
    }

    int refundHeight = refundHeightLabel_->text().toInt();
    if (currentBlockHeight_ < refundHeight) {
        QMessageBox::warning(
            this,
            "Timelock Not Expired",
            QString("Cannot refund yet. Current block: %1, Refund at: %2\n"
                    "Blocks remaining: %3")
                .arg(currentBlockHeight_)
                .arg(refundHeight)
                .arg(refundHeight - currentBlockHeight_)
        );
        return;
    }

    auto reply = QMessageBox::question(
        this,
        "Refund to Buyer",
        QString("Refund escrow to buyer for contract %1?\n\n"
                "Timelock has expired. This will return funds to the buyer.")
            .arg(selectedContractId_),
        QMessageBox::Yes | QMessageBox::No
    );

    if (reply == QMessageBox::Yes) {
        QJsonObject params{{"contract_id", selectedContractId_}, {"tx_type", "refund"}};
        callRpc("contract.getsighash", QJsonArray{params});
        appendLog(QString("Requesting refund signing package for contract %1. "
                          "No transaction will be broadcast without a real imported signature.")
                      .arg(selectedContractId_));
    }
}

void EscrowWidget::onViewDetails()
{
    if (selectedContractId_.isEmpty()) return;

    if (contractsCache_.contains(selectedContractId_)) {
        showDetailsDialog(contractsCache_[selectedContractId_]);
    }
}

void EscrowWidget::onViewScript()
{
    if (selectedContractId_.isEmpty()) return;

    if (contractsCache_.contains(selectedContractId_)) {
        QString script = contractsCache_[selectedContractId_]["redeem_script"].toString();
        showScriptDialog(script);
    }
}

void EscrowWidget::onCopyAddress()
{
    QString address = p2shAddressLabel_->text();
    if (address != "-") {
        QApplication::clipboard()->setText(address);
        appendLog(QString("Copied P2SH address: %1").arg(address));
        statusLabel_->setText("✅ Copied!");
        QTimer::singleShot(2000, [this]() {
            statusLabel_->setText(formatStatus(statusLabel_->text()));
        });
    }
}

void EscrowWidget::onExportCsv()
{
    QString filename = QFileDialog::getSaveFileName(
        this,
        "Export Contracts to CSV",
        QDir::homePath() + "/escrow_contracts.csv",
        "CSV Files (*.csv)"
    );

    if (filename.isEmpty()) return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "Export Failed", "Could not open file for writing.");
        return;
    }

    QTextStream out(&file);

    // Header
    out << "Contract ID,P2SH Address,Amount,Status,Buyer,Seller,Refund Block,Created\n";

    // Data
    for (int row = 0; row < tableModel_->rowCount(); ++row) {
        for (int col = 0; col < COL_COUNT; ++col) {
            out << tableModel_->item(row, col)->text();
            if (col < COL_COUNT - 1) out << ",";
        }
        out << "\n";
    }

    file.close();
    appendLog(QString("Exported %1 contracts to %2").arg(tableModel_->rowCount()).arg(filename));
    QMessageBox::information(this, "Export Complete", QString("Exported %1 contracts successfully.").arg(tableModel_->rowCount()));
}

void EscrowWidget::onExportSighash()
{
    QMessageBox::warning(
        this, "Contract Signing Unavailable",
        "Contract fund movement is disabled in v8.1.9. The daemon does not yet "
        "produce a canonical signing package bound to the funding outpoint, action, "
        "destination, amount, chain, and expiry.");
    return;

    if (selectedContractId_.isEmpty()) {
        QMessageBox::warning(this, "No Selection", "Please select a contract first.");
        return;
    }

    if (!contractsCache_.contains(selectedContractId_)) {
        QMessageBox::warning(this, "Contract Not Found", "Contract details not loaded.");
        return;
    }

    QJsonObject contract = contractsCache_[selectedContractId_];
    QString status = contract["status"].toString();

    if (status != "locked") {
        QMessageBox::warning(this, "Invalid Status",
            "Contract must be locked to export sighash.\nCurrent status: " + status);
        return;
    }

    // Ask user: release or refund?
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Export Sighash");
    msgBox.setText("Export sighash for which action?");
    msgBox.setInformativeText("Release: Send funds to seller (requires 2 signatures)\n"
                              "Refund: Return funds to buyer (requires timelock expiry)");
    QPushButton* releaseBtn = msgBox.addButton("Release", QMessageBox::ActionRole);
    QPushButton* refundBtn = msgBox.addButton("Refund", QMessageBox::ActionRole);
    msgBox.addButton(QMessageBox::Cancel);
    msgBox.exec();

    bool isRefund = false;
    if (msgBox.clickedButton() == releaseBtn) {
        isRefund = false;
    } else if (msgBox.clickedButton() == refundBtn) {
        isRefund = true;

        // Check timelock for refund
        int refundHeight = contract["refund_time"].toInt();
        if (currentBlockHeight_ < refundHeight) {
            QMessageBox::warning(this, "Timelock Not Expired",
                QString("Cannot refund yet. Timelock expires at block %1 (current: %2)")
                    .arg(refundHeight).arg(currentBlockHeight_));
            return;
        }
    } else {
        return; // Cancelled
    }

    exportForRelease_ = !isRefund;

    // Get destination address
    QString defaultAddr = isRefund ?
        contract["keys"].toObject()["buyer"].toString() :
        contract["keys"].toObject()["seller"].toString();

    bool ok;
    QString toAddress = QInputDialog::getText(this,
        isRefund ? "Refund Address" : "Release Address",
        QString("Enter destination address (din1q...):\n\n"
                "Default: First 20 chars of %1 pubkey...")
            .arg(isRefund ? "buyer" : "seller"),
        QLineEdit::Normal, "", &ok);

    if (!ok || toAddress.isEmpty()) {
        // Ask if they want to use a wallet address
        toAddress = QInputDialog::getText(this,
            "Destination Address",
            "Enter a wallet address to receive the funds:",
            QLineEdit::Normal, "", &ok);

        if (!ok || toAddress.isEmpty()) {
            appendLog("Sighash export cancelled - no address provided");
            return;
        }
    }

    // Choose filename
    QString defaultFilename = QString("%1/%2_%3_sighash.json")
        .arg(QDir::homePath())
        .arg(selectedContractId_)
        .arg(isRefund ? "refund" : "release");

    QString filename = QFileDialog::getSaveFileName(
        this,
        "Export Sighash for Offline Signing",
        defaultFilename,
        "JSON Files (*.json)"
    );

    if (filename.isEmpty()) {
        appendLog("Sighash export cancelled");
        return;
    }

    // Store for later use in onRpcResult
    pendingExportFile_ = filename;

    // Call getsighash RPC
    QJsonObject params{
        {"contract_id", selectedContractId_},
        {"to_address", toAddress},
        {"is_refund", isRefund}
    };

    callRpc("contract.getsighash", QJsonArray{params});
    appendLog(QString("Requesting sighash for %1 to %2...")
        .arg(isRefund ? "refund" : "release")
        .arg(toAddress.left(20) + "..."));
}

void EscrowWidget::onImportSignatures()
{
    QMessageBox::warning(
        this, "Contract Broadcast Unavailable",
        "Imported contract signatures cannot be broadcast in v8.1.9 because the "
        "daemon cannot validate a bound signing package. No RPC was called.");
    return;

    if (selectedContractId_.isEmpty()) {
        QMessageBox::warning(this, "No Selection", "Please select a contract first.");
        return;
    }

    QString filename = QFileDialog::getOpenFileName(
        this,
        "Import Signed Transaction",
        QDir::homePath(),
        "JSON Files (*.json)"
    );

    if (filename.isEmpty()) return;

    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, "Import Failed", "Could not read file.");
        return;
    }

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    file.close();

    if (error.error != QJsonParseError::NoError) {
        QMessageBox::critical(this, "Parse Error",
            QString("Invalid JSON: %1").arg(error.errorString()));
        return;
    }

    QJsonObject sigData = doc.object();

    // Validate required fields
    QStringList requiredFields = {"contract_id", "sighash", "is_refund"};
    for (const QString& field : requiredFields) {
        if (!sigData.contains(field)) {
            QMessageBox::critical(this, "Invalid File",
                QString("Missing required field: %1").arg(field));
            return;
        }
    }

    // Check contract ID matches
    if (sigData["contract_id"].toString() != selectedContractId_) {
        auto reply = QMessageBox::question(this, "Contract ID Mismatch",
            QString("File is for contract: %1\nCurrently selected: %2\n\nContinue anyway?")
                .arg(sigData["contract_id"].toString())
                .arg(selectedContractId_),
            QMessageBox::Yes | QMessageBox::No);

        if (reply == QMessageBox::No) return;
    }

    // Check for signatures
    bool isRefund = sigData["is_refund"].toBool();
    bool hasBuyerSig = sigData.contains("sig_buyer") && !sigData["sig_buyer"].toString().isEmpty();
    bool hasSellerSig = sigData.contains("sig_seller") && !sigData["sig_seller"].toString().isEmpty();

    if (!hasBuyerSig && !hasSellerSig) {
        QMessageBox::critical(this, "No Signatures",
            "No signatures found in file. Please sign the sighash first.");
        return;
    }

    // Build confirmation message
    QString confirmMsg = QString("Import and broadcast %1 transaction?\n\n")
        .arg(isRefund ? "refund" : "release");
    confirmMsg += QString("Contract: %1\n").arg(sigData["contract_id"].toString());
    confirmMsg += QString("To Address: %1\n").arg(sigData["to_address"].toString());
    confirmMsg += QString("Amount: %1 DIN\n\n").arg(sigData["amount"].toDouble(), 0, 'f', 8);
    confirmMsg += "Signatures:\n";
    if (hasBuyerSig) confirmMsg += QString("  ✓ Buyer: %1...\n").arg(sigData["sig_buyer"].toString().left(16));
    if (hasSellerSig) confirmMsg += QString("  ✓ Seller: %1...\n").arg(sigData["sig_seller"].toString().left(16));

    auto reply = QMessageBox::question(this, "Confirm Broadcast",
        confirmMsg, QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::No) {
        appendLog("Import cancelled by user");
        return;
    }

    QMessageBox::critical(this, "Contract Broadcast Disabled",
                          "The signature package was parsed but not submitted. "
                          "Bound contract signing is not available in v8.1.9.");
}

void EscrowWidget::onContractSelected(const QModelIndex& index)
{
    if (!index.isValid()) {
        releaseButton_->setEnabled(false);
        refundButton_->setEnabled(false);
        viewDetailsButton_->setEnabled(false);
        return;
    }

    QString contractId = tableModel_->item(index.row(), COL_ID)->text();
    selectedContractId_ = contractId;

    if (!contractsCache_.contains(contractId)) {
        // Fetch full contract details
        callRpc("contract.status", QJsonArray{QJsonObject{{"contract_id", contractId}}});
        return;
    }

    QJsonObject contract = contractsCache_[contractId];

    // Update details panel
    contractIdLabel_->setText(contractId);
    p2shAddressLabel_->setText(contract["p2sh_address"].toString());

    // Show amount with multi-asset info if applicable
    QString amountText = QString("%1 DIN").arg(contract["amount"].toDouble(), 0, 'f', 8);
    if (contract.contains("output_currency")) {
        QString outputCurrency = contract["output_currency"].toString();
        if (!outputCurrency.isEmpty() && outputCurrency != "DIN") {
            if (contract.contains("exchange_rate")) {
                double rate = contract["exchange_rate"].toDouble();
                double releaseAmount = contract["amount"].toDouble() * rate;
                amountText += QString(" → %1 %2").arg(releaseAmount, 0, 'f', 2).arg(outputCurrency);

                // Show swap status with emoji indicator
                if (contract.contains("swap_status")) {
                    QString swapStatus = contract["swap_status"].toString();
                    if (swapStatus == "completed") {
                        amountText += " ✅";
                    } else if (swapStatus == "pending") {
                        amountText += " ⏳";
                    } else if (swapStatus == "failed") {
                        amountText += " ❌";
                    }
                }
            } else {
                amountText += QString(" → %1").arg(outputCurrency);
            }
        }
    }
    amountLabel_->setText(amountText);

    statusLabel_->setText(formatStatus(contract["status"].toString()));
    buyerLabel_->setText(contract["keys"].toObject()["buyer"].toString().left(20) + "...");
    sellerLabel_->setText(contract["keys"].toObject()["seller"].toString().left(20) + "...");
    mediatorLabel_->setText(contract["keys"].toObject()["mediator"].toString().left(20) + "...");
    lockTxidLabel_->setText(contract["lock_txid"].toString().isEmpty() ? "(not funded)" : contract["lock_txid"].toString().left(20) + "...");

    int refundHeight = contract["refund_time"].toInt();
    refundHeightLabel_->setText(QString::number(refundHeight));
    currentHeightLabel_->setText(QString::number(currentBlockHeight_));

    QString timeRemaining = formatTimelock(refundHeight, currentBlockHeight_);
    timeRemainingLabel_->setText(timeRemaining);

    // Timelock progress
    if (refundHeight > 0 && currentBlockHeight_ > 0) {
        int blocksRemaining = refundHeight - currentBlockHeight_;
        int totalBlocks = contract["refund_blocks"].toInt(100);
        int progress = std::max(0, std::min(100, 100 - (blocksRemaining * 100 / totalBlocks)));
        timelockProgress_->setValue(progress);
        timelockProgress_->setFormat(QString("%1 blocks remaining").arg(std::max(0, blocksRemaining)));
    }

    // Enable/disable buttons based on status
    QString status = contract["status"].toString();
    releaseButton_->setEnabled(status == "locked");
    refundButton_->setEnabled(status == "locked" && currentBlockHeight_ >= refundHeight);
    viewDetailsButton_->setEnabled(true);
    exportSighashButton_->setEnabled(status == "locked");
    importSigsButton_->setEnabled(status == "locked");

    // Enable QR button if we have a P2SH address
    bool hasP2SH = !contract["p2sh_address"].toString().isEmpty();
    showQRButton_->setEnabled(hasP2SH);
}

void EscrowWidget::onRpcResult(const QString& method, const QJsonValue& result)
{
    if (method == "contract.list") {
        QJsonObject obj = result.toObject();
        if (obj.contains("contracts")) {
            QJsonArray contracts = obj["contracts"].toArray();
            populateTable(contracts);
            statsLabel_->setText(QString("%1 contract(s)").arg(contracts.size()));
        }
    }
    else if (method == "blockchain.getblockcount") {
        currentBlockHeight_ = result.toInt();
        currentHeightLabel_->setText(QString::number(currentBlockHeight_));
    }
    else if (method == "contract.createescrow") {
        QJsonObject obj = result.toObject();
        QString contractId = obj["contract_id"].toString();
        QString p2sh = obj["p2sh_address"].toString();

        appendLog(QString("✅ Contract created: %1").arg(contractId));
        appendLog(QString("   P2SH: %1").arg(p2sh));
        appendLog(QString("   Send funds to this address to activate."));

        QMessageBox::information(
            this,
            "Contract Created",
            QString("Escrow contract created successfully!\n\n"
                    "Contract ID: %1\n"
                    "P2SH Address: %2\n\n"
                    "Send funds to the P2SH address to activate the escrow.")
                .arg(contractId)
                .arg(p2sh)
        );

        onRefreshContracts();
    }
    else if (method == "contract.status") {
        QJsonObject contract = result.toObject();
        QString contractId = contract["contract_id"].toString();
        contractsCache_[contractId] = contract;

        // If this is the selected contract, update display
        if (contractId == selectedContractId_) {
            onContractSelected(contractsTable_->currentIndex());
        }
    }
    else if (method == "contract.getsighash") {
        QJsonObject obj = result.toObject();

        // Handle sighash export if requested
        if (!pendingExportFile_.isEmpty()) {
            QJsonObject exportData{
                {"contract_id", selectedContractId_},
                {"sighash", obj["sighash"].toString()},
                {"redeem_script", obj["redeem_script"].toString()},
                {"amount", obj["amount"].toDouble()},
                {"to_address", obj["to_address"].toString()},
                {"is_refund", !exportForRelease_},
                {"instructions", "Sign the sighash with your private key and add the signature(s) to this file"},
                {"sig_buyer", ""},
                {"sig_seller", ""}
            };

            QFile file(pendingExportFile_);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(QJsonDocument(exportData).toJson(QJsonDocument::Indented));
                file.close();

                appendLog(QString("✅ Sighash exported to: %1").arg(pendingExportFile_));

                // Ask if user wants to also display as QR code
                QMessageBox msgBox(this);
                msgBox.setWindowTitle("Export Successful");
                msgBox.setText("Sighash exported successfully!");
                msgBox.setInformativeText(QString("File: %1\n\n"
                                                  "Would you like to also display the sighash data as a QR code?\n"
                                                  "This allows scanning with mobile signing devices.")
                                          .arg(QFileInfo(pendingExportFile_).fileName()));
                QPushButton* showQRBtn = msgBox.addButton("📱 Show QR Code", QMessageBox::ActionRole);
                QPushButton* doneBtn = msgBox.addButton("Done", QMessageBox::AcceptRole);
                msgBox.exec();

                if (msgBox.clickedButton() == showQRBtn) {
                    // Display sighash data as QR code
                    QString jsonCompact = QJsonDocument(exportData).toJson(QJsonDocument::Compact);
                    showSighashQRDialog(jsonCompact, exportData);
                }
            } else {
                appendLog(QString("❌ Failed to write file: %1").arg(pendingExportFile_));
                QMessageBox::critical(this, "Export Failed", "Could not write to file.");
            }

            pendingExportFile_.clear();
        }
    }
    else if (method == "contract.broadcastrelease" || method == "contract.broadcastrefund") {
        QJsonObject obj = result.toObject();
        QString txHex = obj["tx_hex"].toString();

        if (!txHex.isEmpty()) {
            appendLog(QString("✅ Transaction built successfully (length: %1 bytes)").arg(txHex.length() / 2));
            appendLog("   Note: Transaction requires real signatures to broadcast.");
        }

        // Check for swap errors in multi-asset escrow
        if (obj.contains("swap_error") && !obj["swap_error"].toString().isEmpty()) {
            QString error = obj["swap_error"].toString();
            appendLog(QString("⚠️ Swap Error: %1").arg(error));
            QMessageBox::warning(this, "Swap Failed",
                QString("Contract released successfully, but currency conversion failed:\n\n"
                        "%1\n\n"
                        "The funds remain in DIN. The seller can manually convert via the Bridge.")
                    .arg(error));
        }

        onRefreshContracts();
    }
}

void EscrowWidget::onRpcError(const QString& method, int code, const QString& message)
{
    appendLog(QString("❌ RPC Error [%1]: %2 - %3").arg(method).arg(code).arg(message));

    // Don't show popup dialogs - user gets error in MainWindow status bar
    // Error is already logged above for debugging
}

void EscrowWidget::onWebSocketEvent(const QString& topic, const QJsonObject& data)
{
    if (topic == "contract_update") {
        QString contractId = data["contract_id"].toString();
        QString status = data["status"].toString();
        onContractStatusUpdate(contractId, status);
    }
    else if (topic == "new_block") {
        int height = data["height"].toInt();
        onNewBlock(height);
    }
}

void EscrowWidget::onContractStatusUpdate(const QString& contractId, const QString& status)
{
    appendLog(QString("🔔 Contract %1 status changed: %2").arg(contractId.left(20)).arg(status));

    // Update cache
    if (contractsCache_.contains(contractId)) {
        contractsCache_[contractId]["status"] = status;
    }

    // Refresh display
    onRefreshContracts();
}

void EscrowWidget::onNewBlock(int height)
{
    currentBlockHeight_ = height;
    currentHeightLabel_->setText(QString::number(height));

    // Update timelock progress if a contract is selected
    if (!selectedContractId_.isEmpty()) {
        onContractSelected(contractsTable_->currentIndex());
    }
}

void EscrowWidget::populateTable(const QJsonArray& contracts)
{
    tableModel_->removeRows(0, tableModel_->rowCount());

    StatusFilter filter = filterCombo_->currentData().value<StatusFilter>();

    // Calculate statistics
    int pendingCount = 0, lockedCount = 0, releasedCount = 0, refundedCount = 0;
    double totalVolume = 0.0, activeVolume = 0.0;

    for (const auto& v : contracts) {
        QJsonObject c = v.toObject();
        QString status = c["status"].toString();
        double amount = c["amount"].toDouble();

        // Update statistics (before filtering)
        totalVolume += amount;
        if (status == "pending") {
            pendingCount++;
        } else if (status == "locked") {
            lockedCount++;
            activeVolume += amount;
        } else if (status == "released") {
            releasedCount++;
        } else if (status == "refunded") {
            refundedCount++;
        }

        // Apply filter
        if (filter != StatusFilter::All) {
            bool show = false;
            switch (filter) {
                case StatusFilter::Pending: show = (status == "pending"); break;
                case StatusFilter::Locked: show = (status == "locked"); break;
                case StatusFilter::Released: show = (status == "released"); break;
                case StatusFilter::Refunded: show = (status == "refunded"); break;
                case StatusFilter::Expired: show = (status == "expired"); break;
                default: break;
            }
            if (!show) continue;
        }

        // Cache contract data
        QString contractId = c["contract_id"].toString();
        contractsCache_[contractId] = c;

        QList<QStandardItem*> row;
        row << new QStandardItem(contractId);
        row << new QStandardItem(c["p2sh_address"].toString());
        row << new QStandardItem(QString::number(c["amount"].toDouble(), 'f', 8));

        auto* statusItem = new QStandardItem(formatStatus(status));
        statusItem->setBackground(QBrush(statusColor(status)));
        row << statusItem;

        row << new QStandardItem(c["keys"].toObject()["buyer"].toString().left(20) + "...");
        row << new QStandardItem(c["keys"].toObject()["seller"].toString().left(20) + "...");
        row << new QStandardItem(QString::number(c["refund_time"].toInt()));
        row << new QStandardItem(QDateTime::fromSecsSinceEpoch(c["created_at"].toInteger()).toString("yyyy-MM-dd hh:mm"));

        tableModel_->appendRow(row);
    }

    // Update enhanced statistics label
    QString statsText = QString("%1 total | %2 DIN volume | ")
        .arg(contracts.size())
        .arg(totalVolume, 0, 'f', 2);

    if (lockedCount > 0) {
        statsText += QString("🔒 %1 locked (%2 DIN) | ")
            .arg(lockedCount)
            .arg(activeVolume, 0, 'f', 2);
    }

    statsText += QString("⏳ %1 pending | ✅ %2 released | ↩️ %3 refunded")
        .arg(pendingCount)
        .arg(releasedCount)
        .arg(refundedCount);

    statsLabel_->setText(statsText);
}

void EscrowWidget::appendLog(const QString& message)
{
    QString timestamp = QDateTime::currentDateTime().toString("[hh:mm:ss] ");
    eventLog_->append(timestamp + message);
}

QString EscrowWidget::formatStatus(const QString& status)
{
    if (status == "pending") return "⏳ Pending";
    if (status == "locked") return "🔒 Locked";
    if (status == "released") return "✅ Released";
    if (status == "refunded") return "↩️ Refunded";
    if (status == "expired") return "⏰ Expired";
    return status;
}

QColor EscrowWidget::statusColor(const QString& status)
{
    if (status == "pending") return QColor(255, 248, 220);    // Light yellow
    if (status == "locked") return QColor(173, 216, 230);     // Light blue
    if (status == "released") return QColor(144, 238, 144);   // Light green
    if (status == "refunded") return QColor(255, 182, 193);   // Light pink
    if (status == "expired") return QColor(255, 160, 122);    // Light coral
    return QColor(Qt::white);
}

QString EscrowWidget::formatTimelock(int blockHeight, int currentHeight)
{
    if (blockHeight == 0 || currentHeight == 0) {
        return "Unknown";
    }

    int blocksRemaining = blockHeight - currentHeight;
    if (blocksRemaining <= 0) {
        return "⏰ Expired (refund available)";
    }

    // Estimate time (assuming ~10 min blocks)
    int minutesRemaining = blocksRemaining * 10;
    int hoursRemaining = minutesRemaining / 60;
    int daysRemaining = hoursRemaining / 24;

    if (daysRemaining > 0) {
        return QString("%1 days (%2 blocks)").arg(daysRemaining).arg(blocksRemaining);
    } else if (hoursRemaining > 0) {
        return QString("%1 hours (%2 blocks)").arg(hoursRemaining).arg(blocksRemaining);
    } else {
        return QString("%1 minutes (%2 blocks)").arg(minutesRemaining).arg(blocksRemaining);
    }
}

void EscrowWidget::showCreateDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Create New Escrow Contract");
    dialog.setMinimumWidth(500);

    auto* layout = new QFormLayout(&dialog);

    auto* buyerEdit = new QLineEdit();
    buyerEdit->setPlaceholderText("027... (66 hex characters)");

    auto* sellerEdit = new QLineEdit();
    sellerEdit->setPlaceholderText("02c... (66 hex characters)");

    auto* mediatorEdit = new QLineEdit();
    mediatorEdit->setPlaceholderText("02f... (66 hex characters)");

    auto* amountSpin = new QDoubleSpinBox();
    amountSpin->setRange(0.00000001, 1000000000.0);
    amountSpin->setDecimals(8);
    amountSpin->setValue(10.0);
    amountSpin->setSuffix(" DIN");

    // Output currency dropdown for multi-asset escrow
    auto* outputCurrencyCombo = new QComboBox();
    outputCurrencyCombo->addItem("🪙 DIN (same currency)", "DIN");
    outputCurrencyCombo->addItem("💵 USDT (via Bridge)", "USDT");
    outputCurrencyCombo->addItem("₿ BTC (via Bridge)", "BTC");
    outputCurrencyCombo->setToolTip("Select which currency to release to seller.\n"
                                   "Bridge conversion uses locked exchange rate.");

    auto* refundBlocksSpin = new QSpinBox();
    refundBlocksSpin->setRange(10, 100000);
    refundBlocksSpin->setValue(2880);  // ~20 days
    refundBlocksSpin->setSuffix(" blocks (~20 days)");

    layout->addRow("Buyer Public Key:", buyerEdit);
    layout->addRow("Seller Public Key:", sellerEdit);
    layout->addRow("Mediator Public Key:", mediatorEdit);
    layout->addRow("Amount:", amountSpin);
    layout->addRow("Output Currency:", outputCurrencyCombo);
    layout->addRow("Refund Timelock:", refundBlocksSpin);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttonBox);

    if (dialog.exec() == QDialog::Accepted) {
        QString buyer = buyerEdit->text().trimmed();
        QString seller = sellerEdit->text().trimmed();
        QString mediator = mediatorEdit->text().trimmed();
        QString outputCurrency = outputCurrencyCombo->currentData().toString();

        // Validation
        if (buyer.isEmpty() || seller.isEmpty() || mediator.isEmpty()) {
            QMessageBox::warning(this, "Validation Error", "All public keys are required.");
            return;
        }

        if (buyer.length() != 66 || seller.length() != 66 || mediator.length() != 66) {
            QMessageBox::warning(this, "Validation Error", "Public keys must be 66 hex characters (compressed).");
            return;
        }

        QJsonObject params{
            {"buyer_pubkey", buyer},
            {"seller_pubkey", seller},
            {"mediator_pubkey", mediator},
            {"amount", amountSpin->value()},
            {"refund_blocks", refundBlocksSpin->value()},
            {"output_currency", outputCurrency}
        };

        callRpc("contract.createescrow", QJsonArray{params});

        QString logMsg = QString("Creating escrow: %1 DIN, refund in %2 blocks")
            .arg(amountSpin->value(), 0, 'f', 8)
            .arg(refundBlocksSpin->value());

        if (outputCurrency != "DIN") {
            logMsg += QString(", output: %1").arg(outputCurrency);
        }

        appendLog(logMsg);
    }
}

void EscrowWidget::showDetailsDialog(const QJsonObject& contract)
{
    QDialog dialog(this);
    dialog.setWindowTitle("Contract Details");
    dialog.setMinimumWidth(600);

    auto* layout = new QVBoxLayout(&dialog);
    auto* textEdit = new QTextEdit();
    textEdit->setReadOnly(true);

    QString details;
    details += QString("<h3>Contract: %1</h3>").arg(contract["contract_id"].toString());
    details += QString("<b>P2SH Address:</b> %1<br>").arg(contract["p2sh_address"].toString());
    details += QString("<b>Lock Amount:</b> %1 DIN<br>").arg(contract["amount"].toDouble(), 0, 'f', 8);

    // Show multi-asset information if present
    if (contract.contains("output_currency")) {
        QString outputCurrency = contract["output_currency"].toString();
        if (!outputCurrency.isEmpty() && outputCurrency != "DIN") {
            details += QString("<b>Output Currency:</b> %1<br>").arg(outputCurrency);

            if (contract.contains("exchange_rate")) {
                double rate = contract["exchange_rate"].toDouble();
                double expectedAmount = contract["amount"].toDouble() * rate;
                details += QString("<b>Exchange Rate:</b> 1 DIN = %1 %2 (locked at creation)<br>")
                    .arg(rate, 0, 'f', 4)
                    .arg(outputCurrency);
                details += QString("<b>Expected Amount:</b> ~%1 %2<br>")
                    .arg(expectedAmount, 0, 'f', 2)
                    .arg(outputCurrency);

                // Show actual amount received if swap completed
                if (contract.contains("actual_amount_received")) {
                    double actualAmount = contract["actual_amount_received"].toDouble();
                    details += QString("<b>Actual Amount:</b> %1 %2<br>")
                        .arg(actualAmount, 0, 'f', 2)
                        .arg(outputCurrency);

                    // Calculate and show bridge fee if applicable
                    if (actualAmount < expectedAmount) {
                        double fee = expectedAmount - actualAmount;
                        details += QString("<small style='color: #666;'>(Bridge fee: %1 %2)</small><br>")
                            .arg(fee, 0, 'f', 2)
                            .arg(outputCurrency);
                    }
                }

                // Show swap status
                if (contract.contains("swap_status")) {
                    QString swapStatus = contract["swap_status"].toString();
                    QString statusText;
                    if (swapStatus == "completed") {
                        statusText = "✅ Completed";
                    } else if (swapStatus == "pending") {
                        statusText = "⏳ Pending";
                    } else if (swapStatus == "failed") {
                        statusText = "❌ Failed";
                        if (contract.contains("swap_error")) {
                            statusText += QString(" (%1)").arg(contract["swap_error"].toString());
                        }
                    } else {
                        statusText = swapStatus;
                    }
                    details += QString("<b>Swap Status:</b> %1<br>").arg(statusText);
                }

                // Show swap transaction ID if available
                if (contract.contains("swap_txid") && !contract["swap_txid"].toString().isEmpty()) {
                    QString swapTxid = contract["swap_txid"].toString();
                    details += QString("<b>Swap TXID:</b> %1...<br>")
                        .arg(swapTxid.left(20));
                }
            }
        }
    }

    details += QString("<b>Status:</b> %1<br>").arg(contract["status"].toString());
    details += QString("<b>Buyer Pubkey:</b> %1<br>").arg(contract["keys"].toObject()["buyer"].toString());
    details += QString("<b>Seller Pubkey:</b> %1<br>").arg(contract["keys"].toObject()["seller"].toString());
    details += QString("<b>Mediator Pubkey:</b> %1<br>").arg(contract["keys"].toObject()["mediator"].toString());
    details += QString("<b>Lock TXID:</b> %1<br>").arg(contract["lock_txid"].toString());
    details += QString("<b>Refund Height:</b> %1<br>").arg(contract["refund_time"].toInt());
    details += QString("<b>Script Hash:</b> %1<br>").arg(contract["script_hash"].toString());
    details += QString("<b>Redeem Script:</b> %1<br>").arg(contract["redeem_script"].toString());

    textEdit->setHtml(details);
    layout->addWidget(textEdit);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(buttonBox);

    dialog.exec();
}

void EscrowWidget::showScriptDialog(const QString& redeemScript)
{
    QDialog dialog(this);
    dialog.setWindowTitle("Redeem Script");
    dialog.setMinimumWidth(700);

    auto* layout = new QVBoxLayout(&dialog);

    layout->addWidget(new QLabel("<b>Redeem Script (Hex):</b>"));

    auto* hexEdit = new QTextEdit();
    hexEdit->setPlainText(redeemScript);
    hexEdit->setReadOnly(true);
    hexEdit->setMaximumHeight(100);
    layout->addWidget(hexEdit);

    layout->addWidget(new QLabel("<b>Decoded Script:</b>"));

    auto* decodedEdit = new QTextEdit();
    decodedEdit->setPlainText("(Script decoding not yet implemented)");
    decodedEdit->setReadOnly(true);
    layout->addWidget(decodedEdit);

    auto* copyButton = new QPushButton("📋 Copy Script");
    connect(copyButton, &QPushButton::clicked, [redeemScript]() {
        QApplication::clipboard()->setText(redeemScript);
    });
    layout->addWidget(copyButton);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(buttonBox);

    dialog.exec();
}

void EscrowWidget::onShowQRCode()
{
    if (selectedContractId_.isEmpty()) {
        return;
    }

    if (!contractsCache_.contains(selectedContractId_)) {
        return;
    }

    QJsonObject contract = contractsCache_[selectedContractId_];
    QString p2shAddress = contract["p2sh_address"].toString();

    if (p2shAddress.isEmpty()) {
        QMessageBox::warning(this, "No Address", "No P2SH address available for this contract.");
        return;
    }

    showQRDialog(p2shAddress, "Contract P2SH Address", true);
}

void EscrowWidget::onExportQRCode()
{
    // Direct export as QR - trigger the normal export flow
    // which now offers QR code display after file save
    onExportSighash();
}

void EscrowWidget::onImportFromQR()
{
    // Show dialog to scan/paste QR code data
    showImportQRDialog();
}

void EscrowWidget::showQRDialog(const QString& data, const QString& title, bool allowSave)
{
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.setMinimumSize(400, 500);

    auto* layout = new QVBoxLayout(&dialog);

    // Title label
    auto* titleLabel = new QLabel("<b>" + title + "</b>");
    titleLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(titleLabel);

    // Data label
    auto* dataLabel = new QLabel(data);
    dataLabel->setWordWrap(true);
    dataLabel->setAlignment(Qt::AlignCenter);
    dataLabel->setStyleSheet("background-color: #f0f0f0; padding: 10px; border-radius: 5px;");
    layout->addWidget(dataLabel);

    // Generate QR code using QrUtil
    QImage qrImage = QrUtil::makeQr(data, 300, 4);

    if (qrImage.isNull()) {
        QMessageBox::warning(this, "QR Generation Failed", "Failed to generate QR code.");
        return;
    }

    // Display QR code
    auto* qrLabel = new QLabel();
    qrLabel->setPixmap(QPixmap::fromImage(qrImage));
    qrLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(qrLabel);

    // Button layout
    auto* buttonLayout = new QHBoxLayout();

    // Copy button
    auto* copyButton = new QPushButton("📋 Copy to Clipboard");
    connect(copyButton, &QPushButton::clicked, [data]() {
        QApplication::clipboard()->setText(data);
    });
    buttonLayout->addWidget(copyButton);

    // Save QR button (if allowed)
    if (allowSave) {
        auto* saveButton = new QPushButton("💾 Save QR Image");
        connect(saveButton, &QPushButton::clicked, [this, &qrImage, &data]() {
            QString filename = QFileDialog::getSaveFileName(
                this,
                "Save QR Code",
                QDir::homePath() + "/contract_qr.png",
                "PNG Image (*.png)"
            );

            if (!filename.isEmpty()) {
                if (qrImage.save(filename)) {
                    QMessageBox::information(this, "Success", "QR code saved to:\n" + filename);
                } else {
                    QMessageBox::warning(this, "Error", "Failed to save QR code image.");
                }
            }
        });
        buttonLayout->addWidget(saveButton);
    }

    layout->addLayout(buttonLayout);

    // Close button
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(buttonBox);

    dialog.exec();
}

void EscrowWidget::showSighashQRDialog(const QString& jsonData, const QJsonObject& sighashData)
{
    QDialog dialog(this);
    dialog.setWindowTitle("Sighash QR Code - Offline Signing");
    dialog.setMinimumSize(500, 700);

    auto* layout = new QVBoxLayout(&dialog);

    // Title
    auto* titleLabel = new QLabel("<h3>📱 Sighash QR Code</h3>");
    titleLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(titleLabel);

    // Info about the transaction
    auto* infoGroup = new QGroupBox("Transaction Details");
    auto* infoLayout = new QGridLayout();

    infoLayout->addWidget(new QLabel("<b>Contract ID:</b>"), 0, 0);
    infoLayout->addWidget(new QLabel(sighashData["contract_id"].toString()), 0, 1);

    infoLayout->addWidget(new QLabel("<b>Amount:</b>"), 1, 0);
    infoLayout->addWidget(new QLabel(QString("%1 DIN").arg(sighashData["amount"].toDouble(), 0, 'f', 8)), 1, 1);

    infoLayout->addWidget(new QLabel("<b>To Address:</b>"), 2, 0);
    infoLayout->addWidget(new QLabel(sighashData["to_address"].toString()), 2, 1);

    infoLayout->addWidget(new QLabel("<b>Action:</b>"), 3, 0);
    infoLayout->addWidget(new QLabel(sighashData["is_refund"].toBool() ? "Refund" : "Release"), 3, 1);

    infoLayout->addWidget(new QLabel("<b>Sighash:</b>"), 4, 0);
    auto* sighashLabel = new QLabel(sighashData["sighash"].toString().left(32) + "...");
    sighashLabel->setStyleSheet("font-family: monospace; font-size: 9pt;");
    infoLayout->addWidget(sighashLabel, 4, 1);

    infoGroup->setLayout(infoLayout);
    layout->addWidget(infoGroup);

    // Warning about data size
    int dataSize = jsonData.length();
    QString sizeWarning;
    if (dataSize > 1000) {
        sizeWarning = QString("<b>⚠️ Large Data Warning:</b> This QR contains %1 bytes. "
                             "Some QR scanners may have difficulty with large QR codes. "
                             "Consider using the JSON file method instead.").arg(dataSize);
    } else {
        sizeWarning = QString("QR Code Size: %1 bytes (optimal for scanning)").arg(dataSize);
    }

    auto* warningLabel = new QLabel(sizeWarning);
    warningLabel->setWordWrap(true);
    warningLabel->setStyleSheet("background-color: #fff3cd; padding: 8px; border-radius: 4px; color: #856404;");
    layout->addWidget(warningLabel);

    // Generate QR code
    QImage qrImage = QrUtil::makeQr(jsonData, 400, 4);

    if (qrImage.isNull()) {
        QMessageBox::warning(this, "QR Generation Failed",
            "Failed to generate QR code. The data may be too large.\n\n"
            "Please use the JSON file export method instead.");
        return;
    }

    // Display QR in scrollable area (in case it's large)
    auto* scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setMinimumHeight(420);

    auto* qrLabel = new QLabel();
    qrLabel->setPixmap(QPixmap::fromImage(qrImage));
    qrLabel->setAlignment(Qt::AlignCenter);
    scrollArea->setWidget(qrLabel);
    ScrollSupport::enableForScrollArea(scrollArea, qrLabel);

    layout->addWidget(scrollArea);

    // Instructions
    auto* instructionsLabel = new QLabel(
        "<b>Instructions for Offline Signing:</b><br>"
        "1. Scan this QR code with your offline signing device<br>"
        "2. Sign the sighash with your private key(s)<br>"
        "3. The signed transaction can be returned via QR or JSON file<br>"
        "4. Use '📥 Import Signatures' to complete the transaction"
    );
    instructionsLabel->setWordWrap(true);
    instructionsLabel->setStyleSheet("background-color: #f0f0f0; padding: 10px; border-radius: 5px;");
    layout->addWidget(instructionsLabel);

    // Button layout
    auto* buttonLayout = new QHBoxLayout();

    // Copy JSON button
    auto* copyButton = new QPushButton("📋 Copy JSON");
    connect(copyButton, &QPushButton::clicked, [jsonData]() {
        QApplication::clipboard()->setText(jsonData);
    });
    buttonLayout->addWidget(copyButton);

    // Save QR button
    auto* saveButton = new QPushButton("💾 Save QR Image");
    connect(saveButton, &QPushButton::clicked, [this, &qrImage]() {
        QString filename = QFileDialog::getSaveFileName(
            this,
            "Save Sighash QR Code",
            QDir::homePath() + "/sighash_qr.png",
            "PNG Image (*.png)"
        );

        if (!filename.isEmpty()) {
            if (qrImage.save(filename)) {
                QMessageBox::information(this, "Success", "QR code saved to:\n" + filename);
            } else {
                QMessageBox::warning(this, "Error", "Failed to save QR code image.");
            }
        }
    });
    buttonLayout->addWidget(saveButton);

    layout->addLayout(buttonLayout);

    // Close button
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(buttonBox);

    dialog.exec();
}

void EscrowWidget::showImportQRDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Import Signed Transaction from QR");
    dialog.setMinimumSize(500, 400);

    auto* layout = new QVBoxLayout(&dialog);

    // Title
    auto* titleLabel = new QLabel("<h3>📱 Import Signed Transaction</h3>");
    titleLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(titleLabel);

    // Instructions
    auto* instructionsLabel = new QLabel(
        "<b>Import Methods:</b><br><br>"
        "1. <b>Paste JSON Data:</b> Copy signed transaction JSON and paste below<br>"
        "2. <b>Load from File:</b> Use the '📥 Import Signatures' button instead<br>"
        "3. <b>Scan QR (Future):</b> Camera-based QR scanning coming soon"
    );
    instructionsLabel->setWordWrap(true);
    layout->addWidget(instructionsLabel);

    // Text area for pasting JSON
    auto* jsonEdit = new QTextEdit();
    jsonEdit->setPlaceholderText("Paste signed transaction JSON here...");
    jsonEdit->setMinimumHeight(150);
    layout->addWidget(jsonEdit);

    // Button layout
    auto* buttonLayout = new QHBoxLayout();

    // Import button
    auto* importButton = new QPushButton("✅ Import and Broadcast");
    importButton->setStyleSheet("background-color: #4CAF50; color: white; font-weight: bold; padding: 10px;");
    connect(importButton, &QPushButton::clicked, [this, &dialog, jsonEdit]() {
        QString jsonData = jsonEdit->toPlainText().trimmed();

        if (jsonData.isEmpty()) {
            QMessageBox::warning(&dialog, "No Data", "Please paste the signed transaction JSON.");
            return;
        }

        // Parse JSON
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(jsonData.toUtf8(), &error);

        if (error.error != QJsonParseError::NoError) {
            QMessageBox::critical(&dialog, "Parse Error",
                QString("Invalid JSON: %1").arg(error.errorString()));
            return;
        }

        QJsonObject sigData = doc.object();

        // Validate required fields
        QStringList requiredFields = {"contract_id", "sighash", "is_refund"};
        for (const QString& field : requiredFields) {
            if (!sigData.contains(field)) {
                QMessageBox::critical(&dialog, "Invalid Data",
                    QString("Missing required field: %1").arg(field));
                return;
            }
        }

        // Check for signatures
        bool hasBuyerSig = sigData.contains("sig_buyer") && !sigData["sig_buyer"].toString().isEmpty();
        bool hasSellerSig = sigData.contains("sig_seller") && !sigData["sig_seller"].toString().isEmpty();

        if (!hasBuyerSig && !hasSellerSig) {
            QMessageBox::warning(&dialog, "No Signatures",
                "No signatures found in the JSON data.\n\n"
                "Please ensure the transaction has been signed.");
            return;
        }

        // Confirm import
        QString confirmMsg = QString("Ready to broadcast transaction:\n\n"
                                    "Contract: %1\n"
                                    "Action: %2\n"
                                    "Buyer signature: %3\n"
                                    "Seller signature: %4\n\n"
                                    "Proceed with broadcast?")
            .arg(sigData["contract_id"].toString())
            .arg(sigData["is_refund"].toBool() ? "Refund" : "Release")
            .arg(hasBuyerSig ? "✅ Present" : "❌ Missing")
            .arg(hasSellerSig ? "✅ Present" : "❌ Missing");

        auto reply = QMessageBox::question(&dialog, "Confirm Broadcast", confirmMsg);

        if (reply == QMessageBox::Yes) {
            QMessageBox::critical(&dialog, "Contract Broadcast Disabled",
                                  "The package was not submitted. Bound contract signing "
                                  "is not available in v8.1.9.");
        }
    });
    buttonLayout->addWidget(importButton);

    // Cancel button
    auto* cancelButton = new QPushButton("Cancel");
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    buttonLayout->addWidget(cancelButton);

    layout->addLayout(buttonLayout);

    dialog.exec();
}

void EscrowWidget::callRpc(const QString& method, const QJsonArray& params)
{
    rpc_->call(method, params);
}
