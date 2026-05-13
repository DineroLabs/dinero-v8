#pragma once
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QListWidget>
#include <QLabel>
#include <QJsonArray>
#include <QJsonObject>
#include <QListWidgetItem>

class RpcClient;
class LedgerInspector;

class ExplorerTab : public QWidget {
    Q_OBJECT
    
public:
    explicit ExplorerTab(QWidget *parent = nullptr);
    void setRpcClient(RpcClient *client);

public slots:
    void onConnectionChanged(bool connected);

private slots:
    void performSearch();
    void refreshMempool();
    void onMempoolItemClicked(QListWidgetItem *item);

private:
    void setupUI();
    void searchAsBlockHash(const QString &hash);
    void displayTransactionResult(const QJsonObject &tx);
    void updateMempoolList(const QJsonArray &mempool);
    
    // UI Components
    QLineEdit *m_searchEdit;
    QPushButton *m_searchButton;
    QTextEdit *m_resultDisplay;
    QPushButton *m_refreshMempoolButton;
    QLabel *m_mempoolCountLabel;
    QListWidget *m_mempoolList;
    
    // Ledger Inspector
    LedgerInspector *m_ledgerInspector;
    
    // Backend
    RpcClient *m_rpcClient;
};
