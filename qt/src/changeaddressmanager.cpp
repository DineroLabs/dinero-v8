#include "changeaddressmanager.h"
#include <QDir>

ChangeAddressManager::ChangeAddressManager(const QString& dataDir, QObject* parent)
    : QObject(parent), dataDir_(dataDir)
{
}

QString ChangeAddressManager::filePath() const {
    return dataDir_ + QDir::separator() + "change_reservations.json";
}

void ChangeAddressManager::setWalletIdentityKey(const QString& identityKey) {
    walletIdentityKey_ = identityKey;
    load();
    reconcileStaleReservations();
}

void ChangeAddressManager::load() {
    reservations_.clear();
    cursorIndex_ = 0;

    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly)) return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) return;

    QJsonObject root = doc.object();
    QJsonArray arr = root["reservations"].toArray();
    for (const auto& val : arr) {
        QJsonObject obj = val.toObject();
        Reservation r;
        r.reservationId     = obj["reservationId"].toString();
        r.walletIdentityKey = obj["walletIdentityKey"].toString();
        r.changeIndex       = obj["changeIndex"].toInt();
        r.address           = obj["address"].toString();
        r.createdAt         = QDateTime::fromString(obj["createdAt"].toString(), Qt::ISODate);
        r.status            = obj["status"].toString();
        reservations_.push_back(r);
    }

    // Recover cursor: max used/reserved index for this wallet + 1
    for (const auto& r : reservations_) {
        if (r.walletIdentityKey == walletIdentityKey_ &&
            (r.status == "used" || r.status == "reserved")) {
            if (r.changeIndex >= cursorIndex_) {
                cursorIndex_ = r.changeIndex + 1;
            }
        }
    }
}

void ChangeAddressManager::save() {
    QJsonArray arr;
    for (const auto& r : reservations_) {
        QJsonObject obj;
        obj["reservationId"]     = r.reservationId;
        obj["walletIdentityKey"] = r.walletIdentityKey;
        obj["changeIndex"]       = r.changeIndex;
        obj["address"]           = r.address;
        obj["createdAt"]         = r.createdAt.toString(Qt::ISODate);
        obj["status"]            = r.status;
        arr.append(obj);
    }

    QJsonObject root;
    root["reservations"] = arr;

    QFile file(filePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.close();
    }
}

ChangeAddressManager::Reservation ChangeAddressManager::reserve() {
    Reservation r;
    r.reservationId     = QUuid::createUuid().toString(QUuid::WithoutBraces);
    r.walletIdentityKey = walletIdentityKey_;
    r.changeIndex       = cursorIndex_;
    r.createdAt         = QDateTime::currentDateTimeUtc();
    r.status            = "reserved";
    // address is set by caller after deriving via RPC (wallet.deriveaddress)

    cursorIndex_++;
    reservations_.push_back(r);
    save();
    return r;
}

void ChangeAddressManager::markUsed(const QString& reservationId) {
    if (auto* r = findReservation(reservationId)) {
        r->status = "used";
        save();
    }
}

void ChangeAddressManager::release(const QString& reservationId) {
    if (auto* r = findReservation(reservationId)) {
        r->status = "released";
        save();
    }
}

void ChangeAddressManager::reconcileStaleReservations(int staleThresholdMinutes) {
    QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-staleThresholdMinutes * 60);
    bool changed = false;
    for (auto& r : reservations_) {
        if (r.walletIdentityKey == walletIdentityKey_ &&
            r.status == "reserved" && r.createdAt < cutoff) {
            r.status = "stale";
            changed = true;
        }
    }
    if (changed) save();
}

int ChangeAddressManager::currentCursorIndex() const {
    return cursorIndex_;
}

ChangeAddressManager::Reservation* ChangeAddressManager::findReservation(const QString& reservationId) {
    for (auto& r : reservations_) {
        if (r.reservationId == reservationId) return &r;
    }
    return nullptr;
}
