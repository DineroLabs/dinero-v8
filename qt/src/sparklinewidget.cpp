// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sparklinewidget.h"

#include <QPaintEvent>
#include <QPainter>

#include <algorithm>

namespace dinero::qt::dashboard {

namespace {
constexpr int kDefaultWidthPx  = 200;
constexpr int kDefaultHeightPx = 14;
constexpr int kBarSpacingPx    = 1;
}  // namespace

SparklineWidget::SparklineWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(kDefaultHeightPx);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize SparklineWidget::sizeHint() const {
    return QSize(kDefaultWidthPx, kDefaultHeightPx);
}

void SparklineWidget::setSamples(const QVector<qint64>& samples) {
    samples_ = samples;
    update();
}

void SparklineWidget::paintEvent(QPaintEvent* /*event*/) {
    if (samples_.isEmpty()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const qint64 max_val =
        *std::max_element(samples_.constBegin(), samples_.constEnd());
    if (max_val <= 0) return;  // all zero → nothing to draw

    const int w = width();
    const int h = height();
    const int n = samples_.size();
    if (n <= 0 || w <= 0 || h <= 0) return;

    // Bar width: fit n bars + (n-1) spacing into width().
    const double bar_w = static_cast<double>(
        std::max(1, (w - (n - 1) * kBarSpacingPx))) / n;

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(80, 160, 240));  // soft blue, default scheme

    for (int i = 0; i < n; ++i) {
        const qint64 v = std::max<qint64>(0, samples_[i]);
        const int bar_h = static_cast<int>(
            (static_cast<double>(v) / max_val) * h);
        const int x = static_cast<int>(i * (bar_w + kBarSpacingPx));
        p.drawRect(x, h - bar_h, static_cast<int>(bar_w), bar_h);
    }
}

}  // namespace dinero::qt::dashboard
