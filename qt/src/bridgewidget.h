#pragma once

#include <QWidget>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <QProgressBar>

class RpcClient;
class WebSocketClient;

/**
 * BridgeWidget - Fiat/Crypto conversion interface
 *
 * Features:
 * - Live exchange rates from multiple providers
 * - Multi-hop routing (DIN→BTC→USD, etc.)
 * - Real-time rate updates via WebSocket
 * - Provider selection (DEX, Hybrid, Custodial)
 * - Conversion execution
 * - Route visualization
 */
class BridgeWidget : public QWidget {
    Q_OBJECT

public:
    explicit BridgeWidget(RpcClient* rpc, WebSocketClient* ws, QWidget* parent = nullptr);
    ~BridgeWidget();

private Q_SLOTS:
    void onRefreshRate();
    void onConvertClicked();
    void onFindRouteClicked();
    void onProviderSelected(int index);
    void updateRateDisplay(const QJsonObject& rateInfo);
    void updateRouteDisplay(const QJsonObject& routeInfo);
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);
    void onWebSocketEvent(const QString& topic, const QJsonObject& data);

private:
    void setupUi();
    void setupConnections();
    void callRpc(const QString& method, const QJsonArray& params = QJsonArray());
    QString formatRate(double rate, int decimals = 6);
    QString formatAmount(double amount);

    // UI elements - Swap Panel
    QComboBox* fromCombo;
    QComboBox* toCombo;
    QLineEdit* amountEdit;
    QLabel* rateLabel;
    QLabel* effectiveRateLabel;
    QLabel* providerLabel;
    QLabel* feeLabel;
    QLabel* lastUpdateLabel;
    QPushButton* convertButton;
    QPushButton* refreshButton;
    QPushButton* findRouteButton;

    // Provider selection
    QComboBox* providerCombo;

    // Route visualization
    QTableWidget* routeTable;
    QLabel* routeDescLabel;

    // Status
    QLabel* statusLabel;
    QProgressBar* progressBar;

    // Data
    RpcClient* rpc_;
    WebSocketClient* ws_;
    QTimer refreshTimer_;
    QJsonObject currentRoute_;
    QJsonObject currentRateInfo_;

    // Auto-refresh
    static constexpr int REFRESH_INTERVAL_MS = 15000;  // 15 seconds
};
