// Copyright (c) 2026 Dinero Labs.
//
// Where a remembered pool ops token lives. Never in QSettings: on macOS it is
// the login Keychain; elsewhere nothing is stored and the operator pastes the
// token each session, as before.

#pragma once

#include <QString>

#include <memory>

class PoolTokenStore {
public:
    virtual ~PoolTokenStore() = default;
    /// False when this platform has no secure store; the panel then hides "remember".
    virtual bool available() const = 0;
    virtual QString load(const QString& account) const = 0;
    virtual bool save(const QString& account, const QString& token) = 0;
    virtual void remove(const QString& account) = 0;
};

/// The platform store. `service` names the Keychain item; tests pass their own.
std::unique_ptr<PoolTokenStore> makeSystemPoolTokenStore(
    const QString& service = QStringLiteral("org.dinerolabs.dinero-qt.pool-ops-token"));
