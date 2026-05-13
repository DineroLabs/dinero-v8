#pragma once

#include <QWidget>
#include <QJsonValue>

class QPushButton;
class QLineEdit;
class QTextEdit;
class RpcClient;

/**
 * Developer Tools Pane
 * Provides regtest-only mining and validation controls
 */
class DevPane : public QWidget {
    Q_OBJECT

public:
    explicit DevPane(QWidget* parent = nullptr);
    
    void setRpcClient(RpcClient* rpcClient);
    void checkNetworkAndUpdateVisibility();

signals:
    void blockMined(const QString& blockHash);

private slots:
    void onMineBlock();
    void onValidateGBT();

private:
    void setupUI();
    void showToast(const QString& message);
    
    RpcClient* m_rpcClient = nullptr;
    QPushButton* m_mineBlockBtn = nullptr;
    QPushButton* m_validateGbtBtn = nullptr;
    QLineEdit* m_miningAddressEdit = nullptr;
    QTextEdit* m_statusArea = nullptr;
};
