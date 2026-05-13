#ifndef CHANGEADDRESSMANAGER_H
#define CHANGEADDRESSMANAGER_H

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QUuid>
#include <QFile>
#include <vector>

/**
 * Intent-based change address reservation for sequential sends.
 *
 * Each send reserves a change address before broadcast. The reservation
 * is persisted immediately so that a crash between reserve and broadcast
 * leaves a recoverable "stale" reservation rather than a silent reuse.
 *
 * On startup, stale reservations (status "reserved" older than 10 min)
 * are released automatically.
 *
 * Identity model (matches DineroDPI):
 *   - walletIdentityKey = "fingerprint:accountIndex" (cryptographic identity)
 *   - Used for scoping reservations to a specific wallet+account
 *   - NOT wallet name/label (mutable, not cryptographic)
 *   - NOT bare fingerprint (4 bytes, collision risk, shared across accounts)
 */
class ChangeAddressManager : public QObject {
    Q_OBJECT
public:
    struct Reservation {
        QString reservationId;
        QString walletIdentityKey;  // fingerprint:accountIndex
        int     changeIndex = 0;
        QString address;
        QDateTime createdAt;
        QString status;  // "reserved", "used", "released", "stale"
    };

    explicit ChangeAddressManager(const QString& dataDir, QObject* parent = nullptr);

    /// Set wallet identity key (fingerprint:accountIndex).
    void setWalletIdentityKey(const QString& identityKey);
    QString walletIdentityKey() const { return walletIdentityKey_; }

    /// Reserve the next change address. Returns the reserved address.
    /// The reservation is persisted immediately with status "reserved".
    Reservation reserve();

    /// Mark a reservation as used (send succeeded).
    void markUsed(const QString& reservationId);

    /// Release a reservation (send failed, tx evicted, etc.).
    void release(const QString& reservationId);

    /// Called on startup — releases reservations stuck in "reserved"
    /// state for longer than staleThresholdMinutes.
    void reconcileStaleReservations(int staleThresholdMinutes = 10);

    /// Current cursor index for the active wallet.
    int currentCursorIndex() const;

private:
    void load();
    void save();
    Reservation* findReservation(const QString& reservationId);

    QString dataDir_;
    QString walletIdentityKey_;
    int cursorIndex_ = 0;
    std::vector<Reservation> reservations_;

    QString filePath() const;
};

#endif // CHANGEADDRESSMANAGER_H
