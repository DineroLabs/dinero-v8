// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"
#include <QWidget>

class QLabel;
class QPushButton;

namespace dinero::qt::dashboard {

// ⚡ YOU section — renders the current node's identity, posture,
// relay role, mining status, uptime, and daemon version.
class IdentitySection : public QWidget {
    Q_OBJECT
public:
    explicit IdentitySection(QWidget* parent = nullptr);

public Q_SLOTS:
    void onIdentityUpdated(const NodeIdentity& id);
    void onDaemonStateChanged(bool reachable);

public:
    // Static formatting helpers exposed for unit testing.
    static QString formatNodeIdHex(const QString& raw_hex);
    static QString reachabilityLine(const NodeIdentity& id);
    static QString relayingLine(const NodeIdentity& id);
    static QString miningLine(const NodeIdentity& id);
    static QString footerLine(const NodeIdentity& id);

private:
    QLabel*      nodeIdLabel_{nullptr};
    QPushButton* nodeIdCopyBtn_{nullptr};
    QLabel*      reachabilityLabel_{nullptr};
    QLabel*      relayingLabel_{nullptr};
    QLabel*      miningLabel_{nullptr};
    QLabel*      footerLabel_{nullptr};
};

}  // namespace dinero::qt::dashboard
