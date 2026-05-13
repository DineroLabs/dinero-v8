#pragma once
#include <QLockFile>
#include <QString>
#include <QObject>
#include <memory>

/**
 * @brief Single-instance application lock
 * 
 * Prevents multiple GUI instances from running simultaneously
 * and conflicting with each other.
 */
class AppLock : public QObject {
    Q_OBJECT
    
public:
    explicit AppLock(const QString& network, QObject* parent = nullptr);
    ~AppLock();
    
    /**
     * @brief Try to acquire the lock
     * @return true if lock acquired, false if another instance is running
     */
    bool tryLock();
    
    /**
     * @brief Release the lock (called automatically in destructor)
     */
    void unlock();
    
    /**
     * @brief Get lock file path for debugging
     */
    QString lockFilePath() const;
    
    /**
     * @brief Check if we currently hold the lock
     */
    bool isLocked() const;
    
private:
    std::unique_ptr<QLockFile> m_lockFile;
    QString m_network;
    bool m_locked{false};
};
