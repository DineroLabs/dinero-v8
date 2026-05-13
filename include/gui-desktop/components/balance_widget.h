#pragma once

#include <QWidget>

class QPaintEvent;

class BalanceWidget : public QWidget {
    Q_OBJECT

public:
    explicit BalanceWidget(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};
