// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "peerssection.h"

#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

namespace dinero::qt::dashboard {

namespace {

constexpr qint64 kStaleHeightThreshold = 12;

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

PeersSection::PeersSection(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(6);

    headerLabel_ = new QLabel("🛰 PEERS (0 connected)", this);
    headerLabel_->setStyleSheet("font-weight: bold; font-size: 14px;");
    root->addWidget(headerLabel_);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(5);
    tree_->setHeaderLabels({"Q", "dir", "who", "height", "ping"});
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(false);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::DescendingOrder);
    tree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    connect(tree_, &QTreeWidget::itemClicked,
            this, &PeersSection::onRowClicked);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested,
            this, &PeersSection::onContextMenuRequested);
    root->addWidget(tree_, 1);
}

QString PeersSection::stoplightGlyph(int q, bool handshake_complete) {
    if (!handshake_complete) return "○";
    if (q < 0)               return "○";
    if (q >= 70)             return "●";
    if (q >= 40)             return "◐";
    return "⚠";
}

void PeersSection::onPeersUpdated(const QVector<PeerRow>& peers) {
    lastPeers_ = peers;
    headerLabel_->setText(QString("🛰 PEERS (%1 connected)")
        .arg(peers.size()));

    tree_->setSortingEnabled(false);
    tree_->clear();

    for (const auto& r : peers) {
        auto* item = new QTreeWidgetItem(tree_);
        populateRow(item, r);
        if (expandedAddrs_.contains(r.addr)) {
            populateDetailChild(item, r);
            item->setExpanded(true);
        }
    }

    tree_->setSortingEnabled(true);
}

void PeersSection::setReferenceHeight(qint64 height) {
    if (referenceHeight_ == height) return;
    referenceHeight_ = height;
    if (!lastPeers_.isEmpty()) onPeersUpdated(lastPeers_);
}

void PeersSection::populateRow(QTreeWidgetItem* item, const PeerRow& r) {
    const QString glyph = stoplightGlyph(r.quality_score, r.handshake_complete);
    item->setText(0, QString("%1 %2").arg(glyph)
        .arg(r.quality_score < 0 ? QString("—") : QString::number(r.quality_score)));
    item->setData(0, Qt::UserRole, r.quality_score);

    item->setText(1, r.via_relay ? "⇄" : (r.is_inbound ? "↓" : "↑"));

    QString who = r.addr;
    if (!r.fleet_name.isEmpty()) who += " · " + r.fleet_name;
    if (r.via_relay && !r.relay_via_addr.isEmpty()) {
        who += QString(" (via %1)").arg(r.relay_via_addr);
    }
    item->setText(2, who);

    const bool stale = r.height >= 0 && referenceHeight_ > 0 &&
                       referenceHeight_ - r.height > kStaleHeightThreshold;
    item->setText(3, r.height < 0 ? QString("—")
                                  : stale ? tr("%1 · stale").arg(r.height)
                                          : QString::number(r.height));
    if (stale) {
        item->setToolTip(3, tr("This peer is %1 blocks behind your current network estimate.")
            .arg(referenceHeight_ - r.height));
        item->setForeground(3, QColor(QStringLiteral("#d99a3e")));
    }
    item->setText(4, r.ping_ms < 0 ? QString("—")
                                   : QString("%1 ms").arg(r.ping_ms));

    item->setData(0, Qt::UserRole + 1, r.addr);
    item->setData(0, Qt::UserRole + 2, QVariant::fromValue(r));
}

void PeersSection::populateDetailChild(QTreeWidgetItem* parent,
                                       const PeerRow& r) {
    auto* child = new QTreeWidgetItem(parent);
    const QString detail = QString(
        "services 0x%1 · subver %2 · ↑%3 KB ↓%4 KB · age %5s · last msg %6s ago")
        .arg(r.services, 0, 16)
        .arg(r.subversion.isEmpty() ? "—" : r.subversion)
        .arg(r.bytes_sent / 1024)
        .arg(r.bytes_recv / 1024)
        .arg(r.connected_for.count())
        .arg(r.last_message_ago.count());
    child->setFirstColumnSpanned(true);
    child->setText(0, detail);
    child->setForeground(0, Qt::gray);
}

void PeersSection::onRowClicked(QTreeWidgetItem* item, int /*column*/) {
    if (!item || item->parent() != nullptr) return;
    const QString addr = item->data(0, Qt::UserRole + 1).toString();
    if (addr.isEmpty()) return;

    if (expandedAddrs_.contains(addr)) {
        expandedAddrs_.remove(addr);
        while (item->childCount() > 0) {
            delete item->takeChild(0);
        }
        item->setExpanded(false);
    } else {
        expandedAddrs_.insert(addr);
        auto* placeholder = new QTreeWidgetItem(item);
        placeholder->setFirstColumnSpanned(true);
        placeholder->setText(0, "(loading details — refresh in <5s)");
        placeholder->setForeground(0, Qt::gray);
        item->setExpanded(true);
    }
}

void PeersSection::onContextMenuRequested(const QPoint& pos) {
    auto* item = tree_ ? tree_->itemAt(pos) : nullptr;
    if (!item) return;
    if (item->parent()) item = item->parent();
    const auto peer = item->data(0, Qt::UserRole + 2).value<PeerRow>();
    if (peer.addr.isEmpty()) return;

    QMenu menu(this);
    auto* copy_endpoint = menu.addAction(tr("Copy endpoint"));
    auto* copy_details = menu.addAction(tr("Copy peer details"));
    menu.addSeparator();
    auto* reconnect = menu.addAction(tr("Try direct reconnect"));
    reconnect->setEnabled(!peer.addr.isEmpty() && !isRelayEndpoint(peer.addr));
    auto* disconnect = menu.addAction(tr("Disconnect peer"));
    auto* ban_1h = menu.addAction(tr("Ban 1 hour"));
    auto* ban_24h = menu.addAction(tr("Ban 24 hours"));
    const bool can_ban = isBannableEndpoint(peer.addr);
    ban_1h->setEnabled(can_ban);
    ban_24h->setEnabled(can_ban);

    QAction* chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == copy_endpoint) {
        Q_EMIT copyEndpointRequested(peer.addr);
    } else if (chosen == copy_details) {
        Q_EMIT copyPeerDetailsRequested(peer);
    } else if (chosen == reconnect) {
        Q_EMIT tryDirectReconnectRequested(peer.addr);
    } else if (chosen == disconnect) {
        Q_EMIT disconnectPeerRequested(peer.addr);
    } else if (chosen == ban_1h) {
        Q_EMIT banPeerRequested(peer.addr, 3600);
    } else if (chosen == ban_24h) {
        Q_EMIT banPeerRequested(peer.addr, 86400);
    }
}

bool PeersSection::isRelayEndpoint(const QString& endpoint) {
    return endpoint.trimmed().startsWith(QStringLiteral("relay:"));
}

}  // namespace dinero::qt::dashboard
