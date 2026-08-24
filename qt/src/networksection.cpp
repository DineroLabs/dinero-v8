// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "networksection.h"

#include <QGridLayout>
#include <QCheckBox>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>
#include <algorithm>

namespace dinero::qt::dashboard {

NetworkSection::NetworkSection(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* header = new QLabel(tr("Network diagnostics (your node's view)"), this);
    header->setStyleSheet("font-weight: bold; font-size: 14px;");
    root->addWidget(header);

    youBar_ = new QProgressBar(this);
    youBar_->setFormat("you  %v");
    youBar_->setTextVisible(true);
    youBar_->setStyleSheet(
        "QProgressBar { min-height: 12px; border: 1px solid #2f3a4d; border-radius: 3px; text-align: center; }"
        "QProgressBar::chunk { background-color: #4f9cff; }");
    root->addWidget(youBar_);

    netBar_ = new QProgressBar(this);
    netBar_->setFormat("peer estimate  %v");
    netBar_->setTextVisible(true);
    netBar_->setStyleSheet(
        "QProgressBar { min-height: 12px; border: 1px solid #2f3a4d; border-radius: 3px; text-align: center; }"
        "QProgressBar::chunk { background-color: #4f9cff; }");
    root->addWidget(netBar_);

    deltaLabel_ = new QLabel(this);
    deltaLabel_->setStyleSheet("font-weight: bold;");
    root->addWidget(deltaLabel_);

    auto* grid = new QGridLayout();
    difficultyLabel_ = new QLabel(this);
    mempoolLabel_    = new QLabel(this);
    medianFeeLabel_  = new QLabel(this);
    grid->addWidget(new QLabel("difficulty", this), 0, 0);
    grid->addWidget(difficultyLabel_,               0, 1);
    grid->addWidget(new QLabel("mempool",    this), 1, 0);
    grid->addWidget(mempoolLabel_,                  1, 1);
    grid->addWidget(new QLabel("median fee", this), 2, 0);
    grid->addWidget(medianFeeLabel_,                2, 1);
    root->addLayout(grid);

    torControl_ = new QCheckBox(tr("Improve reachability with Tor"), this);
    torControl_->setObjectName(QStringLiteral("torReachabilityControl"));
    torControl_->setEnabled(false);
    torControl_->setToolTip(tr(
        "Tor is configured when the Dinero service starts. This control shows status only; it never installs or starts Tor."));
    root->addWidget(torControl_);
    torStatusLabel_ = new QLabel(this);
    torStatusLabel_->setObjectName(QStringLiteral("torStatus"));
    torStatusLabel_->setWordWrap(true);
    root->addWidget(torStatusLabel_);
    root->addStretch(1);

    onChainInfoUpdated({});
    setTorStatus(TorState::Unsupported);
}

void NetworkSection::onChainInfoUpdated(const ChainInfo& info) {
    const qint64 maxH = std::max(info.our_height, info.max_peer_height);
    if (maxH > 0) {
        youBar_->setRange(0, static_cast<int>(maxH));
        youBar_->setValue(static_cast<int>(info.our_height));
        youBar_->setFormat(QStringLiteral("you  %1").arg(info.our_height));
        netBar_->setRange(0, static_cast<int>(maxH));
        netBar_->setValue(static_cast<int>(info.net_consensus_height));
        netBar_->setFormat(tr("peer estimate  %1").arg(info.net_consensus_height));
    }

    deltaLabel_->setText(
        tipDeltaAnnotation(info.our_height, info.net_consensus_height));

    difficultyLabel_->setText(info.difficulty > 0.0
        ? QString::number(info.difficulty, 'f', 2)
        : QStringLiteral("—"));
    mempoolLabel_->setText(QString("%1 tx / %2 KB")
        .arg(info.mempool_tx_count)
        .arg(info.mempool_bytes / 1024));
    medianFeeLabel_->setText(info.has_median_fee
        ? QStringLiteral("%1 una/vB").arg(info.median_fee_una_per_vbyte, 0, 'f', 2)
        : QStringLiteral("—"));
}

QString NetworkSection::tipDeltaAnnotation(qint64 our, qint64 net) {
    if (net <= 0 && our <= 0) return "—";
    const qint64 delta = net - our;
    if (delta == 0) return tr("● In sync with the peer estimate");
    if (delta > 0)  return tr("● %1 block(s) behind the peer estimate").arg(delta);
    return tr("● %1 block(s) ahead of the peer estimate; peers may still be catching up")
        .arg(-delta);
}

void NetworkSection::setTorStatus(TorState state, const QString& onionAddress) {
    torControl_->setChecked(state == TorState::Active || state == TorState::Error);
    torStatusLabel_->setText(torStatusText(state, onionAddress));
}

void NetworkSection::setOnionServiceStatus(const OnionServiceStatus& status) {
    if (!status.available) {
        setTorStatus(TorState::Unsupported);
    } else if (!status.requested) {
        setTorStatus(TorState::Off);
    } else if (status.active) {
        setTorStatus(TorState::Active, status.address);
    } else {
        setTorStatus(TorState::Error);
        // Keep daemon-provided error details out of this surface: Tor control
        // messages can contain local paths or authentication configuration.
        torStatusLabel_->setToolTip(tr("Check the local daemon log for details."));
    }
}

QString NetworkSection::torStatusText(TorState state, const QString& onionAddress) {
    switch (state) {
    case TorState::Off:
        return tr("Off. To opt in, enable listenonion in the daemon configuration and restart. Tor will not be installed or started automatically.");
    case TorState::Active: {
        const QString safeAddress = onionAddress.trimmed().endsWith(
            QStringLiteral(".onion"), Qt::CaseInsensitive) ? onionAddress.trimmed() : QString();
        return safeAddress.isEmpty()
            ? tr("Active. Connections are using Tor where configured.")
            : tr("Active · onion address: %1").arg(safeAddress);
    }
    case TorState::Error:
        return tr("Configured, but the onion service is not active. Check the daemon log; credentials are hidden here.");
    case TorState::Unsupported:
    default:
        return tr("Not available as a live switch in this version. Configure Tor in the daemon, then restart. No software will be installed or started automatically.");
    }
}

}  // namespace dinero::qt::dashboard
