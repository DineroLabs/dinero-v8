// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"
#include <QWidget>

class RpcClient;

namespace dinero::qt::dashboard {

class NodePoller;
class IdentitySection;
class NetworkSection;
class PeersSection;
class ContributionSection;
class DiscoverySection;

class MyNodeDashboard : public QWidget {
    Q_OBJECT
public:
    explicit MyNodeDashboard(RpcClient* rpc, QWidget* parent = nullptr);

    void start();   // begin polling
    void stop();    // stop polling

    // The qt-app side knows about miners the daemon doesn't (Qt-launched
    // internal/external/stratum). Set a provider that returns current
    // local mining state; it's merged into the identity update before the
    // IdentitySection renders.
    void setLocalMiningProvider(LocalMiningProvider provider);

private:
    NodePoller*          poller_{nullptr};
    IdentitySection*     identitySection_{nullptr};
    NetworkSection*      networkSection_{nullptr};
    PeersSection*        peersSection_{nullptr};
    ContributionSection* contributionSection_{nullptr};
    DiscoverySection*    discoverySection_{nullptr};

    LocalMiningProvider local_mining_provider_;
};

}  // namespace dinero::qt::dashboard
