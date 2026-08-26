// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "overviewconnectivitycard.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QHBoxLayout>
#include <QJsonValue>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace dinero::qt {

namespace {

constexpr auto kTorConsentSetting = "network/tor_consent_v1";

QString pathState(bool active, const QString& activeText, const QString& inactiveText) {
    return active ? activeText : inactiveText;
}

}  // namespace

OverviewConnectivityCard::OverviewConnectivityCard(QWidget* parent)
    : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(6);

    summaryLabel_ = new QLabel(tr("Checking available network paths…"), this);
    summaryLabel_->setObjectName(QStringLiteral("overviewConnectivitySummary"));
    summaryLabel_->setWordWrap(true);
    summaryLabel_->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 600; color: #d6dde6; padding-bottom: 2px;"));
    root->addWidget(summaryLabel_);

    torToggle_ = new QCheckBox(tr("Private and resilient connectivity"), this);
    torToggle_->setObjectName(QStringLiteral("overviewTorToggle"));
    torToggle_->setEnabled(false);
    torToggle_->setToolTip(tr(
        "Uses Dinero's included Tor component for Dinero P2P only. Ordinary P2P remains available if Tor cannot start."));
    root->addWidget(torToggle_);

    torStatusLabel_ = new QLabel(
        tr("Use Dinero's included privacy network when needed."), this);
    torStatusLabel_->setObjectName(QStringLiteral("overviewTorStatus"));
    torStatusLabel_->setWordWrap(true);
    torStatusLabel_->setStyleSheet(QStringLiteral("color: #aeb8c4; padding-left: 22px;"));
    root->addWidget(torStatusLabel_);

    onionAddressRow_ = new QWidget(this);
    onionAddressRow_->setObjectName(QStringLiteral("overviewOnionAddressRow"));
    auto* onionAddressLayout = new QHBoxLayout(onionAddressRow_);
    onionAddressLayout->setContentsMargins(22, 0, 0, 0);
    onionAddressLayout->setSpacing(8);
    onionAddressLabel_ = new QLabel(onionAddressRow_);
    onionAddressLabel_->setObjectName(QStringLiteral("overviewOnionAddress"));
    onionAddressLabel_->setStyleSheet(QStringLiteral("color: #aeb8c4;"));
    onionAddressLayout->addWidget(onionAddressLabel_, 1);
    copyOnionAddressButton_ = new QPushButton(tr("Copy"), onionAddressRow_);
    copyOnionAddressButton_->setObjectName(QStringLiteral("overviewCopyOnionAddress"));
    copyOnionAddressButton_->setToolTip(tr("Copy the full public Dinero P2P onion address"));
    onionAddressLayout->addWidget(copyOnionAddressButton_);
    onionAddressRow_->hide();
    root->addWidget(onionAddressRow_);

    relayToggle_ = new QCheckBox(tr("Enable relay service"), this);
    relayToggle_->setObjectName(QStringLiteral("overviewRelayToggle"));
    relayToggle_->setEnabled(false);
    relayToggle_->setToolTip(tr(
        "Serves only Dinero P2P traffic within conservative automatic limits; it is not a web proxy or a system-wide relay."));
    root->addWidget(relayToggle_);

    relayStatusLabel_ = new QLabel(
        tr("Make this node available to other Dinero nodes within safe limits."), this);
    relayStatusLabel_->setObjectName(QStringLiteral("overviewRelayStatus"));
    relayStatusLabel_->setWordWrap(true);
    relayStatusLabel_->setStyleSheet(QStringLiteral("color: #aeb8c4; padding-left: 22px;"));
    root->addWidget(relayStatusLabel_);

    connect(torToggle_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!torSupported_ || torPending_) return;
        if (enabled && !QSettings().value(QString::fromLatin1(kTorConsentSetting), false).toBool()) {
            const auto answer = QMessageBox::question(
                this, tr("Private and resilient connectivity"),
                tr("Allow Dinero to use its included Tor component for private and resilient network connectivity."));
            if (answer != QMessageBox::Yes) {
                const QSignalBlocker blocker(torToggle_);
                torToggle_->setChecked(false);
                return;
            }
            QSettings().setValue(QString::fromLatin1(kTorConsentSetting), true);
        }
        setTorPending(true);
        Q_EMIT torModeRequested(enabled ? QStringLiteral("automatic")
                                       : QStringLiteral("off"));
    });
    connect(relayToggle_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!relaySupported_ || relayPending_) return;
        setRelayPending(true);
        Q_EMIT relayModeRequested(enabled ? QStringLiteral("automatic")
                                         : QStringLiteral("off"));
    });
    connect(copyOnionAddressButton_, &QPushButton::clicked, this, [this]() {
        if (!onionAddress_.isEmpty()) QApplication::clipboard()->setText(onionAddress_);
    });
}

void OverviewConnectivityCard::setNetworkInfo(const QJsonObject& info) {
    directActive_ = info.value(QStringLiteral("direct_reachable")).toBool(false);
    relayFallbackReady_ = info.value(QStringLiteral("relay_fallback_eligible")).toBool(false);

    const auto onionValue = info.value(QStringLiteral("onion_service"));
    if (onionValue.isObject()) {
        setOnionServiceStatus(onionValue.toObject());
    } else {
        torActive_ = false;
        torSupported_ = false;
        setTorPending(false);
        torToggle_->setEnabled(false);
        torStatusLabel_->setText(tr("This daemon does not support live Tor controls."));
        updateOnionAddress({});
    }
    updateSummary();
}

void OverviewConnectivityCard::setOnionServiceStatus(const QJsonObject& onion) {
    torSupported_ = true;
    const bool requested = onion.value(QStringLiteral("requested")).toBool(false);
    torActive_ = onion.value(QStringLiteral("active")).toBool(false);
    const QString mode = onion.value(QStringLiteral("mode")).toString(
        requested ? QStringLiteral("automatic") : QStringLiteral("off"));
    {
        const QSignalBlocker blocker(torToggle_);
        torToggle_->setChecked(requested && mode != QStringLiteral("off"));
    }
    setTorPending(false);
    torStatusLabel_->setText(torActive_
        ? tr("Tor is active for Dinero P2P; ordinary connections remain available.")
        : requested
            ? tr("Tor is starting or recovering; ordinary P2P continues normally.")
            : tr("Use Dinero's included privacy network when needed."));
    updateOnionAddress(torActive_
        ? onion.value(QStringLiteral("address")).toString()
        : QString{});
    updateSummary();
}

void OverviewConnectivityCard::updateOnionAddress(const QString& address) {
    static const QRegularExpression publicOnionPattern(
        QStringLiteral("^[a-z2-7]{56}\\.onion$"),
        QRegularExpression::CaseInsensitiveOption);
    const QString candidate = address.trimmed().toLower();
    onionAddress_ = publicOnionPattern.match(candidate).hasMatch() ? candidate : QString{};
    const bool available = !onionAddress_.isEmpty();
    onionAddressRow_->setVisible(available);
    if (!available) {
        onionAddressLabel_->clear();
        onionAddressLabel_->setToolTip({});
        return;
    }

    const QString compact = QStringLiteral("%1…%2")
        .arg(onionAddress_.left(12), onionAddress_.right(12));
    onionAddressLabel_->setText(tr("Public Dinero P2P address: %1").arg(compact));
    onionAddressLabel_->setToolTip(onionAddress_);
}

void OverviewConnectivityCard::setRelayServiceStatus(const QJsonObject& status) {
    relaySupported_ = !status.isEmpty();
    const QString mode = status.value(QStringLiteral("mode")).toString(QStringLiteral("off"));
    const bool enabled = status.value(QStringLiteral("enabled")).toBool(false);
    {
        const QSignalBlocker blocker(relayToggle_);
        relayToggle_->setChecked(mode != QStringLiteral("off"));
    }
    setRelayPending(false);
    if (relaySupported_) {
        relayStatusLabel_->setText(enabled
            ? tr("Relay service is active within conservative automatic limits.")
            : mode != QStringLiteral("off")
                ? tr("Relay service is enabled and will activate when this node is eligible.")
                : tr("Make this node available to other Dinero nodes within safe limits."));
    }
}

void OverviewConnectivityCard::setTorActionError(bool unsupported) {
    setTorPending(false);
    if (unsupported) {
        torSupported_ = false;
        torToggle_->setEnabled(false);
        torStatusLabel_->setText(tr("This daemon does not support live Tor controls."));
        updateOnionAddress({});
    } else {
        torStatusLabel_->setText(tr(
            "Could not change Tor connectivity. Ordinary P2P continues normally."));
    }
}

void OverviewConnectivityCard::setRelayActionError(bool unsupported) {
    setRelayPending(false);
    if (unsupported) {
        relaySupported_ = false;
        relayToggle_->setEnabled(false);
        relayStatusLabel_->setText(tr("This daemon does not support live relay controls."));
    } else {
        relayStatusLabel_->setText(tr(
            "Could not change relay service. Existing network paths remain available."));
    }
}

void OverviewConnectivityCard::updateSummary() {
    const QString direct = pathState(directActive_, tr("Direct active"), tr("Direct inbound unavailable"));
    const QString relay = pathState(relayFallbackReady_, tr("Relay fallback ready"), tr("Relay fallback idle"));
    const QString tor = pathState(torActive_, tr("Tor active"), tr("Tor inactive"));
    summaryLabel_->setText(QStringLiteral("%1 · %2 · %3").arg(direct, relay, tor));
}

void OverviewConnectivityCard::setTorPending(bool pending) {
    torPending_ = pending;
    torToggle_->setEnabled(torSupported_ && !pending);
    if (pending) torStatusLabel_->setText(tr("Applying securely…"));
}

void OverviewConnectivityCard::setRelayPending(bool pending) {
    relayPending_ = pending;
    relayToggle_->setEnabled(relaySupported_ && !pending);
    if (pending) relayStatusLabel_->setText(tr("Applying safe automatic limits…"));
}

}  // namespace dinero::qt
