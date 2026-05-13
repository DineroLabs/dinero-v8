#pragma once

#include <QObject>
#include <QWidget>
#include <QTimer>
#include <QLabel>
#include <QVBoxLayout>
#include <QSet>
#include <QHash>
#include <QDateTime>
#include "gui-desktop/performance/performance_manager.h"

// Forward declarations
class MainWindow;
class StatusTab;
class WalletTab;

namespace dinero::performance {

/**
 * GUI Performance Integration
 * Integrates performance monitoring and optimization with existing GUI components
 */
class GuiPerformanceIntegration : public QObject {
    Q_OBJECT

public:
    struct TabPerformanceData {
        QString tabName;
        QDateTime lastUpdateTime;
        qint64 memoryUsage;
        double cpuUsage;
    };

    explicit GuiPerformanceIntegration(QObject* parent = nullptr);

    // Initialization
    void initializeForApplication();
    
    // Widget optimization
    void optimizeMainWindow(MainWindow* mainWindow);
    void optimizeTabWidget(QWidget* tabWidget, const QString& tabName);
    void optimizeStatusTab(StatusTab* statusTab);
    void optimizeWalletTab(WalletTab* walletTab);
    
    // Performance monitoring
    void enableDebugOverlay(bool enabled);
    PerformanceMetrics getCurrentMetrics() const;
    void setPerformanceLevel(PerformanceLevel level);

signals:
    void performanceMetricsUpdated(const PerformanceMetrics& metrics);
    void lowPerformanceDetected(const PerformanceMetrics& metrics);
    void highMemoryUsageDetected(qint64 usage);

private slots:
    void updatePerformanceMetrics();
    void handleLowPerformance(const PerformanceMetrics& metrics);
    void handleHighMemoryUsage(qint64 usage);
    void updatePerformanceOverlay(const PerformanceMetrics& metrics);

private:
    void createPerformanceOverlay();
    void destroyPerformanceOverlay();
    
    PerformanceManager* m_performanceManager;
    QTimer* m_monitoringTimer;
    
    // Optimized widgets tracking
    QSet<QWidget*> m_optimizedWidgets;
    QHash<QWidget*, TabPerformanceData> m_tabPerformanceData;
    
    // Debug overlay
    QWidget* m_performanceOverlay = nullptr;
    QLabel* m_fpsLabel = nullptr;
    QLabel* m_memoryLabel = nullptr;
    QLabel* m_cpuLabel = nullptr;
};

} // namespace dinero::performance
