#pragma once

#include <QWidget>
#include <QTableView>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QStandardItemModel>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QTabWidget>

class RpcClient;
class WebSocketClient;

/**
 * MarketplaceWidget - P2P Trading Marketplace Interface
 *
 * Features:
 * - Browse and search marketplace offers
 * - Create new offers (buy/sell DIN, goods, services)
 * - Accept offers (automatically creates escrow)
 * - View and manage your own offers
 * - View trade history and reputation
 * - Real-time updates via WebSocket
 *
 * RPC Methods Used:
 * - market.listoffers     - Browse all offers
 * - market.search         - Search offers by keyword
 * - market.getoffer       - Get offer details
 * - market.createoffer    - Create new offer
 * - market.canceloffer    - Cancel your offer
 * - market.updateoffer    - Update offer details
 * - market.acceptoffer    - Accept an offer (creates escrow)
 * - market.myoffers       - List your offers
 * - market.mytrades       - List your trades
 * - market.getreputation  - View user reputation
 */
class MarketplaceWidget : public QWidget {
    Q_OBJECT

public:
    explicit MarketplaceWidget(RpcClient* rpc, WebSocketClient* ws, QWidget* parent = nullptr);
    ~MarketplaceWidget();

private Q_SLOTS:
    // Offer Management
    void onCreateOffer();
    void onCancelOffer();
    void onUpdateOffer();
    void onAcceptOffer();
    void onViewOfferDetails();

    // Browse & Search
    void onRefreshOffers();
    void onSearchOffers();
    void onFilterChanged();
    void onOfferSelected(const QModelIndex& index);

    // Trade Management
    void onRefreshTrades();
    void onCompleteTrade();
    void onDisputeTrade();
    void onViewTradeDetails();

    // Reputation
    void onViewReputation();

    // RPC Callbacks
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);

    // WebSocket Events
    void onWebSocketEvent(const QString& topic, const QJsonObject& data);
    void onNewOffer(const QJsonObject& offer);
    void onOfferUpdate(const QString& offer_id, const QString& status);
    void onTradeUpdate(const QString& trade_id, const QString& status);

private:
    void setupUi();
    void setupConnections();
    void callRpc(const QString& method, const QJsonArray& params = QJsonArray());
    void populateOffersTable(const QJsonArray& offers);
    void populateTradesTable(const QJsonArray& trades);
    void appendLog(const QString& message);
    QString formatOfferType(const QString& type);
    QString formatStatus(const QString& status);
    QColor statusColor(const QString& status);
    QString formatTimestamp(int64_t timestamp);
    void showCreateOfferDialog();
    void showOfferDetailsDialog(const QJsonObject& offer);
    void showAcceptOfferDialog(const QJsonObject& offer);
    void showReputationDialog(const QString& user_pubkey);

    // Tab creation methods
    QWidget* createBrowseTab();
    QWidget* createMyOffersTab();
    QWidget* createMyTradesTab();
    QWidget* createReputationTab();

    // UI Components - Header
    QLabel* titleLabel_;
    QLabel* statsLabel_;
    QPushButton* createOfferButton_;
    QPushButton* refreshButton_;
    QPushButton* myOffersButton_;
    QPushButton* myTradesButton_;

    // UI Components - Main Tabs
    QTabWidget* mainTabs_;

    // Browse Tab Components
    QLineEdit* searchInput_;
    QPushButton* searchButton_;
    QComboBox* typeFilter_;
    QComboBox* assetFilter_;
    QDoubleSpinBox* minPriceFilter_;
    QDoubleSpinBox* maxPriceFilter_;
    QTableView* offersTable_;
    QStandardItemModel* offersModel_;
    QPushButton* acceptOfferButton_;
    QPushButton* viewOfferButton_;

    // My Offers Tab Components
    QTableView* myOffersTable_;
    QStandardItemModel* myOffersModel_;
    QPushButton* cancelOfferButton_;
    QPushButton* updateOfferButton_;
    QComboBox* myOffersStatusFilter_;

    // My Trades Tab Components
    QTableView* tradesTable_;
    QStandardItemModel* tradesModel_;
    QPushButton* completeTradeButton_;
    QPushButton* disputeTradeButton_;
    QPushButton* viewTradeButton_;
    QComboBox* tradesRoleFilter_;
    QComboBox* tradesStatusFilter_;

    // Reputation Tab Components
    QLabel* reputationScoreLabel_;
    QLabel* totalTradesLabel_;
    QLabel* successfulTradesLabel_;
    QLabel* ratingDistributionLabel_;
    QPushButton* viewOtherReputationButton_;

    // Details Panel
    QGroupBox* detailsGroup_;
    QLabel* offerIdLabel_;
    QLabel* offerTypeLabel_;
    QLabel* offerAssetLabel_;
    QLabel* offerAmountLabel_;
    QLabel* offerPriceLabel_;
    QLabel* offerDescriptionLabel_;
    QLabel* sellerPubkeyLabel_;
    QLabel* sellerReputationLabel_;
    QPushButton* viewSellerRepButton_;

    // Event Log
    QTextEdit* eventLog_;

    // Data
    RpcClient* rpc_;
    WebSocketClient* ws_;
    QTimer refreshTimer_;
    QString selectedOfferId_;
    QString selectedTradeId_;
    QMap<QString, QJsonObject> offersCache_;
    QMap<QString, QJsonObject> tradesCache_;

    // Refresh interval
    static constexpr int REFRESH_INTERVAL_MS = 15000;  // 15 seconds

    // Table columns - Browse Offers
    enum OfferColumn {
        COL_OFFER_ID = 0,
        COL_TYPE,
        COL_ASSET,
        COL_AMOUNT,
        COL_PRICE,
        COL_CURRENCY,
        COL_DESCRIPTION,
        COL_SELLER,
        COL_REPUTATION,
        COL_CREATED,
        COL_OFFER_COUNT
    };

    // Table columns - My Trades
    enum TradeColumn {
        COL_TRADE_ID = 0,
        COL_OFFER_REF,
        COL_COUNTERPARTY,
        COL_ROLE,
        COL_TRADE_AMOUNT,
        COL_TOTAL_VALUE,
        COL_TRADE_STATUS,
        COL_ESCROW,
        COL_TRADE_CREATED,
        COL_TRADE_COUNT
    };
};
