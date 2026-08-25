// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <QJsonObject>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;

namespace dinero::qt {

// Simple Overview controls backed by the same daemon RPCs as Command-K.
// This surface intentionally exposes only safe automatic modes; expert Tor
// endpoints and relay limits remain in the advanced dashboard.
class OverviewConnectivityCard : public QWidget {
    Q_OBJECT
public:
    explicit OverviewConnectivityCard(QWidget* parent = nullptr);

    void setNetworkInfo(const QJsonObject& info);
    void setOnionServiceStatus(const QJsonObject& status);
    void setRelayServiceStatus(const QJsonObject& status);
    void setTorActionError(bool unsupported = false);
    void setRelayActionError(bool unsupported = false);

Q_SIGNALS:
    void torModeRequested(const QString& mode);
    void relayModeRequested(const QString& mode);
    void advancedControlsRequested();

private:
    void updateSummary();
    void setTorPending(bool pending);
    void setRelayPending(bool pending);

    QCheckBox* torToggle_{nullptr};
    QCheckBox* relayToggle_{nullptr};
    QLabel* summaryLabel_{nullptr};
    QLabel* torStatusLabel_{nullptr};
    QLabel* relayStatusLabel_{nullptr};
    QPushButton* advancedButton_{nullptr};
    bool directActive_{false};
    bool relayFallbackReady_{false};
    bool torActive_{false};
    bool torSupported_{false};
    bool relaySupported_{false};
    bool torPending_{false};
    bool relayPending_{false};
};

}  // namespace dinero::qt
