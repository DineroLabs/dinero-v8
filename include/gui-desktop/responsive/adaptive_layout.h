#pragma once

#include <QWidget>
#include <QLayout>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedLayout>
#include <QSplitter>
#include <QScrollArea>
#include <QResizeEvent>
#include <QScreen>
#include <QApplication>
#include <QTimer>
#include <functional>

namespace dinero::responsive {

/**
 * Responsive Breakpoints
 * Standard breakpoints for different device categories
 */
enum class Breakpoint {
    XSmall,   // < 576px  (Small phones)
    Small,    // 576-767px (Phones)
    Medium,   // 768-991px (Tablets)
    Large,    // 992-1199px (Small desktops)
    XLarge,   // 1200-1399px (Desktops)
    XXLarge   // >= 1400px (Large desktops)
};

/**
 * Device Orientation
 */
enum class Orientation {
    Portrait,
    Landscape
};

/**
 * Layout Mode
 * Different layout strategies for different screen sizes
 */
enum class LayoutMode {
    Mobile,    // Single column, stacked layout
    Tablet,    // Two column, some stacking
    Desktop,   // Multi-column, full features
    Compact    // Condensed desktop for smaller screens
};

/**
 * Responsive Utilities
 * Helper functions for responsive design
 */
class ResponsiveUtils {
public:
    static Breakpoint getCurrentBreakpoint(int width);
    static LayoutMode getLayoutMode(Breakpoint breakpoint);
    static Orientation getOrientation(const QSize& size);
    
    static int getSpacing(Breakpoint breakpoint);
    static int getMargin(Breakpoint breakpoint);
    static int getColumnCount(Breakpoint breakpoint);
    static int getMinimumTouchTarget(Breakpoint breakpoint);
    
    static QSize getOptimalCardSize(Breakpoint breakpoint);
    static int getOptimalFontSize(int baseFontSize, Breakpoint breakpoint);
    
    static bool isMobile(Breakpoint breakpoint);
    static bool isTablet(Breakpoint breakpoint);
    static bool isDesktop(Breakpoint breakpoint);
    
    static QString breakpointToString(Breakpoint breakpoint);
    static QString layoutModeToString(LayoutMode mode);
};

/**
 * Adaptive Layout Manager
 * Manages responsive behavior for widgets and layouts
 */
class AdaptiveLayoutManager : public QObject {
    Q_OBJECT

public:
    explicit AdaptiveLayoutManager(QWidget* target, QObject* parent = nullptr);

    // Breakpoint management
    void setBreakpointCallbacks(const QHash<Breakpoint, std::function<void()>>& callbacks);
    void addBreakpointCallback(Breakpoint breakpoint, std::function<void()> callback);
    
    // Layout mode management
    void setLayoutModeCallbacks(const QHash<LayoutMode, std::function<void()>>& callbacks);
    void addLayoutModeCallback(LayoutMode mode, std::function<void()> callback);
    
    // Current state
    Breakpoint currentBreakpoint() const { return m_currentBreakpoint; }
    LayoutMode currentLayoutMode() const { return m_currentLayoutMode; }
    Orientation currentOrientation() const { return m_currentOrientation; }
    
    // Manual triggers
    void updateLayout();
    void forceBreakpoint(Breakpoint breakpoint);
    void resetBreakpoint();

signals:
    void breakpointChanged(Breakpoint newBreakpoint, Breakpoint oldBreakpoint);
    void layoutModeChanged(LayoutMode newMode, LayoutMode oldMode);
    void orientationChanged(Orientation newOrientation, Orientation oldOrientation);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onResizeTimer();

private:
    void updateBreakpoint(const QSize& size);
    void executeCallbacks();
    
    QWidget* m_targetWidget;
    QTimer* m_resizeTimer;
    
    Breakpoint m_currentBreakpoint = Breakpoint::Large;
    LayoutMode m_currentLayoutMode = LayoutMode::Desktop;
    Orientation m_currentOrientation = Orientation::Landscape;
    
    bool m_forcedBreakpoint = false;
    Breakpoint m_forcedBreakpointValue = Breakpoint::Large;
    
    QHash<Breakpoint, std::function<void()>> m_breakpointCallbacks;
    QHash<LayoutMode, std::function<void()>> m_layoutModeCallbacks;
};

/**
 * Adaptive Grid Layout
 * Grid layout that adapts column count based on screen size
 */
class AdaptiveGridLayout : public QGridLayout {
    Q_OBJECT

public:
    explicit AdaptiveGridLayout(QWidget* parent = nullptr);

    // Column configuration
    void setColumnCounts(const QHash<Breakpoint, int>& columnCounts);
    void setColumnCount(Breakpoint breakpoint, int count);
    
    // Spacing configuration
    void setResponsiveSpacing(bool enabled);
    void setSpacingOverrides(const QHash<Breakpoint, int>& spacingOverrides);
    
    // Item management
    void addResponsiveWidget(QWidget* widget, int priority = 0);
    void removeResponsiveWidget(QWidget* widget);
    
    void setItemBreakpointVisibility(QWidget* widget, Breakpoint minBreakpoint, Breakpoint maxBreakpoint = Breakpoint::XXLarge);

public slots:
    void updateLayout();
    void onBreakpointChanged(Breakpoint newBreakpoint);

private:
    struct ResponsiveItem {
        QWidget* widget;
        int priority;
        Breakpoint minBreakpoint = Breakpoint::XSmall;
        Breakpoint maxBreakpoint = Breakpoint::XXLarge;
        bool visible = true;
    };
    
    void relayoutItems();
    void updateSpacing();
    void updateItemVisibility(Breakpoint breakpoint);
    
    QHash<Breakpoint, int> m_columnCounts;
    QHash<Breakpoint, int> m_spacingOverrides;
    QList<ResponsiveItem> m_responsiveItems;
    
    bool m_responsiveSpacing = true;
    Breakpoint m_currentBreakpoint = Breakpoint::Large;
    
    AdaptiveLayoutManager* m_layoutManager = nullptr;
};

/**
 * Responsive Splitter
 * Splitter that adapts to screen size with collapsible panels
 */
class ResponsiveSplitter : public QSplitter {
    Q_OBJECT

public:
    explicit ResponsiveSplitter(Qt::Orientation orientation, QWidget* parent = nullptr);

    // Panel configuration
    void addResponsiveWidget(QWidget* widget, int stretch = 0, Breakpoint minBreakpoint = Breakpoint::XSmall);
    void setWidgetBreakpointBehavior(QWidget* widget, Breakpoint collapseAt, bool hideCompletely = false);
    
    // Responsive behavior
    void setStackedModeBreakpoint(Breakpoint breakpoint);
    void setAutoCollapse(bool enabled);

public slots:
    void onBreakpointChanged(Breakpoint newBreakpoint);
    void updateResponsiveBehavior();

private:
    struct ResponsivePanel {
        QWidget* widget;
        int stretch;
        Breakpoint minBreakpoint;
        Breakpoint collapseAt = Breakpoint::XSmall;
        bool hideWhenCollapsed = false;
        QList<int> savedSizes;
    };
    
    void updatePanelVisibility(Breakpoint breakpoint);
    void saveSplitterSizes();
    void restoreSplitterSizes();
    void switchToStackedMode();
    void switchToSplitterMode();
    
    QList<ResponsivePanel> m_responsivePanels;
    Breakpoint m_stackedModeBreakpoint = Breakpoint::Medium;
    bool m_autoCollapse = true;
    bool m_isStackedMode = false;
    
    QStackedLayout* m_stackedLayout = nullptr;
    AdaptiveLayoutManager* m_layoutManager = nullptr;
};

/**
 * Responsive Card Container
 * Container that arranges cards responsively based on screen size
 */
class ResponsiveCardContainer : public QScrollArea {
    Q_OBJECT

public:
    explicit ResponsiveCardContainer(QWidget* parent = nullptr);

    // Card management
    void addCard(QWidget* card, int priority = 0);
    void removeCard(QWidget* card);
    void clearCards();
    
    // Layout configuration
    void setCardSpacing(int spacing);
    void setCardMargins(int margins);
    void setMinimumCardWidth(int width);
    void setMaximumCardWidth(int width);
    
    // Responsive behavior
    void setResponsiveCardSizing(bool enabled);
    void setCardSizeOverrides(const QHash<Breakpoint, QSize>& sizeOverrides);

public slots:
    void updateCardLayout();
    void onBreakpointChanged(Breakpoint newBreakpoint);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    struct ResponsiveCard {
        QWidget* card;
        int priority;
        QSize preferredSize;
    };
    
    void relayoutCards();
    int calculateColumnsForWidth(int availableWidth);
    QSize getCardSizeForBreakpoint(Breakpoint breakpoint);
    
    QWidget* m_contentWidget;
    QGridLayout* m_gridLayout;
    
    QList<ResponsiveCard> m_cards;
    QHash<Breakpoint, QSize> m_cardSizeOverrides;
    
    int m_cardSpacing = 16;
    int m_cardMargins = 16;
    int m_minimumCardWidth = 300;
    int m_maximumCardWidth = 400;
    bool m_responsiveCardSizing = true;
    
    AdaptiveLayoutManager* m_layoutManager = nullptr;
};

/**
 * Responsive Navigation
 * Navigation component that adapts between tabs, sidebar, and hamburger menu
 */
class ResponsiveNavigation : public QWidget {
    Q_OBJECT

public:
    enum class NavigationMode {
        Tabs,        // Horizontal tabs (desktop)
        Sidebar,     // Vertical sidebar (tablet)
        Drawer,      // Collapsible drawer (mobile)
        BottomBar    // Bottom navigation (mobile)
    };

    explicit ResponsiveNavigation(QWidget* parent = nullptr);

    // Navigation items
    void addNavigationItem(const QString& id, const QString& text, const QIcon& icon, QWidget* content);
    void removeNavigationItem(const QString& id);
    void setCurrentItem(const QString& id);
    
    // Mode configuration
    void setModeForBreakpoint(Breakpoint breakpoint, NavigationMode mode);
    void setAutoModeSelection(bool enabled);

signals:
    void currentItemChanged(const QString& id);
    void navigationModeChanged(NavigationMode mode);

public slots:
    void onBreakpointChanged(Breakpoint newBreakpoint);
    void toggleDrawer();

private:
    struct NavigationItem {
        QString id;
        QString text;
        QIcon icon;
        QWidget* content;
        QPushButton* button = nullptr;
    };
    
    void setupLayouts();
    void updateNavigationMode(NavigationMode mode);
    void createTabsLayout();
    void createSidebarLayout();
    void createDrawerLayout();
    void createBottomBarLayout();
    
    QList<NavigationItem> m_items;
    QHash<Breakpoint, NavigationMode> m_modeForBreakpoint;
    
    NavigationMode m_currentMode = NavigationMode::Tabs;
    QString m_currentItemId;
    bool m_autoModeSelection = true;
    bool m_drawerOpen = false;
    
    // UI components
    QStackedLayout* m_mainLayout;
    QTabWidget* m_tabWidget = nullptr;
    QSplitter* m_sidebarSplitter = nullptr;
    QWidget* m_drawerWidget = nullptr;
    QWidget* m_bottomBarWidget = nullptr;
    QStackedWidget* m_contentStack = nullptr;
    
    AdaptiveLayoutManager* m_layoutManager = nullptr;
};

} // namespace dinero::responsive
