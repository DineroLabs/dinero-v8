#pragma once
#include <QFrame>
#include <QSet>
#include "upgradepolicy.h"

class QLabel;
class QTimer;

// Thin strip at the top of the main window announcing updates and scheduled
// network upgrades. Hidden when there is nothing to say.
class UpgradeBanner : public QFrame {
    Q_OBJECT
public:
    explicit UpgradeBanner(QWidget* parent = nullptr);
    void setActivationHeight(quint32 height) { activationHeight_ = height; }
    // releaseTag: version to name in the message; durationText: estimate for blocksLeft.
    void present(const UpgradePolicy::Result& result, const QString& releaseTag, const QString& durationText);
    QString text() const;
    // Informational messages ("available", "scheduled") hide after this long
    // and stay hidden for the session; required-update warnings never do.
    void setAutoHideMs(int ms) { autoHideMs_ = ms; }
    int autoHideMs() const { return autoHideMs_; }

private:
    QLabel* message_ = nullptr;
    QLabel* link_ = nullptr;
    quint32 activationHeight_ = 0;
    QTimer* autoHide_ = nullptr;
    int autoHideMs_ = 30000;
    QString currentKey_;
    QSet<QString> retired_;
};
