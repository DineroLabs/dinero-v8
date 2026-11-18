#include "mempoolsparkline.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

MempoolSparkline::MempoolSparkline(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("overviewMempoolSparkline"));
    setMinimumHeight(30);
    setMaximumHeight(30);
    setToolTip(QStringLiteral("Pending transactions over the last few minutes"));
}

void MempoolSparkline::addSample(int pending) {
    samples_.push_back(std::max(0, pending));
    if (samples_.size() > kMaxSamples) samples_.remove(0, samples_.size() - kMaxSamples);
    update();
}

void MempoolSparkline::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF area = QRectF(rect()).adjusted(1, 3, -1, -2);
    // Baseline: always visible so an empty pool still reads as "live, zero".
    painter.setPen(QPen(QColor(0x3a, 0x40, 0x48), 1));
    painter.drawLine(QPointF(area.left(), area.bottom()), QPointF(area.right(), area.bottom()));
    if (samples_.size() < 2) return;
    const int top = std::max(1, *std::max_element(samples_.begin(), samples_.end()));
    const double step = area.width() / (kMaxSamples - 1);
    const double x0 = area.right() - step * (samples_.size() - 1);
    QPainterPath line;
    for (int i = 0; i < samples_.size(); ++i) {
        const QPointF p(x0 + step * i, area.bottom() - area.height() * samples_[i] / top);
        if (i == 0) line.moveTo(p); else line.lineTo(p);
    }
    QPainterPath fill = line;
    fill.lineTo(area.right(), area.bottom());
    fill.lineTo(x0, area.bottom());
    fill.closeSubpath();
    QLinearGradient shade(area.topLeft(), area.bottomLeft());
    shade.setColorAt(0.0, QColor(0xd5, 0x8a, 0x32, 70));
    shade.setColorAt(1.0, QColor(0xd5, 0x8a, 0x32, 0));
    painter.fillPath(fill, shade);
    painter.setPen(QPen(QColor(0xd5, 0x8a, 0x32), 1.5));
    painter.drawPath(line);
}
