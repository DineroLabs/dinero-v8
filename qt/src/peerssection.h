// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"
#include <QSet>
#include <QVector>
#include <QWidget>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace dinero::qt::dashboard {

// 🛰 PEERS — sortable table, click row to expand details.
// Phase 1 omits the topology view toggle (Phase 3).
class PeersSection : public QWidget {
    Q_OBJECT
public:
    explicit PeersSection(QWidget* parent = nullptr);

    // Quality stoplight glyph helper, public for unit testing.
    static QString stoplightGlyph(int quality_score, bool handshake_complete);

public Q_SLOTS:
    void onPeersUpdated(const QVector<PeerRow>& peers);

Q_SIGNALS:
    void copyEndpointRequested(const QString& endpoint);
    void copyPeerDetailsRequested(const PeerRow& peer);
    void disconnectPeerRequested(const QString& peer_addr);
    void banPeerRequested(const QString& endpoint, int seconds);
    void tryDirectReconnectRequested(const QString& endpoint);

private Q_SLOTS:
    void onRowClicked(QTreeWidgetItem* item, int column);
    void onContextMenuRequested(const QPoint& pos);

private:
    QLabel*      headerLabel_{nullptr};
    QTreeWidget* tree_{nullptr};
    QSet<QString> expandedAddrs_;  // persisted across polls

    void populateRow(QTreeWidgetItem* item, const PeerRow& r);
    void populateDetailChild(QTreeWidgetItem* parent, const PeerRow& r);
    static bool isRelayEndpoint(const QString& endpoint);
};

}  // namespace dinero::qt::dashboard
