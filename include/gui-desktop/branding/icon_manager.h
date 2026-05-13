#pragma once

#include <QIcon>
#include <QPixmap>
#include <QColor>
#include <QSize>
#include <QHash>
#include <QString>
#include <QSvgRenderer>
#include <QPainter>

namespace dinero::branding {

/**
 * Icon Manager for Dinero Desktop
 * Centralizes icon loading, caching, and theming
 */
class IconManager : public QObject {
    Q_OBJECT

public:
    enum class IconType {
        // Brand icons
        DineroLogo,
        
        // Network icons
        NetworkMainnet,
        NetworkTestnet,
        NetworkRegtest,
        NetworkOffline,
        
        // Feature icons
        Block,
        Transaction,
        Wallet,
        Mining,
        Settings,
        
        // UI icons
        Copy,
        Refresh,
        Search,
        Filter,
        Sort,
        Export,
        Import,
        Help,
        Info,
        Warning,
        Error,
        Success,
        
        // Navigation icons
        Home,
        Back,
        Forward,
        Up,
        Down,
        Left,
        Right,
        Menu,
        Close,
        
        // Status icons
        Online,
        Offline,
        Syncing,
        Connected,
        Disconnected
    };

    enum class IconSize {
        Small = 16,    // Toolbar, inline icons
        Medium = 24,   // Standard UI icons
        Large = 32,    // Headers, prominent features
        XLarge = 48,   // Splash, about dialog
        XXLarge = 64   // Application icon
    };

    enum class IconTheme {
        Light,
        Dark,
        Auto  // Follow system theme
    };

    static IconManager* instance();

    // Core icon methods
    QIcon getIcon(IconType type, IconSize size = IconSize::Medium, IconTheme theme = IconTheme::Auto);
    QPixmap getPixmap(IconType type, IconSize size = IconSize::Medium, IconTheme theme = IconTheme::Auto);
    
    // Themed icon variants
    QIcon getThemedIcon(IconType type, const QColor& color, IconSize size = IconSize::Medium);
    QPixmap getColoredPixmap(IconType type, const QColor& color, IconSize size = IconSize::Medium);
    
    // Network-specific icons with status
    QIcon getNetworkIcon(const QString& network, bool connected = true, IconSize size = IconSize::Medium);
    QIcon getStatusIcon(const QString& status, IconSize size = IconSize::Medium);
    
    // Dynamic icon generation
    QIcon createStatusIndicator(const QColor& color, IconSize size = IconSize::Medium, bool animated = false);
    QIcon createBadgeIcon(IconType baseIcon, const QString& badgeText, const QColor& badgeColor);
    
    // Theme management
    void setGlobalTheme(IconTheme theme);
    IconTheme currentTheme() const { return m_currentTheme; }
    
    // Cache management
    void clearCache();
    void preloadIcons();
    
    // Icon registration (for custom icons)
    void registerIcon(IconType type, const QString& lightPath, const QString& darkPath = QString());
    void registerSvgIcon(IconType type, const QString& svgPath);

signals:
    void themeChanged(IconTheme newTheme);

private slots:
    void onSystemThemeChanged();

private:
    explicit IconManager(QObject* parent = nullptr);
    
    QString getIconPath(IconType type, IconTheme theme) const;
    QString iconTypeToString(IconType type) const;
    QPixmap renderSvg(const QString& svgPath, IconSize size, const QColor& color = QColor()) const;
    QPixmap createPixmap(IconType type, IconSize size, IconTheme theme) const;
    QString getCacheKey(IconType type, IconSize size, IconTheme theme, const QColor& color = QColor()) const;
    
    bool isDarkTheme() const;
    IconTheme resolveTheme(IconTheme requestedTheme) const;
    
    static IconManager* s_instance;
    IconTheme m_currentTheme = IconTheme::Auto;
    
    // Cache for loaded icons and pixmaps
    QHash<QString, QIcon> m_iconCache;
    QHash<QString, QPixmap> m_pixmapCache;
    
    // Custom icon paths
    QHash<IconType, QString> m_lightIconPaths;
    QHash<IconType, QString> m_darkIconPaths;
    QHash<IconType, QString> m_svgIconPaths;
};

/**
 * Icon Helper Functions
 * Convenience functions for common icon operations
 */
namespace icons {

// Quick access functions
inline QIcon get(IconManager::IconType type, IconManager::IconSize size = IconManager::IconSize::Medium) {
    return IconManager::instance()->getIcon(type, size);
}

inline QPixmap pixmap(IconManager::IconType type, IconManager::IconSize size = IconManager::IconSize::Medium) {
    return IconManager::instance()->getPixmap(type, size);
}

// Network status icons
inline QIcon networkStatus(const QString& network, bool connected = true) {
    return IconManager::instance()->getNetworkIcon(network, connected);
}

// Colored icons
inline QIcon colored(IconManager::IconType type, const QColor& color, IconManager::IconSize size = IconManager::IconSize::Medium) {
    return IconManager::instance()->getThemedIcon(type, color, size);
}

// Status indicators
inline QIcon status(const QString& status) {
    return IconManager::instance()->getStatusIcon(status);
}

// Brand icons
inline QIcon logo(IconManager::IconSize size = IconManager::IconSize::Large) {
    return get(IconManager::IconType::DineroLogo, size);
}

} // namespace icons

/**
 * Icon Resource Initializer
 * Registers all built-in icons with the IconManager
 */
class IconResourceInitializer {
public:
    static void initialize();
    
private:
    static void registerBuiltinIcons();
    static void registerNetworkIcons();
    static void registerUIIcons();
    static void registerStatusIcons();
};

} // namespace dinero::branding

// Convenience macros for common icon usage
#define DINERO_ICON(type) dinero::branding::icons::get(dinero::branding::IconManager::IconType::type)
#define DINERO_ICON_SIZE(type, size) dinero::branding::icons::get(dinero::branding::IconManager::IconType::type, dinero::branding::IconManager::IconSize::size)
#define DINERO_ICON_COLOR(type, color) dinero::branding::icons::colored(dinero::branding::IconManager::IconType::type, color)
#define DINERO_LOGO() dinero::branding::icons::logo()
#define DINERO_NETWORK_ICON(network, connected) dinero::branding::icons::networkStatus(network, connected)
