// Responsive GUI Demo - Shows adaptive layouts in action
// Demonstrates how Dinero Desktop adapts to different screen sizes

#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QGroupBox>
#include <QTabWidget>
#include <QSplitter>
#include <QScrollArea>
#include <QTimer>
#include <QResizeEvent>
#include <QDebug>

// Mock responsive classes for demo
namespace dinero::responsive {

enum class Breakpoint { XSmall, Small, Medium, Large, XLarge, XXLarge };

class ResponsiveUtils {
public:
    static Breakpoint getCurrentBreakpoint(int width) {
        if (width < 576) return Breakpoint::XSmall;
        if (width < 768) return Breakpoint::Small;
        if (width < 992) return Breakpoint::Medium;
        if (width < 1200) return Breakpoint::Large;
        if (width < 1400) return Breakpoint::XLarge;
        return Breakpoint::XXLarge;
    }
    
    static QString breakpointName(Breakpoint bp) {
        switch (bp) {
            case Breakpoint::XSmall: return "XSmall (<576px)";
            case Breakpoint::Small: return "Small (576-767px)";
            case Breakpoint::Medium: return "Medium (768-991px)";
            case Breakpoint::Large: return "Large (992-1199px)";
            case Breakpoint::XLarge: return "XLarge (1200-1399px)";
            case Breakpoint::XXLarge: return "XXLarge (≥1400px)";
        }
        return "Unknown";
    }
    
    static int getColumnCount(Breakpoint bp) {
        switch (bp) {
            case Breakpoint::XSmall: return 1;
            case Breakpoint::Small: return 1;
            case Breakpoint::Medium: return 2;
            case Breakpoint::Large: return 3;
            case Breakpoint::XLarge: return 4;
            case Breakpoint::XXLarge: return 4;
        }
        return 3;
    }
    
    static int getSpacing(Breakpoint bp) {
        switch (bp) {
            case Breakpoint::XSmall: return 8;
            case Breakpoint::Small: return 12;
            case Breakpoint::Medium: return 16;
            case Breakpoint::Large: return 20;
            case Breakpoint::XLarge: return 24;
            case Breakpoint::XXLarge: return 24;
        }
        return 16;
    }
};

} // namespace

class ResponsiveDashboard : public QMainWindow {
    Q_OBJECT

public:
    ResponsiveDashboard(QWidget* parent = nullptr) : QMainWindow(parent) {
        setupUI();
        setupTimer();
        updateLayout();
        
        setWindowTitle("Dinero Desktop - Responsive Layout Demo");
        resize(1200, 800);
    }

private slots:
    void updateLayout() {
        int width = centralWidget()->width();
        auto breakpoint = dinero::responsive::ResponsiveUtils::getCurrentBreakpoint(width);
        
        // Update breakpoint indicator
        m_breakpointLabel->setText(QString("Current: %1 (%2px)")
            .arg(dinero::responsive::ResponsiveUtils::breakpointName(breakpoint))
            .arg(width));
        
        // Update card layout
        updateCardLayout(breakpoint);
        
        // Update navigation layout
        updateNavigationLayout(breakpoint);
        
        // Update sidebar visibility
        updateSidebarLayout(breakpoint);
    }
    
    void onWidthSliderChanged(int value) {
        resize(value, height());
        updateLayout();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QMainWindow::resizeEvent(event);
        // Debounce resize events
        m_resizeTimer->start();
    }

private:
    void setupUI() {
        auto central = new QWidget(this);
        setCentralWidget(central);
        
        auto layout = new QVBoxLayout(central);
        
        // Responsive controls
        auto controlsGroup = createControlsGroup();
        layout->addWidget(controlsGroup);
        
        // Main content area
        m_mainSplitter = new QSplitter(Qt::Horizontal);
        
        // Sidebar
        m_sidebar = createSidebar();
        m_mainSplitter->addWidget(m_sidebar);
        
        // Content area
        auto contentWidget = new QWidget();
        auto contentLayout = new QVBoxLayout(contentWidget);
        
        // Navigation tabs
        m_tabWidget = new QTabWidget();
        m_tabWidget->addTab(createDashboardTab(), "Dashboard");
        m_tabWidget->addTab(createBlocksTab(), "Blocks");
        m_tabWidget->addTab(createWalletTab(), "Wallet");
        contentLayout->addWidget(m_tabWidget);
        
        m_mainSplitter->addWidget(contentWidget);
        m_mainSplitter->setStretchFactor(1, 1);
        
        layout->addWidget(m_mainSplitter);
        
        // Status bar
        statusBar()->addWidget(new QLabel("Responsive Demo Ready"));
    }
    
    QGroupBox* createControlsGroup() {
        auto group = new QGroupBox("Responsive Controls");
        auto layout = new QHBoxLayout(group);
        
        // Breakpoint indicator
        m_breakpointLabel = new QLabel("Current: Large (1200px)");
        m_breakpointLabel->setStyleSheet("font-weight: bold; color: #0066cc;");
        
        // Width slider
        auto widthLabel = new QLabel("Width:");
        m_widthSlider = new QSlider(Qt::Horizontal);
        m_widthSlider->setRange(320, 1600);
        m_widthSlider->setValue(1200);
        connect(m_widthSlider, &QSlider::valueChanged, this, &ResponsiveDashboard::onWidthSliderChanged);
        
        layout->addWidget(m_breakpointLabel);
        layout->addStretch();
        layout->addWidget(widthLabel);
        layout->addWidget(m_widthSlider);
        
        return group;
    }
    
    QWidget* createSidebar() {
        auto sidebar = new QWidget();
        sidebar->setFixedWidth(250);
        sidebar->setStyleSheet("background-color: #f8f9fa; border-right: 1px solid #dee2e6;");
        
        auto layout = new QVBoxLayout(sidebar);
        layout->setContentsMargins(16, 16, 16, 16);
        
        // Logo area
        auto logoLabel = new QLabel("DINERO");
        logoLabel->setStyleSheet("font-size: 24px; font-weight: bold; color: #ffd700; text-align: center;");
        logoLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(logoLabel);
        
        layout->addSpacing(20);
        
        // Navigation items
        auto navItems = QStringList{"Dashboard", "Blocks", "Transactions", "Wallet", "Mining", "Settings"};
        for (const auto& item : navItems) {
            auto button = new QPushButton(item);
            button->setStyleSheet(R"(
                QPushButton {
                    background: transparent;
                    border: none;
                    padding: 12px 16px;
                    text-align: left;
                    border-radius: 6px;
                }
                QPushButton:hover {
                    background-color: #e9ecef;
                }
            )");
            layout->addWidget(button);
        }
        
        layout->addStretch();
        return sidebar;
    }
    
    QWidget* createDashboardTab() {
        auto widget = new QWidget();
        
        // Create scrollable area for cards
        auto scrollArea = new QScrollArea();
        scrollArea->setWidgetResizable(true);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        
        m_cardContainer = new QWidget();
        m_cardLayout = new QGridLayout(m_cardContainer);
        
        // Create demo cards
        createDemoCards();
        
        scrollArea->setWidget(m_cardContainer);
        
        auto layout = new QVBoxLayout(widget);
        layout->addWidget(scrollArea);
        
        return widget;
    }
    
    QWidget* createBlocksTab() {
        auto widget = new QWidget();
        auto layout = new QVBoxLayout(widget);
        
        auto label = new QLabel("Recent Blocks");
        label->setStyleSheet("font-size: 18px; font-weight: 600; margin-bottom: 16px;");
        layout->addWidget(label);
        
        // Mock block list
        for (int i = 0; i < 10; i++) {
            auto blockWidget = createBlockItem(QString("Block #%1").arg(1000 - i), 
                                             QString("hash_%1abcdef...").arg(i));
            layout->addWidget(blockWidget);
        }
        
        layout->addStretch();
        return widget;
    }
    
    QWidget* createWalletTab() {
        auto widget = new QWidget();
        auto layout = new QVBoxLayout(widget);
        
        auto label = new QLabel("Wallet Overview");
        label->setStyleSheet("font-size: 18px; font-weight: 600; margin-bottom: 16px;");
        layout->addWidget(label);
        
        // Mock wallet info
        auto balanceCard = createInfoCard("Balance", "1,234.56 DIN", "#28a745");
        auto addressCard = createInfoCard("Address", "din1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh", "#0066cc");
        
        layout->addWidget(balanceCard);
        layout->addWidget(addressCard);
        layout->addStretch();
        
        return widget;
    }
    
    void createDemoCards() {
        // Clear existing cards
        while (m_cardLayout->count()) {
            auto item = m_cardLayout->takeAt(0);
            delete item->widget();
            delete item;
        }
        
        // Create cards with different content
        m_demoCards = {
            createInfoCard("Network Status", "REGTEST - Connected", "#ffc107"),
            createInfoCard("Block Height", "1,337", "#0066cc"),
            createInfoCard("Best Hash", "00000abc...def123", "#6c757d"),
            createInfoCard("Difficulty", "1.00000000", "#17a2b8"),
            createInfoCard("Chainwork", "000000000...001", "#28a745"),
            createInfoCard("Mempool", "0 transactions", "#ffc107"),
            createInfoCard("Uptime", "2h 15m 30s", "#6c757d"),
            createInfoCard("Peers", "0 connected", "#dc3545")
        };
    }
    
    QWidget* createInfoCard(const QString& title, const QString& value, const QString& color) {
        auto card = new QWidget();
        card->setFixedHeight(120);
        card->setStyleSheet(QString(R"(
            QWidget {
                background-color: white;
                border: 1px solid #dee2e6;
                border-radius: 8px;
                padding: 16px;
            }
        )"));
        
        auto layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 16, 16, 16);
        
        auto titleLabel = new QLabel(title);
        titleLabel->setStyleSheet("font-size: 12px; color: #6c757d; font-weight: 500;");
        
        auto valueLabel = new QLabel(value);
        valueLabel->setStyleSheet(QString("font-size: 18px; font-weight: 600; color: %1;").arg(color));
        valueLabel->setWordWrap(true);
        
        layout->addWidget(titleLabel);
        layout->addWidget(valueLabel);
        layout->addStretch();
        
        return card;
    }
    
    QWidget* createBlockItem(const QString& height, const QString& hash) {
        auto item = new QWidget();
        item->setStyleSheet(R"(
            QWidget {
                background-color: white;
                border: 1px solid #dee2e6;
                border-radius: 6px;
                padding: 12px;
                margin: 2px 0;
            }
            QWidget:hover {
                background-color: #f8f9fa;
            }
        )");
        
        auto layout = new QHBoxLayout(item);
        
        auto heightLabel = new QLabel(height);
        heightLabel->setStyleSheet("font-weight: 600; color: #212529;");
        
        auto hashLabel = new QLabel(hash);
        hashLabel->setStyleSheet("font-family: monospace; color: #6c757d; font-size: 12px;");
        
        layout->addWidget(heightLabel);
        layout->addWidget(hashLabel);
        layout->addStretch();
        
        return item;
    }
    
    void updateCardLayout(dinero::responsive::Breakpoint breakpoint) {
        int columns = dinero::responsive::ResponsiveUtils::getColumnCount(breakpoint);
        int spacing = dinero::responsive::ResponsiveUtils::getSpacing(breakpoint);
        
        m_cardLayout->setSpacing(spacing);
        
        // Relayout cards in grid
        for (int i = 0; i < m_demoCards.size(); i++) {
            int row = i / columns;
            int col = i % columns;
            m_cardLayout->addWidget(m_demoCards[i], row, col);
        }
    }
    
    void updateNavigationLayout(dinero::responsive::Breakpoint breakpoint) {
        // On small screens, consider switching to bottom navigation or drawer
        bool showTabs = (breakpoint >= dinero::responsive::Breakpoint::Medium);
        
        if (!showTabs) {
            // Could implement drawer navigation here
            m_tabWidget->setTabPosition(QTabWidget::South);
        } else {
            m_tabWidget->setTabPosition(QTabWidget::North);
        }
    }
    
    void updateSidebarLayout(dinero::responsive::Breakpoint breakpoint) {
        bool showSidebar = (breakpoint >= dinero::responsive::Breakpoint::Large);
        m_sidebar->setVisible(showSidebar);
        
        if (!showSidebar) {
            // Could implement hamburger menu here
            statusBar()->showMessage("Sidebar hidden on smaller screens");
        } else {
            statusBar()->clearMessage();
        }
    }
    
    void setupTimer() {
        m_resizeTimer = new QTimer(this);
        m_resizeTimer->setSingleShot(true);
        m_resizeTimer->setInterval(100); // 100ms debounce
        connect(m_resizeTimer, &QTimer::timeout, this, &ResponsiveDashboard::updateLayout);
    }
    
    // UI components
    QLabel* m_breakpointLabel;
    QSlider* m_widthSlider;
    QSplitter* m_mainSplitter;
    QWidget* m_sidebar;
    QTabWidget* m_tabWidget;
    QWidget* m_cardContainer;
    QGridLayout* m_cardLayout;
    QTimer* m_resizeTimer;
    
    QList<QWidget*> m_demoCards;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    // Set application properties
    app.setApplicationName("Dinero Desktop");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Dinero");
    
    ResponsiveDashboard dashboard;
    dashboard.show();
    
    return app.exec();
}

#include "responsive_gui_demo.moc"
