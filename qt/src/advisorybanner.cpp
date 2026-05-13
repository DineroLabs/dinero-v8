#include "advisorybanner.h"
#include <QMessageBox>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>

// ── AdvisoryBannerQueue ────────────────────────────────────────────

namespace {

QString walletScopeFileComponent(const QString& walletScope) {
    QString scope = walletScope.trimmed();
    if (scope.isEmpty()) {
        return QString();
    }

    scope.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    return scope;
}

} // namespace

AdvisoryBannerQueue::AdvisoryBannerQueue(const QString& dataDir,
                                           QWidget* parentWidget,
                                           QObject* parent)
    : QObject(parent), dataDir_(dataDir), parentWidget_(parentWidget)
{
}

void AdvisoryBannerQueue::setDataDir(const QString& dataDir) {
    dataDir_ = dataDir;
    resetState();
}

void AdvisoryBannerQueue::setWalletScope(const QString& walletScope) {
    walletScope_ = walletScope.trimmed();
    resetState();
}

void AdvisoryBannerQueue::resetState() {
    pendingEvents_.clear();
    acknowledgedEventIds_.clear();
    activeLineageKeys_.clear();
    showing_ = false;

    if (dataDir_.isEmpty() || walletScope_.isEmpty()) {
        return;
    }

    loadEvents();
    loadAcknowledged();

    if (pendingEvents_.empty() && acknowledgedEventIds_.empty()) {
        const QString legacyEvents = legacyEventsFilePath();
        const QString legacyAck = legacyAcknowledgedFilePath();
        if (QFile::exists(legacyEvents) || QFile::exists(legacyAck)) {
            loadEventsFromPath(legacyEvents);
            loadAcknowledgedFromPath(legacyAck);
            saveEvents();
            saveAcknowledged();
            QFile::remove(legacyEvents);
            QFile::remove(legacyAck);
        }
    }

    activeLineageKeys_.clear();
    for (const auto& event : pendingEvents_) {
        if (!event.lineageKey.isEmpty()) {
            activeLineageKeys_.insert(event.lineageKey);
        }
    }
}

QString AdvisoryBannerQueue::eventsFilePath() const {
    const QString scopedComponent = walletScopeFileComponent(walletScope_);
    if (dataDir_.isEmpty() || scopedComponent.isEmpty()) {
        return QString();
    }
    return dataDir_ + QDir::separator() +
           QStringLiteral("advisory_events_%1.json").arg(scopedComponent);
}

QString AdvisoryBannerQueue::acknowledgedFilePath() const {
    const QString scopedComponent = walletScopeFileComponent(walletScope_);
    if (dataDir_.isEmpty() || scopedComponent.isEmpty()) {
        return QString();
    }
    return dataDir_ + QDir::separator() +
           QStringLiteral("advisory_acknowledged_%1.json").arg(scopedComponent);
}

QString AdvisoryBannerQueue::legacyEventsFilePath() const {
    if (dataDir_.isEmpty()) {
        return QString();
    }
    return dataDir_ + QDir::separator() + QStringLiteral("advisory_events.json");
}

QString AdvisoryBannerQueue::legacyAcknowledgedFilePath() const {
    if (dataDir_.isEmpty()) {
        return QString();
    }
    return dataDir_ + QDir::separator() + QStringLiteral("advisory_acknowledged.json");
}

QString AdvisoryBannerQueue::bannerTitle(TxStatus type) {
    switch (type) {
    case TxStatus::Replaced: return "Send Replaced";
    case TxStatus::Evicted:  return "Send Dropped";
    case TxStatus::Failed:   return "Send Rejected";
    default:                 return "Transaction Alert";
    }
}

QString AdvisoryBannerQueue::bannerMessage(TxStatus type) {
    switch (type) {
    case TxStatus::Replaced:
        return "A higher-fee replacement is now pending.\n\n"
               "Your original transaction was replaced in the mempool.";
    case TxStatus::Evicted:
        return "This send is no longer present in the mempool.\n\n"
               "The transaction may have been dropped due to low fees or mempool pressure.";
    case TxStatus::Failed:
        return "The node rejected this send.\n\n"
               "Check the transaction details and try again.";
    default:
        return "Transaction status changed.";
    }
}

void AdvisoryBannerQueue::enqueue(const TerminalAdvisoryEvent& event) {
    if (walletScope_.isEmpty()) {
        return;
    }

    QString lineageKey = event.reservationId.isEmpty() ? event.txid : event.reservationId;

    // Deduplicate by lineage
    if (activeLineageKeys_.contains(lineageKey)) return;

    QString eventId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // Check if we've already queued a banner for this lineage
    for (const auto& existing : pendingEvents_) {
        if (existing.lineageKey == lineageKey) return;
    }

    AdvisoryEvent ae;
    ae.eventId = eventId;
    ae.lineageKey = lineageKey;
    ae.txid = event.txid;
    ae.type = event.status;
    ae.message = bannerMessage(event.status);
    ae.createdAt = QDateTime::currentDateTimeUtc();

    pendingEvents_.push_back(ae);
    activeLineageKeys_.insert(lineageKey);
    saveEvents();
    if (autoDisplayEnabled_) {
        showNext();
    }
}

void AdvisoryBannerQueue::replayOnStartup() {
    resetState();

    // Remove already-acknowledged events
    pendingEvents_.erase(
        std::remove_if(pendingEvents_.begin(), pendingEvents_.end(),
            [this](const AdvisoryEvent& e) {
                return acknowledgedEventIds_.contains(e.eventId);
            }),
        pendingEvents_.end());

    // Rebuild active lineage keys
    activeLineageKeys_.clear();
    for (const auto& e : pendingEvents_)
        activeLineageKeys_.insert(e.lineageKey);

    saveEvents();
    if (autoDisplayEnabled_) {
        showNext();
    }
}

void AdvisoryBannerQueue::showNext() {
    if (showing_ || !autoDisplayEnabled_) return;  // prevent re-entrant popups

    while (!pendingEvents_.empty()) {
        // Find first unacknowledged event
        auto it = std::find_if(pendingEvents_.begin(), pendingEvents_.end(),
            [this](const AdvisoryEvent& e) {
                return !acknowledgedEventIds_.contains(e.eventId);
            });
        if (it == pendingEvents_.end()) break;

        showing_ = true;
        AdvisoryEvent event = *it;

        QMessageBox::Icon icon = QMessageBox::Warning;
        if (event.type == TxStatus::Failed) icon = QMessageBox::Critical;

        QMessageBox msgBox(parentWidget_);
        msgBox.setIcon(icon);
        msgBox.setWindowTitle(bannerTitle(event.type));
        msgBox.setText(event.message);
        if (!event.txid.isEmpty()) {
            msgBox.setInformativeText("Transaction: " + event.txid.left(16) + "...");
        }
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.exec();

        // After user closes the popup, mark as acknowledged
        acknowledgedEventIds_.insert(event.eventId);
        saveAcknowledged();

        // Remove from pending and free the lineage key
        QString lineageKey = event.lineageKey;
        pendingEvents_.erase(
            std::remove_if(pendingEvents_.begin(), pendingEvents_.end(),
                [&event](const AdvisoryEvent& e) { return e.eventId == event.eventId; }),
            pendingEvents_.end());
        if (!lineageKey.isEmpty())
            activeLineageKeys_.remove(lineageKey);
        saveEvents();

        showing_ = false;
    }
}

// ── Persistence ────────────────────────────────────────────────────

void AdvisoryBannerQueue::loadEvents() {
    const QString path = eventsFilePath();
    if (path.isEmpty()) {
        pendingEvents_.clear();
        return;
    }

    loadEventsFromPath(path);
}

bool AdvisoryBannerQueue::loadEventsFromPath(const QString& path) {
    pendingEvents_.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isArray()) return false;

    for (const auto& val : doc.array()) {
        QJsonObject o = val.toObject();
        AdvisoryEvent e;
        e.eventId    = o["event_id"].toString();
        e.lineageKey = o["lineage_key"].toString();
        e.txid       = o["txid"].toString();
        e.type       = static_cast<TxStatus>(o["type"].toInt());
        e.message    = o["message"].toString();
        e.createdAt  = QDateTime::fromString(o["created_at"].toString(), Qt::ISODate);
        pendingEvents_.push_back(e);
    }

    return true;
}

void AdvisoryBannerQueue::saveEvents() {
    const QString path = eventsFilePath();
    if (path.isEmpty()) {
        return;
    }

    QJsonArray arr;
    for (const auto& e : pendingEvents_) {
        QJsonObject o;
        o["event_id"]    = e.eventId;
        o["lineage_key"] = e.lineageKey;
        o["txid"]        = e.txid;
        o["type"]        = static_cast<int>(e.type);
        o["message"]     = e.message;
        o["created_at"]  = e.createdAt.toString(Qt::ISODate);
        arr.append(o);
    }
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
        file.close();
    }
}

void AdvisoryBannerQueue::loadAcknowledged() {
    const QString path = acknowledgedFilePath();
    if (path.isEmpty()) {
        acknowledgedEventIds_.clear();
        return;
    }

    loadAcknowledgedFromPath(path);
}

bool AdvisoryBannerQueue::loadAcknowledgedFromPath(const QString& path) {
    acknowledgedEventIds_.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isArray()) return false;

    for (const auto& val : doc.array())
        acknowledgedEventIds_.insert(val.toString());

    return true;
}

void AdvisoryBannerQueue::saveAcknowledged() {
    const QString path = acknowledgedFilePath();
    if (path.isEmpty()) {
        return;
    }

    QJsonArray arr;
    for (const auto& id : acknowledgedEventIds_)
        arr.append(id);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
        file.close();
    }
}
