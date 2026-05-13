#pragma once

#include <QWidget>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QTableWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QPixmap>
#include <QMap>
#include <QComboBox>

class RpcClient;
class WebSocketClient;

/**
 * PaymentsWidget - DineroPay merchant interface
 *
 * Features:
 * - Invoice generation with QR codes
 * - Real-time payment detection via WebSocket
 * - Risk analysis integration
 * - Fiat value display
 * - Confirmation tracking
 * - Payment history
 */
class PaymentsWidget : public QWidget {
    Q_OBJECT

public:
    explicit PaymentsWidget(RpcClient* rpc, WebSocketClient* ws, QWidget* parent = nullptr);
    ~PaymentsWidget();

private Q_SLOTS:
    void onCreateInvoice();
    void onCheckStatus();
    void onShowQrCode();
    void onWebSocketEvent(const QString& topic, const QJsonObject& data);
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);
    void updateFiatValues();
    void onTableSelectionChanged();
    void onBuyDnr();
    void onCurrencyChanged();
    void onToggleFavorite();
    void onSearchCurrency(const QString& text);

private:
    void setupUi();
    void setupConnections();
    void appendPaymentRow(const QJsonObject& payment);
    void updatePaymentStatus(const QString& address, const QJsonObject& status);
    void callRpc(const QString& method, const QJsonArray& params = QJsonArray());
    QString formatDnrAmount(double amount);
    QString formatFiatAmount(double amount);
    QPixmap generateQrCode(const QString& data, int size = 300);
    QString getCurrencySymbol(const QString& currency);
    QStringList getSupportedCurrencies();
    void populateCurrencyCombo();
    void saveFavorites();
    void loadFavorites();

    // UI elements - Invoice Creation
    QLineEdit* addressEdit;
    QLineEdit* amountEdit;
    QLineEdit* labelEdit;
    QPushButton* createButton;

    // Multi-fiat and on-ramp
    QComboBox* currencyCombo;
    QLineEdit* currencySearch;
    QPushButton* favoriteButton;
    QPushButton* buyButton;

    // Payments table
    QTableWidget* paymentTable;

    // Selected payment details
    QLabel* qrLabel;
    QLabel* selectedAddressLabel;
    QLabel* selectedAmountLabel;
    QLabel* fiatValueLabel;
    QLabel* statusLabel;
    QPushButton* showQrButton;
    QPushButton* checkStatusButton;

    // ARP (Anchor Reference Price) display
    QLabel* arpInfoLabel;
    QLabel* arpPriceLabel;
    QLabel* arpBlendLabel;

    // Data
    RpcClient* rpc_;
    WebSocketClient* ws_;
    QMap<QString, QJsonObject> activeInvoices_;  // address → payment info
    QMap<QString, QString> subscriptionIds_;     // address → subscription_id
    QMap<QString, double> fiatRates_;            // currency → rate (DIN to fiat)
    QString selectedCurrency_;
    QStringList favoriteCurrencies_;             // User's favorite currencies

    // ARP (Anchor Reference Price) data
    double arpPriceUsd_;                         // Current ARP price (e.g., 0.10 USD/DIN)
    double arpConfidence_;                       // Market confidence (0.0-1.0)
    QString arpSource_;                          // "arp_only", "blended", "market"

    // Fiat update timer
    QTimer fiatUpdateTimer_;
    static constexpr int FIAT_UPDATE_INTERVAL_MS = 30000;  // 30 seconds
};
