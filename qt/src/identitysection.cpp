// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "identitysection.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace dinero::qt::dashboard {

IdentitySection::IdentitySection(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(6);

    auto* header = new QLabel("⚡ YOU", this);
    header->setStyleSheet("font-weight: bold; font-size: 14px;");
    root->addWidget(header);

    auto* idRow = new QHBoxLayout();
    nodeIdLabel_ = new QLabel(this);
    nodeIdLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    nodeIdLabel_->setStyleSheet("font-family: monospace;");
    nodeIdCopyBtn_ = new QPushButton("📋", this);
    nodeIdCopyBtn_->setFixedWidth(28);
    nodeIdCopyBtn_->setToolTip("Copy node_id");
    connect(nodeIdCopyBtn_, &QPushButton::clicked, this, [this]() {
        const QString raw = nodeIdLabel_->text().remove(' ');
        QApplication::clipboard()->setText(raw);
    });
    idRow->addWidget(nodeIdLabel_, 1);
    idRow->addWidget(nodeIdCopyBtn_, 0);
    root->addLayout(idRow);

    reachabilityLabel_ = new QLabel(this);
    relayingLabel_     = new QLabel(this);
    miningLabel_       = new QLabel(this);
    dpLabel_           = new QLabel(this);
    footerLabel_       = new QLabel(this);
    footerLabel_->setStyleSheet("color: #888; font-size: 11px;");

    root->addWidget(reachabilityLabel_);
    root->addWidget(relayingLabel_);
    root->addWidget(miningLabel_);
    root->addWidget(dpLabel_);
    root->addWidget(footerLabel_);
    root->addStretch(1);

    onIdentityUpdated({});  // initial placeholder state
    onDynamicP2POverviewUpdated({});  // initial placeholder DPP state
}

void IdentitySection::onIdentityUpdated(const NodeIdentity& id) {
    nodeIdLabel_->setText(formatNodeIdHex(id.node_id_hex));
    reachabilityLabel_->setText(reachabilityLine(id));
    relayingLabel_->setText(relayingLine(id));
    miningLabel_->setText(miningLine(id));
    footerLabel_->setText(footerLine(id));
}

void IdentitySection::onDynamicP2POverviewUpdated(const DynamicP2POverview& overview) {
    if (dpLabel_) dpLabel_->setText(dynamicP2PLine(overview));
}

void IdentitySection::onDaemonStateChanged(bool reachable) {
    if (!reachable) {
        reachabilityLabel_->setText("● UNREACHABLE · daemon not responding");
        reachabilityLabel_->setStyleSheet("color: #c33;");
    } else {
        reachabilityLabel_->setStyleSheet("");
    }
}

QString IdentitySection::formatNodeIdHex(const QString& raw_hex) {
    if (raw_hex.isEmpty()) return "—";
    QString out;
    for (int i = 0; i < raw_hex.size(); i += 4) {
        if (!out.isEmpty()) out += ' ';
        out += raw_hex.mid(i, 4);
    }
    return out;
}

QString IdentitySection::reachabilityLine(const NodeIdentity& id) {
    switch (id.reachability) {
    case NodeIdentity::DIRECT:
        if (id.local_addr.isEmpty() && id.local_port == 0) {
            return "●  DIRECT · reachable";
        }
        if (id.local_addr.isEmpty()) {
            return QString("●  DIRECT · listening on port %1").arg(id.local_port);
        }
        return QString("●  DIRECT · reachable on %1:%2")
            .arg(id.local_addr).arg(id.local_port);
    case NodeIdentity::BEHIND_RELAY:
        if (id.local_port != 0) {
            return QString("●  BEHIND-RELAY · listening on port %1 · NAT'd")
                .arg(id.local_port);
        }
        return "●  BEHIND-RELAY · reachable via relay-virtual peers";
    case NodeIdentity::UNREACHABLE:
        return "○  UNREACHABLE · not listening";
    case NodeIdentity::UNKNOWN:
    default:
        return "○  …";
    }
}

QString IdentitySection::relayingLine(const NodeIdentity& id) {
    if (!id.is_relay_active) {
        return "⤴  RELAYING · OFF";
    }
    return QString("⤴  RELAYING for %1 peer%2 · %3 in grace")
        .arg(id.registrants_count)
        .arg(id.registrants_count == 1 ? "" : "s")
        .arg(id.grace_count);
}

QString IdentitySection::miningLine(const NodeIdentity& id) {
    if (!id.is_mining) {
        return "⛏  MINING · OFF";
    }
    // shares_per_min is reused to carry MH/s when read from mining.status
    return QString("⛏  MINING to %1 · %2 MH/s")
        .arg(id.mining_destination.isEmpty() ? "—" : id.mining_destination)
        .arg(id.shares_per_min, 0, 'f', 2);
}

QString IdentitySection::footerLine(const NodeIdentity& id) {
    const auto secs = id.uptime.count();
    const int h = static_cast<int>(secs / 3600);
    const int m = static_cast<int>((secs % 3600) / 60);
    return QString("uptime  %1h %2m       %3")
        .arg(h).arg(m, 2, 10, QChar('0'))
        .arg(id.subversion.isEmpty() ? "—" : id.subversion);
}

QString IdentitySection::dynamicP2PLine(const DynamicP2POverview& o) {
    if (o.mode.isEmpty()) {
        return "🌐  DYNAMIC P2P · —";
    }
    if (!o.enabled) {
        return QString("🌐  DYNAMIC P2P · %1").arg(o.mode);
    }
    // Active. Surface the counts that matter most to the operator's eye.
    return QString("🌐  DYNAMIC P2P · %1 · %2 hot, %3 warm, %4 demote")
        .arg(o.mode).arg(o.hot_peers).arg(o.warm_candidates).arg(o.demote_candidates);
}

}  // namespace dinero::qt::dashboard
