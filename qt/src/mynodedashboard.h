// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <QWidget>

class RpcClient;

namespace dinero::qt::dashboard {

class NodePoller;
class IdentitySection;
class NetworkSection;
class PeersSection;

class MyNodeDashboard : public QWidget {
    Q_OBJECT
public:
    explicit MyNodeDashboard(RpcClient* rpc, QWidget* parent = nullptr);

    void start();   // begin polling
    void stop();    // stop polling

private:
    NodePoller*      poller_{nullptr};
    IdentitySection* identitySection_{nullptr};
    NetworkSection*  networkSection_{nullptr};
    PeersSection*    peersSection_{nullptr};
};

}  // namespace dinero::qt::dashboard
