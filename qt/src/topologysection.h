// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"

#include <QColor>
#include <QFrame>
#include <QPointF>

class QLabel;
class QGraphicsScene;
class QGraphicsView;

namespace dinero::qt::dashboard {

// Passive local topology view for Cmd+K. It renders the Qt-side
// TopologySnapshot; it does not initiate network actions.
class TopologySection : public QFrame {
    Q_OBJECT
public:
    explicit TopologySection(QWidget* parent = nullptr);

    int renderedNodeCountForTest() const { return rendered_nodes_; }
    int renderedEdgeCountForTest() const { return rendered_edges_; }

public Q_SLOTS:
    void setTopologySnapshot(const TopologySnapshot& snapshot);

Q_SIGNALS:
    void copyEndpointRequested(const QString& endpoint);
    void disconnectPeerRequested(const QString& peer_addr);
    void tryDirectReconnectRequested(const QString& endpoint);
    void banPeerRequested(const QString& endpoint, int seconds);
    void dialRelayHintRequested(const HintRow& hint);

private Q_SLOTS:
    void onContextMenuRequested(const QPoint& pos);

private:
    QLabel* header_label_{nullptr};
    QGraphicsView* view_{nullptr};
    QGraphicsScene* scene_{nullptr};
    int rendered_nodes_{0};
    int rendered_edges_{0};

    static QPointF positionFor(int index, int count, double radius,
                               double phase_radians = 0.0);
    static QString nodeTooltip(const TopologyNode& node);
    static QColor colorForNode(const TopologyNode& node);
    static QColor colorForEdge(const TopologyEdge& edge);
    static QString targetIdFromHintNode(const TopologyNode& node);
};

}  // namespace dinero::qt::dashboard
