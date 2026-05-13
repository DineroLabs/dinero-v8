#include "bridgewidget.h"
#include "rpcclient.h"
#include "websocketclient.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QJsonArray>
#include <QDateTime>

BridgeWidget::BridgeWidget(RpcClient* rpc, WebSocketClient* ws, QWidget* parent)
    : QWidget(parent)
    , rpc_(rpc)
    , ws_(ws)
{
    setupUi();
    setupConnections();

    // Start auto-refresh timer
    refreshTimer_.setInterval(REFRESH_INTERVAL_MS);
    connect(&refreshTimer_, &QTimer::timeout, this, &BridgeWidget::onRefreshRate);
    refreshTimer_.start();

    // Initial rate fetch
    onRefreshRate();

    statusLabel->setText("Bridge ready - Auto-refresh every 15s");
}

BridgeWidget::~BridgeWidget() {}

void BridgeWidget::setupUi()
{
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);

    // ═══════════════════════════════════════════════════════════════
    // Swap Panel
    // ═══════════════════════════════════════════════════════════════
    auto swapGroup = new QGroupBox("Asset Conversion", this);
    auto swapLayout = new QGridLayout(swapGroup);

    // Row 1: Asset pair selection
    swapLayout->addWidget(new QLabel("From:"), 0, 0);
    fromCombo = new QComboBox(this);
    fromCombo->addItems({"DIN", "BTC", "ETH", "USDT", "USDC"});
    swapLayout->addWidget(fromCombo, 0, 1);

    swapLayout->addWidget(new QLabel("To:"), 0, 2);
    toCombo = new QComboBox(this);
    toCombo->addItems({"USD", "EUR", "GBP", "BTC", "ETH", "USDT", "USDC"});
    swapLayout->addWidget(toCombo, 0, 3);

    // Row 2: Provider selection
    swapLayout->addWidget(new QLabel("Provider:"), 1, 0);
    providerCombo = new QComboBox(this);
    providerCombo->addItems({"Auto (Best Rate)", "DEX (Non-Custodial)", "Hybrid (SimpleSwap)", "Custodial (Coinbase/Binance)"});
    swapLayout->addWidget(providerCombo, 1, 1, 1, 3);

    // Row 3: Amount
    swapLayout->addWidget(new QLabel("Amount:"), 2, 0);
    amountEdit = new QLineEdit(this);
    amountEdit->setPlaceholderText("Enter amount to convert");
    swapLayout->addWidget(amountEdit, 2, 1, 1, 3);

    // Row 4: Rate display
    rateLabel = new QLabel("Rate: –", this);
    rateLabel->setStyleSheet("font-weight: bold; font-size: 14px;");
    swapLayout->addWidget(rateLabel, 3, 0, 1, 4);

    effectiveRateLabel = new QLabel("Effective rate (after fees): –", this);
    swapLayout->addWidget(effectiveRateLabel, 4, 0, 1, 4);

    feeLabel = new QLabel("Fees: –", this);
    swapLayout->addWidget(feeLabel, 5, 0, 1, 2);

    providerLabel = new QLabel("Provider: –", this);
    swapLayout->addWidget(providerLabel, 5, 2, 1, 2);

    lastUpdateLabel = new QLabel("Last update: –", this);
    lastUpdateLabel->setStyleSheet("color: #666; font-size: 10px;");
    swapLayout->addWidget(lastUpdateLabel, 6, 0, 1, 4);

    // ARP info note
    auto arpNoteLabel = new QLabel("ℹ️ Rates include ARP (Anchor Reference Price) for stable pricing during launch", this);
    arpNoteLabel->setStyleSheet("color: #FF9800; font-size: 10px; font-style: italic;");
    arpNoteLabel->setWordWrap(true);
    swapLayout->addWidget(arpNoteLabel, 7, 0, 1, 4);

    // Row 5: Action buttons
    auto buttonLayout = new QHBoxLayout;
    refreshButton = new QPushButton("⟳ Refresh Rate", this);
    refreshButton->setStyleSheet("background-color: #2196F3; color: white; padding: 8px;");
    buttonLayout->addWidget(refreshButton);

    findRouteButton = new QPushButton("🔍 Find Best Route", this);
    findRouteButton->setStyleSheet("background-color: #FF9800; color: white; padding: 8px;");
    buttonLayout->addWidget(findRouteButton);

    convertButton = new QPushButton("💱 Convert", this);
    convertButton->setStyleSheet("background-color: #4CAF50; color: white; padding: 8px; font-weight: bold;");
    buttonLayout->addWidget(convertButton);

    swapLayout->addLayout(buttonLayout, 7, 0, 1, 4);

    mainLayout->addWidget(swapGroup);

    // ═══════════════════════════════════════════════════════════════
    // Route Visualization
    // ═══════════════════════════════════════════════════════════════
    auto routeGroup = new QGroupBox("Multi-Hop Route (if applicable)", this);
    auto routeLayout = new QVBoxLayout(routeGroup);

    routeDescLabel = new QLabel("No route calculated yet", this);
    routeDescLabel->setStyleSheet("font-style: italic; color: #666;");
    routeLayout->addWidget(routeDescLabel);

    routeTable = new QTableWidget(this);
    routeTable->setColumnCount(5);
    routeTable->setHorizontalHeaderLabels({"From", "To", "Rate", "Fee (bps)", "Provider"});
    routeTable->horizontalHeader()->setStretchLastSection(true);
    routeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    routeTable->setMaximumHeight(150);
    routeTable->setVisible(false);  // Hidden until route found
    routeLayout->addWidget(routeTable);

    mainLayout->addWidget(routeGroup);

    // ═══════════════════════════════════════════════════════════════
    // Status Bar
    // ═══════════════════════════════════════════════════════════════
    statusLabel = new QLabel("Initializing...", this);
    statusLabel->setStyleSheet("background-color: #f0f0f0; padding: 8px; border-radius: 4px;");
    mainLayout->addWidget(statusLabel);

    progressBar = new QProgressBar(this);
    progressBar->setVisible(false);
    mainLayout->addWidget(progressBar);

    mainLayout->addStretch();
    setLayout(mainLayout);
}

void BridgeWidget::setupConnections()
{
    // Button connections
    connect(refreshButton, &QPushButton::clicked, this, &BridgeWidget::onRefreshRate);
    connect(convertButton, &QPushButton::clicked, this, &BridgeWidget::onConvertClicked);
    connect(findRouteButton, &QPushButton::clicked, this, &BridgeWidget::onFindRouteClicked);
    connect(providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &BridgeWidget::onProviderSelected);

    // RPC client connections
    connect(rpc_, &RpcClient::rpcResult, this, &BridgeWidget::onRpcResult);
    connect(rpc_, &RpcClient::rpcError, this, &BridgeWidget::onRpcError);

    // WebSocket connections (for live rate updates)
    if (ws_) {
        connect(ws_, &WebSocketClient::subscriptionEvent, this, &BridgeWidget::onWebSocketEvent);
        ws_->subscribe("bridge_rate_update");
    }

    // Combo box changes trigger rate refresh
    connect(fromCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &BridgeWidget::onRefreshRate);
    connect(toCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &BridgeWidget::onRefreshRate);
}

void BridgeWidget::callRpc(const QString& method, const QJsonArray& params)
{
    if (!rpc_) return;
    progressBar->setVisible(true);
    progressBar->setRange(0, 0);  // Indeterminate progress
    rpc_->call(method, params);
}

void BridgeWidget::onRefreshRate()
{
    QString from = fromCombo->currentText();
    QString to = toCombo->currentText();

    QJsonArray params;
    params.append(from);
    params.append(to);

    statusLabel->setText("Fetching rate for " + from + " → " + to + "...");
    callRpc("bridge.getrate", params);
}

void BridgeWidget::onFindRouteClicked()
{
    QString from = fromCombo->currentText();
    QString to = toCombo->currentText();

    QJsonArray params;
    params.append(from);
    params.append(to);

    statusLabel->setText("Finding best route for " + from + " → " + to + "...");
    callRpc("bridge.findroute", params);
}

void BridgeWidget::onConvertClicked()
{
    QString from = fromCombo->currentText();
    QString to = toCombo->currentText();
    bool ok;
    double amount = amountEdit->text().toDouble(&ok);

    if (!ok || amount <= 0) {
        QMessageBox::warning(this, "Invalid Input", "Please enter a valid amount greater than 0.");
        return;
    }

    // Determine provider hint
    QString providerHint;
    int providerIndex = providerCombo->currentIndex();
    if (providerIndex == 1) providerHint = "dex";
    else if (providerIndex == 2) providerHint = "hybrid";
    else if (providerIndex == 3) providerHint = "custodial";
    // Index 0 = auto, no hint

    QJsonArray params;
    params.append(from);
    params.append(to);
    params.append(amount);
    if (!providerHint.isEmpty()) {
        params.append(providerHint);
    }

    statusLabel->setText("Converting " + QString::number(amount) + " " + from + " → " + to + "...");
    callRpc("bridge.convert", params);
}

void BridgeWidget::onProviderSelected(int index)
{
    // Auto-refresh when provider changes
    onRefreshRate();
}

void BridgeWidget::updateRateDisplay(const QJsonObject& rateInfo)
{
    currentRateInfo_ = rateInfo;

    QString from = rateInfo["from"].toString();
    QString to = rateInfo["to"].toString();
    double rate = rateInfo["rate"].toDouble();
    QString provider = rateInfo.value("provider").toString("–");

    rateLabel->setText(QString("Rate: 1 %1 = %2 %3")
                          .arg(from)
                          .arg(formatRate(rate))
                          .arg(to));

    providerLabel->setText("Provider: " + provider);

    // Check for route info (multi-hop)
    if (rateInfo.contains("effective_rate")) {
        double effectiveRate = rateInfo["effective_rate"].toDouble();
        effectiveRateLabel->setText(QString("Effective rate (after fees): 1 %1 = %2 %3")
                                       .arg(from)
                                       .arg(formatRate(effectiveRate))
                                       .arg(to));
    } else {
        effectiveRateLabel->setText("Effective rate: (direct conversion, minimal fees)");
    }

    if (rateInfo.contains("total_fee_bps")) {
        double feeBps = rateInfo["total_fee_bps"].toDouble();
        feeLabel->setText(QString("Fees: %1%").arg(feeBps / 100.0, 0, 'f', 2));
    } else {
        feeLabel->setText("Fees: ~1.5%");
    }

    lastUpdateLabel->setText("Last update: " + QDateTime::currentDateTime().toString("hh:mm:ss"));
    progressBar->setVisible(false);
    statusLabel->setText("Rate updated successfully");
}

void BridgeWidget::updateRouteDisplay(const QJsonObject& routeInfo)
{
    currentRoute_ = routeInfo;

    if (!routeInfo.contains("hops")) {
        routeTable->setVisible(false);
        routeDescLabel->setText("Direct conversion (1 hop)");
        return;
    }

    QJsonArray hops = routeInfo["hops"].toArray();
    int hopCount = routeInfo["hop_count"].toInt();

    routeDescLabel->setText(routeInfo["route"].toString() +
                           QString(" (%1 hops)").arg(hopCount));

    routeTable->setRowCount(hops.size());
    for (int i = 0; i < hops.size(); ++i) {
        QJsonObject hop = hops[i].toObject();

        routeTable->setItem(i, 0, new QTableWidgetItem(hop["from"].toString()));
        routeTable->setItem(i, 1, new QTableWidgetItem(hop["to"].toString()));
        routeTable->setItem(i, 2, new QTableWidgetItem(formatRate(hop["rate"].toDouble())));
        routeTable->setItem(i, 3, new QTableWidgetItem(QString::number(hop["fee_bps"].toDouble())));
        routeTable->setItem(i, 4, new QTableWidgetItem(hop["provider"].toString()));
    }

    routeTable->setVisible(true);
    routeTable->resizeColumnsToContents();
}

void BridgeWidget::onRpcResult(const QString& method, const QJsonValue& result)
{
    QJsonObject obj = result.toObject();

    if (method == "bridge.getrate") {
        if (obj.contains("rate")) {
            updateRateDisplay(obj);
        } else if (obj.contains("error")) {
            statusLabel->setText("Error: " + obj["error"].toString());
            progressBar->setVisible(false);
        }
    }
    else if (method == "bridge.findroute") {
        if (obj.contains("route")) {
            updateRateDisplay(obj);
            updateRouteDisplay(obj);
        } else if (obj.contains("error")) {
            statusLabel->setText("No route found: " + obj["error"].toString());
            progressBar->setVisible(false);
        }
    }
    else if (method == "bridge.convert") {
        progressBar->setVisible(false);

        if (obj.contains("success") && obj["success"].toBool()) {
            double received = obj["received_amount"].toDouble();
            QString to = obj.value("to").toString(toCombo->currentText());
            QString txid = obj["txid"].toString();

            QMessageBox::information(this, "Conversion Successful",
                QString("Conversion completed!\n\n"
                       "Received: %1 %2\n"
                       "Transaction ID: %3\n"
                       "Provider: %4")
                       .arg(formatAmount(received))
                       .arg(to)
                       .arg(txid)
                       .arg(obj["provider"].toString()));

            statusLabel->setText("✅ Conversion successful: " + txid);
        } else {
            QString error = obj.value("error").toString("Unknown error");
            QMessageBox::warning(this, "Conversion Failed", "Conversion failed: " + error);
            statusLabel->setText("❌ Conversion failed");
        }
    }
}

void BridgeWidget::onRpcError(const QString& method, int code, const QString& message)
{
    progressBar->setVisible(false);
    statusLabel->setText(QString("RPC Error [%1]: %2").arg(code).arg(message));

    // Don't show popup dialogs - user gets error in MainWindow status bar
    // Just update the widget's own status label silently
}

void BridgeWidget::onWebSocketEvent(const QString& topic, const QJsonObject& data)
{
    if (topic == "bridge_rate_update") {
        // Live rate update from EventBus
        QString from = data["from"].toString();
        QString to = data["to"].toString();

        if (from == fromCombo->currentText() && to == toCombo->currentText()) {
            updateRateDisplay(data);
            statusLabel->setText("Rate updated via WebSocket");
        }
    }
}

QString BridgeWidget::formatRate(double rate, int decimals)
{
    if (rate < 0.000001) {
        return QString::number(rate, 'e', decimals);
    } else if (rate < 0.01) {
        return QString::number(rate, 'f', 8);
    } else if (rate < 1.0) {
        return QString::number(rate, 'f', 6);
    } else {
        return QString::number(rate, 'f', 2);
    }
}

QString BridgeWidget::formatAmount(double amount)
{
    if (amount < 0.01) {
        return QString::number(amount, 'f', 8);
    } else {
        return QString::number(amount, 'f', 4);
    }
}
