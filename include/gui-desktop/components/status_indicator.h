#pragma once
#include <QWidget>

class StatusIndicator : public QWidget {
    Q_OBJECT
public:
    enum Status { Disconnected, Connected };
    explicit StatusIndicator(QWidget *parent = nullptr);
    void setStatus(Status status);
};
