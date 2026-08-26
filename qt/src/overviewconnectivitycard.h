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

// Self-contained Overview controls for safe automatic Tor and relay modes.
// Detailed diagnostics remain available independently through Command-K.
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

private:
    void updateSummary();
    void updateOnionAddress(const QString& address);
    void setTorPending(bool pending);
    void setRelayPending(bool pending);

    QCheckBox* torToggle_{nullptr};
    QCheckBox* relayToggle_{nullptr};
    QLabel* summaryLabel_{nullptr};
    QLabel* torStatusLabel_{nullptr};
    QWidget* onionAddressRow_{nullptr};
    QLabel* onionAddressLabel_{nullptr};
    QPushButton* copyOnionAddressButton_{nullptr};
    QLabel* relayStatusLabel_{nullptr};
    QString onionAddress_;
    bool directActive_{false};
    bool relayFallbackReady_{false};
    bool torActive_{false};
    bool torSupported_{false};
    bool relaySupported_{false};
    bool torPending_{false};
    bool relayPending_{false};
};

}  // namespace dinero::qt
