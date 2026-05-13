#pragma once

#include <QWidget>
#include <QJsonObject>

class QLabel;

class AiStatusStrip : public QWidget {
    Q_OBJECT
public:
    explicit AiStatusStrip(QWidget* parent = nullptr);

    void updateStatus(int height, int headers, int peerCount,
                      double balance, bool miningActive, double hashrateHps,
                      bool bridgeActive = false, int proofCacheEntries = 0);

Q_SIGNALS:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void setupUI();

    QLabel* statusLabel_;
    QLabel* shortcutHint_;
};
