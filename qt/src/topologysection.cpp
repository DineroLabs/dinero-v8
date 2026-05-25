// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "topologysection.h"

#include <QBrush>
#include <QFont>
#include <QGraphicsEllipseItem>
#include <QGraphicsLineItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QRegularExpression>
#include <QStringList>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace dinero::qt::dashboard {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kNodeRadius = 16.0;
constexpr double kSceneHalfWidth = 260.0;
constexpr double kSceneHalfHeight = 170.0;

QVector<TopologyNode> nodesOfKind(const TopologySnapshot& snapshot,
                                  std::initializer_list<QString> kinds) {
    QVector<TopologyNode> out;
    for (const auto& node : snapshot.nodes) {
        for (const auto& kind : kinds) {
            if (node.kind == kind) {
                out.push_back(node);
                break;
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    return out;
}

bool isBannableEndpoint(const QString& endpoint) {
    if (endpoint.trimmed().startsWith(QStringLiteral("relay:"))) {
        return false;
    }
    QString host = endpoint.trimmed();
    if (host.startsWith('[')) {
        const int close = host.indexOf(']');
        if (close <= 1) return false;
        host = host.mid(1, close - 1);
    } else if (host.count(':') == 1) {
        host = host.left(host.lastIndexOf(':'));
    }
    static const QRegularExpression ipv4(
        QStringLiteral(R"(^(\d{1,3}\.){3}\d{1,3}$)"));
    static const QRegularExpression ipv6(
        QStringLiteral(R"(^[0-9a-fA-F:]+$)"));
    return ipv4.match(host).hasMatch() ||
           (host.contains(':') && ipv6.match(host).hasMatch());
}

}  // namespace

TopologySection::TopologySection(QWidget* parent) : QFrame(parent) {
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Sunken);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    header_label_ = new QLabel(tr("Topology — waiting for peers"), this);
    QFont f = header_label_->font();
    f.setBold(true);
    header_label_->setFont(f);
    root->addWidget(header_label_);

    scene_ = new QGraphicsScene(this);
    scene_->setSceneRect(-kSceneHalfWidth, -kSceneHalfHeight,
                         kSceneHalfWidth * 2.0, kSceneHalfHeight * 2.0);

    view_ = new QGraphicsView(scene_, this);
    view_->setRenderHint(QPainter::Antialiasing, true);
    view_->setFrameShape(QFrame::NoFrame);
    view_->setMinimumHeight(260);
    view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view_, &QGraphicsView::customContextMenuRequested,
            this, &TopologySection::onContextMenuRequested);
    root->addWidget(view_);
}

void TopologySection::setTopologySnapshot(const TopologySnapshot& snapshot) {
    scene_->clear();
    rendered_nodes_ = snapshot.nodes.size();
    rendered_edges_ = snapshot.edges.size();

    header_label_->setText(tr("Topology — %1 nodes / %2 paths")
        .arg(rendered_nodes_)
        .arg(rendered_edges_));

    QHash<QString, QPointF> positions;
    const QPointF center(0.0, 0.0);

    for (const auto& node : snapshot.nodes) {
        if (node.kind == QStringLiteral("self")) {
            positions.insert(node.id, center);
            break;
        }
    }

    const auto connected = nodesOfKind(snapshot, {
        QStringLiteral("direct"),
        QStringLiteral("fleet"),
        QStringLiteral("relay_virtual"),
    });
    for (int i = 0; i < connected.size(); ++i) {
        positions.insert(connected[i].id,
                         positionFor(i, connected.size(), 104.0, -kPi / 2.0));
    }

    const auto hints = nodesOfKind(snapshot, {QStringLiteral("hint")});
    for (int i = 0; i < hints.size(); ++i) {
        positions.insert(hints[i].id,
                         positionFor(i, hints.size(), 154.0, -kPi / 3.0));
    }

    for (const auto& edge : snapshot.edges) {
        if (!positions.contains(edge.from_id) || !positions.contains(edge.to_id)) {
            continue;
        }
        QPen pen(colorForEdge(edge));
        pen.setWidthF(edge.kind == QStringLiteral("hint") ? 1.1 : 1.8);
        if (edge.kind == QStringLiteral("relay_virtual")) {
            pen.setStyle(Qt::DashLine);
        } else if (edge.kind == QStringLiteral("hint")) {
            pen.setStyle(Qt::DotLine);
        }
        auto* item = scene_->addLine(QLineF(positions.value(edge.from_id),
                                            positions.value(edge.to_id)),
                                     pen);
        if (!edge.via_relay.isEmpty()) {
            item->setToolTip(tr("%1 via %2").arg(edge.kind, edge.via_relay));
        } else {
            item->setToolTip(edge.kind);
        }
    }

    for (const auto& node : snapshot.nodes) {
        const QPointF p = positions.value(node.id, center);
        const QColor c = colorForNode(node);
        auto* dot = scene_->addEllipse(p.x() - kNodeRadius,
                                       p.y() - kNodeRadius,
                                       kNodeRadius * 2.0,
                                       kNodeRadius * 2.0,
                                       QPen(c.darker(135), 1.4),
                                       QBrush(c));
        dot->setToolTip(nodeTooltip(node));
        dot->setData(0, QVariant::fromValue(node));

        auto* text = scene_->addText(node.label);
        QFont font = text->font();
        font.setPointSize(node.kind == QStringLiteral("self") ? 9 : 8);
        font.setBold(node.kind == QStringLiteral("self") ||
                     node.kind == QStringLiteral("fleet"));
        text->setFont(font);
        text->setDefaultTextColor(palette().color(QPalette::WindowText));
        const QRectF b = text->boundingRect();
        text->setPos(p.x() - b.width() / 2.0, p.y() + kNodeRadius + 3.0);
        text->setToolTip(nodeTooltip(node));
        text->setData(0, QVariant::fromValue(node));
    }
}

void TopologySection::onContextMenuRequested(const QPoint& pos) {
    const QPointF scene_pos = view_->mapToScene(pos);
    TopologyNode node;
    bool found = false;
    for (auto* item : scene_->items(scene_pos)) {
        const auto value = item->data(0);
        if (value.canConvert<TopologyNode>()) {
            node = value.value<TopologyNode>();
            found = true;
            break;
        }
    }
    if (!found || node.kind == QStringLiteral("self")) return;

    QMenu menu(this);
    auto* copy_endpoint = menu.addAction(tr("Copy endpoint"));
    copy_endpoint->setEnabled(!node.endpoint.isEmpty());
    auto* reconnect = menu.addAction(tr("Try direct reconnect"));
    reconnect->setEnabled(node.kind != QStringLiteral("relay_virtual") &&
                          node.kind != QStringLiteral("hint") &&
                          !node.endpoint.isEmpty());
    QAction* dial_hint = nullptr;
    if (node.kind == QStringLiteral("hint")) {
        dial_hint = menu.addAction(tr("Dial via relay hint"));
    }
    QAction* disconnect = nullptr;
    QAction* ban_1h = nullptr;
    QAction* ban_24h = nullptr;
    if (node.connected) {
        menu.addSeparator();
        disconnect = menu.addAction(tr("Disconnect peer"));
        ban_1h = menu.addAction(tr("Ban 1 hour"));
        ban_24h = menu.addAction(tr("Ban 24 hours"));
        const bool can_ban = isBannableEndpoint(node.endpoint);
        ban_1h->setEnabled(can_ban);
        ban_24h->setEnabled(can_ban);
    }

    QAction* chosen = menu.exec(view_->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == copy_endpoint) {
        Q_EMIT copyEndpointRequested(node.endpoint);
    } else if (chosen == reconnect) {
        Q_EMIT tryDirectReconnectRequested(node.endpoint);
    } else if (chosen == dial_hint) {
        HintRow hint;
        hint.target_node_id_hex = targetIdFromHintNode(node);
        hint.endpoint = node.endpoint;
        Q_EMIT dialRelayHintRequested(hint);
    } else if (chosen == disconnect) {
        Q_EMIT disconnectPeerRequested(node.endpoint);
    } else if (chosen == ban_1h) {
        Q_EMIT banPeerRequested(node.endpoint, 3600);
    } else if (chosen == ban_24h) {
        Q_EMIT banPeerRequested(node.endpoint, 86400);
    }
}

QPointF TopologySection::positionFor(int index, int count, double radius,
                                     double phase_radians) {
    if (count <= 0) return {};
    const double angle = phase_radians + (2.0 * kPi * index / count);
    return QPointF(std::cos(angle) * radius, std::sin(angle) * radius);
}

QString TopologySection::nodeTooltip(const TopologyNode& node) {
    QStringList lines;
    lines << node.label;
    if (!node.endpoint.isEmpty()) lines << node.endpoint;
    lines << QStringLiteral("kind: %1").arg(node.kind);
    if (!node.bucket.isEmpty()) lines << QStringLiteral("bucket: %1").arg(node.bucket);
    if (node.quality_score >= 0) {
        lines << QStringLiteral("quality: %1").arg(node.quality_score);
    }
    lines << (node.connected ? QStringLiteral("connected")
                             : QStringLiteral("known hint only"));
    return lines.join('\n');
}

QColor TopologySection::colorForNode(const TopologyNode& node) {
    if (node.kind == QStringLiteral("self")) return QColor(48, 120, 210);
    if (node.kind == QStringLiteral("fleet")) return QColor(70, 155, 90);
    if (node.kind == QStringLiteral("relay_virtual")) return QColor(130, 85, 210);
    if (node.kind == QStringLiteral("hint")) return QColor(185, 190, 200);
    if (node.bucket == QStringLiteral("demote")) return QColor(220, 145, 65);
    return QColor(80, 150, 220);
}

QColor TopologySection::colorForEdge(const TopologyEdge& edge) {
    if (edge.kind == QStringLiteral("relay_virtual")) return QColor(130, 85, 210);
    if (edge.kind == QStringLiteral("hint")) return QColor(150, 155, 165);
    return QColor(80, 150, 220);
}

QString TopologySection::targetIdFromHintNode(const TopologyNode& node) {
    if (node.id.startsWith(QStringLiteral("hint:"))) {
        return node.id.mid(QStringLiteral("hint:").size());
    }
    return {};
}

}  // namespace dinero::qt::dashboard
