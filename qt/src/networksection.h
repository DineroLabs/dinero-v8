// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"
#include <QJsonObject>
#include <QVector>
#include <QWidget>

class QLabel;
class QProgressBar;
class QCheckBox;
class QComboBox;
class QSpinBox;

namespace dinero::qt::dashboard {

// 📡 NETWORK · as you see it — chain tip race + secondary metrics.
class NetworkSection : public QWidget {
    Q_OBJECT
public:
    enum class TorState { Unsupported, Off, Active, Error };
    explicit NetworkSection(QWidget* parent = nullptr);

public Q_SLOTS:
    void onChainInfoUpdated(const ChainInfo& info);
    void setTorStatus(TorState state, const QString& onionAddress = {});
    void setOnionServiceStatus(const OnionServiceStatus& status);
    void setTorActionError(bool unsupported);
    void setRelayServiceStatus(const QJsonObject& status);

Q_SIGNALS:
    void torModeRequested(const QString& mode);
    void relayServiceRequested(const QJsonObject& request);

public:
    // Tip-race annotation helper, public for unit testing.
    static QString tipDeltaAnnotation(qint64 our, qint64 net);
    static QString torStatusText(TorState state, const QString& onionAddress = {});

private:
    QProgressBar* youBar_{nullptr};
    QProgressBar* netBar_{nullptr};
    QLabel*       deltaLabel_{nullptr};
    QLabel*       difficultyLabel_{nullptr};
    QLabel*       mempoolLabel_{nullptr};
    QLabel*       medianFeeLabel_{nullptr};
    QComboBox*    torControl_{nullptr};
    QLabel*       torStatusLabel_{nullptr};
    QComboBox*    relayControl_{nullptr};
    QWidget*      relayCustom_{nullptr};
    QVector<QSpinBox*> relayLimits_;
    QLabel*       relayStatusLabel_{nullptr};
};

}  // namespace dinero::qt::dashboard
