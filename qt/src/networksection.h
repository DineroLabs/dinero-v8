// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"
#include <QWidget>

class QLabel;
class QProgressBar;

namespace dinero::qt::dashboard {

// 📡 NETWORK · as you see it — chain tip race + secondary metrics.
class NetworkSection : public QWidget {
    Q_OBJECT
public:
    explicit NetworkSection(QWidget* parent = nullptr);

public Q_SLOTS:
    void onChainInfoUpdated(const ChainInfo& info);

    // Tip-race annotation helper, public for unit testing.
    static QString tipDeltaAnnotation(qint64 our, qint64 net);

private:
    QProgressBar* youBar_{nullptr};
    QProgressBar* netBar_{nullptr};
    QLabel*       deltaLabel_{nullptr};
    QLabel*       difficultyLabel_{nullptr};
    QLabel*       mempoolLabel_{nullptr};
    QLabel*       medianFeeLabel_{nullptr};
};

}  // namespace dinero::qt::dashboard
