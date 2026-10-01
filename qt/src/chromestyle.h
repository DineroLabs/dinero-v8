#pragma once
#include <QString>

// The app's standard button look, shared by MainWindow and the tab widgets.
inline QString chromeButtonStyle() {
  return QStringLiteral(
    "QPushButton { padding: 6px 12px; background: #2b3037; color: #e6ebf1; "
    "border: 1px solid #3c434d; border-radius: 7px; font-weight: 600; } "
    "QPushButton:hover { background: #333942; } "
    "QPushButton:pressed { background: #262b31; } "
    "QPushButton:disabled { background: #21252a; color: #7f8893; border: 1px solid #30353d; }");
}
