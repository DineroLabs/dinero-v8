#pragma once

#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QGroupBox>
#include <QTextEdit>
#include <QTableWidget>
#include <QComboBox>
#include <QDateTime>
#include <QJsonValue>
#include <QTimer>

class RpcClient;

// ─────────────────────────────────────────────────────────────────────────────
// Shielded pool surface — wire-up to the v7 daemon's Phase-5-complete shielded
// RPCs (wallet.shield, wallet.unshield, wallet.transfer, wallet.shieldedbalance,
// wallet.listshielded, wallet.getshieldedaddress).
//
// On mainnet/testnet the activation gate is currently UINT32_MAX, so all
// shielded RPCs return `shielded_not_active`. The widget surfaces that state
// in a banner; on regtest the same widget is fully functional.
// ─────────────────────────────────────────────────────────────────────────────
class ShieldedWidget : public QWidget {
    Q_OBJECT
public:
    explicit ShieldedWidget(RpcClient* rpc, QWidget* parent = nullptr);
    void setWalletScope(const QString& walletName);

public Q_SLOTS:
    void refresh();  // re-fetch balance + receive address

private Q_SLOTS:
    void onRpcResult(const QString& method, const QJsonValue& result);
    void onRpcError(const QString& method, int code, const QString& message);
    void onShieldClicked();
    void onTransferClicked();
    void onUnshieldClicked();
    void onNewAddressClicked();
    void onCopyAddressClicked();
    void onAddressBookActivated(int idx);
    void onAmountDinChanged(const QString& text);
    void onAmountUnaChanged(const QString& text);
    void onTipPoll();

private:
    void setupUI();
    void setActiveBanner(bool active, const QString& reason = {});
    void updateBalanceLabels(const QJsonValue& result);
    void updateReceiveAddress(const QJsonValue& result);
    void updateNotesTable(const QJsonValue& result);
    void recordIssuedAddress(uint64_t j, const QString& addr);
    void loadAddressBook();
    void saveAddressBook() const;
    QString settingsWalletScope() const;
    static QString hrpFromAddress(const QString& addr);

    RpcClient* rpc_;

    // Status banner
    QLabel* statusBanner_ = nullptr;

    // Balance
    QLabel* balanceUnaLabel_ = nullptr;
    QLabel* balanceDinLabel_ = nullptr;
    QLabel* treeSizeLabel_ = nullptr;
    QLabel* noteCountLabel_ = nullptr;
    QLabel* pendingNoteCountLabel_ = nullptr;

    // Receive address
    QLabel* addressLabel_ = nullptr;
    QPushButton* newAddressBtn_ = nullptr;
    QPushButton* copyAddressBtn_ = nullptr;
    QComboBox* addressBookCombo_ = nullptr;
    uint64_t currentJ_ = 0;
    QString currentAddress_;
    struct AddressBookEntry {
        uint64_t j;
        QString address;
        QDateTime issuedAt;
    };
    QList<AddressBookEntry> addressBook_;
    QString walletScope_;

    // Shield form
    QLineEdit* shieldAmountEdit_ = nullptr;
    QLineEdit* shieldFeeEdit_ = nullptr;
    QPushButton* shieldBtn_ = nullptr;
    QLabel* shieldResultLabel_ = nullptr;

    // Transfer form
    QLineEdit* transferAddressEdit_ = nullptr;
    QLineEdit* transferAmountUnaEdit_ = nullptr;
    QLineEdit* transferAmountDinEdit_ = nullptr;
    QLineEdit* transferFeeEdit_ = nullptr;
    QLineEdit* transferMemoEdit_ = nullptr;
    QPushButton* transferBtn_ = nullptr;
    QLabel* transferResultLabel_ = nullptr;
    bool amountUpdating_ = false;  // re-entrancy guard for DIN↔una mirroring

    // Unshield form
    QLineEdit* unshieldAmountEdit_ = nullptr;
    QLineEdit* unshieldFeeEdit_ = nullptr;
    QPushButton* unshieldBtn_ = nullptr;
    QLabel* unshieldResultLabel_ = nullptr;
    QLabel* unshieldAddressLabel_ = nullptr;

    // Activity
    QTextEdit* activityLog_ = nullptr;

    // Notes table
    QTableWidget* notesTable_ = nullptr;

    // Auto-refresh on tip change
    QTimer* tipPollTimer_ = nullptr;
    qint64 lastSeenTip_ = -1;

    bool shieldedActive_ = false;
};
