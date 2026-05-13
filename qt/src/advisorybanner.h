#ifndef ADVISORYBANNER_H
#define ADVISORYBANNER_H

#include <QObject>
#include <QDateTime>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSet>
#include <vector>
#include "transactiontracker.h"

class QWidget;

// ── Advisory event (persisted) ─────────────────────────────────────

struct AdvisoryEvent {
    QString eventId;         // UUID
    QString lineageKey;      // reservationId (send-intent lineage)
    QString txid;
    TxStatus type;
    QString message;
    QDateTime createdAt;
};

// ── Banner queue with persistence and popup display ────────────────

class AdvisoryBannerQueue : public QObject {
    Q_OBJECT
public:
    explicit AdvisoryBannerQueue(const QString& dataDir,
                                  QWidget* parentWidget,
                                  QObject* parent = nullptr);

    void setDataDir(const QString& dataDir);
    void setWalletScope(const QString& walletScope);
    void setAutoDisplayEnabled(bool enabled) { autoDisplayEnabled_ = enabled; }
    int pendingEventCount() const { return static_cast<int>(pendingEvents_.size()); }

    /// Enqueue a terminal event. Deduplicates by lineage key.
    void enqueue(const TerminalAdvisoryEvent& event);

    /// Replay persisted events on startup (idempotent).
    void replayOnStartup();

private:
    void showNext();
    void loadEvents();
    bool loadEventsFromPath(const QString& path);
    void saveEvents();
    void loadAcknowledged();
    bool loadAcknowledgedFromPath(const QString& path);
    void saveAcknowledged();
    QString eventsFilePath() const;
    QString acknowledgedFilePath() const;
    QString legacyEventsFilePath() const;
    QString legacyAcknowledgedFilePath() const;
    void resetState();
    static QString bannerTitle(TxStatus type);
    static QString bannerMessage(TxStatus type);

    QString dataDir_;
    QString walletScope_;
    QWidget* parentWidget_;

    std::vector<AdvisoryEvent> pendingEvents_;
    QSet<QString> acknowledgedEventIds_;
    QSet<QString> activeLineageKeys_;

    bool showing_ = false;  // guard against re-entrant popups
    bool autoDisplayEnabled_ = true;
};

#endif // ADVISORYBANNER_H
