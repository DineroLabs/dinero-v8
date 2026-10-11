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

// Whether a wallet name may still be deleted when the setup wizard is cancelled.
// If the node answered with an error it created nothing, and the name may belong
// to an existing wallet; only a timeout (unknown outcome) or a success keeps it.
inline bool keepRollbackCandidateAfterRpc(bool daemonResponded, bool success) {
  return success || !daemonResponded;
}
