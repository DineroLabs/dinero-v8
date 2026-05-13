#pragma once

#include <QObject>
#include <QTimer>
#include <QList>
#include <atomic>
#include "gui-desktop/utils/net_defaults.h"

// Forward declarations
namespace dinero::ui {
    class SmoothTransitions;
}

namespace dinero::widgets {
    class NetworkStatusWidget;
    class BlockchainInfoWidget;
    class MempoolStatusWidget;
}

namespace dinero::network {

/**
 * Animated Network Switcher
 * Provides smooth animated transitions when switching between networks
 */
class AnimatedNetworkSwitcher : public QObject {
    Q_OBJECT

public:
    explicit AnimatedNetworkSwitcher(QObject* parent = nullptr);

    // Network switching with animations
    void requestNetworkSwitch(Network targetNetwork);
    
    // Widget registration for animations
    void registerNetworkStatusWidget(dinero::widgets::NetworkStatusWidget* widget);
    void registerBlockchainInfoWidget(dinero::widgets::BlockchainInfoWidget* widget);
    void registerMempoolStatusWidget(dinero::widgets::MempoolStatusWidget* widget);
    
    // Error handling
    void handleSwitchError(const QString& error);
    
    bool isSwitchInProgress() const { return m_switchInProgress; }

signals:
    void switchStarted(Network targetNetwork);
    void performNetworkSwitch(Network targetNetwork);
    void switchCompleted(Network targetNetwork);
    void switchFailed(Network targetNetwork, const QString& error);

private slots:
    void performDelayedSwitch();

private:
    QString getNetworkName(Network network) const;
    Network convertWidgetNetworkType(dinero::widgets::NetworkStatusWidget::NetworkType widgetType) const;
    
    // Animation system
    dinero::ui::SmoothTransitions* m_transitions;
    
    // Switch state
    std::atomic<bool> m_switchInProgress;
    Network m_targetNetwork = Network::Regtest;
    QTimer* m_switchTimer;
    
    // Registered widgets for animation
    QList<dinero::widgets::NetworkStatusWidget*> m_networkStatusWidgets;
    QList<dinero::widgets::BlockchainInfoWidget*> m_blockchainInfoWidgets;
    QList<dinero::widgets::MempoolStatusWidget*> m_mempoolStatusWidgets;
};

} // namespace dinero::network
