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

// The app's page look (dark palette, cards, inputs, tabs, buttons), set once
// on the main window's central widget; every tab inherits it.
inline QString appPageStyle() {
  return QStringLiteral(
    "QWidget { background: #181b20; color: #d6dde6; } "
    "QTabWidget::pane { border: 1px solid #2f343c; background: #1a1d22; border-radius: 8px; margin-top: 6px; } "
    "QTabBar::tab { background: #242932; color: #d5dce5; border: 1px solid #353b45; border-bottom: 3px solid transparent; "
    "padding: 6px 8px; min-height: 20px; font-size: 12px; font-weight: 500; "
    "border-top-left-radius: 6px; border-top-right-radius: 6px; margin-right: 2px; } "
    "QTabBar::tab:hover { background: #2a3039; color: #e7ecf2; border-color: #46505d; border-bottom-color: #46505d; } "
    "QTabBar::tab:selected { background: #303844; color: #f2f5f8; border-color: #46505d; border-bottom: 3px solid #d58a32; } "
    "QGroupBox { border: 1px solid #30353d; border-radius: 10px; margin-top: 10px; padding-top: 8px; background: #20242a; font-weight: 600; } "
    "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: #cad2db; } "
    "QLineEdit, QComboBox, QSpinBox, QTextEdit, QPlainTextEdit { background: #1f2328; color: #d7dde5; "
    "border: 1px solid #353b44; border-radius: 6px; padding: 6px; selection-background-color: #3e4550; } "
    "QPushButton { background: #2b3037; color: #e6ebf1; border: 1px solid #3c434d; border-radius: 7px; padding: 6px 12px; font-weight: 600; } "
    "QPushButton:hover { background: #333942; } "
    "QPushButton:pressed { background: #262b31; } "
    "QPushButton:disabled { background: #21252a; color: #7f8893; border: 1px solid #30353d; }");
}
