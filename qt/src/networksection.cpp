// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "networksection.h"

#include <QGridLayout>
#include <QComboBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
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

    root->addWidget(new QLabel(tr("Tor connectivity (Admin only)"), this));
    torControl_ = new QComboBox(this);
    torControl_->setObjectName(QStringLiteral("torReachabilityControl"));
    torControl_->addItem(tr("Off"), QStringLiteral("off"));
    torControl_->addItem(tr("Automatic — recommended"), QStringLiteral("automatic"));
    torControl_->addItem(tr("External Tor — advanced"), QStringLiteral("external"));
    torControl_->setEnabled(false);
    torControl_->setToolTip(tr(
        "Uses this node's existing authenticated RPC session and local node cookie; no additional credentials are required. Automatic uses only Dinero's included Tor component."));
    root->addWidget(torControl_);
    connect(torControl_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString mode = torControl_->itemData(index).toString();
        if (mode == QStringLiteral("automatic") &&
            !QSettings().value(QStringLiteral("network/tor_consent_v1"), false).toBool()) {
            const auto answer = QMessageBox::question(this, tr("Tor connectivity"), tr(
                "Allow Dinero to use its included Tor component for private and resilient network connectivity."));
            if (answer != QMessageBox::Yes) {
                QSignalBlocker blocker(torControl_);
                torControl_->setCurrentIndex(0);
                return;
            }
            QSettings().setValue(QStringLiteral("network/tor_consent_v1"), true);
        }
        torControl_->setEnabled(false);
        torStatusLabel_->setText(tr("Applying Tor connectivity preference…"));
        Q_EMIT torModeRequested(mode);
    });
    torStatusLabel_ = new QLabel(this);
    torStatusLabel_->setObjectName(QStringLiteral("torStatus"));
    torStatusLabel_->setWordWrap(true);
    root->addWidget(torStatusLabel_);

    root->addWidget(new QLabel(tr("Enable relay service (Admin only)"), this));
    relayControl_ = new QComboBox(this);
    relayControl_->setObjectName(QStringLiteral("relayServiceControl"));
    relayControl_->addItem(tr("Off"), QStringLiteral("off"));
    relayControl_->addItem(tr("Automatic — recommended"), QStringLiteral("automatic"));
    relayControl_->addItem(tr("Custom limits"), QStringLiteral("custom"));
    relayControl_->setToolTip(tr("Serves encrypted Dinero P2P relay circuits only. It is not a web proxy and cannot relay other software."));
    root->addWidget(relayControl_);
    relayCustom_ = new QWidget(this);
    auto* form = new QFormLayout(relayCustom_);
    const QList<QPair<QString, QPair<int, int>>> specs = {
        {tr("Concurrent circuits"), {1, 25}}, {tr("Bandwidth (KiB/s)"), {64, 20480}},
        {tr("Circuits per peer"), {1, 25}}, {tr("Circuit lifetime (seconds)"), {60, 86400}},
        {tr("Requests per peer/minute"), {1, 60}}};
    const QList<int> defaults = {12, 2048, 2, 1800, 6};
    for (int i = 0; i < specs.size(); ++i) {
        auto* spin = new QSpinBox(relayCustom_);
        spin->setRange(specs[i].second.first, specs[i].second.second);
        spin->setValue(defaults[i]);
        form->addRow(specs[i].first, spin);
        relayLimits_.append(spin);
    }
    relayCustom_->setVisible(false);
    root->addWidget(relayCustom_);
    auto* applyRelay = new QPushButton(tr("Apply relay service"), this);
    root->addWidget(applyRelay);
    relayStatusLabel_ = new QLabel(tr("Relay service status unavailable."), this);
    relayStatusLabel_->setWordWrap(true);
    root->addWidget(relayStatusLabel_);
    connect(relayControl_, &QComboBox::currentIndexChanged, this, [this](int) {
        relayCustom_->setVisible(relayControl_->currentData().toString() == QStringLiteral("custom"));
    });
    connect(applyRelay, &QPushButton::clicked, this, [this] {
        QJsonObject limits{{"max_circuits", relayLimits_[0]->value()},
            {"bandwidth_bytes_per_second", relayLimits_[1]->value() * 1024},
            {"max_circuits_per_peer", relayLimits_[2]->value()},
            {"circuit_lifetime_seconds", relayLimits_[3]->value()},
            {"requests_per_peer_per_minute", relayLimits_[4]->value()}};
        Q_EMIT relayServiceRequested(QJsonObject{{"mode", relayControl_->currentData().toString()}, {"limits", limits}});
    });
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
    const QSignalBlocker blocker(torControl_);
    torControl_->setCurrentIndex(state == TorState::Off || state == TorState::Unsupported ? 0 : 1);
    torStatusLabel_->setText(torStatusText(state, onionAddress));
}

void NetworkSection::setOnionServiceStatus(const OnionServiceStatus& status) {
    if (!status.available) {
        torControl_->setEnabled(false);
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
    if (status.available) {
        const QSignalBlocker blocker(torControl_);
        const QString mode = status.requested && status.mode == QStringLiteral("off")
            ? QStringLiteral("automatic") : status.mode;
        const int index = torControl_->findData(mode);
        torControl_->setCurrentIndex(index >= 0 ? index : (status.requested ? 1 : 0));
        torControl_->setEnabled(true);
    }
}

void NetworkSection::setRelayServiceStatus(const QJsonObject& status) {
    const QString mode = status.value(QStringLiteral("mode")).toString(QStringLiteral("automatic"));
    const QSignalBlocker blocker(relayControl_);
    const int index = relayControl_->findData(mode);
    relayControl_->setCurrentIndex(index >= 0 ? index : 1);
    relayCustom_->setVisible(mode == QStringLiteral("custom"));
    relayStatusLabel_->setText(status.value(QStringLiteral("enabled")).toBool()
        ? tr("Dinero relay service is active within the configured limits.")
        : tr("Dinero relay service is not accepting new circuits."));
}

void NetworkSection::setTorActionError(bool unsupported) {
    if (unsupported) {
        OnionServiceStatus status;
        setOnionServiceStatus(status);
        return;
    }
    torControl_->setEnabled(true);
    torStatusLabel_->setText(tr(
        "Could not change Tor reachability. Check RPC authorization and the local daemon log."));
}

QString NetworkSection::torStatusText(TorState state, const QString& onionAddress) {
    switch (state) {
    case TorState::Off:
        return tr("Off. Ordinary direct and Dinero relay connections remain available.");
    case TorState::Active: {
        const QString safeAddress = onionAddress.trimmed().endsWith(
            QStringLiteral(".onion"), Qt::CaseInsensitive) ? onionAddress.trimmed() : QString();
        return safeAddress.isEmpty()
            ? tr("Tor active. Direct and relay paths remain available.")
            : tr("Active · onion address: %1").arg(safeAddress);
    }
    case TorState::Error:
        return tr("Configured, but the onion service is not active. Check the daemon log; credentials are hidden here.");
    case TorState::Unsupported:
    default:
        return tr("This daemon is older and does not support live Tor controls. Ordinary P2P continues normally.");
    }
}

}  // namespace dinero::qt::dashboard
