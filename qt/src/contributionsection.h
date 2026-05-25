// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DINERO_QT_CONTRIBUTIONSECTION_H
#define DINERO_QT_CONTRIBUTIONSECTION_H

#include "dashboardtypes.h"

#include <QFrame>
#include <QVector>

class QEvent;
class QLabel;

namespace dinero::qt::dashboard {

class SparklineWidget;

// Phase 2a — "your contribution to the network": three sparklines
// (bytes in / out / relay traffic) over the last 5 minutes, a small
// stat grid, and a one-line decentralization score with plain-English
// label. All data arrives via slots; the widget has no polling state.
class ContributionSection : public QFrame {
    Q_OBJECT
public:
    explicit ContributionSection(QWidget* parent = nullptr);

public Q_SLOTS:
    void setContributionStats(const ContributionStats& stats);
    void setDecentralizationScore(const DecentralizationScore& score);
    void setBytesInSamples(const QVector<qint64>& samples);
    void setBytesOutSamples(const QVector<qint64>& samples);
    void setRelayBytesSamples(const QVector<qint64>& samples);

private:
    SparklineWidget* spark_in_{nullptr};
    SparklineWidget* spark_out_{nullptr};
    SparklineWidget* spark_relay_{nullptr};

    QLabel* in_rate_label_{nullptr};
    QLabel* out_rate_label_{nullptr};
    QLabel* relay_rate_label_{nullptr};

    QLabel* circuits_value_{nullptr};
    QLabel* blocks_served_value_{nullptr};
    QLabel* hints_sent_value_{nullptr};
    QLabel* peers_via_gossip_value_{nullptr};

    QLabel* score_total_label_{nullptr};
    QLabel* score_phrase_label_{nullptr};

    DecentralizationScore last_score_{};   // cached for the tooltip

    static QString formatBytesPerSec(qint64 bytes_per_5s);
    static QString buildScoreTooltipHtml(const DecentralizationScore& s);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

}  // namespace dinero::qt::dashboard

#endif  // DINERO_QT_CONTRIBUTIONSECTION_H
