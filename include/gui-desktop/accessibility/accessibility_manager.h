#pragma once

#include <QObject>
#include <QWidget>
#include <QApplication>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QKeySequence>
#include <QShortcut>
#include <QTimer>
#include <QSettings>
#include <QHash>
#include <QColor>
#include <QFont>
#include <functional>

namespace dinero::accessibility {

/**
 * Accessibility Standards Compliance
 * Ensures WCAG 2.1 AA compliance for cryptocurrency desktop applications
 */
enum class AccessibilityLevel {
    A,      // WCAG 2.1 Level A (minimum)
    AA,     // WCAG 2.1 Level AA (standard)
    AAA     // WCAG 2.1 Level AAA (enhanced)
};

/**
 * User Preference Categories
 * Different accessibility needs and preferences
 */
enum class AccessibilityCategory {
    Vision,         // Visual impairments, color blindness
    Motor,          // Motor disabilities, limited dexterity
    Cognitive,      // Cognitive disabilities, attention disorders
    Hearing,        // Hearing impairments
    Vestibular,     // Motion sensitivity, vestibular disorders
    General         // General usability preferences
};

/**
 * Accessibility Manager
 * Central hub for accessibility features and compliance
 */
class AccessibilityManager : public QObject {
    Q_OBJECT

public:
    static AccessibilityManager* instance();

    // Core accessibility features
    void initialize();
    void enableAccessibilityFeatures(bool enabled);
    bool accessibilityEnabled() const { return m_accessibilityEnabled; }
    
    // Standards compliance
    void setComplianceLevel(AccessibilityLevel level);
    AccessibilityLevel complianceLevel() const { return m_complianceLevel; }
    
    // User preferences
    void loadUserPreferences();
    void saveUserPreferences();
    void resetToDefaults();
    
    // Feature toggles
    void setHighContrast(bool enabled);
    void setLargeText(bool enabled);
    void setReducedMotion(bool enabled);
    void setScreenReaderSupport(bool enabled);
    void setKeyboardNavigation(bool enabled);
    void setFocusIndicators(bool enabled);
    
    // Getters for current settings
    bool highContrastEnabled() const { return m_highContrast; }
    bool largeTextEnabled() const { return m_largeText; }
    bool reducedMotionEnabled() const { return m_reducedMotion; }
    bool screenReaderEnabled() const { return m_screenReaderSupport; }
    bool keyboardNavigationEnabled() const { return m_keyboardNavigation; }
    bool focusIndicatorsEnabled() const { return m_focusIndicators; }
    
    // Color and contrast
    double getContrastRatio(const QColor& foreground, const QColor& background) const;
    bool meetsContrastRequirement(const QColor& fg, const QColor& bg, AccessibilityLevel level = AccessibilityLevel::AA) const;
    QColor getHighContrastColor(const QColor& originalColor, bool isBackground = false) const;
    
    // Text and fonts
    QFont getAccessibleFont(const QFont& baseFont) const;
    int getAccessibleFontSize(int baseSize) const;
    
    // Keyboard shortcuts
    void registerGlobalShortcut(const QString& id, const QKeySequence& sequence, std::function<void()> callback);
    void unregisterGlobalShortcut(const QString& id);
    void enableShortcutHelp(bool enabled);
    
    // Screen reader announcements
    void announceToScreenReader(const QString& text, int priority = 0);
    void announceStatusChange(const QString& status);
    void announceError(const QString& error);
    void announceSuccess(const QString& success);
    
    // Focus management
    void setFocusChain(const QList<QWidget*>& widgets);
    void moveFocusToNext();
    void moveFocusToPrevious();
    void moveFocusToFirst();
    void moveFocusToLast();
    
    // Widget enhancement
    void enhanceWidget(QWidget* widget);
    void setAccessibleName(QWidget* widget, const QString& name);
    void setAccessibleDescription(QWidget* widget, const QString& description);
    void setAccessibleRole(QWidget* widget, QAccessible::Role role);

signals:
    void accessibilitySettingsChanged();
    void highContrastChanged(bool enabled);
    void largeTextChanged(bool enabled);
    void reducedMotionChanged(bool enabled);
    void screenReaderStatusChanged(bool enabled);

private slots:
    void onSystemThemeChanged();
    void onSystemAccessibilityChanged();

private:
    explicit AccessibilityManager(QObject* parent = nullptr);
    
    void setupSystemIntegration();
    void updateGlobalStyles();
    void detectSystemPreferences();
    
    static AccessibilityManager* s_instance;
    
    bool m_accessibilityEnabled = true;
    AccessibilityLevel m_complianceLevel = AccessibilityLevel::AA;
    
    // Feature flags
    bool m_highContrast = false;
    bool m_largeText = false;
    bool m_reducedMotion = false;
    bool m_screenReaderSupport = false;
    bool m_keyboardNavigation = true;
    bool m_focusIndicators = true;
    
    // Settings
    QSettings* m_settings;
    QHash<QString, QShortcut*> m_globalShortcuts;
    QList<QWidget*> m_focusChain;
    
    QTimer* m_announceTimer;
    QString m_pendingAnnouncement;
};

/**
 * Keyboard Navigation Helper
 * Enhances keyboard navigation for complex widgets
 */
class KeyboardNavigationHelper : public QObject {
    Q_OBJECT

public:
    explicit KeyboardNavigationHelper(QWidget* targetWidget, QObject* parent = nullptr);

    // Navigation configuration
    void setNavigationMode(Qt::NavigationMode mode);
    void setTabOrder(const QList<QWidget*>& widgets);
    void setCustomKeyBindings(const QHash<QKeySequence, std::function<void()>>& bindings);
    
    // Focus handling
    void setFocusPolicy(Qt::FocusPolicy policy);
    void setFocusIndicatorStyle(const QString& style);
    
    // Shortcuts
    void addNavigationShortcut(const QKeySequence& key, const QString& description, std::function<void()> callback);
    void enableShortcutTooltips(bool enabled);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void handleKeyPress(QKeyEvent* event);
    void updateFocusIndicator(QWidget* widget);
    void showShortcutTooltip(QWidget* widget);
    
    QWidget* m_targetWidget;
    QList<QWidget*> m_tabOrder;
    QHash<QKeySequence, std::function<void()>> m_keyBindings;
    QHash<QKeySequence, QString> m_shortcutDescriptions;
    
    Qt::NavigationMode m_navigationMode = Qt::TabFocusAllControls;
    Qt::FocusPolicy m_focusPolicy = Qt::StrongFocus;
    QString m_focusIndicatorStyle;
    bool m_shortcutTooltipsEnabled = false;
};

/**
 * Screen Reader Support
 * Provides comprehensive screen reader integration
 */
class ScreenReaderSupport : public QObject {
    Q_OBJECT

public:
    explicit ScreenReaderSupport(QObject* parent = nullptr);

    // Screen reader detection
    bool isScreenReaderActive() const;
    QString getActiveScreenReader() const;
    
    // Content announcements
    void announceText(const QString& text, int priority = 0);
    void announceRegion(QWidget* widget);
    void announceLiveRegion(QWidget* widget, const QString& text);
    
    // Widget descriptions
    void setAccessibleDescription(QWidget* widget, const QString& description);
    void setLiveRegion(QWidget* widget, QAccessible::Role role = QAccessible::StatusBar);
    void setLandmark(QWidget* widget, const QString& landmark);
    
    // Table support
    void setupAccessibleTable(QWidget* table);
    void setTableHeaders(QWidget* table, const QStringList& columnHeaders, const QStringList& rowHeaders = {});
    void setTableCaption(QWidget* table, const QString& caption);
    
    // Form support
    void setupAccessibleForm(QWidget* form);
    void associateLabel(QWidget* control, QWidget* label);
    void setFieldDescription(QWidget* field, const QString& description);
    void setRequired(QWidget* field, bool required);
    void setInvalid(QWidget* field, bool invalid, const QString& errorMessage = QString());

private:
    void detectScreenReader();
    void updateAccessibilityTree();
    
    bool m_screenReaderActive = false;
    QString m_activeScreenReader;
    QTimer* m_announceTimer;
    QStringList m_announcementQueue;
};

/**
 * High Contrast Theme Manager
 * Manages high contrast themes for visual accessibility
 */
class HighContrastManager : public QObject {
    Q_OBJECT

public:
    explicit HighContrastManager(QObject* parent = nullptr);

    // Theme management
    void enableHighContrast(bool enabled);
    bool isHighContrastEnabled() const { return m_highContrastEnabled; }
    
    // Color schemes
    void setColorScheme(const QString& schemeName);
    void registerColorScheme(const QString& name, const QHash<QString, QColor>& colors);
    QStringList availableColorSchemes() const;
    
    // Color utilities
    QColor getContrastingColor(const QColor& baseColor) const;
    QColor getHighContrastBackground() const;
    QColor getHighContrastForeground() const;
    QColor getHighContrastAccent() const;
    
    // Automatic contrast adjustment
    void adjustWidgetContrast(QWidget* widget);
    void adjustStyleSheet(QString& styleSheet);

signals:
    void highContrastToggled(bool enabled);
    void colorSchemeChanged(const QString& schemeName);

private:
    void applyHighContrastTheme();
    void restoreNormalTheme();
    void loadColorSchemes();
    
    bool m_highContrastEnabled = false;
    QString m_currentColorScheme = "default";
    QHash<QString, QHash<QString, QColor>> m_colorSchemes;
    QString m_originalStyleSheet;
};

/**
 * Motion Sensitivity Manager
 * Handles reduced motion preferences and vestibular safety
 */
class MotionSensitivityManager : public QObject {
    Q_OBJECT

public:
    explicit MotionSensitivityManager(QObject* parent = nullptr);

    // Motion preferences
    void setReducedMotion(bool enabled);
    bool reducedMotionEnabled() const { return m_reducedMotion; }
    
    void setRespectSystemPreferences(bool respect);
    bool respectsSystemPreferences() const { return m_respectSystemPrefs; }
    
    // Animation control
    int getSafeAnimationDuration(int originalDuration) const;
    QEasingCurve getSafeEasingCurve(const QEasingCurve& originalCurve) const;
    
    // Parallax and motion effects
    void disableParallaxEffects();
    void disableAutoplayingContent();
    void reduceMotionIntensity(double factor = 0.1);

signals:
    void reducedMotionChanged(bool enabled);

private slots:
    void onSystemPreferencesChanged();

private:
    void detectSystemPreferences();
    void updateAnimationSettings();
    
    bool m_reducedMotion = false;
    bool m_respectSystemPrefs = true;
    double m_motionReductionFactor = 0.1;
};

/**
 * Accessibility Testing Tools
 * Built-in tools for accessibility testing and validation
 */
class AccessibilityTester : public QObject {
    Q_OBJECT

public:
    struct TestResult {
        bool passed;
        QString issue;
        QString suggestion;
        AccessibilityLevel level;
        AccessibilityCategory category;
    };

    explicit AccessibilityTester(QObject* parent = nullptr);

    // Widget testing
    QList<TestResult> testWidget(QWidget* widget, AccessibilityLevel targetLevel = AccessibilityLevel::AA);
    QList<TestResult> testApplication(AccessibilityLevel targetLevel = AccessibilityLevel::AA);
    
    // Specific tests
    TestResult testColorContrast(QWidget* widget);
    TestResult testKeyboardNavigation(QWidget* widget);
    TestResult testScreenReaderSupport(QWidget* widget);
    TestResult testFocusIndicators(QWidget* widget);
    TestResult testTextAlternatives(QWidget* widget);
    
    // Reporting
    QString generateReport(const QList<TestResult>& results);
    void saveReport(const QString& filename, const QList<TestResult>& results);

private:
    double calculateLuminance(const QColor& color) const;
    bool hasAccessibleName(QWidget* widget) const;
    bool hasProperFocusIndicator(QWidget* widget) const;
    bool isKeyboardAccessible(QWidget* widget) const;
};

} // namespace dinero::accessibility

// Convenience macros for accessibility
#define DINERO_A11Y() dinero::accessibility::AccessibilityManager::instance()
#define DINERO_ANNOUNCE(text) dinero::accessibility::AccessibilityManager::instance()->announceToScreenReader(text)
#define DINERO_SET_ACCESSIBLE_NAME(widget, name) dinero::accessibility::AccessibilityManager::instance()->setAccessibleName(widget, name)
#define DINERO_SET_ACCESSIBLE_DESC(widget, desc) dinero::accessibility::AccessibilityManager::instance()->setAccessibleDescription(widget, desc)
