#include "upgradebanner.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>

namespace {
const char* kReleasesUrl = "https://github.com/DineroLabs/dinero-v8/releases/latest";
}

UpgradeBanner::UpgradeBanner(QWidget* parent) : QFrame(parent) {
    setObjectName("upgradeBanner");
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 6, 12, 6);
    message_ = new QLabel(this);
    message_->setWordWrap(true);
    message_->setTextFormat(Qt::PlainText);
    link_ = new QLabel(this);
    link_->setTextFormat(Qt::RichText);
    link_->setOpenExternalLinks(true);
    layout->addWidget(message_, 1);
    layout->addWidget(link_);
    autoHide_ = new QTimer(this);
    autoHide_->setSingleShot(true);
    connect(autoHide_, &QTimer::timeout, this, [this]() {
        retired_.insert(currentKey_);
        hide();
    });
    hide();
}

void UpgradeBanner::present(const UpgradePolicy::Result& r, const QString& releaseTag,
                            const QString& durationText) {
    using UpgradePolicy::State;
    const bool informational = r.state == State::UpdateAvailable || r.state == State::ScheduledReady;
    // Identity of the message, without the countdown, which changes every block.
    const QString key = QString("%1|%2|%3").arg(UpgradePolicy::stateName(r.state), releaseTag)
                            .arg(activationHeight_);
    if (r.state == State::None || (informational && retired_.contains(key))) {
        autoHide_->stop();
        hide();
        return;
    }
    if (!informational) {
        autoHide_->stop();
    } else if (key != currentKey_ || !autoHide_->isActive()) {
        autoHide_->start(autoHideMs_);
    }
    currentKey_ = key;
    QString text, background, linkText = QStringLiteral("Download");
    switch (r.state) {
    case State::None:
        hide();
        return;
    case State::UpdateAvailable:
        text = QString("Dinero %1 is available.").arg(releaseTag);
        background = "#1c3a5e";
        break;
    case State::UpdateRequired:
        text = QString("Update required: Dinero %1 activates at block %2 (in %3 blocks, %4). "
                       "Older versions will be disconnected.")
                   .arg(releaseTag).arg(activationHeight_).arg(r.blocksLeft).arg(durationText);
        background = "#6b4a00";
        break;
    case State::RequiredOverdue:
        text = QString("This version is out of date. The network upgraded at block %1 and no longer "
                       "accepts it.").arg(activationHeight_);
        linkText = QString("Download Dinero %1").arg(releaseTag);
        background = "#6b1f1f";
        break;
    case State::ScheduledReady:
        text = QString("Network upgrade at block %1 (in %2 blocks, %3). You're on a compatible version.")
                   .arg(activationHeight_).arg(r.blocksLeft).arg(durationText);
        background = "#1f4d36";
        linkText.clear();
        break;
    }
    message_->setText(text);
    link_->setText(linkText.isEmpty() ? QString()
                                      : QString("<a style=\"color:#ffd97a\" href=\"%1\">%2</a>")
                                            .arg(kReleasesUrl, linkText.toHtmlEscaped()));
    link_->setVisible(!linkText.isEmpty());
    setStyleSheet(QString("#upgradeBanner { background:%1; } #upgradeBanner QLabel { color:#ffffff; }")
                      .arg(background));
    show();
}

QString UpgradeBanner::text() const { return message_->text(); }
