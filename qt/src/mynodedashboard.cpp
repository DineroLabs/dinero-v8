// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "mynodedashboard.h"

#include "contributionsection.h"
#include "dashboardactioncontroller.h"
#include "discoverysection.h"
#include "identitysection.h"
#include "networksection.h"
#include "nodepoller.h"
#include "peerssection.h"
#include "rpcclient.h"
#include "topologysection.h"

#include <QScrollArea>
#include <QJsonObject>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace dinero::qt::dashboard {

MyNodeDashboard::MyNodeDashboard(RpcClient* rpc, QWidget* parent)
    : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget(scroll);
    auto* layout  = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    identitySection_     = new IdentitySection(content);
    networkSection_      = new NetworkSection(content);
    peersSection_        = new PeersSection(content);
    contributionSection_ = new ContributionSection(content);
    discoverySection_    = new DiscoverySection(content);
    topologySection_     = new TopologySection(content);
    actionController_    = new DashboardActionController(rpc, this, this);

    layout->addWidget(identitySection_);

    advancedToggle_ = new QToolButton(content);
    advancedToggle_->setObjectName(QStringLiteral("advancedToggle"));
    advancedToggle_->setText(tr("Advanced details"));
    advancedToggle_->setToolTip(tr(
        "Show technical node, peer, relay, and discovery diagnostics."));
    advancedToggle_->setCheckable(true);
    // Keep the operator dashboard discoverable.  A previous collapsed-by-
    // default version combined with a stretch in IdentitySection could push
    // this control below the viewport, making every diagnostic appear lost.
    // Existing users get the complete dashboard; their explicit choice is
    // remembered after the first toggle.
    const bool advanced_visible =
        QSettings().value(QStringLiteral("dashboard/advanced_visible"), true).toBool();
    advancedToggle_->setChecked(advanced_visible);
    advancedToggle_->setArrowType(Qt::RightArrow);
    advancedToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    layout->addWidget(advancedToggle_);

    advancedContainer_ = new QWidget(content);
    advancedContainer_->setObjectName(QStringLiteral("advancedContainer"));
    auto* advancedLayout = new QVBoxLayout(advancedContainer_);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    advancedLayout->setSpacing(0);
    advancedLayout->addWidget(networkSection_);
    advancedLayout->addWidget(peersSection_, 1);
    advancedLayout->addWidget(contributionSection_);
    advancedLayout->addWidget(discoverySection_);
    advancedLayout->addWidget(topologySection_);
    advancedContainer_->setVisible(advanced_visible);
    identitySection_->setAdvancedVisible(advanced_visible);
    advancedToggle_->setArrowType(advanced_visible ? Qt::DownArrow : Qt::RightArrow);
    advancedToggle_->setText(advanced_visible ? tr("Hide advanced details")
                                               : tr("Advanced details"));
    layout->addWidget(advancedContainer_);
    connect(advancedToggle_, &QToolButton::toggled, this, [this](bool expanded) {
        advancedToggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        advancedToggle_->setText(expanded ? tr("Hide advanced details")
                                          : tr("Advanced details"));
        advancedContainer_->setVisible(expanded);
        identitySection_->setAdvancedVisible(expanded);
        QSettings().setValue(QStringLiteral("dashboard/advanced_visible"), expanded);
    });

    scroll->setWidget(content);
    outer->addWidget(scroll);

    poller_ = new NodePoller(rpc, this);
    // Intercept identityUpdated so we can overlay qt-app-side local mining
    // state (Qt's internal/external/stratum miners aren't visible to the
    // daemon's mining.status RPC).
    connect(poller_, &NodePoller::identityUpdated,
            this, [this](const NodeIdentity& id) {
                NodeIdentity merged = id;
                if (local_mining_provider_) {
                    const auto lm = local_mining_provider_();
                    if (lm.active) {
                        merged.is_mining = true;
                        if (!lm.miner_type.isEmpty() && lm.miner_type != "none") {
                            merged.mining_destination = lm.miner_type;
                        }
                        merged.shares_per_min = lm.hashrate / 1'000'000.0;
                    }
                    // App uptime: daemon has no getuptime method, so always
                    // overlay the qt-app's launch-time-derived uptime here.
                    if (lm.app_uptime.count() > 0) {
                        merged.uptime = lm.app_uptime;
                    }
                }
                identitySection_->onIdentityUpdated(merged);
            });
    connect(poller_, &NodePoller::chainInfoUpdated, this,
            [this](const ChainInfo& info) {
        networkSection_->onChainInfoUpdated(info);
        peersSection_->setReferenceHeight(
            std::max(info.our_height, info.net_consensus_height));
    });
    connect(poller_, &NodePoller::peersUpdated,
            peersSection_, &PeersSection::onPeersUpdated);
    connect(poller_, &NodePoller::daemonStateChanged,
            identitySection_, &IdentitySection::onDaemonStateChanged);
    connect(poller_, &NodePoller::dynamicP2POverviewUpdated,
            identitySection_, &IdentitySection::onDynamicP2POverviewUpdated);
    connect(poller_, &NodePoller::onionServiceUpdated,
            networkSection_, &NetworkSection::setOnionServiceStatus);
    connect(poller_, &NodePoller::onionServiceUpdated,
            identitySection_, &IdentitySection::onOnionServiceUpdated);
    connect(networkSection_, &NetworkSection::torModeRequested,
            this, [rpc](const QString& mode) {
        if (rpc) rpc->callNamed(QStringLiteral("network.setonionservice"),
                                QJsonObject{{QStringLiteral("mode"), mode}});
    });
    connect(networkSection_, &NetworkSection::relayServiceRequested,
            this, [rpc](const QJsonObject& request) {
        if (rpc) rpc->callNamed(QStringLiteral("network.setrelayservice"), request);
    });
    if (rpc) rpc->callNamed(QStringLiteral("network.getrelayservice"), QJsonObject{});
    if (rpc) {
        connect(rpc, &RpcClient::rpcResult, this,
                [this](const QString& method, const QJsonValue& result) {
            if (method == QStringLiteral("network.getrelayservice") ||
                method == QStringLiteral("network.setrelayservice")) {
                const auto status = result.toObject();
                networkSection_->setRelayServiceStatus(status);
                identitySection_->onRelayServiceStatusUpdated(
                    status.value(QStringLiteral("enabled")).toBool());
                return;
            }
            if (method != QStringLiteral("network.setonionservice")) return;
            const auto obj = result.toObject();
            OnionServiceStatus status;
            status.available = true;
            status.requested = obj.value(QStringLiteral("requested")).toBool();
            status.active = obj.value(QStringLiteral("active")).toBool();
            status.address = obj.value(QStringLiteral("address")).toString();
            status.mode = obj.value(QStringLiteral("mode")).toString();
            networkSection_->setOnionServiceStatus(status);
        });
        connect(rpc, &RpcClient::rpcError, this,
                [this](const QString& method, int code, const QString&) {
            if (method == QStringLiteral("network.setonionservice")) {
                networkSection_->setTorActionError(code == -32601);
            }
        });
    }
    connect(poller_, &NodePoller::contributionStatsUpdated,
            this, [this](const ContributionStats& stats) {
        contributionSection_->setContributionStats(stats);
        contributionSection_->setBytesInSamples(poller_->bytesInBuffer());
        contributionSection_->setBytesOutSamples(poller_->bytesOutBuffer());
        contributionSection_->setRelayBytesSamples(poller_->relayBytesBuffer());
        contributionSection_->setBytesInLongWindows(
            poller_->bytesIn5min(),
            poller_->bytesIn1hr(),
            poller_->bytesIn24hr());
        contributionSection_->setBytesOutLongWindows(
            poller_->bytesOut5min(),
            poller_->bytesOut1hr(),
            poller_->bytesOut24hr());
        contributionSection_->setRelayBytesLongWindows(
            poller_->relayBytes5min(),
            poller_->relayBytes1hr(),
            poller_->relayBytes24hr());
    });
    connect(poller_, &NodePoller::decentralizationScoreUpdated,
            contributionSection_, &ContributionSection::setDecentralizationScore);
    connect(poller_, &NodePoller::hintsUpdated,
            discoverySection_, &DiscoverySection::setHints);
    connect(poller_, &NodePoller::topologyUpdated,
            topologySection_, &TopologySection::setTopologySnapshot);

    connect(peersSection_, &PeersSection::copyEndpointRequested,
            actionController_, &DashboardActionController::copyEndpoint);
    connect(peersSection_, &PeersSection::copyPeerDetailsRequested,
            actionController_, &DashboardActionController::copyPeerDetails);
    connect(peersSection_, &PeersSection::disconnectPeerRequested,
            actionController_, &DashboardActionController::disconnectPeer);
    connect(peersSection_, &PeersSection::banPeerRequested,
            actionController_, &DashboardActionController::banPeer);
    connect(peersSection_, &PeersSection::tryDirectReconnectRequested,
            actionController_, &DashboardActionController::tryDirectReconnect);

    connect(discoverySection_, &DiscoverySection::copyEndpointRequested,
            actionController_, &DashboardActionController::copyEndpoint);
    connect(discoverySection_, &DiscoverySection::dialRelayHintRequested,
            actionController_, &DashboardActionController::dialRelayHint);
    connect(discoverySection_, &DiscoverySection::seederOptInChanged,
            actionController_, &DashboardActionController::setSeederOptIn);
    connect(discoverySection_, &DiscoverySection::startSeederRequested,
            actionController_, &DashboardActionController::startSeeder);
    connect(discoverySection_, &DiscoverySection::stopSeederRequested,
            actionController_, &DashboardActionController::stopSeeder);
    connect(actionController_, &DashboardActionController::seederStateChanged,
            discoverySection_, &DiscoverySection::setSeederState);

    connect(topologySection_, &TopologySection::copyEndpointRequested,
            actionController_, &DashboardActionController::copyEndpoint);
    connect(topologySection_, &TopologySection::disconnectPeerRequested,
            actionController_, &DashboardActionController::disconnectPeer);
    connect(topologySection_, &TopologySection::tryDirectReconnectRequested,
            actionController_, &DashboardActionController::tryDirectReconnect);
    connect(topologySection_, &TopologySection::banPeerRequested,
            actionController_, &DashboardActionController::banPeer);
    connect(topologySection_, &TopologySection::dialRelayHintRequested,
            actionController_, &DashboardActionController::dialRelayHint);
}

void MyNodeDashboard::start() { poller_->start(); }
void MyNodeDashboard::stop()  { poller_->stop();  }

void MyNodeDashboard::setLocalMiningProvider(LocalMiningProvider provider) {
    local_mining_provider_ = std::move(provider);
}

}  // namespace dinero::qt::dashboard
