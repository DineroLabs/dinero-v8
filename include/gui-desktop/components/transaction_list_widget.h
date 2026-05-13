#pragma once

#include <QWidget>
#include <QListWidget>
#include <QListWidgetItem>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>

class ConfirmationProgress;
class TxListModel;

class TransactionListWidget : public QWidget {
    Q_OBJECT

public:
    explicit TransactionListWidget(QWidget* parent = nullptr);
    
    void setModel(TxListModel* model);
    void refreshTransactions();
    
    // Configuration
    void setAutoRefresh(bool enabled, int intervalMs = 30000);
    void setShowConfirmationProgress(bool show) { m_showProgress = show; }
    void setCompactMode(bool compact) { m_compactMode = compact; }

signals:
    void transactionSelected(const QString& txid);
    void transactionDoubleClicked(const QString& txid);

private slots:
    void onModelDataChanged();
    void onItemClicked(QListWidgetItem* item);
    void onItemDoubleClicked(QListWidgetItem* item);
    void onRefreshTimer();

private:
    // Core components
    QListWidget* m_listWidget;
    TxListModel* m_model = nullptr;
    QTimer* m_refreshTimer;
    
    // Configuration
    bool m_showProgress = true;
    bool m_compactMode = false;
    bool m_autoRefresh = true;
    
    // UI helpers
    void setupUI();
    void updateTransactionList();
    QWidget* createTransactionWidget(int modelIndex);
    QWidget* createCompactTransactionWidget(int modelIndex);
    QWidget* createFullTransactionWidget(int modelIndex);
    
    void animateNewTransaction(QListWidgetItem* item);
    void updateConfirmationProgress(int modelIndex, ConfirmationProgress* progressWidget);
    
    // Styling
    QString getTransactionIcon(const QString& category, bool isCoinbase) const;
    QColor getAmountColor(double amount, const QString& category) const;
    QString formatTransactionTime(const QDateTime& time) const;
};
