#pragma once

#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QtMath>

// Keep telemetry accessible as label text while drawing compact scoreboard
// glyphs. Unsupported text uses QLabel rendering.
class MiningScoreboardLabel final : public QLabel {
public:
  explicit MiningScoreboardLabel(const QString& value, QWidget* parent = nullptr)
      : QLabel(value, parent) {}

  void setGlyphScale(qreal scale) {
    if (qFuzzyCompare(glyphScale_, scale)) return;
    glyphScale_ = scale;
    updateGeometry();
    update();
  }

  void setFitToWidth(bool enabled) {
    if (fitToWidth_ == enabled) return;
    fitToWidth_ = enabled;
    updateGeometry();
    update();
  }

  QSize sizeHint() const override {
    const QString value = text();
    if (!isScoreboardText(value)) return QLabel::sizeHint();
    qreal width = 0;
    for (const QChar glyph : value) width += glyphAdvance(glyph);
    return QSize(qCeil((width - kDigitGap) * glyphScale_),
                 qMax(QLabel::sizeHint().height(), 18));
  }

  QSize minimumSizeHint() const override {
    if (!isScoreboardText(text())) return QLabel::minimumSizeHint();
    if (fitToWidth_) {
      // Let a crowded stat row contract without forcing neighboring values
      // outside the window. paintEvent scales the complete value to fit.
      return QSize(qMin(sizeHint().width(), 24),
                   qMax(QLabel::sizeHint().height(), 18));
    }
    return sizeHint();
  }

protected:
  void paintEvent(QPaintEvent* event) override {
    const QString value = text();
    if (!isScoreboardText(value)) {
      QLabel::paintEvent(event);
      return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(palette().color(QPalette::WindowText), 1.5,
                        Qt::SolidLine, Qt::RoundCap));
    qreal advance = 0;
    for (const QChar glyph : value) advance += glyphAdvance(glyph);
    const qreal naturalWidth = advance - kDigitGap;
    const qreal renderScale = fitToWidth_
        ? qMin(glyphScale_, qMax<qreal>(0.01, width() / naturalWidth))
        : glyphScale_;
    const qreal drawnWidth = naturalWidth * renderScale;
    qreal leftOffset = 0;
    if (alignment() & Qt::AlignHCenter)
      leftOffset = qMax<qreal>(0, (width() - drawnWidth) / 2.0);
    else if (alignment() & Qt::AlignRight)
      leftOffset = qMax<qreal>(0, width() - drawnWidth);
    painter.translate(leftOffset, (height() - kDigitHeight * renderScale) / 2.0);
    painter.scale(renderScale, renderScale);
    const qreal top = 0;
    qreal left = 0;
    for (const QChar glyph : value) {
      const QChar lower = glyph.toLower();
      if (lower == QLatin1Char(' ')) {
        left += kSpaceStep;
        continue;
      }
      if (lower == QLatin1Char('.')) {
        painter.drawPoint(QPointF(left + 1, top + kDigitHeight - 1));
        left += kPointStep;
        continue;
      }
      if (lower == QLatin1Char(',')) {
        painter.drawLine(QPointF(left + 2, top + kDigitHeight - 2),
                         QPointF(left + 1, top + kDigitHeight + 1));
        left += kPointStep;
        continue;
      }
      if (lower == QLatin1Char(':')) {
        painter.drawPoint(QPointF(left + 1, top + 4));
        painter.drawPoint(QPointF(left + 1, top + kDigitHeight - 4));
        left += kPointStep;
        continue;
      }
      const qreal right = left + kDigitWidth;
      const qreal middle = top + kDigitHeight / 2.0;
      if (lower == QLatin1Char('x')) {
        painter.drawLine(QPointF(left + 1, top + 3),
                         QPointF(right - 1, top + kDigitHeight - 2));
        painter.drawLine(QPointF(right - 1, top + 3),
                         QPointF(left + 1, top + kDigitHeight - 2));
        left += kDigitStep;
        continue;
      }
      if (lower == QLatin1Char('m')) {
        // A narrow two-hump glyph for the runtime unit, using the same pen.
        const qreal bottom = top + kDigitHeight - 1.5;
        painter.drawLine(QPointF(left + 1, bottom), QPointF(left + 1, middle + 1));
        painter.drawLine(QPointF(left + 1, middle + 1), QPointF(left + 3, middle - 1));
        painter.drawLine(QPointF(left + 3, middle - 1), QPointF(left + 4.25, middle + 1));
        painter.drawLine(QPointF(left + 4.25, middle + 1), QPointF(left + 6, middle - 1));
        painter.drawLine(QPointF(left + 6, middle - 1), QPointF(right - 1, middle + 1));
        painter.drawLine(QPointF(right - 1, middle + 1), QPointF(right - 1, bottom));
        left += kDigitStep;
        continue;
      }
      const int bits = segmentMask(lower);
      if (bits & 0x01) {
        painter.drawLine(QPointF(left + 1.5, top + 1),
                         QPointF(right - 1.5, top + 1));
      }
      if (bits & 0x02) {
        painter.drawLine(QPointF(right - 1, top + 2.5),
                         QPointF(right - 1, middle - 1));
      }
      if (bits & 0x04) {
        painter.drawLine(QPointF(right - 1, middle + 1),
                         QPointF(right - 1, top + kDigitHeight - 2.5));
      }
      if (bits & 0x08) {
        painter.drawLine(QPointF(left + 1.5, top + kDigitHeight - 1),
                         QPointF(right - 1.5, top + kDigitHeight - 1));
      }
      if (bits & 0x10) {
        painter.drawLine(QPointF(left + 1, middle + 1),
                         QPointF(left + 1, top + kDigitHeight - 2.5));
      }
      if (bits & 0x20) {
        painter.drawLine(QPointF(left + 1, top + 2.5),
                         QPointF(left + 1, middle - 1));
      }
      if (bits & 0x40) {
        painter.drawLine(QPointF(left + 1.5, middle),
                         QPointF(right - 1.5, middle));
      }
      left += kDigitStep;
    }
  }

private:
  static int segmentMask(const QChar& glyph) {
    static constexpr unsigned char digits[10] = {
      0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    if (glyph >= QLatin1Char('0') && glyph <= QLatin1Char('9'))
      return digits[glyph.unicode() - '0'];
    switch (glyph.unicode()) {
      case 'a': return 0x77;
      case 'b': return 0x7c;
      case 'c': return 0x58;
      case 'd': return 0x5e;
      case 'e': return 0x79;
      case 'f': return 0x71;
      case 'h': return 0x74;
      case 's': return 0x6d;
      default: return -1;
    }
  }

  static bool isScoreboardText(const QString& value) {
    if (value.isEmpty()) return false;
    for (const QChar glyph : value) {
      const QChar lower = glyph.toLower();
      if (segmentMask(lower) < 0 && lower != QLatin1Char('x') &&
          lower != QLatin1Char('m') && lower != QLatin1Char('.') &&
          lower != QLatin1Char(',') &&
          lower != QLatin1Char(':') && lower != QLatin1Char(' ')) return false;
    }
    return true;
  }

  static qreal glyphAdvance(const QChar& glyph) {
    if (glyph == QLatin1Char(' ')) return kSpaceStep;
    if (glyph == QLatin1Char('.') || glyph == QLatin1Char(',') ||
        glyph == QLatin1Char(':')) return kPointStep;
    return kDigitStep;
  }

  static constexpr qreal kDigitWidth = 8.5;
  static constexpr qreal kDigitGap = 2.0;
  static constexpr qreal kDigitStep = kDigitWidth + kDigitGap;
  static constexpr qreal kDigitHeight = 14.0;
  static constexpr qreal kPointStep = 4.0;
  static constexpr qreal kSpaceStep = 5.0;
  qreal glyphScale_ = 1.0;
  bool fitToWidth_ = false;
};
