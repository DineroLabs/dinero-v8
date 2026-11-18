#pragma once

#include <QVector>
#include <QWidget>

// A thin line of the mempool's pending-transaction count over recent polls.
// Repaints only when a sample is added (no timer of its own).
class MempoolSparkline : public QWidget {
    Q_OBJECT
public:
    explicit MempoolSparkline(QWidget* parent = nullptr);
    void addSample(int pending);
    QSize sizeHint() const override { return {160, 30}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<int> samples_;
    static constexpr int kMaxSamples = 72;   // ~6 minutes at the 5 s poll
};
