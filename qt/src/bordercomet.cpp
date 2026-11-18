#include "bordercomet.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QWindow>

#include <cmath>

namespace {
constexpr double kLapMs = 14000.0;      // one full lap around the frame
constexpr int kFrameMs = 33;            // ~30 fps while visible
constexpr int kIdleMs = 1000;           // check once a second when hidden
constexpr double kTailPx = 110.0;       // comet tail length along the border
constexpr double kSampleStepPx = 2.0;
constexpr qreal kRadius = 8.0;          // matches QTabWidget::pane border-radius
const QColor kTail(0xd5, 0x8a, 0x32);   // selected-tab accent
const QColor kHead(0xff, 0xe2, 0xb8);
}

bool BorderComet::Enabled() {
    return QSettings().value(QStringLiteral("ui/borderComet"), true).toBool();
}

BorderComet::BorderComet(QTabWidget* tabs) : QWidget(tabs), host_(tabs), radius_(kRadius) {
    // The pane border sits one pixel outside the page stack.
    frame_ = [tabs]() -> QRect {
        auto* stack = tabs->findChild<QStackedWidget*>();
        return stack ? stack->geometry().adjusted(-1, -1, 1, 1) : QRect();
    };
    if (auto* stack = tabs->findChild<QStackedWidget*>()) stack->installEventFilter(this);
    start();
}

BorderComet::BorderComet(QWidget* host, qreal radius) : QWidget(host), host_(host), radius_(radius) {
    frame_ = [host]() { return host->rect(); };
    start();
}

void BorderComet::start() {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    setObjectName(QStringLiteral("borderComet"));
    if (!Enabled()) {
        hide();
        return;
    }
    host_->installEventFilter(this);
    connect(&timer_, &QTimer::timeout, this, &BorderComet::tick);
    timer_.setTimerType(Qt::CoarseTimer);
    timer_.start(kFrameMs);
    clock_.start();
    relayout();
    raise();
}

namespace {
class CometInstaller : public QObject {
public:
    explicit CometInstaller(QWidget* main) : QObject(main), main_(main) {}
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::Show) return false;
        auto* window = qobject_cast<QWidget*>(watched);
        if (!window || !window->isWindow() || window == main_) return false;
        const auto type = window->windowType();
        if (type != Qt::Window && type != Qt::Dialog && type != Qt::Sheet) return false;
        if (window->property("dineroBorderComet").toBool()) return false;
        window->setProperty("dineroBorderComet", true);
        new BorderComet(window, 10.0);
        return false;
    }
private:
    QWidget* main_;
};
}  // namespace

void BorderComet::InstallForAllWindows(QWidget* mainWindow) {
    if (!Enabled() || !qApp) return;
    qApp->installEventFilter(new CometInstaller(mainWindow));
}

bool BorderComet::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
        event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show)
        relayout();
    else if (watched == host_ && event->type() == QEvent::ChildAdded)
        QTimer::singleShot(0, this, [this] { raise(); });
    return QWidget::eventFilter(watched, event);
}

void BorderComet::relayout() {
    const QRect pane = frame_ ? frame_() : QRect();
    if (pane.width() < 4 * radius_ || pane.height() < 4 * radius_) return;
    if (geometry() != pane) setGeometry(pane);
    raise();
    QPainterPath border;
    border.addRoundedRect(QRectF(0.5, 0.5, pane.width() - 1.0, pane.height() - 1.0), radius_, radius_);
    const int samples = std::max(64, int(border.length() / kSampleStepPx));
    QVector<QPointF> points;
    points.reserve(samples);
    for (int i = 0; i < samples; ++i) points.push_back(border.pointAtPercent(double(i) / samples));
    const double progress = path_.isEmpty() ? 0.0 : head_ / path_.size();
    path_ = std::move(points);
    head_ = progress * path_.size();
    update();
}

bool BorderComet::onScreen() const {
    const QWidget* top = window();
    if (!top || !top->isVisible() || top->isMinimized()) return false;
    const QWindow* handle = top->windowHandle();
    return !handle || handle->isExposed();
}

QRect BorderComet::cometBounds(double head) const {
    if (path_.isEmpty()) return {};
    const int n = path_.size();
    const int tail = int(kTailPx / kSampleStepPx);
    QRectF box(path_[int(head) % n], QSizeF(0, 0));
    for (int i = 0; i <= tail; i += 4) {
        const QPointF& p = path_[((int(head) - i) % n + n) % n];
        box |= QRectF(p, QSizeF(0, 0));
    }
    return box.toAlignedRect().adjusted(-8, -8, 8, 8);
}

void BorderComet::tick() {
    if (path_.isEmpty()) return;
    if (!onScreen()) {
        if (timer_.interval() != kIdleMs) timer_.setInterval(kIdleMs);
        clock_.restart();
        return;
    }
    if (timer_.interval() != kFrameMs) timer_.setInterval(kFrameMs);
    const double elapsed = double(clock_.restart());
    const QRect before = cometBounds(head_);
    head_ = std::fmod(head_ + elapsed / kLapMs * path_.size(), double(path_.size()));
    const QRect after = cometBounds(head_);
    update(before.united(after));
}

void BorderComet::paintEvent(QPaintEvent*) {
    if (path_.isEmpty()) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int n = path_.size();
    const int tail = int(kTailPx / kSampleStepPx);
    const int head = int(head_);
    // Tail: short segments fading from the head backwards.
    for (int i = tail; i > 0; --i) {
        const double fade = 1.0 - double(i) / tail;
        QColor color = kTail;
        color.setAlphaF(0.85 * fade * fade);
        painter.setPen(QPen(color, 1.0 + 1.2 * fade, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(path_[((head - i) % n + n) % n], path_[((head - i + 1) % n + n) % n]);
    }
    // Head: a soft glow with a bright core.
    const QPointF at = path_[head % n];
    QRadialGradient glow(at, 6.0);
    QColor core = kHead;
    QColor edge = kTail;
    edge.setAlpha(0);
    glow.setColorAt(0.0, core);
    glow.setColorAt(0.35, QColor(kTail.red(), kTail.green(), kTail.blue(), 170));
    glow.setColorAt(1.0, edge);
    painter.setPen(Qt::NoPen);
    painter.setBrush(glow);
    painter.drawEllipse(at, 6.0, 6.0);
}
