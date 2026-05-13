#pragma once
#include <QWidget>
#include <QVector>

class SparklineWidget : public QWidget {
    Q_OBJECT
public:
    explicit SparklineWidget(QWidget* parent = nullptr);

    void setCapacity(int n);            // samples to keep (default 120)
    int  capacity() const { return cap_; }
    void pushSample(double v);          // append; auto-trims to capacity
    void clear();

protected:
    void paintEvent(QPaintEvent*) override;
    QSize minimumSizeHint() const override { return {220, 60}; }

private:
    QVector<double> samples_;
    int cap_ = 120;
};
