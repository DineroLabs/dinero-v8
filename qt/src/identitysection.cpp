// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "identitysection.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

namespace dinero::qt::dashboard {

IdentitySection::IdentitySection(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(6);

    auto* header = new QLabel(tr("Connection"), this);
    header->setStyleSheet("font-weight: bold; font-size: 14px;");
    root->addWidget(header);

    reachabilityLabel_ = new QLabel(this);
    reachabilityLabel_->setObjectName(QStringLiteral("connectivitySummary"));
    reachabilityLabel_->setWordWrap(true);
    root->addWidget(reachabilityLabel_);

    advancedDetails_ = new QWidget(this);
    advancedDetails_->setObjectName(QStringLiteral("advancedIdentityDetails"));
    auto* details = new QVBoxLayout(advancedDetails_);
    details->setContentsMargins(0, 0, 0, 0);
    details->setSpacing(6);
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
    details->addLayout(idRow);

    relayingLabel_     = new QLabel(this);
    miningLabel_       = new QLabel(this);
    dpLabel_           = new QLabel(this);
    footerLabel_       = new QLabel(this);
    footerLabel_->setStyleSheet("color: #888; font-size: 11px;");

    details->addWidget(relayingLabel_);
    details->addWidget(miningLabel_);
    details->addWidget(dpLabel_);
    details->addWidget(footerLabel_);
    root->addWidget(advancedDetails_);
    advancedDetails_->setVisible(false);
    onIdentityUpdated({});  // initial placeholder state
    onDynamicP2POverviewUpdated({});  // initial placeholder DPP state
}

void IdentitySection::setAdvancedVisible(bool visible) {
    advancedDetails_->setVisible(visible);
}

void IdentitySection::onIdentityUpdated(const NodeIdentity& id) {
    currentIdentity_ = id;
    nodeIdLabel_->setText(formatNodeIdHex(id.node_id_hex));
    refreshConnectivityLabels();
    miningLabel_->setText(miningLine(id));
    footerLabel_->setText(footerLine(id));
}

void IdentitySection::onOnionServiceUpdated(const OnionServiceStatus& status) {
    torActive_ = status.active;
    refreshConnectivityLabels();
}

void IdentitySection::onRelayServiceStatusUpdated(bool enabled) {
    relayServiceReady_ = enabled;
    refreshConnectivityLabels();
}

void IdentitySection::refreshConnectivityLabels() {
    reachabilityLabel_->setText(connectivitySummaryLine(
        currentIdentity_, torActive_));
    relayingLabel_->setText(relayingLine(currentIdentity_, relayServiceReady_));
}

void IdentitySection::onDynamicP2POverviewUpdated(const DynamicP2POverview& overview) {
    if (dpLabel_) dpLabel_->setText(dynamicP2PLine(overview));
}

void IdentitySection::onDaemonStateChanged(bool reachable) {
    if (!reachable) {
        reachabilityLabel_->setText(tr("○ Offline — the Dinero service is not responding."));
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
            return tr("● Connected directly and securely.");
        }
        if (id.local_addr.isEmpty()) {
            return tr("● Connected directly and securely (port %1).")
                .arg(id.local_port);
        }
        return tr("● Connected directly and securely on %1:%2.")
            .arg(id.local_addr).arg(id.local_port);
    case NodeIdentity::BEHIND_RELAY:
        if (id.local_port != 0) {
            return tr("● Connected securely through a Dinero relay. Direct inbound access is unavailable; recovery is automatic. Listening locally on port %1.")
                .arg(id.local_port);
        }
        return tr("● Connected securely through a Dinero relay. Direct inbound access is unavailable; recovery is automatic.");
    case NodeIdentity::UNREACHABLE:
        return tr("○ Offline — the node is not accepting connections.");
    case NodeIdentity::UNKNOWN:
    default:
        return tr("○ Checking secure connectivity…");
    }
}

QString IdentitySection::connectivitySummaryLine(const NodeIdentity& id,
                                                 bool torActive) {
    QStringList paths;
    if (id.reachability == NodeIdentity::DIRECT) {
        paths << tr("Direct active");
    } else if (id.outbound_connections > 0) {
        paths << tr("Direct outbound active");
    } else {
        paths << tr("Direct connection unavailable");
    }

    if (id.relay_fallback_eligible || id.is_relay_active) {
        paths << tr("Relay fallback ready");
    }
    if (torActive) paths << tr("Tor active");
    return QStringLiteral("● %1").arg(paths.join(QStringLiteral(" · ")));
}

QString IdentitySection::relayingLine(const NodeIdentity& id,
                                      bool relayServiceReady) {
    if (id.registrants_count > 0 || id.is_relay_active) {
        return QString("⤴  RELAY SERVICE · ACTIVE · %1 active circuit%2 · %3 in grace")
        .arg(id.registrants_count)
        .arg(id.registrants_count == 1 ? "" : "s")
        .arg(id.grace_count);
    }
    if (relayServiceReady) {
        return QStringLiteral("⤴  RELAY SERVICE · READY · 0 active circuits");
    }
    return QStringLiteral("⤴  RELAY SERVICE · OFF");
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
