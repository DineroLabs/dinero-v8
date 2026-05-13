#pragma once

#include <QObject>
#include <QWidget>
#include <QSet>
#include <QHash>
#include "gui-desktop/accessibility/accessibility_manager.h"

// Forward declarations
class MainWindow;
class StatusTab;
class WalletTab;

namespace dinero::accessibility {

class KeyboardNavigationHelper;
class ScreenReaderSupport;

/**
 * GUI Accessibility Integration
 * Integrates accessibility features with existing GUI components
 */
class GuiAccessibilityIntegration : public QObject {
    Q_OBJECT

public:
    explicit GuiAccessibilityIntegration(QObject* parent = nullptr);

    // Initialization
    void initializeForApplication();
    
    // Widget enhancement
    void enhanceMainWindow(MainWindow* mainWindow);
    void enhanceTabWidget(QWidget* tabWidget, const QString& tabName, const QString& description);
    void enhanceStatusTab(StatusTab* statusTab);
    void enhanceWalletTab(WalletTab* walletTab);
    
    // Accessibility state
    bool isAccessibilityEnabled() const;
    
    // Screen reader announcements
    void announceMessage(const QString& message);
    void announceError(const QString& error);
    void announceSuccess(const QString& success);

signals:
    void accessibilitySettingsChanged();
    void highContrastChanged(bool enabled);
    void largeTextChanged(bool enabled);
    void reducedMotionChanged(bool enabled);

private slots:
    void onAccessibilitySettingsChanged();
    void onHighContrastChanged(bool enabled);
    void onLargeTextChanged(bool enabled);
    void onReducedMotionChanged(bool enabled);

private:
    void setupMainWindowKeyboardNavigation(MainWindow* mainWindow);
    void setupMainWindowScreenReaderSupport(MainWindow* mainWindow);
    void setupStatusTabAccessibility(StatusTab* statusTab);
    void setupWalletTabAccessibility(WalletTab* walletTab);
    void registerGlobalShortcuts();
    
    AccessibilityManager* m_accessibilityManager;
    
    // Enhanced widgets tracking
    QSet<QWidget*> m_enhancedWidgets;
    QHash<QWidget*, KeyboardNavigationHelper*> m_keyboardHelpers;
    QHash<QWidget*, ScreenReaderSupport*> m_screenReaderSupports;
};

} // namespace dinero::accessibility
