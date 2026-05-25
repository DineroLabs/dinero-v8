// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "networksection.h"

#include <QGridLayout>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>
#include <algorithm>

namespace dinero::qt::dashboard {

NetworkSection::NetworkSection(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* header = new QLabel("📡 NETWORK · as you see it", this);
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
    netBar_->setFormat("net  %v");
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
    root->addStretch(1);

    onChainInfoUpdated({});
}

void NetworkSection::onChainInfoUpdated(const ChainInfo& info) {
    const qint64 maxH = std::max(info.our_height, info.max_peer_height);
    if (maxH > 0) {
        youBar_->setRange(0, static_cast<int>(maxH));
        youBar_->setValue(static_cast<int>(info.our_height));
        youBar_->setFormat(QStringLiteral("you  %1").arg(info.our_height));
        netBar_->setRange(0, static_cast<int>(maxH));
        netBar_->setValue(static_cast<int>(info.net_consensus_height));
        netBar_->setFormat(QStringLiteral("net  %1").arg(info.net_consensus_height));
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
    if (delta == 0) return "● in sync";
    if (delta > 0)  return QString("● +%1 behind net").arg(delta);
    return QString("● %1 ahead of net").arg(-delta);
}

}  // namespace dinero::qt::dashboard
