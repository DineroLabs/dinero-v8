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
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QStringList>
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

}  // namespace dinero::qt::dashboard
