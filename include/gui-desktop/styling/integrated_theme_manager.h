#pragma once

#include <QObject>
#include <QString>

namespace dinero::styling {

/**
 * Integrated Theme Manager
 * Combines our modern styling system with existing GUI components
 */
class IntegratedThemeManager : public QObject {
    Q_OBJECT

public:
    enum class Theme {
        Light,
        Dark,
        Auto
    };

    static IntegratedThemeManager* instance();

    // Theme management
    void applyTheme(Theme theme);
    Theme currentTheme() const { return m_currentTheme; }
    
    // Theme generation
    QString generateLightTheme();
    QString generateDarkTheme();
    
    // System integration
    bool isSystemDarkMode();

signals:
    void themeChanged(Theme theme);

private:
    explicit IntegratedThemeManager(QObject* parent = nullptr);
    void loadThemes();
    
    static IntegratedThemeManager* s_instance;
    Theme m_currentTheme = Theme::Light;
};

} // namespace dinero::styling
