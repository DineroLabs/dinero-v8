// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "contributionsection.h"

#include "sparklinewidget.h"

#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QToolTip>
#include <QVBoxLayout>

namespace dinero::qt::dashboard {

namespace {

QLabel* makeSectionHeader(const QString& text) {
    auto* label = new QLabel(text);
    QFont f = label->font();
    f.setBold(true);
    label->setFont(f);
    return label;
}

}  // namespace

ContributionSection::ContributionSection(QWidget* parent)
    : QFrame(parent) {
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Sunken);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    root->addWidget(makeSectionHeader(tr("Your contribution")));

    // Sparkline rows: label | sparkline | spot rate
    auto* spark_grid = new QGridLayout();
    spark_grid->setHorizontalSpacing(8);
    spark_grid->setVerticalSpacing(4);
    spark_grid->setColumnStretch(1, 1);

    spark_in_         = new SparklineWidget(this);
    spark_out_        = new SparklineWidget(this);
    spark_relay_      = new SparklineWidget(this);
    in_rate_label_    = new QLabel(QStringLiteral("—"));
    out_rate_label_   = new QLabel(QStringLiteral("—"));
    relay_rate_label_ = new QLabel(QStringLiteral("—"));

    spark_grid->addWidget(new QLabel(tr("Bytes in")),         0, 0);
    spark_grid->addWidget(spark_in_,                          0, 1);
    spark_grid->addWidget(in_rate_label_,                     0, 2);
    spark_grid->addWidget(new QLabel(tr("Bytes out")),        1, 0);
    spark_grid->addWidget(spark_out_,                         1, 1);
    spark_grid->addWidget(out_rate_label_,                    1, 2);
    spark_grid->addWidget(new QLabel(tr("Relay traffic")),    2, 0);
    spark_grid->addWidget(spark_relay_,                       2, 1);
    spark_grid->addWidget(relay_rate_label_,                  2, 2);
    root->addLayout(spark_grid);

    // 2x2 stat grid
    auto* stat_grid = new QGridLayout();
    stat_grid->setHorizontalSpacing(12);
    stat_grid->setVerticalSpacing(4);

    circuits_value_         = new QLabel(QStringLiteral("0"));
    blocks_served_value_    = new QLabel(QStringLiteral("0"));
    hints_sent_value_       = new QLabel(QStringLiteral("0"));
    peers_via_gossip_value_ = new QLabel(QStringLiteral("0"));

    stat_grid->addWidget(new QLabel(tr("Registrants active:")), 0, 0);
    stat_grid->addWidget(circuits_value_,                       0, 1);
    stat_grid->addWidget(new QLabel(tr("Blocks served (24h):")), 0, 2);
    stat_grid->addWidget(blocks_served_value_,                  0, 3);
    stat_grid->addWidget(new QLabel(tr("Hints sent:")),         1, 0);
    stat_grid->addWidget(hints_sent_value_,                     1, 1);
    stat_grid->addWidget(new QLabel(tr("Peers via gossip:")),   1, 2);
    stat_grid->addWidget(peers_via_gossip_value_,               1, 3);
    root->addLayout(stat_grid);

    // Decentralization score line
    auto* score_row = new QHBoxLayout();
    score_row->setContentsMargins(0, 4, 0, 0);
    score_row->setSpacing(8);
    auto* score_caption = new QLabel(tr("Decentralization score:"));
    score_total_label_  = new QLabel(QStringLiteral("0.0 / 10"));
    score_phrase_label_ = new QLabel(QStringLiteral("—"));
    QFont score_font = score_total_label_->font();
    score_font.setBold(true);
    score_total_label_->setFont(score_font);
    score_total_label_->installEventFilter(this);
    score_total_label_->setMouseTracking(true);
    score_row->addWidget(score_caption);
    score_row->addWidget(score_total_label_);
    score_row->addWidget(score_phrase_label_, 1);
    root->addLayout(score_row);
}

void ContributionSection::setContributionStats(const ContributionStats& stats) {
    circuits_value_->setText(QString::number(stats.registrants_active));
    blocks_served_value_->setText(QString::number(stats.blocks_served_24h));
    hints_sent_value_->setText(QString::number(stats.hints_sent));
    peers_via_gossip_value_->setText(QString::number(stats.peers_via_gossip));
    in_rate_label_->setText(formatBytesPerSec(stats.bytes_in_rate));
    out_rate_label_->setText(formatBytesPerSec(stats.bytes_out_rate));
    relay_rate_label_->setText(formatBytesPerSec(stats.relay_bytes_rate));
}

void ContributionSection::setDecentralizationScore(
        const DecentralizationScore& score) {
    last_score_ = score;
    score_total_label_->setText(
        QStringLiteral("%1 / 10").arg(score.total, 0, 'f', 1));
    score_phrase_label_->setText(score.label);
}

void ContributionSection::setBytesInSamples(const QVector<qint64>& s) {
    spark_in_->setSamples(s);
}
void ContributionSection::setBytesOutSamples(const QVector<qint64>& s) {
    spark_out_->setSamples(s);
}
void ContributionSection::setRelayBytesSamples(const QVector<qint64>& s) {
    spark_relay_->setSamples(s);
}

QString ContributionSection::formatBytesPerSec(qint64 bytes_per_5s) {
    // Per-tick byte delta is over the 5s poll interval; divide to get B/s.
    const double bps = static_cast<double>(bytes_per_5s) / 5.0;
    if (bps < 1024.0)         return QStringLiteral("%1 B/s").arg(bps, 0, 'f', 0);
    if (bps < 1024.0 * 1024.0) return QStringLiteral("%1 KB/s").arg(bps / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB/s").arg(bps / (1024.0 * 1024.0), 0, 'f', 1);
}

bool ContributionSection::eventFilter(QObject* watched, QEvent* event) {
    if (watched == score_total_label_ && event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        QToolTip::showText(help->globalPos(),
                           buildScoreTooltipHtml(last_score_),
                           score_total_label_);
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

QString ContributionSection::buildScoreTooltipHtml(
        const DecentralizationScore& s) {
    auto hint = [](double v, double cap, const QString& better) {
        return v >= cap ? QStringLiteral("✓") : better;
    };
    return QStringLiteral(
        "<html><body><b>Decentralization breakdown</b><br>"
        "<table cellspacing='4'>"
        "<tr><td>Reachable</td><td>%1 / 1.0</td><td>%2</td></tr>"
        "<tr><td>Relay active</td><td>%3 / 2.0</td><td>%4</td></tr>"
        "<tr><td>Uptime</td><td>%5 / 1.5</td><td>%6</td></tr>"
        "<tr><td>Peer diversity</td><td>%7 / 1.5</td><td>%8</td></tr>"
        "<tr><td>Traffic</td><td>%9 / 1.0</td><td>%10</td></tr>"
        "<tr><td>Mining</td><td>%11 / 1.5</td><td>%12</td></tr>"
        "<tr><td>Gossip reach</td><td>%13 / 1.5</td><td>%14</td></tr>"
        "</table></body></html>")
        .arg(s.breakdown.reachable, 0, 'f', 1)
        .arg(hint(s.breakdown.reachable, 1.0,
                  QStringLiteral("Open port 20999 inbound")))
        .arg(s.breakdown.relay_active, 0, 'f', 1)
        .arg(hint(s.breakdown.relay_active, 2.0,
                  QStringLiteral("Enable relay mode")))
        .arg(s.breakdown.uptime, 0, 'f', 2)
        .arg(hint(s.breakdown.uptime, 1.5,
                  QStringLiteral("Keep running (30d = full)")))
        .arg(s.breakdown.peer_diversity, 0, 'f', 2)
        .arg(hint(s.breakdown.peer_diversity, 1.5,
                  QStringLiteral("Connect to more /16 subnets")))
        .arg(s.breakdown.traffic, 0, 'f', 2)
        .arg(hint(s.breakdown.traffic, 1.0,
                  QStringLiteral("Become a relay to carry more traffic")))
        .arg(s.breakdown.mining, 0, 'f', 2)
        .arg(hint(s.breakdown.mining, 1.5,
                  QStringLiteral("Mine if you can")))
        .arg(s.breakdown.gossip_reach, 0, 'f', 2)
        .arg(hint(s.breakdown.gossip_reach, 1.5,
                  QStringLiteral("Peers will discover you via gossip")));
}

}  // namespace dinero::qt::dashboard
