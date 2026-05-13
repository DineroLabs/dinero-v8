#pragma once

#include <QObject>
#include <QFuture>
#include <QFutureWatcher>
#include <QTimer>
#include <QThread>
#include <QDateTime>
// #include <QtConcurrent/QtConcurrent> // Temporarily disabled
#include <functional>
#include <memory>
#include <unordered_map>

namespace dinero {
namespace gui {

/**
 * Background job manager for GUI applications
 * Provides QtConcurrent + signals for responsive UI during DB/RPC operations
 */
class BackgroundJobs : public QObject {
    Q_OBJECT

public:
    explicit BackgroundJobs(QObject* parent = nullptr);
    ~BackgroundJobs();

    // Job execution with progress tracking
    template<typename T>
    void executeJob(const QString& jobName, std::function<T()> job, 
                   std::function<void(const T&)> onSuccess,
                   std::function<void(const QString&)> onError = nullptr);

    // Simple void job execution
    void executeVoidJob(const QString& jobName, std::function<void()> job,
                       std::function<void()> onSuccess = nullptr,
                       std::function<void(const QString&)> onError = nullptr);

    // Void job with custom timeout
    void executeVoidJob(const QString& jobName, std::function<void()> job,
                       std::function<void()> onSuccess,
                       std::function<void(const QString&)> onError,
                       int timeoutMs);

    // Job status management
    bool isJobRunning(const QString& jobName) const;
    void cancelJob(const QString& jobName);
    void cancelAllJobs();
    
    // Progress tracking
    int getActiveJobCount() const { return m_activeJobs.size(); }
    QStringList getActiveJobNames() const;

signals:
    void jobStarted(const QString& jobName);
    void jobCompleted(const QString& jobName);
    void jobFailed(const QString& jobName, const QString& error);
    void allJobsCompleted();

private slots:
    void onJobFinished();
    void cleanupCompletedJobs();

private:
    struct JobInfo {
        QString name;
        QFutureWatcher<void>* watcher;
        QTimer* timeoutTimer;
        QDateTime startTime;
    };

    void cleanupJob(const QString& jobName);
    void startJobTimeout(const QString& jobName);
    void startJobTimeoutWithTimeout(const QString& jobName, int timeoutMs);

    std::unordered_map<QString, std::unique_ptr<JobInfo>> m_activeJobs;
    QTimer* m_cleanupTimer{nullptr};
    
    static constexpr int JOB_TIMEOUT_MS = 30000; // 30 seconds
    static constexpr int CLEANUP_INTERVAL_MS = 5000; // 5 seconds

public:
    static constexpr int LONG_OP_TIMEOUT_MS = 120000; // 2 minutes for heavy ops
};

// Template implementation
template<typename T>
void BackgroundJobs::executeJob(const QString& jobName, std::function<T()> job,
                               std::function<void(const T&)> onSuccess,
                               std::function<void(const QString&)> onError) {
    // Cancel existing job with same name
    if (isJobRunning(jobName)) {
        cancelJob(jobName);
    }
    
    // Create job info
    auto jobInfo = std::make_unique<JobInfo>();
    jobInfo->name = jobName;
    jobInfo->startTime = QDateTime::currentDateTime();
    
    // Create future watcher
    auto* watcher = new QFutureWatcher<T>(this);
    jobInfo->watcher = reinterpret_cast<QFutureWatcher<void>*>(watcher);
    
    // Connect signals
    connect(watcher, &QFutureWatcher<T>::finished, this, [this, jobName, onSuccess, onError, watcher]() {
        try {
            T result = watcher->result();
            if (onSuccess) {
                onSuccess(result);
            }
            emit jobCompleted(jobName);
        } catch (const std::exception& e) {
            QString error = QString("Job failed: %1").arg(e.what());
            if (onError) {
                onError(error);
            }
            emit jobFailed(jobName, error);
        }
        cleanupJob(jobName);
    });
    
    // Store job info
    m_activeJobs[jobName] = std::move(jobInfo);
    
    // Start timeout timer
    startJobTimeout(jobName);
    
    // Execute job
    // QFuture<T> future = QtConcurrent::run(job); // Temporarily disabled
    // watcher->setFuture(future); // Temporarily disabled
    
    emit jobStarted(jobName);
}

} // namespace gui
} // namespace dinero
