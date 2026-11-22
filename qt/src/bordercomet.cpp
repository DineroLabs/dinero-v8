#include "bordercomet.h"

#include <QEvent>
#include <QLinearGradient>
#include <QScrollBar>
#include <QMouseEvent>
#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPixmap>
#include <QRadialGradient>
#include <QRegion>
#include <QScreen>
#include <QSettings>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kLapMs = 20000.0;      // one full lap around the frame
constexpr int kFrameMs = 25;            // ~40 fps while visible
constexpr int kIdleMs = 1000;           // check once a second when hidden
constexpr double kTailPx = 420.0;       // comet tail length along the border
constexpr int kComets = 2;              // evenly spaced around the frame
constexpr double kBaseAlpha = 0.16;     // faint always-on accent line
constexpr double kSampleStepPx = 2.0;
constexpr qreal kRadius = 8.0;          // matches QTabWidget::pane border-radius
const QColor kTail(0xd5, 0x8a, 0x32);   // selected-tab accent
const QColor kHead(0xff, 0xe2, 0xb8);

// Only the main navigation uses this overlay. On Mining it draws the usual
// white/orange underlines, changing at most one while a comet tip passes.
class NavigationCometUnderline final : public QWidget {
public:
    explicit NavigationCometUnderline(QTabBar* bar) : QWidget(bar), bar_(bar) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        setObjectName(QStringLiteral("navigationCometUnderline"));
        setStyleSheet(QStringLiteral("background: transparent;"));
        setGeometry(bar_->rect());
        bar_->installEventFilter(this);
        show();
        raise();
    }

    int litTab() const { return litTab_; }

    void setLitTab(int index) {
        if (index == litTab_) return;
        litTab_ = index;
        // A queued partial update can leave the previous key orange while
        // mining keeps the UI busy. Paint all underline colors immediately in
        // the same timer tick that moved the comet tip.
        repaint();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == bar_ && (event->type() == QEvent::Resize ||
                                event->type() == QEvent::LayoutRequest ||
                                event->type() == QEvent::Show)) {
            setGeometry(bar_->rect());
            update();
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent* event) override {
        QPainter painter(this);
        for (int i = 0; i < bar_->count(); ++i) {
            if (!bar_->isTabVisible(i)) continue;
            const QRect tab = bar_->tabRect(i).intersected(rect());
            if (tab.isEmpty() || !tab.intersects(event->rect())) continue;
            const QRect line(tab.left(), tab.bottom() - 2, tab.width() - 2, 3);
            const bool selected = i == bar_->currentIndex();
            if (selected) {
                // Clear the preceding tip's halo before drawing this frame.
                painter.fillRect(line.adjusted(2, -6, -2, -3), QColor(0x30, 0x38, 0x44));
            }
            if (i == litTab_ && selected) {
                // The Mining tab is already orange: lift its light slightly.
                QLinearGradient halo(0, line.top() - 6, 0, line.bottom() + 1);
                halo.setColorAt(0.0, QColor(255, 166, 62, 0));
                halo.setColorAt(0.7, QColor(255, 166, 62, 65));
                halo.setColorAt(1.0, QColor(255, 185, 92, 100));
                painter.fillRect(line.adjusted(2, -6, -2, 0), halo);
                painter.fillRect(line, QColor(245, 174, 79));
            } else {
                painter.fillRect(line, selected || i == litTab_
                                           ? kTail : QColor(0xdf, 0xe5, 0xec));
            }
        }
    }

private:
    QTabBar* bar_;
    int litTab_ = -1;
};
}

bool BorderComet::Enabled() {
    return QSettings().value(QStringLiteral("ui/borderComet"), true).toBool();
}

BorderComet::BorderComet(QTabWidget* tabs)
    : QWidget(tabs), host_(tabs), navigationTabs_(tabs), radius_(kRadius) {
    // The pane border sits one pixel outside the page stack. A window taller
    // or wider than its screen hides part of that border; trace only the part
    // the user can see, so the comet never travels off screen.
    frame_ = [tabs]() -> QRect {
        auto* stack = tabs->findChild<QStackedWidget*>();
        if (!stack) return QRect();
        QRect pane = stack->geometry().adjusted(-1, -1, 1, 1);
        if (const QScreen* screen = tabs->screen()) {
            const QRect visible = screen->availableGeometry();
            const QRect local(tabs->mapFromGlobal(visible.topLeft()), visible.size());
            // Stay clear of the screen edges, and of the Dock, whose panel
            // reaches a little above the area macOS reports as available.
            pane = pane.intersected(local.adjusted(6, 6, -6, -18));
        }
        return pane;
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
    if (navigationTabs_) {
        navigationUnderline_ = new NavigationCometUnderline(navigationTabs_->tabBar());
        connect(navigationTabs_, &QTabWidget::currentChanged, this,
                [this] { updateNavigationUnderline(); });
    }
    // Step aside while the user scrolls: a see-through layer over a scrolling
    // page defeats Qt's scroll fast path (stale areas, stutter on macOS).
    if (qApp) qApp->installEventFilter(this);
    resume_.setSingleShot(true);
    resume_.setInterval(250);
    connect(&resume_, &QTimer::timeout, this, [this] {
        clock_.restart();
        show();
        raise();
    });
    connect(&timer_, &QTimer::timeout, this, &BorderComet::tick);
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.start(kFrameMs);
    clock_.start();
    relayout();
    raise();
}


bool BorderComet::eventFilter(QObject* watched, QEvent* event) {
    const auto type = event->type();
    if (type == QEvent::Wheel || type == QEvent::ScrollPrepare || type == QEvent::Scroll ||
        ((type == QEvent::MouseButtonPress || type == QEvent::MouseMove) &&
         qobject_cast<QScrollBar*>(watched) && static_cast<QMouseEvent*>(event)->buttons())) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget && widget != this && host_->isAncestorOf(widget)) {
            if (isVisible()) hide();
            updateNavigationUnderline();
            resume_.start();
        }
        return false;
    }
    if (watched != host_ && watched != watchedWindow_ &&
        !(host_->isAncestorOf(qobject_cast<QWidget*>(watched)) && qobject_cast<QStackedWidget*>(watched)))
        return false;
    if (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
        event->type() == QEvent::Show || event->type() == QEvent::ParentChange ||
        event->type() == QEvent::ScreenChangeInternal)
        relayout();
    else if (watched == host_ && event->type() == QEvent::ChildAdded)
        QTimer::singleShot(0, this, [this] { raise(); });
    return QWidget::eventFilter(watched, event);
}

void BorderComet::relayout() {
    // The host is often created before it is placed in its window. Follow
    // whichever window it ends up in: moving or resizing that window changes
    // which part of the frame is visible.
    if (QWidget* top = host_->window(); top && top != host_ && top != watchedWindow_) {
        if (watchedWindow_) watchedWindow_->removeEventFilter(this);
        watchedWindow_ = top;
        top->installEventFilter(this);
    }
    const QRect pane = frame_ ? frame_() : QRect();
    if (pane.width() < 4 * radius_ || pane.height() < 4 * radius_) return;
    if (pane == geometry() && !path_.isEmpty()) return;  // nothing moved
    setGeometry(pane);
    raise();
    QPainterPath border;
    border.addRoundedRect(QRectF(0.5, 0.5, pane.width() - 1.0, pane.height() - 1.0), radius_, radius_);
    const int samples = std::max(64, int(border.length() / kSampleStepPx));
    QVector<QPointF> points;
    points.reserve(samples);
    for (int i = 0; i < samples; ++i) points.push_back(border.pointAtPercent(double(i) / samples));
    const double progress = path_.isEmpty() ? 0.0 : head_ / path_.size();
    path_ = std::move(points);
    const qreal dpr = devicePixelRatioF();
    base_ = QPixmap((QSizeF(pane.size()) * dpr).toSize());
    base_.setDevicePixelRatio(dpr);
    base_.fill(Qt::transparent);
    {
        QPainter cache(&base_);
        cache.setRenderHint(QPainter::Antialiasing);
        QColor base = kTail;
        base.setAlphaF(kBaseAlpha);
        cache.setPen(QPen(base, 1.5));
        cache.drawPath(border);
    }
    head_ = progress * path_.size();
    update();
    updateNavigationUnderline();
}

bool BorderComet::onScreen() const {
    const QWidget* top = window();
    if (!top || !top->isVisible() || top->isMinimized()) return false;
    const QWindow* handle = top->windowHandle();
    return !handle || handle->isExposed();
}

QRegion BorderComet::cometRegion(double head) const {
    // A thin band of small boxes along each tail: repaint cost follows the
    // comets' length, not the bounding box of a tail that turns a corner.
    QRegion region;
    if (path_.isEmpty()) return region;
    const int n = path_.size();
    const int tail = int(kTailPx / kSampleStepPx);
    for (int c = 0; c < kComets; ++c) {
        const int h = int(head + double(c) * n / kComets);
        for (int i = -2; i <= tail + 2; i += 4) {
            const QPointF& p = path_[((h - i) % n + n) % n];
            region += QRect(int(p.x()) - 12, int(p.y()) - 12, 25, 25);
        }
    }
    return region;
}

void BorderComet::tick() {
    if (path_.isEmpty()) return;
    if (!onScreen()) {
        if (timer_.interval() != kIdleMs) timer_.setInterval(kIdleMs);
        clock_.restart();
        updateNavigationUnderline();
        return;
    }
    if (timer_.interval() != kFrameMs) timer_.setInterval(kFrameMs);
    const double elapsed = double(clock_.restart());
    const QRegion before = cometRegion(head_);
    head_ = std::fmod(head_ + elapsed / kLapMs * path_.size(), double(path_.size()));
    update(before.united(cometRegion(head_)));
    updateNavigationUnderline();
}

void BorderComet::updateNavigationUnderline() {
    if (!navigationTabs_ || !navigationUnderline_) return;
    auto* underline = static_cast<NavigationCometUnderline*>(navigationUnderline_.data());
    QTabBar* bar = navigationTabs_->tabBar();
    const int selected = navigationTabs_->currentIndex();
    if (selected < 0) {
        underline->hide();
        underline->setLitTab(-1);
        return;
    }
    const QString selectedName = bar->tabData(selected).toString().isEmpty()
        ? navigationTabs_->tabText(selected) : bar->tabData(selected).toString();
    if (selectedName != QStringLiteral("Mining") || !isVisible() || !onScreen() || path_.isEmpty()) {
        underline->hide();
        underline->setLitTab(-1);
        return;
    }
    if (!underline->isVisible()) underline->show();

    // Both tips can light a tab. If the wide window puts them above different
    // tabs at once, keep the currently lit tab until its tip leaves, so no two
    // underlines flash orange together and the handoff does not flicker.
    const int n = path_.size();
    int chosen = -1;
    for (int c = 0; c < kComets; ++c) {
        const double position = std::fmod(head_ + double(c) * n / kComets, double(n));
        const int head = int(position);
        const double frac = position - head;
        const QPointF tip = path_[head] + (path_[(head + 1) % n] - path_[head]) * frac;
        if (tip.y() > radius_ + 1.5) continue; // only the top edge passes the menu
        const QPoint inBar = mapTo(bar, tip.toPoint());
        const QPoint hit(inBar.x(), bar->height() / 2);
        if (!bar->rect().contains(hit)) continue;
        const int index = bar->tabAt(hit);
        if (index < 0 || !bar->isTabVisible(index)) continue;
        if (index == underline->litTab()) { chosen = index; break; }
        if (chosen < 0) chosen = index;
    }
    underline->setLitTab(chosen);
}

void BorderComet::paintEvent(QPaintEvent* event) {
    if (path_.isEmpty()) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int n = path_.size();
    // Continuous: the faint accent along the whole frame line is drawn once
    // per resize into a cached image; each frame only copies the exposed strip.
    for (const QRect& r : event->region()) {
        const qreal dpr = base_.devicePixelRatio();
        painter.drawPixmap(r, base_, QRectF(r.topLeft() * dpr, r.size() * dpr));
    }
    constexpr int kStep = 3;  // tail segments every ~6 px
    const int tail = int(kTailPx / kSampleStepPx);
    const auto at = [&](int i) -> const QPointF& { return path_[((i % n) + n) % n]; };
    for (int c = 0; c < kComets; ++c) {
        const double position = std::fmod(head_ + double(c) * n / kComets, double(n));
        const int head = int(position);
        const double frac = position - head;
        // The head sits between two samples so it glides instead of stepping.
        const QPointF tip = at(head) + (at(head + 1) - at(head)) * frac;
        QPointF previous = tip;
        for (int i = 0; i <= tail; i += kStep) {
            const QPointF& point = at(head - i);
            const double fade = 1.0 - (double(i) + frac) / (tail + 1);
            if (fade <= 0.0) break;
            QColor color = kTail;
            color.setAlphaF(std::max(kBaseAlpha, 0.95 * fade));
            painter.setPen(QPen(color, 1.5 + 1.5 * fade, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(previous, point);
            previous = point;
        }
        // Head: a soft glow with a bright core.
        constexpr qreal kGlow = 9.0;
        QRadialGradient glow(tip, kGlow);
        QColor edge = kTail;
        edge.setAlpha(0);
        glow.setColorAt(0.0, kHead);
        glow.setColorAt(0.35, QColor(kTail.red(), kTail.green(), kTail.blue(), 170));
        glow.setColorAt(1.0, edge);
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
        painter.drawEllipse(tip, kGlow, kGlow);
        painter.setBrush(Qt::NoBrush);
    }
}
