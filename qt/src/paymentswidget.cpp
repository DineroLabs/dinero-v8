#include "paymentswidget.h"
#include "rpcclient.h"
#include "websocketclient.h"
#include "QrUtil.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QJsonArray>
#include <QDateTime>
#include <QPainter>
#include <QClipboard>
#include <QApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QSettings>

PaymentsWidget::PaymentsWidget(RpcClient* rpc, WebSocketClient* ws, QWidget* parent)
    : QWidget(parent)
    , rpc_(rpc)
    , ws_(ws)
    , selectedCurrency_("USD")
    , arpPriceUsd_(0.10)      // Default $0.10 USD/DIN
    , arpConfidence_(0.0)     // Pure ARP initially
    , arpSource_("arp_only")
{
    // Initialize default rates for global currencies
    // Americas
    fiatRates_["USD"] = 0.98;   // United States Dollar
    fiatRates_["BRL"] = 4.95;   // Brazilian Real
    fiatRates_["MXN"] = 17.0;   // Mexican Peso
    fiatRates_["ARS"] = 350.0;  // Argentine Peso

    // Europe
    fiatRates_["EUR"] = 0.92;   // Euro
    fiatRates_["GBP"] = 0.78;   // British Pound
    fiatRates_["BAM"] = 1.80;   // Bosnia Mark
    fiatRates_["RSD"] = 108.0;  // Serbian Dinar

    // Asia-Pacific
    fiatRates_["JPY"] = 145.0;  // Japanese Yen
    fiatRates_["CNY"] = 7.10;   // Chinese Yuan
    fiatRates_["INR"] = 83.0;   // Indian Rupee
    fiatRates_["PKR"] = 278.0;  // Pakistani Rupee
    fiatRates_["THB"] = 35.0;   // Thai Baht
    fiatRates_["VND"] = 24500;  // Vietnamese Dong
    fiatRates_["IDR"] = 15600;  // Indonesian Rupiah

    // Middle East
    fiatRates_["SAR"] = 3.67;   // Saudi Riyal
    fiatRates_["AED"] = 3.60;   // UAE Dirham
    fiatRates_["QAR"] = 3.56;   // Qatari Riyal
    fiatRates_["TRY"] = 32.0;   // Turkish Lira
    fiatRates_["IRR"] = 42000;  // Iranian Rial

    // Africa
    fiatRates_["ZAR"] = 18.5;   // South African Rand
    fiatRates_["EGP"] = 48.0;   // Egyptian Pound
    fiatRates_["NGN"] = 1550;   // Nigerian Naira
    fiatRates_["KES"] = 129.0;  // Kenyan Shilling

    // Eastern Europe
    fiatRates_["RUB"] = 92.0;   // Russian Ruble

    // Cryptocurrencies
    fiatRates_["BTC"] = 0.000023;  // Bitcoin
    fiatRates_["ETH"] = 0.00038;   // Ethereum
    fiatRates_["USDT"] = 0.98;     // Tether
    fiatRates_["USDC"] = 0.98;     // USD Coin
    fiatRates_["SOL"] = 0.012;     // Solana
    fiatRates_["BNB"] = 0.0025;    // Binance Coin
    fiatRates_["XRP"] = 1.85;      // Ripple
    fiatRates_["ADA"] = 2.45;      // Cardano
    fiatRates_["DOGE"] = 12.5;     // Dogecoin

    setupUi();
    setupConnections();

    // Load saved favorite currencies
    loadFavorites();

    // Start fiat value update timer
    fiatUpdateTimer_.setInterval(FIAT_UPDATE_INTERVAL_MS);
    connect(&fiatUpdateTimer_, &QTimer::timeout, this, &PaymentsWidget::updateFiatValues);
    fiatUpdateTimer_.start();

    // Initial fiat rate fetch
    updateFiatValues();

    statusLabel->setText("DineroPay ready - Create your first invoice");
}

PaymentsWidget::~PaymentsWidget() {}

void PaymentsWidget::setupUi()
{
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);

    // ═══════════════════════════════════════════════════════════════
    // Invoice Creation Panel
    // ═══════════════════════════════════════════════════════════════
    auto invoiceGroup = new QGroupBox("Create Payment Invoice", this);
    auto invoiceLayout = new QGridLayout(invoiceGroup);

    invoiceLayout->addWidget(new QLabel("Amount (DNR):"), 0, 0);
    amountEdit = new QLineEdit(this);
    amountEdit->setPlaceholderText("e.g., 50.0");
    invoiceLayout->addWidget(amountEdit, 0, 1);

    invoiceLayout->addWidget(new QLabel("Label (optional):"), 1, 0);
    labelEdit = new QLineEdit(this);
    labelEdit->setPlaceholderText("e.g., Order #12345");
    invoiceLayout->addWidget(labelEdit, 1, 1);

    invoiceLayout->addWidget(new QLabel("Address (optional):"), 2, 0);
    addressEdit = new QLineEdit(this);
    addressEdit->setPlaceholderText("Leave blank for auto-generated");
    invoiceLayout->addWidget(addressEdit, 2, 1);

    createButton = new QPushButton("📄 Create Invoice", this);
    createButton->setStyleSheet("background-color: #4CAF50; color: white; padding: 10px; font-weight: bold;");
    invoiceLayout->addWidget(createButton, 3, 0, 1, 2);

    mainLayout->addWidget(invoiceGroup);

    // ═══════════════════════════════════════════════════════════════
    // Multi-Fiat Selector & On-Ramp
    // ═══════════════════════════════════════════════════════════════
    auto fiatGroup = new QGroupBox("Fiat Conversion & Buy DIN", this);
    auto fiatLayout = new QHBoxLayout(fiatGroup);

    // Search box
    fiatLayout->addWidget(new QLabel("Search:"));
    currencySearch = new QLineEdit(this);
    currencySearch->setPlaceholderText("Type to filter currencies...");
    currencySearch->setMaximumWidth(180);
    fiatLayout->addWidget(currencySearch);

    // Currency dropdown
    currencyCombo = new QComboBox(this);
    currencyCombo->setEditable(false);
    populateCurrencyCombo();  // Populate with all currencies
    fiatLayout->addWidget(currencyCombo);

    // Favorite/Pin button
    favoriteButton = new QPushButton("⭐", this);
    favoriteButton->setMaximumWidth(40);
    favoriteButton->setToolTip("Add/Remove from favorites");
    favoriteButton->setStyleSheet("font-size: 16px;");
    fiatLayout->addWidget(favoriteButton);

    fiatLayout->addStretch();

    buyButton = new QPushButton("💳 Buy DIN", this);
    buyButton->setStyleSheet("background-color: #2196F3; color: white; padding: 8px; font-weight: bold;");
    buyButton->setToolTip("Open MoonPay/Ramp on-ramp in browser");
    fiatLayout->addWidget(buyButton);

    mainLayout->addWidget(fiatGroup);

    // ═══════════════════════════════════════════════════════════════
    // Payment History Table
    // ═══════════════════════════════════════════════════════════════
    auto historyGroup = new QGroupBox("Payment History", this);
    auto historyLayout = new QVBoxLayout(historyGroup);

    paymentTable = new QTableWidget(this);
    paymentTable->setColumnCount(6);
    paymentTable->setHorizontalHeaderLabels({
        "Address", "Amount (DNR)", "USD Value", "Status", "Risk", "TXID"
    });
    paymentTable->horizontalHeader()->setStretchLastSection(true);
    paymentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    paymentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    paymentTable->setSelectionMode(QAbstractItemView::SingleSelection);
    historyLayout->addWidget(paymentTable);

    mainLayout->addWidget(historyGroup);

    // ═══════════════════════════════════════════════════════════════
    // Selected Payment Details
    // ═══════════════════════════════════════════════════════════════
    auto detailsGroup = new QGroupBox("Selected Invoice Details", this);
    auto detailsLayout = new QHBoxLayout(detailsGroup);

    // Left: QR Code
    auto qrLayout = new QVBoxLayout;
    qrLabel = new QLabel(this);
    qrLabel->setFixedSize(200, 200);
    qrLabel->setAlignment(Qt::AlignCenter);
    qrLabel->setStyleSheet("border: 2px solid #ccc; background-color: white;");
    qrLabel->setText("No invoice selected");
    qrLayout->addWidget(qrLabel);

    showQrButton = new QPushButton("📱 Show QR Code", this);
    showQrButton->setEnabled(false);
    qrLayout->addWidget(showQrButton);

    detailsLayout->addLayout(qrLayout);

    // Right: Details
    auto infoLayout = new QVBoxLayout;
    selectedAddressLabel = new QLabel("Address: –", this);
    selectedAddressLabel->setWordWrap(true);
    infoLayout->addWidget(selectedAddressLabel);

    selectedAmountLabel = new QLabel("Amount: –", this);
    infoLayout->addWidget(selectedAmountLabel);

    fiatValueLabel = new QLabel("Fiat value: –", this);
    fiatValueLabel->setStyleSheet("font-weight: bold; color: #2196F3;");
    infoLayout->addWidget(fiatValueLabel);

    checkStatusButton = new QPushButton("🔍 Check Payment Status", this);
    checkStatusButton->setEnabled(false);
    infoLayout->addWidget(checkStatusButton);

    infoLayout->addStretch();
    detailsLayout->addLayout(infoLayout);

    mainLayout->addWidget(detailsGroup);

    // ═══════════════════════════════════════════════════════════════
    // ARP (Anchor Reference Price) Display
    // ═══════════════════════════════════════════════════════════════
    auto arpGroup = new QGroupBox("📊 Price Reference (ARP)", this);
    auto arpLayout = new QHBoxLayout(arpGroup);

    // ARP Info
    arpInfoLabel = new QLabel("Loading ARP...", this);
    arpInfoLabel->setToolTip("Anchor Reference Price - Soft price guide for early market phase");
    arpLayout->addWidget(arpInfoLabel);

    arpLayout->addStretch();

    // Current ARP Price
    arpPriceLabel = new QLabel("$0.10 USD/DIN", this);
    arpPriceLabel->setStyleSheet("font-size: 14pt; font-weight: bold; color: #4CAF50;");
    arpLayout->addWidget(arpPriceLabel);

    arpLayout->addStretch();

    // Blend Indicator
    arpBlendLabel = new QLabel("100% ARP", this);
    arpBlendLabel->setStyleSheet("color: #FF9800; font-weight: bold;");
    arpBlendLabel->setToolTip("Shows the blend ratio of ARP vs Market price");
    arpLayout->addWidget(arpBlendLabel);

    mainLayout->addWidget(arpGroup);

    // ═══════════════════════════════════════════════════════════════
    // Status Bar
    // ═══════════════════════════════════════════════════════════════
    statusLabel = new QLabel("Initializing...", this);
    statusLabel->setStyleSheet("background-color: #f0f0f0; padding: 8px; border-radius: 4px;");
    mainLayout->addWidget(statusLabel);

    setLayout(mainLayout);
}

void PaymentsWidget::setupConnections()
{
    // Button connections
    connect(createButton, &QPushButton::clicked, this, &PaymentsWidget::onCreateInvoice);
    connect(showQrButton, &QPushButton::clicked, this, &PaymentsWidget::onShowQrCode);
    connect(checkStatusButton, &QPushButton::clicked, this, &PaymentsWidget::onCheckStatus);
    connect(buyButton, &QPushButton::clicked, this, &PaymentsWidget::onBuyDnr);
    connect(favoriteButton, &QPushButton::clicked, this, &PaymentsWidget::onToggleFavorite);

    // Currency selector
    connect(currencyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PaymentsWidget::onCurrencyChanged);

    // Search box
    connect(currencySearch, &QLineEdit::textChanged, this, &PaymentsWidget::onSearchCurrency);

    // Table selection
    connect(paymentTable, &QTableWidget::itemSelectionChanged,
            this, &PaymentsWidget::onTableSelectionChanged);

    // RPC client connections
    connect(rpc_, &RpcClient::rpcResult, this, &PaymentsWidget::onRpcResult);
    connect(rpc_, &RpcClient::rpcError, this, &PaymentsWidget::onRpcError);

    // WebSocket connections (for live payment events)
    if (ws_) {
        connect(ws_, &WebSocketClient::subscriptionEvent, this, &PaymentsWidget::onWebSocketEvent);
        ws_->subscribe("transaction_received");
        ws_->subscribe("payment_update");
    }
}

void PaymentsWidget::callRpc(const QString& method, const QJsonArray& params)
{
    if (!rpc_) return;
    rpc_->call(method, params);
}

void PaymentsWidget::onCreateInvoice()
{
    bool ok;
    double amount = amountEdit->text().toDouble(&ok);

    if (!ok || amount <= 0) {
        QMessageBox::warning(this, "Invalid Amount", "Please enter a valid amount greater than 0.");
        return;
    }

    QString address = addressEdit->text().trimmed();
    QString label = labelEdit->text().trimmed();

    // Build params as JSON object
    QJsonObject paramsObj;
    paramsObj["expected_amount"] = amount;

    if (!address.isEmpty()) {
        paramsObj["address"] = address;
    }
    if (!label.isEmpty()) {
        paramsObj["label"] = label;
    }

    QJsonArray params;
    params.append(paramsObj);

    statusLabel->setText("Creating invoice for " + QString::number(amount) + " DIN...");
    callRpc("payment.watch", params);
}

void PaymentsWidget::onCheckStatus()
{
    int row = paymentTable->currentRow();
    if (row < 0) return;

    QString address = paymentTable->item(row, 0)->text();
    QString subscriptionId = subscriptionIds_.value(address);

    if (subscriptionId.isEmpty()) {
        QMessageBox::warning(this, "No Subscription", "No active subscription found for this address.");
        return;
    }

    QJsonArray params;
    params.append(subscriptionId);

    statusLabel->setText("Checking payment status...");
    callRpc("payment.status", params);
}

void PaymentsWidget::onShowQrCode()
{
    int row = paymentTable->currentRow();
    if (row < 0) return;

    QString address = paymentTable->item(row, 0)->text();
    double amount = paymentTable->item(row, 1)->text().toDouble();

    // Generate QR code with payment URI
    QString paymentUri = QString("dinero:%1?amount=%2")
                            .arg(address)
                            .arg(amount);

    QPixmap qr = generateQrCode(paymentUri, 300);
    qrLabel->setPixmap(qr.scaled(200, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    // Copy address to clipboard
    QApplication::clipboard()->setText(address);

    QMessageBox::information(this, "QR Code Generated",
        QString("QR code generated for payment!\n\n"
               "Address: %1\n"
               "Amount: %2 DIN\n\n"
               "Address copied to clipboard.")
               .arg(address)
               .arg(amount));
}

void PaymentsWidget::onTableSelectionChanged()
{
    int row = paymentTable->currentRow();
    if (row < 0) {
        showQrButton->setEnabled(false);
        checkStatusButton->setEnabled(false);
        selectedAddressLabel->setText("Address: –");
        selectedAmountLabel->setText("Amount: –");
        fiatValueLabel->setText("Fiat value: –");
        qrLabel->clear();
        qrLabel->setText("No invoice selected");
        return;
    }

    QString address = paymentTable->item(row, 0)->text();
    double amount = paymentTable->item(row, 1)->text().toDouble();

    selectedAddressLabel->setText("Address: " + address);
    selectedAmountLabel->setText("Amount: " + formatDnrAmount(amount) + " DIN");

    // Use selected currency rate
    QString currency = selectedCurrency_;
    QString symbol = getCurrencySymbol(currency);
    double rate = fiatRates_.value(currency, 1.0);
    double fiatValue = amount * rate;
    fiatValueLabel->setText(QString("≈ %1%2 %3").arg(symbol).arg(formatFiatAmount(fiatValue)).arg(currency));

    showQrButton->setEnabled(true);
    checkStatusButton->setEnabled(true);

    // Auto-generate QR code preview
    QString paymentUri = QString("dinero:%1?amount=%2").arg(address).arg(amount);
    QPixmap qr = generateQrCode(paymentUri, 200);
    qrLabel->setPixmap(qr);
}

void PaymentsWidget::appendPaymentRow(const QJsonObject& payment)
{
    QString address = payment["address"].toString();
    double amount = payment["expected_amount"].toDouble();
    QString subscriptionId = payment["subscription_id"].toString();

    // Store invoice data
    activeInvoices_[address] = payment;
    subscriptionIds_[address] = subscriptionId;

    int row = paymentTable->rowCount();
    paymentTable->insertRow(row);

    paymentTable->setItem(row, 0, new QTableWidgetItem(address));
    paymentTable->setItem(row, 1, new QTableWidgetItem(formatDnrAmount(amount)));

    // Use selected currency rate
    QString currency = selectedCurrency_;
    QString symbol = getCurrencySymbol(currency);
    double rate = fiatRates_.value(currency, 1.0);
    double fiatValue = amount * rate;
    paymentTable->setItem(row, 2, new QTableWidgetItem(symbol + formatFiatAmount(fiatValue)));

    auto statusItem = new QTableWidgetItem("⏳ WAITING");
    statusItem->setForeground(QBrush(QColor("#FF9800")));
    paymentTable->setItem(row, 3, statusItem);

    paymentTable->setItem(row, 4, new QTableWidgetItem("Low"));
    paymentTable->setItem(row, 5, new QTableWidgetItem("–"));

    // Auto-select new invoice
    paymentTable->selectRow(row);
}

void PaymentsWidget::updatePaymentStatus(const QString& address, const QJsonObject& status)
{
    for (int row = 0; row < paymentTable->rowCount(); ++row) {
        if (paymentTable->item(row, 0)->text() == address) {
            QString state = status["status"].toString();
            QString txid = status["txid"].toString();
            int confirmations = status["confirmations"].toInt();
            QString risk = status.value("risk_level").toString("Low");

            QString statusText;
            QColor statusColor;

            if (state == "paid" || confirmations >= 1) {
                statusText = QString("✅ PAID (%1 conf)").arg(confirmations);
                statusColor = QColor("#4CAF50");
            } else if (state == "pending") {
                statusText = "🔄 PENDING";
                statusColor = QColor("#2196F3");
            } else {
                statusText = "⏳ WAITING";
                statusColor = QColor("#FF9800");
            }

            auto statusItem = new QTableWidgetItem(statusText);
            statusItem->setForeground(QBrush(statusColor));
            paymentTable->setItem(row, 3, statusItem);

            paymentTable->setItem(row, 4, new QTableWidgetItem(risk));

            if (!txid.isEmpty()) {
                paymentTable->setItem(row, 5, new QTableWidgetItem(txid.left(16) + "..."));
            }

            break;
        }
    }
}

void PaymentsWidget::onRpcResult(const QString& method, const QJsonValue& result)
{
    QJsonObject obj = result.toObject();

    if (method == "payment.watch") {
        if (obj.contains("success") && obj["success"].toBool()) {
            appendPaymentRow(obj);
            statusLabel->setText("✅ Invoice created: " + obj["subscription_id"].toString());

            // Clear input fields
            amountEdit->clear();
            labelEdit->clear();
            addressEdit->clear();
        } else {
            QString error = obj.value("message").toString("Unknown error");
            QMessageBox::warning(this, "Invoice Creation Failed", error);
            statusLabel->setText("❌ Failed to create invoice");
        }
    }
    else if (method == "payment.status") {
        QString address = obj["address"].toString();
        updatePaymentStatus(address, obj);
        statusLabel->setText("Status updated for " + address);
    }
    else if (method == "bridge.getrate") {
        if (obj.contains("rate")) {
            QString to = obj["to"].toString();
            double rate = obj["rate"].toDouble();

            // Store the rate for this currency
            fiatRates_[to] = rate;

            // If this is the selected currency, update displays immediately
            if (to == selectedCurrency_) {
                updateFiatValues();
            }
        }
    }
    else if (method == "bridge.getarp") {
        // Handle ARP (Anchor Reference Price) response
        if (obj.contains("price_usd")) {
            arpPriceUsd_ = obj["price_usd"].toDouble();
            arpConfidence_ = obj.contains("confidence") ? obj["confidence"].toDouble() : 0.0;
            arpSource_ = obj.contains("source") ? obj["source"].toString() : "unknown";

            // Update ARP display labels
            arpPriceLabel->setText(QString("$%1 USD/DIN")
                .arg(arpPriceUsd_, 0, 'f', 2));

            // Format blend ratio
            if (arpConfidence_ == 0.0) {
                arpBlendLabel->setText("100% ARP");
                arpBlendLabel->setStyleSheet("color: #FF9800; font-weight: bold;");
                arpInfoLabel->setText("📌 Pure ARP (pre-launch)");
            } else if (arpConfidence_ >= 0.9) {
                arpBlendLabel->setText("100% Market");
                arpBlendLabel->setStyleSheet("color: #4CAF50; font-weight: bold;");
                arpInfoLabel->setText("📈 Market-driven pricing");
            } else {
                int arpPct = static_cast<int>((1.0 - arpConfidence_) * 100);
                int marketPct = static_cast<int>(arpConfidence_ * 100);
                arpBlendLabel->setText(QString("%1% ARP + %2% Market")
                    .arg(arpPct).arg(marketPct));
                arpBlendLabel->setStyleSheet("color: #2196F3; font-weight: bold;");
                arpInfoLabel->setText("🔄 Blended pricing (transitioning)");
            }
        }
    }
}

void PaymentsWidget::onRpcError(const QString& method, int code, const QString& message)
{
    statusLabel->setText(QString("Error [%1]: %2").arg(code).arg(message));

    // Don't show popup dialogs - user gets error in MainWindow status bar
    // Error is already shown in statusLabel above
}

void PaymentsWidget::onWebSocketEvent(const QString& topic, const QJsonObject& data)
{
    if (topic == "transaction_received" || topic == "payment_update") {
        QString address = data["address"].toString();
        QString txid = data["txid"].toString();
        double amount = data["amount"].toDouble() / 100000000.0;  // una to DIN

        if (activeInvoices_.contains(address)) {
            QJsonObject statusUpdate;
            statusUpdate["address"] = address;
            statusUpdate["status"] = "paid";
            statusUpdate["txid"] = txid;
            statusUpdate["confirmations"] = data.value("confirmations").toInt(0);
            statusUpdate["risk_level"] = data.value("risk_level").toString("Low");

            updatePaymentStatus(address, statusUpdate);

            // Show notification
            statusLabel->setText(QString("🔔 Payment received: %1 DIN to %2")
                               .arg(formatDnrAmount(amount))
                               .arg(address.left(16) + "..."));

            // Play sound or show system notification here if desired
        }
    }
}

void PaymentsWidget::updateFiatValues()
{
    // Fetch ARP (Anchor Reference Price) data
    callRpc("bridge.getarp", QJsonArray());

    // Fetch current rates for all supported currencies
    QStringList currencies = getSupportedCurrencies();
    for (const QString& currency : currencies) {
        QJsonArray params;
        params.append("DIN");
        params.append(currency);
        callRpc("bridge.getrate", params);
    }

    // Update all table rows with new fiat values (using selected currency)
    QString currency = selectedCurrency_;
    QString symbol = getCurrencySymbol(currency);
    double rate = fiatRates_.value(currency, 1.0);

    for (int row = 0; row < paymentTable->rowCount(); ++row) {
        double dnrAmount = paymentTable->item(row, 1)->text().toDouble();
        double fiatValue = dnrAmount * rate;
        paymentTable->setItem(row, 2, new QTableWidgetItem(symbol + formatFiatAmount(fiatValue)));
    }

    // Update selected invoice fiat value
    int row = paymentTable->currentRow();
    if (row >= 0) {
        double amount = paymentTable->item(row, 1)->text().toDouble();
        double fiatValue = amount * rate;
        fiatValueLabel->setText(QString("≈ %1%2 %3")
            .arg(symbol)
            .arg(formatFiatAmount(fiatValue))
            .arg(currency));
    }
}

QString PaymentsWidget::formatDnrAmount(double amount)
{
    return QString::number(amount, 'f', 4);
}

QString PaymentsWidget::formatFiatAmount(double amount)
{
    return QString::number(amount, 'f', 2);
}

QPixmap PaymentsWidget::generateQrCode(const QString& data, int size)
{
    // Use QrUtil to generate QR code with the Dinero logo
    QImage qrImage = QrUtil::makeQrWithLogo(data, "Dinero-Coin.png", size, 2);
    return QPixmap::fromImage(qrImage);
}

void PaymentsWidget::onBuyDnr()
{
    QString currency = currencyCombo->currentText();

    // MoonPay on-ramp URL (opens in system browser)
    // Note: Replace YOUR_API_KEY with actual MoonPay API key for production
    QString url = QString("https://buy.moonpay.com?apiKey=YOUR_API_KEY&currencyCode=%1&cryptoCurrencyCode=DNR")
                     .arg(currency.toLower());

    // Alternative: Ramp Network
    // QString url = QString("https://app.ramp.network/buy?defaultAsset=DNR&fiatCurrency=%1").arg(currency);

    QDesktopServices::openUrl(QUrl(url));

    statusLabel->setText(QString("✅ Opening on-ramp for %1 in browser...").arg(currency));
}

void PaymentsWidget::onCurrencyChanged()
{
    // Extract currency code from combo box data
    selectedCurrency_ = currencyCombo->currentData().toString();

    // Immediately update all displayed fiat values
    updateFiatValues();

    statusLabel->setText(QString("Currency changed to %1").arg(selectedCurrency_));
}

QString PaymentsWidget::getCurrencySymbol(const QString& currency)
{
    static QMap<QString, QString> symbols = {
        // Major Fiat
        {"USD", "$"},
        {"EUR", "€"},
        {"GBP", "£"},

        // Americas
        {"BRL", "R$"},
        {"MXN", "$"},
        {"ARS", "$"},

        // Asia-Pacific
        {"JPY", "¥"},
        {"CNY", "¥"},
        {"INR", "₹"},
        {"PKR", "₨"},
        {"THB", "฿"},
        {"VND", "₫"},
        {"IDR", "Rp"},

        // Middle East
        {"SAR", "﷼"},
        {"AED", "د.إ"},
        {"QAR", "﷼"},
        {"TRY", "₺"},
        {"IRR", "﷼"},

        // Africa
        {"ZAR", "R"},
        {"EGP", "£"},
        {"NGN", "₦"},
        {"KES", "KSh"},

        // Eastern Europe
        {"RUB", "₽"},
        {"BAM", "KM"},
        {"RSD", "дин"},

        // Cryptocurrencies
        {"BTC", "₿"},
        {"ETH", "Ξ"},
        {"USDT", "₮"},
        {"USDC", "$"},
        {"SOL", "◎"},
        {"BNB", "⚡"},
        {"XRP", "✕"},
        {"ADA", "₳"},
        {"DOGE", "Ð"}
    };

    return symbols.value(currency, currency);
}

QStringList PaymentsWidget::getSupportedCurrencies()
{
    return {
        // Fiat
        "USD", "EUR", "GBP", "BRL", "MXN", "ARS",
        "JPY", "CNY", "INR", "PKR", "THB", "VND", "IDR",
        "SAR", "AED", "QAR", "TRY", "IRR",
        "ZAR", "EGP", "NGN", "KES",
        "RUB", "BAM", "RSD",
        // Crypto
        "BTC", "ETH", "USDT", "USDC", "SOL", "BNB", "XRP", "ADA", "DOGE"
    };
}

void PaymentsWidget::populateCurrencyCombo()
{
    currencyCombo->clear();

    // === Favorites Section ===
    if (!favoriteCurrencies_.isEmpty()) {
        currencyCombo->addItem("⭐ — FAVORITES —", "");
        currencyCombo->model()->setData(currencyCombo->model()->index(0, 0),
                                        QVariant(0), Qt::UserRole - 1);  // Make non-selectable

        for (const QString& code : favoriteCurrencies_) {
            QString emoji, name;
            if (code == "USD") { emoji = "🇺🇸"; name = "US Dollar"; }
            else if (code == "EUR") { emoji = "🇪🇺"; name = "Euro"; }
            else if (code == "GBP") { emoji = "🇬🇧"; name = "Pound Sterling"; }
            else if (code == "BRL") { emoji = "🇧🇷"; name = "Brazilian Real"; }
            else if (code == "MXN") { emoji = "🇲🇽"; name = "Mexican Peso"; }
            else if (code == "ARS") { emoji = "🇦🇷"; name = "Argentine Peso"; }
            else if (code == "JPY") { emoji = "🇯🇵"; name = "Japanese Yen"; }
            else if (code == "CNY") { emoji = "🇨🇳"; name = "Chinese Yuan"; }
            else if (code == "INR") { emoji = "🇮🇳"; name = "Indian Rupee"; }
            else if (code == "PKR") { emoji = "🇵🇰"; name = "Pakistani Rupee"; }
            else if (code == "THB") { emoji = "🇹🇭"; name = "Thai Baht"; }
            else if (code == "VND") { emoji = "🇻🇳"; name = "Vietnamese Dong"; }
            else if (code == "IDR") { emoji = "🇮🇩"; name = "Indonesian Rupiah"; }
            else if (code == "SAR") { emoji = "🇸🇦"; name = "Saudi Riyal"; }
            else if (code == "AED") { emoji = "🇦🇪"; name = "UAE Dirham"; }
            else if (code == "QAR") { emoji = "🇶🇦"; name = "Qatari Riyal"; }
            else if (code == "TRY") { emoji = "🇹🇷"; name = "Turkish Lira"; }
            else if (code == "IRR") { emoji = "🇮🇷"; name = "Iranian Rial"; }
            else if (code == "ZAR") { emoji = "🇿🇦"; name = "South African Rand"; }
            else if (code == "EGP") { emoji = "🇪🇬"; name = "Egyptian Pound"; }
            else if (code == "NGN") { emoji = "🇳🇬"; name = "Nigerian Naira"; }
            else if (code == "KES") { emoji = "🇰🇪"; name = "Kenyan Shilling"; }
            else if (code == "RUB") { emoji = "🇷🇺"; name = "Russian Ruble"; }
            else if (code == "BAM") { emoji = "🇧🇦"; name = "Bosnia Mark"; }
            else if (code == "RSD") { emoji = "🇷🇸"; name = "Serbian Dinar"; }
            else if (code == "BTC") { emoji = "₿"; name = "Bitcoin"; }
            else if (code == "ETH") { emoji = "Ξ"; name = "Ethereum"; }
            else if (code == "USDT") { emoji = "₮"; name = "Tether"; }
            else if (code == "USDC") { emoji = "🔵"; name = "USD Coin"; }
            else if (code == "SOL") { emoji = "◎"; name = "Solana"; }
            else if (code == "BNB") { emoji = "⚡"; name = "Binance Coin"; }
            else if (code == "XRP") { emoji = "✕"; name = "Ripple"; }
            else if (code == "ADA") { emoji = "₳"; name = "Cardano"; }
            else if (code == "DOGE") { emoji = "Ð"; name = "Dogecoin"; }
            else continue;

            currencyCombo->addItem(QString("⭐ %1 %2 - %3").arg(emoji, code, name), code);
        }

        currencyCombo->insertSeparator(currencyCombo->count());
    }

    // === All Currencies ===
    currencyCombo->addItem("🇺🇸 USD - US Dollar", "USD");
    currencyCombo->addItem("🇪🇺 EUR - Euro", "EUR");
    currencyCombo->addItem("🇬🇧 GBP - Pound Sterling", "GBP");
    currencyCombo->addItem("🇧🇷 BRL - Brazilian Real", "BRL");
    currencyCombo->addItem("🇲🇽 MXN - Mexican Peso", "MXN");
    currencyCombo->addItem("🇦🇷 ARS - Argentine Peso", "ARS");
    currencyCombo->addItem("🇯🇵 JPY - Japanese Yen", "JPY");
    currencyCombo->addItem("🇨🇳 CNY - Chinese Yuan", "CNY");
    currencyCombo->addItem("🇮🇳 INR - Indian Rupee", "INR");
    currencyCombo->addItem("🇵🇰 PKR - Pakistani Rupee", "PKR");
    currencyCombo->addItem("🇹🇭 THB - Thai Baht", "THB");
    currencyCombo->addItem("🇻🇳 VND - Vietnamese Dong", "VND");
    currencyCombo->addItem("🇮🇩 IDR - Indonesian Rupiah", "IDR");
    currencyCombo->addItem("🇸🇦 SAR - Saudi Riyal", "SAR");
    currencyCombo->addItem("🇦🇪 AED - UAE Dirham", "AED");
    currencyCombo->addItem("🇶🇦 QAR - Qatari Riyal", "QAR");
    currencyCombo->addItem("🇹🇷 TRY - Turkish Lira", "TRY");
    currencyCombo->addItem("🇮🇷 IRR - Iranian Rial", "IRR");
    currencyCombo->addItem("🇿🇦 ZAR - South African Rand", "ZAR");
    currencyCombo->addItem("🇪🇬 EGP - Egyptian Pound", "EGP");
    currencyCombo->addItem("🇳🇬 NGN - Nigerian Naira", "NGN");
    currencyCombo->addItem("🇰🇪 KES - Kenyan Shilling", "KES");
    currencyCombo->addItem("🇷🇺 RUB - Russian Ruble", "RUB");
    currencyCombo->addItem("🇧🇦 BAM - Bosnia Mark", "BAM");
    currencyCombo->addItem("🇷🇸 RSD - Serbian Dinar", "RSD");

    currencyCombo->insertSeparator(currencyCombo->count());

    currencyCombo->addItem("₿ BTC - Bitcoin", "BTC");
    currencyCombo->addItem("Ξ ETH - Ethereum", "ETH");
    currencyCombo->addItem("₮ USDT - Tether", "USDT");
    currencyCombo->addItem("🔵 USDC - USD Coin", "USDC");
    currencyCombo->addItem("◎ SOL - Solana", "SOL");
    currencyCombo->addItem("⚡ BNB - Binance Coin", "BNB");
    currencyCombo->addItem("✕ XRP - Ripple", "XRP");
    currencyCombo->addItem("₳ ADA - Cardano", "ADA");
    currencyCombo->addItem("Ð DOGE - Dogecoin", "DOGE");

    currencyCombo->setCurrentIndex(0);
}

void PaymentsWidget::onToggleFavorite()
{
    QString current = currencyCombo->currentData().toString();
    if (current.isEmpty()) return;

    if (favoriteCurrencies_.contains(current)) {
        favoriteCurrencies_.removeAll(current);
        favoriteButton->setText("☆");
        statusLabel->setText(QString("Removed %1 from favorites").arg(current));
    } else {
        favoriteCurrencies_.append(current);
        favoriteButton->setText("⭐");
        statusLabel->setText(QString("Added %1 to favorites").arg(current));
    }

    saveFavorites();
    populateCurrencyCombo();

    // Restore selection
    for (int i = 0; i < currencyCombo->count(); ++i) {
        if (currencyCombo->itemData(i).toString() == current) {
            currencyCombo->setCurrentIndex(i);
            break;
        }
    }
}

void PaymentsWidget::onSearchCurrency(const QString& text)
{
    // Filter dropdown items based on search text
    QString search = text.toLower();

    for (int i = 0; i < currencyCombo->count(); ++i) {
        QString itemText = currencyCombo->itemText(i).toLower();
        QString itemData = currencyCombo->itemData(i).toString().toLower();

        bool matches = itemText.contains(search) || itemData.contains(search);
        currencyCombo->setItemData(i, !matches, Qt::UserRole + 1);  // Hide non-matches
    }

    // Note: QComboBox doesn't natively support hiding items, so we'd need a custom model
    // For now, we'll just highlight matching currencies
    if (!search.isEmpty()) {
        for (int i = 0; i < currencyCombo->count(); ++i) {
            QString itemText = currencyCombo->itemText(i).toLower();
            if (itemText.contains(search)) {
                currencyCombo->setCurrentIndex(i);
                break;
            }
        }
    }
}

void PaymentsWidget::saveFavorites()
{
    QSettings settings("Dinero", "DineroQt");
    settings.setValue("payments/favoriteCurrencies", favoriteCurrencies_);
}

void PaymentsWidget::loadFavorites()
{
    QSettings settings("Dinero", "DineroQt");
    favoriteCurrencies_ = settings.value("payments/favoriteCurrencies").toStringList();
}
