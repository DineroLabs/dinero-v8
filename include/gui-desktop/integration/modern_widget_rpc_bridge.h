#pragma once

#include <QObject>
#include <QTimer>
#include <QList>
#include "gui-desktop/utils/net_defaults.h" // For Network enum

// Forward declarations
class RpcClient;

namespace dinero::widgets {
    class NetworkStatusWidget;
    class BlockchainInfoWidget;
    class MempoolStatusWidget;
}

namespace dinero::integration {

/**
 * Modern Widget RPC Bridge
 * Connects our polished UI widgets to the RPC backend
 */
class ModernWidgetRpcBridge : public QObject {
    Q_OBJECT

public:
    explicit ModernWidgetRpcBridge(QObject* parent = nullptr);

    // RPC client management
    void setRpcClient(RpcClient* client);
    
    // Widget registration
    void registerNetworkStatusWidget(dinero::widgets::NetworkStatusWidget* widget);
    void registerBlockchainInfoWidget(dinero::widgets::BlockchainInfoWidget* widget);
    void registerMempoolStatusWidget(dinero::widgets::MempoolStatusWidget* widget);

signals:
    void networkSwitchRequested(Network network);

public slots:
    void refreshAllWidgets();

private slots:
    void handleDisconnection();

private:
    void updateNetworkStatusWidget(dinero::widgets::NetworkStatusWidget* widget);
    void updateBlockchainInfoWidget(dinero::widgets::BlockchainInfoWidget* widget);
    void updateMempoolStatusWidget(dinero::widgets::MempoolStatusWidget* widget);
    void handleNetworkSwitchRequest(dinero::widgets::NetworkStatusWidget::NetworkType networkType);
    
    RpcClient* m_rpcClient;
    QTimer* m_refreshTimer;
    
    // Registered widgets
    QList<dinero::widgets::NetworkStatusWidget*> m_networkStatusWidgets;
    QList<dinero::widgets::BlockchainInfoWidget*> m_blockchainInfoWidgets;
    QList<dinero::widgets::MempoolStatusWidget*> m_mempoolStatusWidgets;
};

} // namespace dinero::integration
