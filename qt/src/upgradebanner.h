#pragma once
#include <QFrame>
#include "upgradepolicy.h"

class QLabel;

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

private:
    QLabel* message_ = nullptr;
    QLabel* link_ = nullptr;
    quint32 activationHeight_ = 0;
};
