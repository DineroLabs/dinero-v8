#include "marketplacewidget.h"
#include "rpcclient.h"
#include "websocketclient.h"

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
#include <QDateTime>
#include <QFont>
#include <QScrollArea>
#include <QSplitter>

MarketplaceWidget::MarketplaceWidget(RpcClient* rpc, WebSocketClient* ws, QWidget* parent)
    : QWidget(parent)
    , rpc_(rpc)
    , ws_(ws)
{
    setupUi();
    setupConnections();

    // Initial data load
    onRefreshOffers();

    // Start auto-refresh
    refreshTimer_.start(REFRESH_INTERVAL_MS);
}

MarketplaceWidget::~MarketplaceWidget() = default;

void MarketplaceWidget::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    // ========== HEADER ==========
    auto* headerLayout = new QHBoxLayout();

    titleLabel_ = new QLabel("<h2>🛒 P2P Marketplace</h2>");
    statsLabel_ = new QLabel("Loading...");
    statsLabel_->setStyleSheet("color: #666; font-size: 12px;");

    createOfferButton_ = new QPushButton("➕ Create Offer");
    createOfferButton_->setStyleSheet("font-weight: bold; padding: 8px 16px; background: #51cf66; color: white;");

    refreshButton_ = new QPushButton("🔄 Refresh");
    myOffersButton_ = new QPushButton("📋 My Offers");
    myTradesButton_ = new QPushButton("🤝 My Trades");

    headerLayout->addWidget(titleLabel_);
    headerLayout->addWidget(statsLabel_);
    headerLayout->addStretch();
    headerLayout->addWidget(createOfferButton_);
    headerLayout->addWidget(myOffersButton_);
    headerLayout->addWidget(myTradesButton_);
    headerLayout->addWidget(refreshButton_);

    mainLayout->addLayout(headerLayout);

    // ========== MAIN TABS ==========
    mainTabs_ = new QTabWidget();
    mainLayout->addWidget(mainTabs_);

    // Create tabs
    mainTabs_->addTab(createBrowseTab(), "🔍 Browse Offers");
    mainTabs_->addTab(createMyOffersTab(), "📋 My Offers");
    mainTabs_->addTab(createMyTradesTab(), "🤝 My Trades");
    mainTabs_->addTab(createReputationTab(), "⭐ Reputation");

    // ========== EVENT LOG ==========
    auto* logLabel = new QLabel("<b>Event Log:</b>");
    eventLog_ = new QTextEdit();
    eventLog_->setReadOnly(true);
    eventLog_->setMaximumHeight(120);
    eventLog_->setStyleSheet("background-color: #f5f5f5; font-family: 'Courier New', monospace; font-size: 11px;");

    mainLayout->addWidget(logLabel);
    mainLayout->addWidget(eventLog_);

    appendLog("Marketplace widget initialized. Ready to browse and create offers.");
}

QWidget* MarketplaceWidget::createBrowseTab()
{
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    // ========== SEARCH & FILTERS ==========
    auto* filterLayout = new QHBoxLayout();

    searchInput_ = new QLineEdit();
    searchInput_->setPlaceholderText("Search offers by keyword...");

    searchButton_ = new QPushButton("🔍 Search");

    typeFilter_ = new QComboBox();
    typeFilter_->addItem("All Types", "all");
    typeFilter_->addItem("🟢 Buy Offers", "buy");
    typeFilter_->addItem("🔴 Sell Offers", "sell");

    assetFilter_ = new QComboBox();
    assetFilter_->addItem("All Assets", "");
    assetFilter_->addItem("DIN", "DIN");
    assetFilter_->addItem("BTC", "BTC");
    assetFilter_->addItem("USDT", "USDT");
    assetFilter_->addItem("Services", "services");
    assetFilter_->addItem("Goods", "goods");

    minPriceFilter_ = new QDoubleSpinBox();
    minPriceFilter_->setPrefix("Min: $");
    minPriceFilter_->setDecimals(2);
    minPriceFilter_->setMinimum(0);
    minPriceFilter_->setMaximum(1000000);
    minPriceFilter_->setValue(0);

    maxPriceFilter_ = new QDoubleSpinBox();
    maxPriceFilter_->setPrefix("Max: $");
    maxPriceFilter_->setDecimals(2);
    maxPriceFilter_->setMinimum(0);
    maxPriceFilter_->setMaximum(1000000);
    maxPriceFilter_->setValue(10000);

    filterLayout->addWidget(new QLabel("Search:"));
    filterLayout->addWidget(searchInput_, 2);
    filterLayout->addWidget(searchButton_);
    filterLayout->addWidget(new QLabel("Type:"));
    filterLayout->addWidget(typeFilter_);
    filterLayout->addWidget(new QLabel("Asset:"));
    filterLayout->addWidget(assetFilter_);
    filterLayout->addWidget(minPriceFilter_);
    filterLayout->addWidget(maxPriceFilter_);

    layout->addLayout(filterLayout);

    // ========== OFFERS TABLE ==========
    offersTable_ = new QTableView();
    offersModel_ = new QStandardItemModel(0, COL_OFFER_COUNT, this);

    offersModel_->setHorizontalHeaderLabels({
        "Offer ID",
        "Type",
        "Asset",
        "Amount",
        "Price",
        "Currency",
        "Description",
        "Seller",
        "Reputation",
        "Created"
    });

    offersTable_->setModel(offersModel_);
    offersTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    offersTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    offersTable_->setAlternatingRowColors(true);
    offersTable_->horizontalHeader()->setStretchLastSection(false);
    offersTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Column widths
    offersTable_->setColumnWidth(COL_OFFER_ID, 120);
    offersTable_->setColumnWidth(COL_TYPE, 60);
    offersTable_->setColumnWidth(COL_ASSET, 80);
    offersTable_->setColumnWidth(COL_AMOUNT, 100);
    offersTable_->setColumnWidth(COL_PRICE, 100);
    offersTable_->setColumnWidth(COL_CURRENCY, 80);
    offersTable_->setColumnWidth(COL_DESCRIPTION, 300);
    offersTable_->setColumnWidth(COL_SELLER, 150);
    offersTable_->setColumnWidth(COL_REPUTATION, 100);
    offersTable_->setColumnWidth(COL_CREATED, 150);

    layout->addWidget(offersTable_);

    // ========== DETAILS PANEL ==========
    detailsGroup_ = new QGroupBox("Selected Offer Details");
    auto* detailsLayout = new QGridLayout();

    offerIdLabel_ = new QLabel("-");
    offerTypeLabel_ = new QLabel("-");
    offerAssetLabel_ = new QLabel("-");
    offerAmountLabel_ = new QLabel("-");
    offerPriceLabel_ = new QLabel("-");
    offerDescriptionLabel_ = new QLabel("-");
    offerDescriptionLabel_->setWordWrap(true);
    sellerPubkeyLabel_ = new QLabel("-");
    sellerReputationLabel_ = new QLabel("-");

    viewSellerRepButton_ = new QPushButton("📊 View Seller Reputation");
    viewSellerRepButton_->setMaximumWidth(180);
    viewSellerRepButton_->setEnabled(false);

    int row = 0;
    detailsLayout->addWidget(new QLabel("<b>Offer ID:</b>"), row, 0);
    detailsLayout->addWidget(offerIdLabel_, row++, 1, 1, 2);

    detailsLayout->addWidget(new QLabel("<b>Type:</b>"), row, 0);
    detailsLayout->addWidget(offerTypeLabel_, row, 1);
    detailsLayout->addWidget(new QLabel("<b>Asset:</b>"), row, 2);
    detailsLayout->addWidget(offerAssetLabel_, row++, 3);

    detailsLayout->addWidget(new QLabel("<b>Amount:</b>"), row, 0);
    detailsLayout->addWidget(offerAmountLabel_, row, 1);
    detailsLayout->addWidget(new QLabel("<b>Price:</b>"), row, 2);
    detailsLayout->addWidget(offerPriceLabel_, row++, 3);

    detailsLayout->addWidget(new QLabel("<b>Description:</b>"), row, 0);
    detailsLayout->addWidget(offerDescriptionLabel_, row++, 1, 1, 3);

    detailsLayout->addWidget(new QLabel("<b>Seller:</b>"), row, 0);
    detailsLayout->addWidget(sellerPubkeyLabel_, row, 1);
    detailsLayout->addWidget(viewSellerRepButton_, row++, 2, 1, 2);

    detailsLayout->addWidget(new QLabel("<b>Seller Reputation:</b>"), row, 0);
    detailsLayout->addWidget(sellerReputationLabel_, row++, 1, 1, 3);

    detailsGroup_->setLayout(detailsLayout);
    layout->addWidget(detailsGroup_);

    // ========== ACTION BUTTONS ==========
    auto* actionLayout = new QHBoxLayout();

    acceptOfferButton_ = new QPushButton("✅ Accept Offer");
    acceptOfferButton_->setStyleSheet("background-color: #51cf66; color: white; font-weight: bold; padding: 10px;");
    acceptOfferButton_->setEnabled(false);

    viewOfferButton_ = new QPushButton("🔍 View Full Details");
    viewOfferButton_->setEnabled(false);

    actionLayout->addWidget(acceptOfferButton_);
    actionLayout->addWidget(viewOfferButton_);
    actionLayout->addStretch();

    layout->addLayout(actionLayout);

    return widget;
}

QWidget* MarketplaceWidget::createMyOffersTab()
{
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    // Filter by status
    auto* filterLayout = new QHBoxLayout();
    filterLayout->addWidget(new QLabel("Status:"));

    myOffersStatusFilter_ = new QComboBox();
    myOffersStatusFilter_->addItem("Active", "active");
    myOffersStatusFilter_->addItem("Filled", "filled");
    myOffersStatusFilter_->addItem("Cancelled", "cancelled");
    myOffersStatusFilter_->addItem("All", "all");

    filterLayout->addWidget(myOffersStatusFilter_);
    filterLayout->addStretch();

    layout->addLayout(filterLayout);

    // My Offers Table
    myOffersTable_ = new QTableView();
    myOffersModel_ = new QStandardItemModel(0, COL_OFFER_COUNT, this);

    myOffersModel_->setHorizontalHeaderLabels({
        "Offer ID",
        "Type",
        "Asset",
        "Amount",
        "Price",
        "Currency",
        "Description",
        "Status",
        "Created",
        "Updated"
    });

    myOffersTable_->setModel(myOffersModel_);
    myOffersTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    myOffersTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    myOffersTable_->setAlternatingRowColors(true);
    myOffersTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    layout->addWidget(myOffersTable_);

    // Action Buttons
    auto* actionLayout = new QHBoxLayout();

    cancelOfferButton_ = new QPushButton("❌ Cancel Offer");
    cancelOfferButton_->setStyleSheet("background-color: #ff6b6b; color: white; font-weight: bold; padding: 8px;");
    cancelOfferButton_->setEnabled(false);

    updateOfferButton_ = new QPushButton("✏️ Update Offer");
    updateOfferButton_->setStyleSheet("background-color: #4c6ef5; color: white; font-weight: bold; padding: 8px;");
    updateOfferButton_->setEnabled(false);

    actionLayout->addWidget(cancelOfferButton_);
    actionLayout->addWidget(updateOfferButton_);
    actionLayout->addStretch();

    layout->addLayout(actionLayout);

    return widget;
}

QWidget* MarketplaceWidget::createMyTradesTab()
{
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    // Filters
    auto* filterLayout = new QHBoxLayout();

    filterLayout->addWidget(new QLabel("Role:"));
    tradesRoleFilter_ = new QComboBox();
    tradesRoleFilter_->addItem("All", "all");
    tradesRoleFilter_->addItem("As Buyer", "buyer");
    tradesRoleFilter_->addItem("As Seller", "seller");
    filterLayout->addWidget(tradesRoleFilter_);

    filterLayout->addWidget(new QLabel("Status:"));
    tradesStatusFilter_ = new QComboBox();
    tradesStatusFilter_->addItem("All", "all");
    tradesStatusFilter_->addItem("Pending Funding", "pending_funding");
    tradesStatusFilter_->addItem("Active", "funded");
    tradesStatusFilter_->addItem("Completed", "completed");
    tradesStatusFilter_->addItem("Disputed", "disputed");
    filterLayout->addWidget(tradesStatusFilter_);

    filterLayout->addStretch();
    layout->addLayout(filterLayout);

    // Trades Table
    tradesTable_ = new QTableView();
    tradesModel_ = new QStandardItemModel(0, COL_TRADE_COUNT, this);

    tradesModel_->setHorizontalHeaderLabels({
        "Trade ID",
        "Offer",
        "Counterparty",
        "Role",
        "Amount",
        "Total Value",
        "Status",
        "Escrow Address",
        "Created"
    });

    tradesTable_->setModel(tradesModel_);
    tradesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tradesTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    tradesTable_->setAlternatingRowColors(true);
    tradesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    layout->addWidget(tradesTable_);

    // Action Buttons
    auto* actionLayout = new QHBoxLayout();

    completeTradeButton_ = new QPushButton("✅ Complete Trade");
    completeTradeButton_->setStyleSheet("background-color: #51cf66; color: white; font-weight: bold; padding: 8px;");
    completeTradeButton_->setEnabled(false);

    disputeTradeButton_ = new QPushButton("⚠️ Open Dispute");
    disputeTradeButton_->setStyleSheet("background-color: #ff922b; color: white; font-weight: bold; padding: 8px;");
    disputeTradeButton_->setEnabled(false);

    viewTradeButton_ = new QPushButton("🔍 View Details");
    viewTradeButton_->setEnabled(false);

    actionLayout->addWidget(completeTradeButton_);
    actionLayout->addWidget(disputeTradeButton_);
    actionLayout->addWidget(viewTradeButton_);
    actionLayout->addStretch();

    layout->addLayout(actionLayout);

    return widget;
}

QWidget* MarketplaceWidget::createReputationTab()
{
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    auto* reputationGroup = new QGroupBox("My Reputation");
    auto* repLayout = new QGridLayout();

    reputationScoreLabel_ = new QLabel("⭐ 0.0");
    reputationScoreLabel_->setStyleSheet("font-size: 24px; font-weight: bold; color: #fab005;");

    totalTradesLabel_ = new QLabel("0 total trades");
    successfulTradesLabel_ = new QLabel("0 successful");
    ratingDistributionLabel_ = new QLabel("No ratings yet");

    repLayout->addWidget(new QLabel("<b>Average Rating:</b>"), 0, 0);
    repLayout->addWidget(reputationScoreLabel_, 0, 1);

    repLayout->addWidget(new QLabel("<b>Total Trades:</b>"), 1, 0);
    repLayout->addWidget(totalTradesLabel_, 1, 1);

    repLayout->addWidget(new QLabel("<b>Successful:</b>"), 2, 0);
    repLayout->addWidget(successfulTradesLabel_, 2, 1);

    repLayout->addWidget(new QLabel("<b>Rating Distribution:</b>"), 3, 0);
    repLayout->addWidget(ratingDistributionLabel_, 3, 1);

    reputationGroup->setLayout(repLayout);
    layout->addWidget(reputationGroup);

    viewOtherReputationButton_ = new QPushButton("🔍 View Other User's Reputation");
    layout->addWidget(viewOtherReputationButton_);

    layout->addStretch();

    return widget;
}

void MarketplaceWidget::setupConnections()
{
    // Button connections
    connect(createOfferButton_, &QPushButton::clicked, this, &MarketplaceWidget::onCreateOffer);
    connect(refreshButton_, &QPushButton::clicked, this, &MarketplaceWidget::onRefreshOffers);
    connect(searchButton_, &QPushButton::clicked, this, &MarketplaceWidget::onSearchOffers);
    connect(acceptOfferButton_, &QPushButton::clicked, this, &MarketplaceWidget::onAcceptOffer);
    connect(viewOfferButton_, &QPushButton::clicked, this, &MarketplaceWidget::onViewOfferDetails);

    connect(cancelOfferButton_, &QPushButton::clicked, this, &MarketplaceWidget::onCancelOffer);
    connect(updateOfferButton_, &QPushButton::clicked, this, &MarketplaceWidget::onUpdateOffer);

    connect(completeTradeButton_, &QPushButton::clicked, this, &MarketplaceWidget::onCompleteTrade);
    connect(disputeTradeButton_, &QPushButton::clicked, this, &MarketplaceWidget::onDisputeTrade);
    connect(viewTradeButton_, &QPushButton::clicked, this, &MarketplaceWidget::onViewTradeDetails);

    connect(viewSellerRepButton_, &QPushButton::clicked, this, &MarketplaceWidget::onViewReputation);
    connect(viewOtherReputationButton_, &QPushButton::clicked, this, &MarketplaceWidget::onViewReputation);

    // Tab switching
    connect(myOffersButton_, &QPushButton::clicked, [this]() { mainTabs_->setCurrentIndex(1); });
    connect(myTradesButton_, &QPushButton::clicked, [this]() { mainTabs_->setCurrentIndex(2); });

    // Table selection
    connect(offersTable_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &MarketplaceWidget::onOfferSelected);

    // Filters
    connect(typeFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MarketplaceWidget::onFilterChanged);
    connect(assetFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MarketplaceWidget::onFilterChanged);

    // Auto-refresh timer
    connect(&refreshTimer_, &QTimer::timeout, this, &MarketplaceWidget::onRefreshOffers);

    // RPC callbacks
    connect(rpc_, &RpcClient::rpcResult, this, &MarketplaceWidget::onRpcResult);
    connect(rpc_, &RpcClient::rpcError, this, &MarketplaceWidget::onRpcError);

    // WebSocket events
    if (ws_) {
        connect(ws_, &WebSocketClient::subscriptionEvent, this, &MarketplaceWidget::onWebSocketEvent);
        ws_->subscribe("market_offer_created");
        ws_->subscribe("market_offer_updated");
        ws_->subscribe("market_trade_created");
        ws_->subscribe("market_trade_updated");
    }
}

void MarketplaceWidget::onRefreshOffers()
{
    QString type = typeFilter_->currentData().toString();
    QString asset = assetFilter_->currentData().toString();
    double min_price = minPriceFilter_->value();
    double max_price = maxPriceFilter_->value();

    QJsonObject params{
        {"type", type},
        {"asset", asset},
        {"min_price", min_price},
        {"max_price", max_price},
        {"limit", 100},
        {"offset", 0}
    };

    callRpc("p2p.listoffers", QJsonArray{params});
}

void MarketplaceWidget::onSearchOffers()
{
    QString query = searchInput_->text().trimmed();
    if (query.isEmpty()) {
        onRefreshOffers();
        return;
    }

    QString asset = assetFilter_->currentData().toString();
    double min_price = minPriceFilter_->value();
    double max_price = maxPriceFilter_->value();

    QJsonObject params{
        {"query", query},
        {"asset", asset},
        {"min_price", min_price},
        {"max_price", max_price},
        {"sort_by", "date"},
        {"sort_order", "desc"}
    };

    callRpc("p2p.search", QJsonArray{params});
}

void MarketplaceWidget::onFilterChanged()
{
    onRefreshOffers();
}

void MarketplaceWidget::onCreateOffer()
{
    showCreateOfferDialog();
}

void MarketplaceWidget::onAcceptOffer()
{
    if (selectedOfferId_.isEmpty()) {
        QMessageBox::warning(this, "No Selection", "Please select an offer first.");
        return;
    }

    if (offersCache_.contains(selectedOfferId_)) {
        showAcceptOfferDialog(offersCache_[selectedOfferId_]);
    }
}

void MarketplaceWidget::onViewOfferDetails()
{
    if (selectedOfferId_.isEmpty()) return;

    if (offersCache_.contains(selectedOfferId_)) {
        showOfferDetailsDialog(offersCache_[selectedOfferId_]);
    }
}

void MarketplaceWidget::onCancelOffer()
{
    if (selectedOfferId_.isEmpty()) {
        QMessageBox::warning(this, "No Selection", "Please select an offer to cancel.");
        return;
    }

    auto reply = QMessageBox::question(
        this,
        "Cancel Offer",
        QString("Cancel this offer?\n\nOffer ID: %1").arg(selectedOfferId_),
        QMessageBox::Yes | QMessageBox::No
    );

    if (reply == QMessageBox::Yes) {
        QJsonObject params{{"offer_id", selectedOfferId_}};
        callRpc("p2p.canceloffer", QJsonArray{params});
        appendLog(QString("Cancelling offer %1...").arg(selectedOfferId_));
    }
}

void MarketplaceWidget::onUpdateOffer()
{
    // TODO: Show update dialog
    QMessageBox::information(this, "Update Offer", "Update offer dialog - coming soon!");
}

void MarketplaceWidget::onCompleteTrade()
{
    // TODO: Show complete trade dialog with rating
    QMessageBox::information(this, "Complete Trade", "Complete trade dialog - coming soon!");
}

void MarketplaceWidget::onDisputeTrade()
{
    // TODO: Show dispute dialog
    QMessageBox::information(this, "Open Dispute", "Dispute dialog - coming soon!");
}

void MarketplaceWidget::onViewTradeDetails()
{
    // TODO: Show trade details
    QMessageBox::information(this, "Trade Details", "Trade details dialog - coming soon!");
}

void MarketplaceWidget::onRefreshTrades()
{
    QJsonObject params{
        {"role", "all"},
        {"status", "all"}
    };

    callRpc("p2p.mytrades", QJsonArray{params});
}

void MarketplaceWidget::onViewReputation()
{
    // TODO: Show reputation dialog for a user
    QMessageBox::information(this, "View Reputation", "Reputation viewer - coming soon!");
}

void MarketplaceWidget::onOfferSelected(const QModelIndex& index)
{
    if (!index.isValid()) {
        acceptOfferButton_->setEnabled(false);
        viewOfferButton_->setEnabled(false);
        return;
    }

    QString offerId = offersModel_->item(index.row(), COL_OFFER_ID)->text();
    selectedOfferId_ = offerId;

    if (!offersCache_.contains(offerId)) {
        QJsonObject params{{"offer_id", offerId}};
        callRpc("p2p.getoffer", QJsonArray{params});
        return;
    }

    QJsonObject offer = offersCache_[offerId];

    // Update details panel
    offerIdLabel_->setText(offerId);
    offerTypeLabel_->setText(formatOfferType(offer["type"].toString()));
    offerAssetLabel_->setText(offer["asset"].toString());
    offerAmountLabel_->setText(QString::number(offer["amount"].toDouble(), 'f', 8));
    offerPriceLabel_->setText(QString("%1 %2")
        .arg(offer["price"].toDouble(), 0, 'f', 2)
        .arg(offer["currency"].toString()));
    offerDescriptionLabel_->setText(offer["description"].toString());
    sellerPubkeyLabel_->setText(offer["creator_pubkey"].toString().left(20) + "...");

    // Enable buttons
    acceptOfferButton_->setEnabled(true);
    viewOfferButton_->setEnabled(true);
    viewSellerRepButton_->setEnabled(true);

    // Load seller reputation
    QJsonObject repParams{{"identifier", offer["creator_pubkey"].toString()}};
    callRpc("p2p.getreputation", QJsonArray{repParams});
}

void MarketplaceWidget::onRpcResult(const QString& method, const QJsonValue& result)
{
    if (method == "p2p.listoffers" || method == "p2p.search") {
        QJsonArray offers = result.toObject()["offers"].toArray();
        populateOffersTable(offers);

        int count = offers.size();
        statsLabel_->setText(QString("%1 offer(s) found").arg(count));
    }
    else if (method == "p2p.getoffer") {
        QJsonObject offer = result.toObject();
        QString offerId = offer["offer_id"].toString();
        offersCache_[offerId] = offer;

        // Update details if this is the selected offer
        if (offerId == selectedOfferId_) {
            onOfferSelected(offersTable_->currentIndex());
        }
    }
    else if (method == "p2p.createoffer") {
        QJsonObject offer = result.toObject();
        QString offerId = offer["offer_id"].toString();

        appendLog(QString("✅ Offer created: %1").arg(offerId));
        QMessageBox::information(
            this,
            "Offer Created",
            QString("Your offer has been created successfully!\n\nOffer ID: %1").arg(offerId)
        );

        onRefreshOffers();
    }
    else if (method == "p2p.canceloffer") {
        appendLog("✅ Offer cancelled");
        QMessageBox::information(this, "Success", "Offer cancelled successfully!");
        onRefreshOffers();
    }
    else if (method == "p2p.acceptoffer") {
        QJsonObject trade = result.toObject();
        QString tradeId = trade["trade_id"].toString();
        QString escrowAddress = trade["escrow_address"].toString();

        appendLog(QString("✅ Offer accepted! Trade: %1").arg(tradeId));
        QMessageBox::information(
            this,
            "Trade Created",
            QString("Trade created successfully!\n\n"
                    "Trade ID: %1\n"
                    "Escrow Address: %2\n\n"
                    "%3")
                .arg(tradeId)
                .arg(escrowAddress)
                .arg(trade["funding_instructions"].toString())
        );

        onRefreshOffers();
        onRefreshTrades();
    }
    else if (method == "p2p.getreputation") {
        QJsonObject rep = result.toObject();

        // Update seller reputation label if in details
        if (!selectedOfferId_.isEmpty()) {
            QString repText = QString("⭐ %1 / 5.0 (%2 trades)")
                .arg(rep["average_rating"].toDouble(), 0, 'f', 1)
                .arg(rep["total_trades"].toInt());
            sellerReputationLabel_->setText(repText);
        }
    }
}

void MarketplaceWidget::onRpcError(const QString& method, int code, const QString& message)
{
    appendLog(QString("❌ RPC Error [%1]: %2 - %3").arg(method).arg(code).arg(message));

    // Don't show popup dialogs - user gets error in MainWindow status bar
    // Error is already logged above for debugging
}

void MarketplaceWidget::onWebSocketEvent(const QString& topic, const QJsonObject& data)
{
    if (topic == "market_offer_created") {
        onNewOffer(data);
    }
    else if (topic == "market_offer_updated") {
        QString offerId = data["offer_id"].toString();
        QString status = data["status"].toString();
        onOfferUpdate(offerId, status);
    }
    else if (topic == "market_trade_updated") {
        QString tradeId = data["trade_id"].toString();
        QString status = data["status"].toString();
        onTradeUpdate(tradeId, status);
    }
}

void MarketplaceWidget::onNewOffer(const QJsonObject& offer)
{
    appendLog(QString("🔔 New offer: %1 - %2 %3")
        .arg(offer["type"].toString())
        .arg(offer["amount"].toDouble())
        .arg(offer["asset"].toString()));

    // Auto-refresh if filter matches
    onRefreshOffers();
}

void MarketplaceWidget::onOfferUpdate(const QString& offerId, const QString& status)
{
    appendLog(QString("🔔 Offer %1 updated: %2").arg(offerId.left(16)).arg(status));

    // Refresh offers
    onRefreshOffers();
}

void MarketplaceWidget::onTradeUpdate(const QString& tradeId, const QString& status)
{
    appendLog(QString("🔔 Trade %1 updated: %2").arg(tradeId.left(16)).arg(status));
}

void MarketplaceWidget::populateOffersTable(const QJsonArray& offers)
{
    offersModel_->removeRows(0, offersModel_->rowCount());

    for (const auto& v : offers) {
        QJsonObject offer = v.toObject();
        QString offerId = offer["offer_id"].toString();

        // Cache offer data
        offersCache_[offerId] = offer;

        QList<QStandardItem*> row;
        row << new QStandardItem(offerId);
        row << new QStandardItem(formatOfferType(offer["type"].toString()));
        row << new QStandardItem(offer["asset"].toString());
        row << new QStandardItem(QString::number(offer["amount"].toDouble(), 'f', 4));
        row << new QStandardItem(QString::number(offer["price"].toDouble(), 'f', 2));
        row << new QStandardItem(offer["currency"].toString());
        row << new QStandardItem(offer["description"].toString());
        row << new QStandardItem(offer["creator_pubkey"].toString().left(16) + "...");
        row << new QStandardItem("⭐ -");  // Reputation loaded separately
        row << new QStandardItem(formatTimestamp(offer["created_at"].toInteger()));

        offersModel_->appendRow(row);
    }
}

void MarketplaceWidget::populateTradesTable(const QJsonArray& trades)
{
    // TODO: Implement
}

void MarketplaceWidget::appendLog(const QString& message)
{
    QString timestamp = QDateTime::currentDateTime().toString("[hh:mm:ss] ");
    eventLog_->append(timestamp + message);
}

QString MarketplaceWidget::formatOfferType(const QString& type)
{
    if (type == "buy") return "🟢 BUY";
    if (type == "sell") return "🔴 SELL";
    return type;
}

QString MarketplaceWidget::formatStatus(const QString& status)
{
    if (status == "active") return "🟢 Active";
    if (status == "filled") return "✅ Filled";
    if (status == "cancelled") return "❌ Cancelled";
    if (status == "expired") return "⏰ Expired";
    return status;
}

QColor MarketplaceWidget::statusColor(const QString& status)
{
    if (status == "active") return QColor(144, 238, 144);    // Light green
    if (status == "filled") return QColor(173, 216, 230);    // Light blue
    if (status == "cancelled") return QColor(255, 182, 193); // Light pink
    if (status == "expired") return QColor(255, 160, 122);   // Light coral
    return QColor(Qt::white);
}

QString MarketplaceWidget::formatTimestamp(int64_t timestamp)
{
    return QDateTime::fromSecsSinceEpoch(timestamp).toString("yyyy-MM-dd hh:mm");
}

void MarketplaceWidget::showCreateOfferDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Create New Marketplace Offer");
    dialog.setMinimumWidth(500);

    auto* layout = new QFormLayout(&dialog);

    // Type (Buy/Sell)
    auto* typeCombo = new QComboBox();
    typeCombo->addItem("🟢 Buy", "buy");
    typeCombo->addItem("🔴 Sell", "sell");

    // Asset
    auto* assetCombo = new QComboBox();
    assetCombo->addItem("DIN", "DIN");
    assetCombo->addItem("BTC", "BTC");
    assetCombo->addItem("USDT", "USDT");
    assetCombo->addItem("Services", "services");
    assetCombo->addItem("Goods", "goods");
    assetCombo->setEditable(true);

    // Amount
    auto* amountSpin = new QDoubleSpinBox();
    amountSpin->setRange(0.00000001, 1000000000.0);
    amountSpin->setDecimals(8);
    amountSpin->setValue(100.0);

    // Price
    auto* priceSpin = new QDoubleSpinBox();
    priceSpin->setRange(0.01, 1000000.0);
    priceSpin->setDecimals(2);
    priceSpin->setValue(0.15);

    // Currency
    auto* currencyCombo = new QComboBox();
    currencyCombo->addItem("USD", "USD");
    currencyCombo->addItem("DIN", "DIN");
    currencyCombo->addItem("BTC", "BTC");
    currencyCombo->setEditable(true);

    // Description
    auto* descriptionEdit = new QLineEdit();
    descriptionEdit->setPlaceholderText("Describe your offer...");

    layout->addRow("Type:", typeCombo);
    layout->addRow("Asset:", assetCombo);
    layout->addRow("Amount:", amountSpin);
    layout->addRow("Price per unit:", priceSpin);
    layout->addRow("Currency:", currencyCombo);
    layout->addRow("Description:", descriptionEdit);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttonBox);

    if (dialog.exec() == QDialog::Accepted) {
        QJsonObject params{
            {"type", typeCombo->currentData().toString()},
            {"asset", assetCombo->currentData().toString()},
            {"amount", amountSpin->value()},
            {"price", priceSpin->value()},
            {"currency", currencyCombo->currentData().toString()},
            {"description", descriptionEdit->text()},
            {"creator_pubkey", ""}  // Will be filled by wallet
        };

        callRpc("p2p.createoffer", QJsonArray{params});
        appendLog(QString("Creating %1 offer for %2 %3...")
            .arg(typeCombo->currentText())
            .arg(amountSpin->value())
            .arg(assetCombo->currentText()));
    }
}

void MarketplaceWidget::showOfferDetailsDialog(const QJsonObject& offer)
{
    QDialog dialog(this);
    dialog.setWindowTitle("Offer Details");
    dialog.setMinimumWidth(600);

    auto* layout = new QVBoxLayout(&dialog);
    auto* textEdit = new QTextEdit();
    textEdit->setReadOnly(true);

    QString details;
    details += QString("<h3>Offer: %1</h3>").arg(offer["offer_id"].toString());
    details += QString("<b>Type:</b> %1<br>").arg(formatOfferType(offer["type"].toString()));
    details += QString("<b>Asset:</b> %1<br>").arg(offer["asset"].toString());
    details += QString("<b>Amount:</b> %1<br>").arg(offer["amount"].toDouble(), 0, 'f', 8);
    details += QString("<b>Price:</b> %1 %2<br>").arg(offer["price"].toDouble(), 0, 'f', 2).arg(offer["currency"].toString());
    details += QString("<b>Total Value:</b> %1 %2<br>").arg(offer["amount"].toDouble() * offer["price"].toDouble(), 0, 'f', 2).arg(offer["currency"].toString());
    details += QString("<b>Description:</b> %1<br>").arg(offer["description"].toString());
    details += QString("<b>Seller:</b> %1<br>").arg(offer["creator_pubkey"].toString());
    details += QString("<b>Created:</b> %1<br>").arg(formatTimestamp(offer["created_at"].toInteger()));

    textEdit->setHtml(details);
    layout->addWidget(textEdit);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(buttonBox);

    dialog.exec();
}

void MarketplaceWidget::showAcceptOfferDialog(const QJsonObject& offer)
{
    QDialog dialog(this);
    dialog.setWindowTitle("Accept Offer");
    dialog.setMinimumWidth(500);

    auto* layout = new QVBoxLayout(&dialog);

    // Offer summary
    QString summary = QString(
        "<b>You are about to accept this offer:</b><br><br>"
        "Type: %1<br>"
        "Asset: %2<br>"
        "Amount: %3<br>"
        "Price: %4 %5<br>"
        "Total: <b>%6 %7</b><br><br>"
        "Description: %8<br><br>"
        "<b>What happens next:</b><br>"
        "1. An escrow contract will be created automatically<br>"
        "2. You will need to fund the escrow with %6 %7<br>"
        "3. Once funded, the trade begins<br>"
        "4. Complete the trade to release funds<br>"
    ).arg(formatOfferType(offer["type"].toString()))
     .arg(offer["asset"].toString())
     .arg(offer["amount"].toDouble(), 0, 'f', 8)
     .arg(offer["price"].toDouble(), 0, 'f', 2)
     .arg(offer["currency"].toString())
     .arg(offer["amount"].toDouble() * offer["price"].toDouble(), 0, 'f', 2)
     .arg(offer["currency"].toString())
     .arg(offer["description"].toString());

    auto* summaryLabel = new QLabel(summary);
    summaryLabel->setWordWrap(true);
    layout->addWidget(summaryLabel);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttonBox);

    if (dialog.exec() == QDialog::Accepted) {
        QJsonObject params{
            {"offer_id", offer["offer_id"].toString()},
            {"buyer_pubkey", ""}  // Will be filled by wallet
        };

        callRpc("p2p.acceptoffer", QJsonArray{params});
        appendLog(QString("Accepting offer %1...").arg(offer["offer_id"].toString()));
    }
}

void MarketplaceWidget::showReputationDialog(const QString& user_pubkey)
{
    // TODO: Implement reputation viewer dialog
}

void MarketplaceWidget::callRpc(const QString& method, const QJsonArray& params)
{
    rpc_->call(method, params);
}
