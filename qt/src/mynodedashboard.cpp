// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "mynodedashboard.h"

#include "contributionsection.h"
#include "identitysection.h"
#include "networksection.h"
#include "nodepoller.h"
#include "peerssection.h"

#include <QScrollArea>
#include <QVBoxLayout>

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

    layout->addWidget(identitySection_);
    layout->addWidget(networkSection_);
    layout->addWidget(peersSection_, 1);
    layout->addWidget(contributionSection_);

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
    connect(poller_, &NodePoller::chainInfoUpdated,
            networkSection_, &NetworkSection::onChainInfoUpdated);
    connect(poller_, &NodePoller::peersUpdated,
            peersSection_, &PeersSection::onPeersUpdated);
    connect(poller_, &NodePoller::daemonStateChanged,
            identitySection_, &IdentitySection::onDaemonStateChanged);
    connect(poller_, &NodePoller::dynamicP2POverviewUpdated,
            identitySection_, &IdentitySection::onDynamicP2POverviewUpdated);
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
}

void MyNodeDashboard::start() { poller_->start(); }
void MyNodeDashboard::stop()  { poller_->stop();  }

void MyNodeDashboard::setLocalMiningProvider(LocalMiningProvider provider) {
    local_mining_provider_ = std::move(provider);
}

}  // namespace dinero::qt::dashboard
