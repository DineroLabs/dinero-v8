#pragma once

#include <QObject>
#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QThread>
#include <QThreadPool>
#include <QRunnable>
#include <QPixmapCache>
#include <QGraphicsEffect>
#include <QAbstractItemModel>
#include <QSortFilterProxyModel>
#include <functional>
#include <memory>

namespace dinero::performance {

/**
 * Performance Metrics
 * Key performance indicators for GUI responsiveness
 */
struct PerformanceMetrics {
    // Rendering metrics
    double averageFPS = 0.0;
    double frameTime = 0.0;           // milliseconds
    int droppedFrames = 0;
    
    // Memory usage
    qint64 totalMemoryUsage = 0;      // bytes
    qint64 pixmapCacheUsage = 0;      // bytes
    qint64 widgetCount = 0;
    
    // CPU usage
    double cpuUsage = 0.0;            // percentage
    double renderingTime = 0.0;       // milliseconds
    double layoutTime = 0.0;          // milliseconds
    
    // Network/RPC metrics
    int pendingRPCCalls = 0;
    double averageRPCTime = 0.0;      // milliseconds
    int failedRPCCalls = 0;
    
    // User interaction
    double inputLatency = 0.0;        // milliseconds
    int queuedEvents = 0;
    
    // Timestamp
    qint64 timestamp = 0;
};

/**
 * Performance Level
 * Different performance optimization levels
 */
enum class PerformanceLevel {
    Maximum,      // All optimizations enabled, may reduce visual quality
    Balanced,     // Good balance of performance and quality
    Quality,      // Prioritize visual quality over performance
    Auto          // Automatically adjust based on system capabilities
};

/**
 * Performance Manager
 * Central hub for GUI performance monitoring and optimization
 */
class PerformanceManager : public QObject {
    Q_OBJECT

public:
    static PerformanceManager* instance();

    // Core functionality
    void initialize();
    void startMonitoring();
    void stopMonitoring();
    bool isMonitoring() const { return m_monitoring; }
    
    // Performance level
    void setPerformanceLevel(PerformanceLevel level);
    PerformanceLevel currentPerformanceLevel() const { return m_performanceLevel; }
    
    // Metrics
    PerformanceMetrics getCurrentMetrics() const;
    PerformanceMetrics getAverageMetrics(int seconds = 60) const;
    void resetMetrics();
    
    // Widget optimization
    void optimizeWidget(QWidget* widget);
    void optimizeLayout(QWidget* widget);
    void optimizeRendering(QWidget* widget);
    void optimizeMemoryUsage(QWidget* widget);
    
    // Rendering optimization
    void enableHardwareAcceleration(bool enabled);
    void setRenderingHints(QPainter::RenderHints hints);
    void optimizePixmapCache(int cacheLimit = 10240); // KB
    void enableAsyncRendering(bool enabled);
    
    // Memory optimization
    void enableLazyLoading(bool enabled);
    void setWidgetPooling(bool enabled);
    void optimizeImageLoading(bool enabled);
    void enableContentCaching(bool enabled);
    
    // CPU optimization
    void enableBackgroundProcessing(bool enabled);
    void setUpdateThrottling(int intervalMs);
    void optimizeEventProcessing(bool enabled);
    void enableSmartRefresh(bool enabled);
    
    // Automatic optimization
    void enableAutoOptimization(bool enabled);
    void setAutoOptimizationThresholds(double cpuThreshold, qint64 memoryThreshold);
    
    // Profiling
    void startProfiling(const QString& sessionName);
    void stopProfiling();
    void saveProfilingReport(const QString& filename);
    
    // Debug information
    void enableDebugOverlay(bool enabled);
    void setDebugUpdateInterval(int ms);

signals:
    void metricsUpdated(const PerformanceMetrics& metrics);
    void performanceLevelChanged(PerformanceLevel level);
    void lowPerformanceDetected(const PerformanceMetrics& metrics);
    void highMemoryUsageDetected(qint64 usage);

private slots:
    void updateMetrics();
    void checkPerformanceThresholds();
    void onLowPerformanceDetected();
    void onHighMemoryUsage();

private:
    explicit PerformanceManager(QObject* parent = nullptr);
    
    void detectSystemCapabilities();
    void applyPerformanceLevel();
    void collectMetrics();
    void optimizeForLowPerformance();
    void optimizeForHighMemoryUsage();
    
    static PerformanceManager* s_instance;
    
    bool m_monitoring = false;
    PerformanceLevel m_performanceLevel = PerformanceLevel::Balanced;
    
    QTimer* m_metricsTimer;
    QElapsedTimer m_frameTimer;
    
    // Performance settings
    bool m_hardwareAcceleration = true;
    bool m_asyncRendering = false;
    bool m_lazyLoading = true;
    bool m_widgetPooling = false;
    bool m_backgroundProcessing = true;
    bool m_autoOptimization = true;
    
    // Thresholds
    double m_cpuThreshold = 80.0;     // percentage
    qint64 m_memoryThreshold = 512;   // MB
    
    // Metrics storage
    mutable QMutex m_metricsMutex;
    QList<PerformanceMetrics> m_metricsHistory;
    int m_maxHistorySize = 3600; // 1 hour at 1 second intervals
    
    // Profiling
    bool m_profiling = false;
    QString m_profilingSession;
    QElapsedTimer m_profilingTimer;
    
    // Debug overlay
    bool m_debugOverlay = false;
    QWidget* m_debugWidget = nullptr;
};

/**
 * Widget Performance Optimizer
 * Optimizes individual widgets for better performance
 */
class WidgetOptimizer : public QObject {
    Q_OBJECT

public:
    explicit WidgetOptimizer(QWidget* targetWidget, QObject* parent = nullptr);

    // Optimization categories
    void optimizeRendering();
    void optimizeLayout();
    void optimizeMemory();
    void optimizeEvents();
    
    // Specific optimizations
    void enableDoubleBuffering(bool enabled);
    void setUpdateMode(Qt::WidgetAttribute mode);
    void optimizeStyleSheet();
    void enableViewportCaching(bool enabled);
    
    // Lazy loading
    void enableLazyChildren(bool enabled);
    void setVisibilityThreshold(int pixels);
    void enableContentOnDemand(bool enabled);
    
    // Event optimization
    void throttleResizeEvents(int intervalMs);
    void throttlePaintEvents(int intervalMs);
    void batchPropertyUpdates(bool enabled);

private:
    void setupRenderingOptimizations();
    void setupLayoutOptimizations();
    void setupMemoryOptimizations();
    void setupEventOptimizations();
    
    QWidget* m_targetWidget;
    QTimer* m_resizeThrottleTimer;
    QTimer* m_paintThrottleTimer;
    
    bool m_lazyChildren = false;
    int m_visibilityThreshold = 100;
    bool m_contentOnDemand = false;
};

/**
 * Async Task Manager
 * Manages background tasks to keep UI responsive
 */
class AsyncTaskManager : public QObject {
    Q_OBJECT

public:
    static AsyncTaskManager* instance();

    // Task management
    void executeAsync(std::function<void()> task, std::function<void()> callback = nullptr);
    void executeDelayed(std::function<void()> task, int delayMs);
    void executeRepeating(std::function<void()> task, int intervalMs, const QString& id);
    void cancelRepeatingTask(const QString& id);
    
    // Thread pool management
    void setMaxThreads(int maxThreads);
    int maxThreads() const;
    int activeThreads() const;
    int queuedTasks() const;
    
    // Priority tasks
    void executeHighPriority(std::function<void()> task, std::function<void()> callback = nullptr);
    void executeLowPriority(std::function<void()> task, std::function<void()> callback = nullptr);

signals:
    void taskCompleted(const QString& taskId);
    void taskFailed(const QString& taskId, const QString& error);

private:
    explicit AsyncTaskManager(QObject* parent = nullptr);
    
    static AsyncTaskManager* s_instance;
    QThreadPool* m_threadPool;
    QHash<QString, QTimer*> m_repeatingTasks;
    int m_taskCounter = 0;
};

/**
 * Memory Pool Manager
 * Manages object pools for frequently created/destroyed objects
 */
template<typename T>
class ObjectPool {
public:
    explicit ObjectPool(int initialSize = 10, int maxSize = 100)
        : m_maxSize(maxSize) {
        for (int i = 0; i < initialSize; ++i) {
            m_pool.append(std::make_unique<T>());
        }
    }

    std::unique_ptr<T> acquire() {
        QMutexLocker locker(&m_mutex);
        if (!m_pool.isEmpty()) {
            return std::move(m_pool.takeLast());
        }
        return std::make_unique<T>();
    }

    void release(std::unique_ptr<T> object) {
        if (!object) return;
        
        QMutexLocker locker(&m_mutex);
        if (m_pool.size() < m_maxSize) {
            // Reset object to clean state
            object.reset();
            m_pool.append(std::move(object));
        }
        // Otherwise, let it be destroyed
    }

    int poolSize() const {
        QMutexLocker locker(&m_mutex);
        return m_pool.size();
    }

private:
    mutable QMutex m_mutex;
    QList<std::unique_ptr<T>> m_pool;
    int m_maxSize;
};

/**
 * Render Cache Manager
 * Caches rendered content to avoid expensive redraws
 */
class RenderCacheManager : public QObject {
    Q_OBJECT

public:
    static RenderCacheManager* instance();

    // Cache management
    void cacheWidget(QWidget* widget, const QString& key);
    QPixmap getCachedPixmap(const QString& key);
    bool hasCachedPixmap(const QString& key);
    void invalidateCache(const QString& key);
    void clearCache();
    
    // Cache policies
    void setCacheLimit(int limitKB);
    void setMaxCacheAge(int seconds);
    void enableSmartCaching(bool enabled);
    
    // Statistics
    int cacheHitCount() const { return m_cacheHits; }
    int cacheMissCount() const { return m_cacheMisses; }
    double cacheHitRatio() const;
    qint64 cacheMemoryUsage() const;

private:
    explicit RenderCacheManager(QObject* parent = nullptr);
    
    void cleanupExpiredEntries();
    
    static RenderCacheManager* s_instance;
    
    struct CacheEntry {
        QPixmap pixmap;
        qint64 timestamp;
        int accessCount;
    };
    
    QHash<QString, CacheEntry> m_cache;
    QTimer* m_cleanupTimer;
    
    int m_cacheLimit = 10240; // KB
    int m_maxCacheAge = 300;   // seconds
    bool m_smartCaching = true;
    
    int m_cacheHits = 0;
    int m_cacheMisses = 0;
};

/**
 * Performance Profiler
 * Detailed performance profiling for optimization
 */
class PerformanceProfiler : public QObject {
    Q_OBJECT

public:
    struct ProfileData {
        QString name;
        qint64 startTime;
        qint64 endTime;
        qint64 duration;
        int callCount;
        QString category;
    };

    explicit PerformanceProfiler(QObject* parent = nullptr);

    // Profiling control
    void startProfiling();
    void stopProfiling();
    bool isProfiling() const { return m_profiling; }
    
    // Measurement
    void startMeasurement(const QString& name, const QString& category = "General");
    void endMeasurement(const QString& name);
    void measureFunction(const QString& name, std::function<void()> func, const QString& category = "General");
    
    // Results
    QList<ProfileData> getProfileData() const;
    void clearProfileData();
    void saveReport(const QString& filename);
    
    // Statistics
    ProfileData getAverageData(const QString& name) const;
    QStringList getSlowFunctions(int thresholdMs = 10) const;

private:
    bool m_profiling = false;
    QHash<QString, qint64> m_activeMeasurements;
    QList<ProfileData> m_profileData;
    mutable QMutex m_dataMutex;
};

/**
 * Smart Update Manager
 * Intelligently batches and throttles UI updates
 */
class SmartUpdateManager : public QObject {
    Q_OBJECT

public:
    static SmartUpdateManager* instance();

    // Update scheduling
    void scheduleUpdate(QWidget* widget, const QString& property, const QVariant& value);
    void scheduleLayoutUpdate(QWidget* widget);
    void scheduleRepaint(QWidget* widget, const QRect& rect = QRect());
    
    // Batching control
    void setBatchingEnabled(bool enabled);
    void setBatchInterval(int ms);
    void flushUpdates();
    
    // Throttling
    void setThrottlingEnabled(bool enabled);
    void setMaxUpdatesPerSecond(int maxUpdates);

private slots:
    void processBatchedUpdates();

private:
    explicit SmartUpdateManager(QObject* parent = nullptr);
    
    static SmartUpdateManager* s_instance;
    
    struct UpdateRequest {
        QWidget* widget;
        QString property;
        QVariant value;
        qint64 timestamp;
    };
    
    QList<UpdateRequest> m_pendingUpdates;
    QTimer* m_batchTimer;
    
    bool m_batchingEnabled = true;
    int m_batchInterval = 16; // ~60 FPS
    bool m_throttlingEnabled = true;
    int m_maxUpdatesPerSecond = 60;
};

} // namespace dinero::performance

// Performance profiling macros
#define DINERO_PERF_START(name) dinero::performance::PerformanceManager::instance()->startMeasurement(name)
#define DINERO_PERF_END(name) dinero::performance::PerformanceManager::instance()->endMeasurement(name)
#define DINERO_PERF_FUNCTION(name, func) dinero::performance::PerformanceManager::instance()->measureFunction(name, func)

// Async execution macros
#define DINERO_ASYNC(task) dinero::performance::AsyncTaskManager::instance()->executeAsync(task)
#define DINERO_ASYNC_CALLBACK(task, callback) dinero::performance::AsyncTaskManager::instance()->executeAsync(task, callback)
