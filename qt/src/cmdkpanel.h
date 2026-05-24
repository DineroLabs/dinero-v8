// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dashboardtypes.h"

#include <QPropertyAnimation>
#include <QWidget>

class AiPanel;
class QStackedWidget;
class QTabBar;
class RpcClient;

namespace dinero::qt::dashboard {

class MyNodeDashboard;

// Top-level slide-in container for the Cmd+K experience.
// Owns the slide animation. Hosts a QTabBar + QStackedWidget with two
// tabs in Phase 1: Dashboard (default) and AI (existing AiPanel,
// passed in by the parent so its lifecycle is unchanged).
class CmdKPanel : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int panelWidth READ panelWidth WRITE setPanelWidth)
public:
    explicit CmdKPanel(RpcClient* rpc, AiPanel* aiPanel,
                       QWidget* parent = nullptr);

    int  panelWidth() const;
    void setPanelWidth(int w);

    void togglePanel();
    bool isPanelOpen() const { return panelOpen_; }

    // Forward to MyNodeDashboard::setLocalMiningProvider — lets MainWindow
    // surface qt-app-side mining state (which the daemon doesn't see).
    void setLocalMiningProvider(LocalMiningProvider provider);

Q_SIGNALS:
    void panelToggled(bool open);

private:
    bool             panelOpen_{false};
    int              targetWidth_{520};
    QPropertyAnimation* slideAnim_{nullptr};
    QTabBar*         tabBar_{nullptr};
    QStackedWidget*  stack_{nullptr};
    MyNodeDashboard* dashboard_{nullptr};
};

}  // namespace dinero::qt::dashboard
