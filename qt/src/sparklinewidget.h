// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <QVector>
#include <QWidget>

namespace dinero::qt::dashboard {

// Minimal vertical-bar sparkline. Takes a vector of non-negative
// integers; paints each as a bar with height proportional to
// sample/max(samples). Zero-sample input renders an empty rect.
//
// No autoscroll, no axis labels — pure presentation. Caller decides
// what data goes in.
class SparklineWidget : public QWidget {
    Q_OBJECT
public:
    explicit SparklineWidget(QWidget* parent = nullptr);

    void setSamples(const QVector<qint64>& samples);
    QVector<qint64> samples() const { return samples_; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<qint64> samples_;
};

}  // namespace dinero::qt::dashboard
