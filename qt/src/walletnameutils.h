#pragma once

#include <QString>
#include <QStringList>

QString normalizeWalletName(const QString& rawName);
QString validateWalletNameInput(const QString& rawName, QString* normalizedName = nullptr);

// Restoring never replaces a wallet. Returns base (normalized) when it is free,
// otherwise "base restored", "base restored 2", ... within the 64-character limit.
// Names are compared case-insensitively after normalization.
QString uniqueRestoreName(const QString& base, const QStringList& existing);

enum class RestoreErrorKind { None, NameExists, Other };
// Classifies a wallet.restore error; NameExists means the daemon refused to overwrite.
RestoreErrorKind classifyRestoreError(const QString& error);
