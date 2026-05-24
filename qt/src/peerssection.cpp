// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "peerssection.h"

#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace dinero::qt::dashboard {

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

    item->setText(3, r.height < 0 ? QString("—")
                                  : QString::number(r.height));
    item->setText(4, r.ping_ms < 0 ? QString("—")
                                   : QString("%1 ms").arg(r.ping_ms));

    item->setData(0, Qt::UserRole + 1, r.addr);
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

}  // namespace dinero::qt::dashboard
