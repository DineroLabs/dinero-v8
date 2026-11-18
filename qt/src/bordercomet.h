#pragma once

#include <QElapsedTimer>
#include <QPixmap>
#include <QPointF>
#include <QRect>
#include <QRegion>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <functional>

class QTabWidget;

// A small light that travels continuously along a frame line. Purely
// decorative:
//  - transparent to the mouse and never takes focus;
//  - the frame path is sampled once per resize; each frame only moves an
//    index and repaints the comet's own small bounding box;
//  - ~30 fps while its window is on screen, 1 Hz idle checks otherwise;
//  - QSettings "ui/borderComet" = false turns every comet off.
class BorderComet : public QWidget {
    Q_OBJECT
public:
    // The main window's content frame (the tab pane border).
    explicit BorderComet(QTabWidget* tabs);
    // Any window: the frame is the host's own edge.
    BorderComet(QWidget* host, qreal radius);

    static bool Enabled();

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void start();
    void relayout();
    void tick();
    QRegion cometRegion(double head) const;
    bool onScreen() const;

    QWidget* host_;
    QWidget* watchedWindow_ = nullptr;
    std::function<QRect()> frame_;
    qreal radius_;
    QTimer timer_;
    QTimer resume_;             // shows the comet again after scrolling stops
    QElapsedTimer clock_;
    QPixmap base_;              // cached faint frame line
    QVector<QPointF> path_;     // frame samples, ~2 px apart, clockwise
    double head_ = 0.0;         // head position as an index into path_
};
