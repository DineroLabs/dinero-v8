// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "cmdkpanel.h"

#include "aipanel.h"
#include "mynodedashboard.h"

#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

namespace dinero::qt::dashboard {

CmdKPanel::CmdKPanel(RpcClient* rpc, AiPanel* aiPanel, QWidget* parent)
    : QWidget(parent) {
    setFixedWidth(0);  // start collapsed; slide animation grows panelWidth

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    tabBar_ = new QTabBar(this);
    tabBar_->addTab("Dashboard");
    tabBar_->addTab("AI");
    root->addWidget(tabBar_);

    stack_ = new QStackedWidget(this);
    dashboard_ = new MyNodeDashboard(rpc, this);
    stack_->addWidget(dashboard_);   // index 0
    stack_->addWidget(aiPanel);      // index 1 — re-parented to us
    root->addWidget(stack_, 1);

    connect(tabBar_, &QTabBar::currentChanged,
            stack_, &QStackedWidget::setCurrentIndex);
    tabBar_->setCurrentIndex(0);

    slideAnim_ = new QPropertyAnimation(this, "panelWidth", this);
    slideAnim_->setDuration(200);
}

int CmdKPanel::panelWidth() const { return width(); }

void CmdKPanel::setPanelWidth(int w) {
    setFixedWidth(w);
}

void CmdKPanel::togglePanel() {
    slideAnim_->stop();
    if (panelOpen_) {
        slideAnim_->setStartValue(targetWidth_);
        slideAnim_->setEndValue(0);
        dashboard_->stop();
        panelOpen_ = false;
    } else {
        slideAnim_->setStartValue(0);
        slideAnim_->setEndValue(targetWidth_);
        dashboard_->start();
        panelOpen_ = true;
    }
    slideAnim_->start();
    Q_EMIT panelToggled(panelOpen_);
}

}  // namespace dinero::qt::dashboard
