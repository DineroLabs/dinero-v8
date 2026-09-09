#pragma once
#include <QWidget>
#include <QJsonArray>
class RpcClient;
class QLabel;
class QLineEdit;
class QComboBox;
class QSpinBox;
class QTableWidget;
class QPushButton;

// Fixed shielded outputs with an absolute earliest spend height. Availability
// is reported by the daemon; no address-prefix or GUI height bypass exists.
class PrivateCovenantWidget : public QWidget {
    Q_OBJECT
public:
    explicit PrivateCovenantWidget(RpcClient*, QWidget* parent=nullptr);
    void setWalletScope(const QString&);
    void refresh();
    bool submissionPending() const { return !pendingMethod_.isEmpty(); }
private:
    void fund();
    void spend(const QJsonObject&);
    void submit(const QString&, const QJsonObject&);
    void render();
    void updateEnabled();
    QString journalKey() const;
    RpcClient* rpc_;
    QString scope_, pendingMethod_, pendingJournal_, ownerRequestScope_;
    bool active_=false, uncertain_=false;
    QJsonArray notes_;
    QLabel *status_;
    QComboBox *source_;
    QLineEdit *owner_, *fee_, *fundingFee_;
    QSpinBox *height_;
    QTableWidget *outputs_, *contracts_;
    QPushButton *fund_, *resolve_;
};
