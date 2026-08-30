#include "lightningwidget.h"
#include "rpcclient.h"
#include "QrUtil.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QMessageBox>
#include <QInputDialog>
#include <QClipboard>
#include <QApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPixmap>

namespace dinero {

LightningWidget::LightningWidget(RpcClient* rpc, QWidget* parent)
    : QWidget(parent)
    , rpc_(rpc)
    , refreshTimer_(new QTimer(this))
    , tabs_(nullptr)
{
    setupUI();

    // Connect RPC client signals
    connect(rpc_, &RpcClient::rpcResult, this, &LightningWidget::onRpcResult);
    connect(rpc_, &RpcClient::rpcError, this, &LightningWidget::onRpcError);

    // Auto-refresh every 10 seconds
    connect(refreshTimer_, &QTimer::timeout, this, &LightningWidget::onRefreshTimer);
    refreshTimer_->start(10000);

    // Initial refresh
    refresh();
}

LightningWidget::~LightningWidget() {
}

void LightningWidget::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);

    // Title
    auto* titleLabel = new QLabel("⚡ Lightning Network");
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    mainLayout->addWidget(titleLabel);

    // Tab widget for different Lightning sections
    tabs_ = new QTabWidget();
    mainLayout->addWidget(tabs_);

    // Create tabs
    createChannelsTab();
    createInvoicesTab();
    createPaymentsTab();
    createWatchtowerTab();
    createNetworkTab();
}

void LightningWidget::createChannelsTab() {
    auto* channelsWidget = new QWidget();
    auto* layout = new QVBoxLayout(channelsWidget);

    // Statistics at top
    auto* statsGroup = new QGroupBox("Channel Statistics");
    auto* statsLayout = new QGridLayout();

    lblActiveChannels_ = new QLabel("0");
    lblTotalCapacity_ = new QLabel("0 DIN");
    lblLocalBalance_ = new QLabel("0 DIN");
    lblRemoteBalance_ = new QLabel("0 DIN");

    statsLayout->addWidget(new QLabel("<b>Active Channels:</b>"), 0, 0);
    statsLayout->addWidget(lblActiveChannels_, 0, 1);
    statsLayout->addWidget(new QLabel("<b>Total Capacity:</b>"), 0, 2);
    statsLayout->addWidget(lblTotalCapacity_, 0, 3);
    statsLayout->addWidget(new QLabel("<b>Local Balance:</b>"), 1, 0);
    statsLayout->addWidget(lblLocalBalance_, 1, 1);
    statsLayout->addWidget(new QLabel("<b>Remote Balance:</b>"), 1, 2);
    statsLayout->addWidget(lblRemoteBalance_, 1, 3);

    statsGroup->setLayout(statsLayout);
    layout->addWidget(statsGroup);

    // Channels table
    tblChannels_ = new QTableWidget();
    tblChannels_->setColumnCount(6);
    tblChannels_->setHorizontalHeaderLabels({
        "Channel ID", "Peer Node ID", "Capacity (DIN)", "Local Balance", "Remote Balance", "Status"
    });
    tblChannels_->horizontalHeader()->setStretchLastSection(true);
    tblChannels_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblChannels_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(tblChannels_, &QTableWidget::cellClicked, this, &LightningWidget::onChannelSelected);
    layout->addWidget(tblChannels_);

    // Open channel controls
    auto* openGroup = new QGroupBox("Open New Channel");
    auto* openLayout = new QGridLayout();

    edtPeerNodeId_ = new QLineEdit();
    edtPeerNodeId_->setPlaceholderText("Peer Node ID (02abcdef...)");
    edtChannelCapacity_ = new QLineEdit();
    edtChannelCapacity_->setPlaceholderText("Capacity (e.g. 1.0 for 1 DIN)");
    edtPushAmount_ = new QLineEdit();
    edtPushAmount_->setPlaceholderText("Push amount (optional, 0.0)");
    spnMinConf_ = new QSpinBox();
    spnMinConf_->setValue(1);
    spnMinConf_->setRange(0, 999999);

    openLayout->addWidget(new QLabel("Peer Node ID:"), 0, 0);
    openLayout->addWidget(edtPeerNodeId_, 0, 1);
    openLayout->addWidget(new QLabel("Capacity (DIN):"), 1, 0);
    openLayout->addWidget(edtChannelCapacity_, 1, 1);
    openLayout->addWidget(new QLabel("Push Amount (DIN):"), 2, 0);
    openLayout->addWidget(edtPushAmount_, 2, 1);
    openLayout->addWidget(new QLabel("Min Confirmations:"), 3, 0);
    openLayout->addWidget(spnMinConf_, 3, 1);

    openGroup->setLayout(openLayout);
    layout->addWidget(openGroup);

    // Control buttons
    auto* btnLayout = new QHBoxLayout();
    btnOpenChannel_ = new QPushButton("Open Channel");
    btnCloseChannel_ = new QPushButton("Close Selected Channel");
    btnForceCloseChannel_ = new QPushButton("Force Close (Emergency)");
    btnRefreshChannels_ = new QPushButton("🔄 Refresh");

    btnForceCloseChannel_->setStyleSheet("QPushButton { background-color: #c0392b; color: white; }");

    connect(btnOpenChannel_, &QPushButton::clicked, this, &LightningWidget::onOpenChannel);
    connect(btnCloseChannel_, &QPushButton::clicked, this, &LightningWidget::onCloseChannel);
    connect(btnForceCloseChannel_, &QPushButton::clicked, this, &LightningWidget::onForceCloseChannel);
    connect(btnRefreshChannels_, &QPushButton::clicked, this, &LightningWidget::onRefreshChannels);

    btnLayout->addWidget(btnOpenChannel_);
    btnLayout->addWidget(btnCloseChannel_);
    btnLayout->addWidget(btnForceCloseChannel_);
    btnLayout->addStretch();
    btnLayout->addWidget(btnRefreshChannels_);
    layout->addLayout(btnLayout);

    tabs_->addTab(channelsWidget, "Channels");
}

void LightningWidget::createInvoicesTab() {
    auto* invoicesWidget = new QWidget();
    auto* layout = new QVBoxLayout(invoicesWidget);

    // Create invoice section
    auto* createGroup = new QGroupBox("Create Invoice");
    auto* createLayout = new QGridLayout();

    edtInvoiceAmount_ = new QLineEdit();
    edtInvoiceAmount_->setPlaceholderText("Amount in DIN (e.g. 0.001)");
    edtInvoiceDescription_ = new QLineEdit();
    edtInvoiceDescription_->setPlaceholderText("Description (e.g. Coffee payment)");
    spnInvoiceExpiry_ = new QSpinBox();
    spnInvoiceExpiry_->setValue(3600);
    spnInvoiceExpiry_->setRange(60, 86400);
    spnInvoiceExpiry_->setSuffix(" seconds");

    createLayout->addWidget(new QLabel("Amount (DIN):"), 0, 0);
    createLayout->addWidget(edtInvoiceAmount_, 0, 1);
    createLayout->addWidget(new QLabel("Description:"), 1, 0);
    createLayout->addWidget(edtInvoiceDescription_, 1, 1);
    createLayout->addWidget(new QLabel("Expiry:"), 2, 0);
    createLayout->addWidget(spnInvoiceExpiry_, 2, 1);

    auto* createBtnLayout = new QHBoxLayout();
    btnCreateInvoice_ = new QPushButton("Create Invoice");
    btnCreateOpenInvoice_ = new QPushButton("Create Open Invoice (Flexible Amount)");
    connect(btnCreateInvoice_, &QPushButton::clicked, this, &LightningWidget::onCreateInvoice);
    connect(btnCreateOpenInvoice_, &QPushButton::clicked, this, &LightningWidget::onCreateOpenInvoice);
    createBtnLayout->addWidget(btnCreateInvoice_);
    createBtnLayout->addWidget(btnCreateOpenInvoice_);
    createLayout->addLayout(createBtnLayout, 3, 0, 1, 2);

    createGroup->setLayout(createLayout);
    layout->addWidget(createGroup);

    // Invoice display with QR code
    auto* displayGroup = new QGroupBox("Generated Invoice");
    auto* displayLayout = new QHBoxLayout();

    auto* qrLayout = new QVBoxLayout();
    lblInvoiceQR_ = new QLabel();
    lblInvoiceQR_->setMinimumSize(200, 200);
    lblInvoiceQR_->setMaximumSize(200, 200);
    lblInvoiceQR_->setAlignment(Qt::AlignCenter);
    lblInvoiceQR_->setStyleSheet("QLabel { border: 2px solid #ccc; background: white; }");
    btnGenerateQR_ = new QPushButton("Generate QR Code");
    connect(btnGenerateQR_, &QPushButton::clicked, this, &LightningWidget::onGenerateQR);
    qrLayout->addWidget(lblInvoiceQR_);
    qrLayout->addWidget(btnGenerateQR_);

    auto* invoiceLayout = new QVBoxLayout();
    edtInvoiceBolt11_ = new QLineEdit();
    edtInvoiceBolt11_->setReadOnly(true);
    edtInvoiceBolt11_->setPlaceholderText("Invoice will appear here...");
    btnCopyInvoice_ = new QPushButton("📋 Copy Invoice");
    connect(btnCopyInvoice_, &QPushButton::clicked, this, &LightningWidget::onCopyInvoice);
    invoiceLayout->addWidget(new QLabel("<b>BOLT 11 Invoice:</b>"));
    invoiceLayout->addWidget(edtInvoiceBolt11_);
    invoiceLayout->addWidget(btnCopyInvoice_);
    invoiceLayout->addStretch();

    displayLayout->addLayout(qrLayout);
    displayLayout->addLayout(invoiceLayout);
    displayGroup->setLayout(displayLayout);
    layout->addWidget(displayGroup);

    // Pay invoice section
    auto* payGroup = new QGroupBox("Pay Invoice");
    auto* payLayout = new QGridLayout();

    edtPayBolt11_ = new QLineEdit();
    edtPayBolt11_->setPlaceholderText("Paste BOLT 11 invoice here");
    edtPayCustomAmount_ = new QLineEdit();
    edtPayCustomAmount_->setPlaceholderText("Custom amount (for open invoices)");

    payLayout->addWidget(new QLabel("Invoice (BOLT 11):"), 0, 0);
    payLayout->addWidget(edtPayBolt11_, 0, 1);
    payLayout->addWidget(new QLabel("Custom Amount:"), 1, 0);
    payLayout->addWidget(edtPayCustomAmount_, 1, 1);

    auto* payBtnLayout = new QHBoxLayout();
    btnDecodeInvoice_ = new QPushButton("Decode Invoice");
    btnPayInvoice_ = new QPushButton("Pay Invoice");
    btnPayInvoice_->setStyleSheet("QPushButton { background-color: #27ae60; color: white; font-weight: bold; }");
    connect(btnDecodeInvoice_, &QPushButton::clicked, this, &LightningWidget::onDecodeInvoice);
    connect(btnPayInvoice_, &QPushButton::clicked, this, &LightningWidget::onPayInvoice);
    payBtnLayout->addWidget(btnDecodeInvoice_);
    payBtnLayout->addWidget(btnPayInvoice_);
    payLayout->addLayout(payBtnLayout, 2, 0, 1, 2);

    txtInvoiceDecoded_ = new QTextEdit();
    txtInvoiceDecoded_->setReadOnly(true);
    txtInvoiceDecoded_->setMaximumHeight(100);
    txtInvoiceDecoded_->setPlaceholderText("Decoded invoice details will appear here...");
    payLayout->addWidget(new QLabel("Decoded Info:"), 3, 0);
    payLayout->addWidget(txtInvoiceDecoded_, 3, 1);

    payGroup->setLayout(payLayout);
    layout->addWidget(payGroup);

    // Invoice list with filter
    auto* listHeader = new QHBoxLayout();
    listHeader->addWidget(new QLabel("<b>Invoice History</b>"));
    cmbInvoiceFilter_ = new QComboBox();
    cmbInvoiceFilter_->addItems({"All", "Pending", "Paid", "Expired"});
    connect(cmbInvoiceFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LightningWidget::onRefreshInvoices);
    listHeader->addWidget(new QLabel("Filter:"));
    listHeader->addWidget(cmbInvoiceFilter_);
    lblInvoiceStats_ = new QLabel();
    listHeader->addStretch();
    listHeader->addWidget(lblInvoiceStats_);
    btnRefreshInvoices_ = new QPushButton("🔄 Refresh");
    connect(btnRefreshInvoices_, &QPushButton::clicked, this, &LightningWidget::onRefreshInvoices);
    listHeader->addWidget(btnRefreshInvoices_);
    layout->addLayout(listHeader);

    tblInvoices_ = new QTableWidget();
    tblInvoices_->setColumnCount(5);
    tblInvoices_->setHorizontalHeaderLabels({
        "Payment Hash", "Amount (DIN)", "Description", "Status", "Created"
    });
    tblInvoices_->horizontalHeader()->setStretchLastSection(true);
    tblInvoices_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblInvoices_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(tblInvoices_);

    tabs_->addTab(invoicesWidget, "Invoices");
}

void LightningWidget::createPaymentsTab() {
    auto* paymentsWidget = new QWidget();
    auto* layout = new QVBoxLayout(paymentsWidget);

    // Payment statistics
    lblPaymentStats_ = new QLabel();
    layout->addWidget(lblPaymentStats_);

    // Filter and controls
    auto* controlLayout = new QHBoxLayout();
    cmbPaymentFilter_ = new QComboBox();
    cmbPaymentFilter_->addItems({"All", "Pending", "Success", "Failed"});
    connect(cmbPaymentFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LightningWidget::onRefreshPayments);
    controlLayout->addWidget(new QLabel("Filter:"));
    controlLayout->addWidget(cmbPaymentFilter_);
    controlLayout->addStretch();
    btnCancelPayment_ = new QPushButton("Cancel Selected Payment");
    btnRefreshPayments_ = new QPushButton("🔄 Refresh");
    connect(btnCancelPayment_, &QPushButton::clicked, this, &LightningWidget::onCancelPayment);
    connect(btnRefreshPayments_, &QPushButton::clicked, this, &LightningWidget::onRefreshPayments);
    controlLayout->addWidget(btnCancelPayment_);
    controlLayout->addWidget(btnRefreshPayments_);
    layout->addLayout(controlLayout);

    // Payments table
    tblPayments_ = new QTableWidget();
    tblPayments_->setColumnCount(6);
    tblPayments_->setHorizontalHeaderLabels({
        "Payment Hash", "Amount (DIN)", "Fee (una)", "Hops", "Status", "Time"
    });
    tblPayments_->horizontalHeader()->setStretchLastSection(true);
    tblPayments_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblPayments_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(tblPayments_);

    // Payment details
    auto* detailsGroup = new QGroupBox("Payment Details");
    auto* detailsLayout = new QVBoxLayout();
    txtPaymentDetails_ = new QTextEdit();
    txtPaymentDetails_->setReadOnly(true);
    txtPaymentDetails_->setMaximumHeight(150);
    detailsLayout->addWidget(txtPaymentDetails_);
    detailsGroup->setLayout(detailsLayout);
    layout->addWidget(detailsGroup);

    tabs_->addTab(paymentsWidget, "Payments");
}

void LightningWidget::createWatchtowerTab() {
    auto* watchtowerWidget = new QWidget();
    auto* layout = new QVBoxLayout(watchtowerWidget);

    // Watchtower statistics
    lblWatchtowerStats_ = new QLabel();
    layout->addWidget(lblWatchtowerStats_);

    // Register watchtower
    auto* registerGroup = new QGroupBox("Register Watchtower");
    auto* registerLayout = new QGridLayout();

    edtWatchtowerUrl_ = new QLineEdit();
    edtWatchtowerUrl_->setPlaceholderText("Watchtower URL (e.g. wt://watchtower.dinero.com:9911)");
    edtWatchtowerReward_ = new QLineEdit();
    edtWatchtowerReward_->setPlaceholderText("Reward per appointment (unas)");
    edtWatchtowerReward_->setText("1000");

    registerLayout->addWidget(new QLabel("Watchtower URL:"), 0, 0);
    registerLayout->addWidget(edtWatchtowerUrl_, 0, 1);
    registerLayout->addWidget(new QLabel("Reward (una):"), 1, 0);
    registerLayout->addWidget(edtWatchtowerReward_, 1, 1);

    auto* wtBtnLayout = new QHBoxLayout();
    btnRegisterWatchtower_ = new QPushButton("Register Watchtower");
    btnUnregisterWatchtower_ = new QPushButton("Unregister Selected");
    btnRefreshWatchtowers_ = new QPushButton("🔄 Refresh");
    connect(btnRegisterWatchtower_, &QPushButton::clicked, this, &LightningWidget::onRegisterWatchtower);
    connect(btnUnregisterWatchtower_, &QPushButton::clicked, this, &LightningWidget::onUnregisterWatchtower);
    connect(btnRefreshWatchtowers_, &QPushButton::clicked, this, &LightningWidget::onRefreshWatchtowers);
    wtBtnLayout->addWidget(btnRegisterWatchtower_);
    wtBtnLayout->addWidget(btnUnregisterWatchtower_);
    wtBtnLayout->addStretch();
    wtBtnLayout->addWidget(btnRefreshWatchtowers_);
    registerLayout->addLayout(wtBtnLayout, 2, 0, 1, 2);

    registerGroup->setLayout(registerLayout);
    layout->addWidget(registerGroup);

    // Watchtower table
    tblWatchtowers_ = new QTableWidget();
    tblWatchtowers_->setColumnCount(5);
    tblWatchtowers_->setHorizontalHeaderLabels({
        "Watchtower ID", "URL", "Appointments", "Status", "Reward (una)"
    });
    tblWatchtowers_->horizontalHeader()->setStretchLastSection(true);
    tblWatchtowers_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblWatchtowers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(tblWatchtowers_);

    // Watchtower info
    auto* infoGroup = new QGroupBox("Watchtower Information");
    auto* infoLayout = new QVBoxLayout();
    txtWatchtowerInfo_ = new QTextEdit();
    txtWatchtowerInfo_->setReadOnly(true);
    txtWatchtowerInfo_->setMaximumHeight(120);
    txtWatchtowerInfo_->setPlainText(
        "Watchtowers protect your channels when you're offline.\n\n"
        "• Register a watchtower to monitor for channel breaches\n"
        "• Watchtowers receive encrypted penalty transactions\n"
        "• If breach detected, watchtower broadcasts penalty\n"
        "• Zero-knowledge: watchtower cannot steal your funds"
    );
    infoLayout->addWidget(txtWatchtowerInfo_);
    infoGroup->setLayout(infoLayout);
    layout->addWidget(infoGroup);

    tabs_->addTab(watchtowerWidget, "Watchtowers");
}

void LightningWidget::createNetworkTab() {
    auto* networkWidget = new QWidget();
    auto* layout = new QVBoxLayout(networkWidget);

    // Network statistics
    auto* statsGroup = new QGroupBox("Network Statistics");
    auto* statsLayout = new QGridLayout();

    lblTotalNodes_ = new QLabel("0");
    lblTotalChannels_ = new QLabel("0");
    lblNetworkCapacity_ = new QLabel("0 DIN");
    lblAvgChannelSize_ = new QLabel("0 DIN");

    statsLayout->addWidget(new QLabel("<b>Total Nodes:</b>"), 0, 0);
    statsLayout->addWidget(lblTotalNodes_, 0, 1);
    statsLayout->addWidget(new QLabel("<b>Total Channels:</b>"), 0, 2);
    statsLayout->addWidget(lblTotalChannels_, 0, 3);
    statsLayout->addWidget(new QLabel("<b>Network Capacity:</b>"), 1, 0);
    statsLayout->addWidget(lblNetworkCapacity_, 1, 1);
    statsLayout->addWidget(new QLabel("<b>Avg Channel Size:</b>"), 1, 2);
    statsLayout->addWidget(lblAvgChannelSize_, 1, 3);

    statsGroup->setLayout(statsLayout);
    layout->addWidget(statsGroup);

    // Peer connection
    auto* peerGroup = new QGroupBox("Connect to Peer");
    auto* peerLayout = new QHBoxLayout();
    edtPeerAddress_ = new QLineEdit();
    edtPeerAddress_->setPlaceholderText("Node ID@host:port (e.g. 02abc@192.168.1.100:9735)");
    btnConnectPeer_ = new QPushButton("Connect");
    btnDisconnectPeer_ = new QPushButton("Disconnect Selected");
    connect(btnConnectPeer_, &QPushButton::clicked, this, &LightningWidget::onConnectPeer);
    connect(btnDisconnectPeer_, &QPushButton::clicked, this, &LightningWidget::onDisconnectPeer);
    peerLayout->addWidget(edtPeerAddress_);
    peerLayout->addWidget(btnConnectPeer_);
    peerLayout->addWidget(btnDisconnectPeer_);
    peerGroup->setLayout(peerLayout);
    layout->addWidget(peerGroup);

    // Route finding
    auto* routeGroup = new QGroupBox("Find Route");
    auto* routeLayout = new QGridLayout();

    edtRouteDestination_ = new QLineEdit();
    edtRouteDestination_->setPlaceholderText("Destination Node ID");
    edtRouteAmount_ = new QLineEdit();
    edtRouteAmount_->setPlaceholderText("Amount (DIN)");
    btnFindRoute_ = new QPushButton("Find Route");
    connect(btnFindRoute_, &QPushButton::clicked, this, &LightningWidget::onFindRoute);

    routeLayout->addWidget(new QLabel("Destination:"), 0, 0);
    routeLayout->addWidget(edtRouteDestination_, 0, 1);
    routeLayout->addWidget(new QLabel("Amount:"), 1, 0);
    routeLayout->addWidget(edtRouteAmount_, 1, 1);
    routeLayout->addWidget(btnFindRoute_, 1, 2);

    txtRouteResult_ = new QTextEdit();
    txtRouteResult_->setReadOnly(true);
    txtRouteResult_->setMaximumHeight(100);
    routeLayout->addWidget(new QLabel("Route:"), 2, 0);
    routeLayout->addWidget(txtRouteResult_, 2, 1, 1, 2);

    routeGroup->setLayout(routeLayout);
    layout->addWidget(routeGroup);

    // Node table
    auto* nodeHeader = new QHBoxLayout();
    nodeHeader->addWidget(new QLabel("<b>Network Nodes</b>"));
    nodeHeader->addStretch();
    btnRefreshNetwork_ = new QPushButton("🔄 Refresh");
    connect(btnRefreshNetwork_, &QPushButton::clicked, this, &LightningWidget::onRefreshNetworkInfo);
    nodeHeader->addWidget(btnRefreshNetwork_);
    layout->addLayout(nodeHeader);

    tblNodes_ = new QTableWidget();
    tblNodes_->setColumnCount(5);
    tblNodes_->setHorizontalHeaderLabels({
        "Node ID", "Alias", "Channels", "Capacity (DIN)", "Last Seen"
    });
    tblNodes_->horizontalHeader()->setStretchLastSection(true);
    tblNodes_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblNodes_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(tblNodes_);

    tabs_->addTab(networkWidget, "Network");
}

// === SLOT IMPLEMENTATIONS ===

void LightningWidget::refresh() {
    onRefreshChannels();
    onRefreshInvoices();
    onRefreshPayments();
    onRefreshWatchtowers();
    onRefreshNetworkInfo();
}

void LightningWidget::onRefreshTimer() {
    refresh();
}

// Channel slots
void LightningWidget::onOpenChannel() {
    QString nodeId = edtPeerNodeId_->text().trimmed();
    QString capacityStr = edtChannelCapacity_->text().trimmed();
    QString pushStr = edtPushAmount_->text().trimmed();
    int minConf = spnMinConf_->value();

    if (nodeId.isEmpty() || capacityStr.isEmpty()) {
        QMessageBox::warning(this, "Invalid Input", "Please enter peer node ID and capacity.");
        return;
    }

    bool ok;
    double capacity = capacityStr.toDouble(&ok);
    if (!ok || capacity <= 0) {
        QMessageBox::warning(this, "Invalid Amount", "Please enter a valid capacity amount.");
        return;
    }

    double pushAmount = pushStr.isEmpty() ? 0.0 : pushStr.toDouble(&ok);
    if (!ok || pushAmount < 0) {
        pushAmount = 0.0;
    }

    // Convert DIN to unas (1 DIN = 100,000,000 una)
    qint64 capacityUnas = qint64(capacity * 100000000);
    qint64 pushUnas = qint64(pushAmount * 100000000);

    // RPC params: peer_node_id, local_amount_unas, push_amount_unas (optional), to_self_delay (optional)
    rpc_->call("ln.openchannel", QJsonArray{nodeId, capacityUnas, pushUnas});
    Q_EMIT statusMessage("Opening channel to " + nodeId + "...");
}

void LightningWidget::onCloseChannel() {
    if (selectedChannelId_.isEmpty()) {
        QMessageBox::warning(this, "No Selection", "Please select a channel to close.");
        return;
    }

    auto reply = QMessageBox::question(this, "Close Channel",
        "Are you sure you want to close this channel?\n\nThis will create a closing transaction on-chain.",
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        // RPC params: channel_id, force (optional, default false)
        rpc_->call("ln.closechannel", QJsonArray{selectedChannelId_});
        Q_EMIT statusMessage("Closing channel " + selectedChannelId_ + "...");
    }
}

void LightningWidget::onForceCloseChannel() {
    if (selectedChannelId_.isEmpty()) {
        QMessageBox::warning(this, "No Selection", "Please select a channel to force close.");
        return;
    }

    auto reply = QMessageBox::warning(this, "Force Close Channel",
        "⚠️ WARNING: Force closing should only be used in emergencies!\n\n"
        "This will broadcast your latest commitment transaction.\n"
        "You may lose funds if the peer has a newer state.\n\n"
        "Are you absolutely sure?",
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        // RPC params: channel_id, force (true for force close)
        rpc_->call("ln.closechannel", QJsonArray{selectedChannelId_, true});
        Q_EMIT statusMessage("Force closing channel " + selectedChannelId_ + "...");
    }
}

void LightningWidget::onRefreshChannels() {
    rpc_->call("ln.listchannels", QJsonArray());
}

void LightningWidget::onChannelSelected(int row, int column) {
    Q_UNUSED(column);
    if (row >= 0 && row < tblChannels_->rowCount()) {
        selectedChannelId_ = tblChannels_->item(row, 0)->text();
    }
}

// Invoice slots
void LightningWidget::onCreateInvoice() {
    QString amountStr = edtInvoiceAmount_->text().trimmed();
    QString description = edtInvoiceDescription_->text().trimmed();
    int expiry = spnInvoiceExpiry_->value();

    if (amountStr.isEmpty() || description.isEmpty()) {
        QMessageBox::warning(this, "Invalid Input", "Please enter amount and description.");
        return;
    }

    bool ok;
    double amount = amountStr.toDouble(&ok);
    if (!ok || amount <= 0) {
        QMessageBox::warning(this, "Invalid Amount", "Please enter a valid amount.");
        return;
    }

    // Convert DIN to milli-una (1 DIN = 100,000,000,000 muna)
    qint64 amountMsat = qint64(amount * 100000000000.0);

    rpc_->call("ln.invoice", QJsonArray{amountMsat, description, expiry});
    Q_EMIT statusMessage("Creating invoice...");
}

void LightningWidget::onCreateOpenInvoice() {
    QString description = edtInvoiceDescription_->text().trimmed();
    int expiry = spnInvoiceExpiry_->value();

    if (description.isEmpty()) {
        QMessageBox::warning(this, "Invalid Input", "Please enter a description.");
        return;
    }

    rpc_->call("ln.createopeninvoice", QJsonArray{description, expiry});
    Q_EMIT statusMessage("Creating open invoice...");
}

void LightningWidget::onPayInvoice() {
    QString bolt11 = edtPayBolt11_->text().trimmed();
    QString customAmountStr = edtPayCustomAmount_->text().trimmed();

    if (bolt11.isEmpty()) {
        QMessageBox::warning(this, "Invalid Input", "Please enter a BOLT 11 invoice.");
        return;
    }

    auto reply = QMessageBox::question(this, "Pay Invoice",
        "Are you sure you want to pay this invoice?",
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        // RPC: bolt11, amount_msat (optional for open invoices)
        if (!customAmountStr.isEmpty()) {
            bool ok;
            double amount = customAmountStr.toDouble(&ok);
            if (ok && amount > 0) {
                qint64 amountMsat = qint64(amount * 100000000000.0);
                rpc_->call("ln.payinvoice", QJsonArray{bolt11, amountMsat});
            } else {
                rpc_->call("ln.payinvoice", QJsonArray{bolt11});
            }
        } else {
            rpc_->call("ln.payinvoice", QJsonArray{bolt11});
        }
        Q_EMIT statusMessage("Sending payment...");
    }
}

void LightningWidget::onDecodeInvoice() {
    QString bolt11 = edtPayBolt11_->text().trimmed();

    if (bolt11.isEmpty()) {
        QMessageBox::warning(this, "Invalid Input", "Please enter a BOLT 11 invoice.");
        return;
    }

    rpc_->call("ln.decodeinvoice", QJsonArray{bolt11});
}

void LightningWidget::onGenerateQR() {
    QString bolt11 = edtInvoiceBolt11_->text().trimmed();
    if (bolt11.isEmpty()) {
        QMessageBox::information(this, "No Invoice", "Create an invoice first.");
        return;
    }

    QPixmap qr = generateQRCode("dinero:" + bolt11, 200);
    lblInvoiceQR_->setPixmap(qr);
}

void LightningWidget::onCopyInvoice() {
    QString bolt11 = edtInvoiceBolt11_->text().trimmed();
    if (!bolt11.isEmpty()) {
        QApplication::clipboard()->setText(bolt11);
        Q_EMIT statusMessage("Invoice copied to clipboard!");
    }
}

void LightningWidget::onRefreshInvoices() {
    QString filter = cmbInvoiceFilter_->currentText().toLower();
    // RPC: status filter (optional)
    if (filter != "all") {
        rpc_->call("ln.listinvoices", QJsonArray{filter});
    } else {
        rpc_->call("ln.listinvoices", QJsonArray());
    }
    rpc_->call("ln.invoicestats", QJsonArray());
}

// Payment slots
void LightningWidget::onRefreshPayments() {
    QString filter = cmbPaymentFilter_->currentText().toLower();
    // RPC: status filter (optional)
    if (filter != "all") {
        rpc_->call("ln.listpayments", QJsonArray{filter});
    } else {
        rpc_->call("ln.listpayments", QJsonArray());
    }
}

void LightningWidget::onCancelPayment() {
    if (selectedPaymentHash_.isEmpty()) {
        QMessageBox::warning(this, "No Selection", "Please select a payment to cancel.");
        return;
    }

    rpc_->call("ln.cancelpayment", QJsonArray{selectedPaymentHash_});
}

// Watchtower slots
void LightningWidget::onRegisterWatchtower() {
    QString url = edtWatchtowerUrl_->text().trimmed();
    QString rewardStr = edtWatchtowerReward_->text().trimmed();

    if (url.isEmpty()) {
        QMessageBox::warning(this, "Invalid Input", "Please enter watchtower URL.");
        return;
    }

    bool ok;
    qint64 reward = rewardStr.toLongLong(&ok);
    if (!ok || reward < 0) {
        reward = 1000; // Default
    }

    rpc_->call("ln.wt.register", QJsonArray{url, reward});
    Q_EMIT statusMessage("Registering watchtower...");
}

void LightningWidget::onUnregisterWatchtower() {
    if (selectedWatchtowerId_.isEmpty()) {
        QMessageBox::warning(this, "No Selection", "Please select a watchtower.");
        return;
    }

    rpc_->call("ln.wt.unregister", QJsonArray{selectedWatchtowerId_});
}

void LightningWidget::onRefreshWatchtowers() {
    rpc_->call("ln.wt.list", QJsonArray());
}

// Network slots
void LightningWidget::onRefreshNetworkInfo() {
    rpc_->call("ln.getnetworkinfo", QJsonArray());
    rpc_->call("ln.listnodes", QJsonArray());
}

void LightningWidget::onConnectPeer() {
    QString address = edtPeerAddress_->text().trimmed();
    if (address.isEmpty()) {
        QMessageBox::warning(this, "Invalid Input", "Please enter peer address.");
        return;
    }

    rpc_->call("ln.connect", QJsonArray{address});
    Q_EMIT statusMessage("Connecting to peer...");
}

void LightningWidget::onDisconnectPeer() {
    // Implementation depends on selected node
    QMessageBox::information(this, "Not Implemented", "Peer disconnection coming soon.");
}

void LightningWidget::onFindRoute() {
    QString dest = edtRouteDestination_->text().trimmed();
    QString amountStr = edtRouteAmount_->text().trimmed();

    if (dest.isEmpty() || amountStr.isEmpty()) {
        QMessageBox::warning(this, "Invalid Input", "Please enter destination and amount.");
        return;
    }

    bool ok;
    double amount = amountStr.toDouble(&ok);
    if (!ok || amount <= 0) {
        QMessageBox::warning(this, "Invalid Amount", "Please enter a valid amount.");
        return;
    }

    qint64 amountMsat = qint64(amount * 100000000000.0);

    rpc_->call("ln.queryroutes", QJsonArray{dest, amountMsat});
}

// RPC response handlers
void LightningWidget::onRpcResult(const QString& method, const QJsonValue& result) {
    if (method == "ln.listchannels") {
        if (result.isObject()) {
            QJsonArray channels = result.toObject()["channels"].toArray();
            updateChannelTable(channels);
        }
    } else if (method == "ln.invoice" || method == "ln.createopeninvoice") {
        if (result.isObject()) {
            QJsonObject obj = result.toObject();
            QString bolt11 = obj["bolt11"].toString();
            edtInvoiceBolt11_->setText(bolt11);
            Q_EMIT statusMessage("Invoice created successfully!");
            onRefreshInvoices();
        }
    } else if (method == "ln.decodeinvoice") {
        if (result.isObject()) {
            QJsonDocument doc(result.toObject());
            txtInvoiceDecoded_->setPlainText(doc.toJson(QJsonDocument::Indented));
        }
    } else if (method == "ln.payinvoice") {
        Q_EMIT statusMessage("Payment successful!");
        onRefreshPayments();
        onRefreshChannels();
    } else if (method == "ln.listinvoices") {
        if (result.isObject()) {
            QJsonArray invoices = result.toObject()["invoices"].toArray();
            updateInvoiceTable(invoices);
        }
    } else if (method == "ln.invoicestats") {
        if (result.isObject()) {
            QJsonObject stats = result.toObject();
            QString statsText = QString("Total: %1 | Paid: %2 | Pending: %3 | Success Rate: %4%")
                .arg(stats["total_invoices"].toInt())
                .arg(stats["paid_invoices"].toInt())
                .arg(stats["pending_invoices"].toInt())
                .arg(stats["success_rate"].toDouble() * 100, 0, 'f', 1);
            lblInvoiceStats_->setText(statsText);
        }
    } else if (method == "ln.listpayments") {
        if (result.isObject()) {
            QJsonArray payments = result.toObject()["payments"].toArray();
            updatePaymentTable(payments);
        }
    } else if (method == "ln.wt.list") {
        if (result.isObject()) {
            QJsonArray watchtowers = result.toObject()["watchtowers"].toArray();
            updateWatchtowerTable(watchtowers);
        }
    } else if (method == "ln.getnetworkinfo") {
        if (result.isObject()) {
            updateNetworkInfo(result.toObject());
        }
    } else if (method == "ln.listnodes") {
        if (result.isObject()) {
            QJsonArray nodes = result.toObject()["nodes"].toArray();
            updateNodeTable(nodes);
        }
    } else if (method == "ln.queryroutes") {
        if (result.isObject()) {
            QJsonDocument doc(result.toObject());
            txtRouteResult_->setPlainText(doc.toJson(QJsonDocument::Indented));
        }
    }
}

void LightningWidget::onRpcError(const QString& method, int code, const QString& error) {
    Q_UNUSED(code);
    Q_EMIT errorOccurred(QString("RPC Error (%1): %2").arg(method).arg(error));
}

// Update UI methods
void LightningWidget::updateChannelTable(const QJsonArray& channels) {
    tblChannels_->setRowCount(0);

    qint64 totalCapacity = 0;
    qint64 localBalance = 0;
    qint64 remoteBalance = 0;
    int activeChannels = 0;

    for (const auto& ch : channels) {
        QJsonObject channel = ch.toObject();

        int row = tblChannels_->rowCount();
        tblChannels_->insertRow(row);

        QString channelId = channel["channel_id"].toString();
        QString nodeId = channel["peer_node_id"].toString();
        qint64 capacity = channel["capacity_sat"].toVariant().toLongLong();
        qint64 local = channel["local_balance_sat"].toVariant().toLongLong();
        qint64 remote = channel["remote_balance_sat"].toVariant().toLongLong();
        QString status = channel["status"].toString();

        tblChannels_->setItem(row, 0, new QTableWidgetItem(channelId));
        tblChannels_->setItem(row, 1, new QTableWidgetItem(nodeId));
        tblChannels_->setItem(row, 2, new QTableWidgetItem(formatAmount(capacity * 1000)));
        tblChannels_->setItem(row, 3, new QTableWidgetItem(formatAmount(local * 1000)));
        tblChannels_->setItem(row, 4, new QTableWidgetItem(formatAmount(remote * 1000)));
        tblChannels_->setItem(row, 5, new QTableWidgetItem(formatChannelStatus(status)));

        totalCapacity += capacity;
        localBalance += local;
        remoteBalance += remote;
        if (status == "active") activeChannels++;
    }

    lblActiveChannels_->setText(QString::number(activeChannels));
    lblTotalCapacity_->setText(formatAmount(totalCapacity * 1000));
    lblLocalBalance_->setText(formatAmount(localBalance * 1000));
    lblRemoteBalance_->setText(formatAmount(remoteBalance * 1000));
}

void LightningWidget::updateInvoiceTable(const QJsonArray& invoices) {
    tblInvoices_->setRowCount(0);

    for (const auto& inv : invoices) {
        QJsonObject invoice = inv.toObject();

        int row = tblInvoices_->rowCount();
        tblInvoices_->insertRow(row);

        QString paymentHash = invoice["payment_hash"].toString();
        qint64 amount = invoice["amount_msat"].toVariant().toLongLong();
        QString description = invoice["description"].toString();
        QString status = invoice["status"].toString();
        qint64 createdAt = invoice["created_at"].toVariant().toLongLong();

        tblInvoices_->setItem(row, 0, new QTableWidgetItem(paymentHash.left(16) + "..."));
        tblInvoices_->setItem(row, 1, new QTableWidgetItem(formatAmount(amount)));
        tblInvoices_->setItem(row, 2, new QTableWidgetItem(description));
        tblInvoices_->setItem(row, 3, new QTableWidgetItem(formatPaymentStatus(status)));
        tblInvoices_->setItem(row, 4, new QTableWidgetItem(formatTimestamp(createdAt)));
    }
}

void LightningWidget::updatePaymentTable(const QJsonArray& payments) {
    tblPayments_->setRowCount(0);

    for (const auto& pay : payments) {
        QJsonObject payment = pay.toObject();

        int row = tblPayments_->rowCount();
        tblPayments_->insertRow(row);

        QString paymentHash = payment["payment_hash"].toString();
        qint64 amount = payment["amount_paid_msat"].toVariant().toLongLong();
        qint64 fee = payment["fee_paid_msat"].toVariant().toLongLong() / 1000;
        int hops = payment["hops"].toInt();
        QString status = payment["status"].toString();
        qint64 paidAt = payment["paid_at"].toVariant().toLongLong();

        tblPayments_->setItem(row, 0, new QTableWidgetItem(paymentHash.left(16) + "..."));
        tblPayments_->setItem(row, 1, new QTableWidgetItem(formatAmount(amount)));
        tblPayments_->setItem(row, 2, new QTableWidgetItem(QString::number(fee)));
        tblPayments_->setItem(row, 3, new QTableWidgetItem(QString::number(hops)));
        tblPayments_->setItem(row, 4, new QTableWidgetItem(formatPaymentStatus(status)));
        tblPayments_->setItem(row, 5, new QTableWidgetItem(formatTimestamp(paidAt)));
    }
}

void LightningWidget::updateWatchtowerTable(const QJsonArray& watchtowers) {
    tblWatchtowers_->setRowCount(0);

    for (const auto& wt : watchtowers) {
        QJsonObject watchtower = wt.toObject();

        int row = tblWatchtowers_->rowCount();
        tblWatchtowers_->insertRow(row);

        QString id = watchtower["id"].toString();
        QString url = watchtower["url"].toString();
        int appointments = watchtower["appointments"].toInt();
        QString status = watchtower["status"].toString();
        qint64 reward = watchtower["reward_satoshis"].toVariant().toLongLong();

        tblWatchtowers_->setItem(row, 0, new QTableWidgetItem(id));
        tblWatchtowers_->setItem(row, 1, new QTableWidgetItem(url));
        tblWatchtowers_->setItem(row, 2, new QTableWidgetItem(QString::number(appointments)));
        tblWatchtowers_->setItem(row, 3, new QTableWidgetItem(status));
        tblWatchtowers_->setItem(row, 4, new QTableWidgetItem(QString::number(reward)));
    }
}

void LightningWidget::updateNetworkInfo(const QJsonObject& info) {
    lblTotalNodes_->setText(QString::number(info["num_nodes"].toInt()));
    lblTotalChannels_->setText(QString::number(info["num_channels"].toInt()));

    qint64 totalCapacity = info["total_network_capacity"].toVariant().toLongLong();
    lblNetworkCapacity_->setText(formatAmount(totalCapacity * 1000));

    qint64 avgSize = info["avg_channel_size"].toVariant().toLongLong();
    lblAvgChannelSize_->setText(formatAmount(avgSize * 1000));
}

void LightningWidget::updateNodeTable(const QJsonArray& nodes) {
    tblNodes_->setRowCount(0);

    for (const auto& n : nodes) {
        QJsonObject node = n.toObject();

        int row = tblNodes_->rowCount();
        tblNodes_->insertRow(row);

        QString nodeId = node["node_id"].toString();
        QString alias = node["alias"].toString();
        int channels = node["num_channels"].toInt();
        qint64 capacity = node["total_capacity"].toVariant().toLongLong();
        qint64 lastSeen = node["last_update"].toVariant().toLongLong();

        tblNodes_->setItem(row, 0, new QTableWidgetItem(nodeId.left(16) + "..."));
        tblNodes_->setItem(row, 1, new QTableWidgetItem(alias));
        tblNodes_->setItem(row, 2, new QTableWidgetItem(QString::number(channels)));
        tblNodes_->setItem(row, 3, new QTableWidgetItem(formatAmount(capacity * 1000)));
        tblNodes_->setItem(row, 4, new QTableWidgetItem(formatTimestamp(lastSeen)));
    }
}

// Utility methods
QString LightningWidget::formatAmount(qint64 amount_msat) const {
    double din = amount_msat / 100000000000.0;
    if (din >= 1.0) {
        return QString("%1 DIN").arg(din, 0, 'f', 8);
    } else {
        qint64 unas = amount_msat / 1000;
        return QString("%1 una").arg(unas);
    }
}

QString LightningWidget::formatTimestamp(qint64 timestamp) const {
    if (timestamp == 0) return "N/A";
    QDateTime dt = QDateTime::fromSecsSinceEpoch(timestamp);
    return dt.toString("yyyy-MM-dd hh:mm:ss");
}

QString LightningWidget::formatChannelStatus(const QString& status) const {
    if (status == "active") return "✅ Active";
    if (status == "pending") return "⏳ Pending";
    if (status == "closing") return "🔄 Closing";
    if (status == "closed") return "❌ Closed";
    return status;
}

QString LightningWidget::formatPaymentStatus(const QString& status) const {
    if (status == "paid" || status == "success") return "✅ Paid";
    if (status == "pending") return "⏳ Pending";
    if (status == "failed") return "❌ Failed";
    if (status == "expired") return "⏰ Expired";
    return status;
}

QPixmap LightningWidget::generateQRCode(const QString& data, int size) {
    return QPixmap::fromImage(QrUtil::makeQr(data, size, 4, /*ecLevel=*/3));
}

} // namespace dinero
