#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>

class TxListModel : public QAbstractListModel {
    Q_OBJECT
    
public:
    enum Roles { 
        HashRole = Qt::UserRole + 1, 
        TimeRole, 
        AmountRole, 
        ConfRole, 
        CategoryRole, 
        AddressRole,
        DisplayRole,
        // Confirmation progress roles
        ProgressRole,
        SecurityLevelRole,
        IsFullyConfirmedRole,
        IsCoinbaseRole,
        RequiredConfirmationsRole
    };

    explicit TxListModel(QObject* parent = nullptr);
    
    // QAbstractListModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    
    void setData(const QJsonArray& transactions);
    void clear();

private:
    struct Transaction {
        QString hash;
        QString category;       // "send", "receive", "immature", "generate"
        QString address;
        QDateTime time;
        double amount = 0.0;
        int confirmations = 0;
        bool is_coinbase = false;
        int required_confirmations = 6; // 6 for regular, 100 for coinbase
        
        QString getDisplayText() const;
        QString getAmountText() const;
        QString getStatusText() const;
        
        // Confirmation progress helpers
        bool isFullyConfirmed() const { 
            return confirmations >= required_confirmations; 
        }
        double getConfirmationProgress() const { 
            return std::min(1.0, static_cast<double>(confirmations) / required_confirmations); 
        }
        QString getSecurityLevel() const {
            if (is_coinbase) {
                if (confirmations >= required_confirmations) return "Mature";
                if (confirmations >= required_confirmations * 0.8) return "Almost Mature";
                return "Immature";
            }
            if (confirmations >= 6) return "Fully Secure";
            if (confirmations >= 3) return "Moderately Secure";
            if (confirmations >= 1) return "Low Security";
            return "High Risk";
        }
    };
    
    QVector<Transaction> m_transactions;
};

