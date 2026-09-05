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
// SHIELDED UI LOCKOUT — single source of truth for the whole Qt app.
//
// The shielded pool IS live on mainnet (shielded_activation_height = 8650), so
// these controls would otherwise work. They are held closed on purpose until
// the spend-authority rollout has an agreed activation height.
//
// A note sent to ANOTHER party's address is currently committed to a key the
// SENDER derives, so the sender retains the ability to spend it back. The
// circuit closing this shipped dormant (shielded_spend_auth_activation_height
// = UINT32_MAX everywhere) and activation additionally needs a paired epoch
// reset. The mainnet pool is empty (shielded_tree_size = 0), so holding the UI
// closed strands nothing.
//
// The daemon independently rejects wallet.shield / wallet.unshield /
// wallet.transfer on mainnet. This UI gate prevents accidental presentation;
// neither gate substitutes for the dormant consensus activation.
//
// Consumed by BOTH shieldedwidget.cpp (tab controls) and mainwindow.cpp (the
// Send tab's mode list). Set to false to restore the feature.
inline constexpr bool kShieldedUiLockedOut = true;

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
    // Make the "Send shielded" title + placeholder show only the active
    // network's prefix (dins on mainnet, tdins testnet, rdins regtest),
    // derived from the loaded receive address, instead of listing all three.
    void applyActiveHrp();
    void loadTransferJournal();
    bool saveTransferJournal(const QString& stage, const QString& address,
                             qint64 amountUna, const QString& memo = {},
                             const QString& txid = {}, qint64 feeUna = 0);
    void clearTransferJournal();
    void setTransferSubmitting(bool submitting);
    bool saveOperationJournal(const QString& operation, const QString& stage,
                              qint64 amountUna, qint64 feeUna = 0,
                              const QString& txid = {});
    void clearOperationJournal(const QString& operation);
    void loadOperationJournals();
    void setShieldSubmitting(bool submitting);
    void setUnshieldSubmitting(bool submitting);

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
    bool shieldSubmitting_ = false;
    QString shieldJournalStage_;

    // Transfer form
    QGroupBox* transferBox_ = nullptr;
    QLineEdit* transferAddressEdit_ = nullptr;
    QLineEdit* transferAmountUnaEdit_ = nullptr;
    QLineEdit* transferAmountDinEdit_ = nullptr;
    QLineEdit* transferFeeEdit_ = nullptr;
    QLineEdit* transferMemoEdit_ = nullptr;
    QPushButton* transferBtn_ = nullptr;
    QLabel* transferResultLabel_ = nullptr;
    bool amountUpdating_ = false;  // re-entrancy guard for DIN↔una mirroring
    bool transferSubmitting_ = false;
    QString transferJournalStage_;

    // Unshield form
    QLineEdit* unshieldAmountEdit_ = nullptr;
    QLineEdit* unshieldFeeEdit_ = nullptr;
    QPushButton* unshieldBtn_ = nullptr;
    QLabel* unshieldResultLabel_ = nullptr;
    QLabel* unshieldAddressLabel_ = nullptr;
    bool unshieldSubmitting_ = false;
    QString unshieldJournalStage_;

    QWidget* fundMovingSurface_ = nullptr;

    // Activity
    QTextEdit* activityLog_ = nullptr;
    bool rpcEverConnected_ = false;
    bool rpcUnavailable_ = false;

    // Notes table
    QTableWidget* notesTable_ = nullptr;

    // Auto-refresh on tip change
    QTimer* tipPollTimer_ = nullptr;
    qint64 lastSeenTip_ = -1;

    bool shieldedActive_ = false;
};
