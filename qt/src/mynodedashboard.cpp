// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "mynodedashboard.h"

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

    identitySection_ = new IdentitySection(content);
    networkSection_  = new NetworkSection(content);
    peersSection_    = new PeersSection(content);

    layout->addWidget(identitySection_);
    layout->addWidget(networkSection_);
    layout->addWidget(peersSection_, 1);

    scroll->setWidget(content);
    outer->addWidget(scroll);

    poller_ = new NodePoller(rpc, this);
    connect(poller_, &NodePoller::identityUpdated,
            identitySection_, &IdentitySection::onIdentityUpdated);
    connect(poller_, &NodePoller::chainInfoUpdated,
            networkSection_, &NetworkSection::onChainInfoUpdated);
    connect(poller_, &NodePoller::peersUpdated,
            peersSection_, &PeersSection::onPeersUpdated);
    connect(poller_, &NodePoller::daemonStateChanged,
            identitySection_, &IdentitySection::onDaemonStateChanged);
}

void MyNodeDashboard::start() { poller_->start(); }
void MyNodeDashboard::stop()  { poller_->stop();  }

}  // namespace dinero::qt::dashboard
