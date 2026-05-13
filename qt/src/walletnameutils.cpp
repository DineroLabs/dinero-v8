#include "walletnameutils.h"

#include <QStringList>

namespace {

QStringList reservedWalletNames() {
  return {
    QStringLiteral("CON"),
    QStringLiteral("PRN"),
    QStringLiteral("AUX"),
    QStringLiteral("NUL"),
    QStringLiteral("COM1"),
    QStringLiteral("COM2"),
    QStringLiteral("COM3"),
    QStringLiteral("COM4"),
    QStringLiteral("COM5"),
    QStringLiteral("COM6"),
    QStringLiteral("COM7"),
    QStringLiteral("COM8"),
    QStringLiteral("COM9"),
    QStringLiteral("LPT1"),
    QStringLiteral("LPT2"),
    QStringLiteral("LPT3"),
    QStringLiteral("LPT4"),
    QStringLiteral("LPT5"),
    QStringLiteral("LPT6"),
    QStringLiteral("LPT7"),
    QStringLiteral("LPT8"),
    QStringLiteral("LPT9"),
  };
}

} // namespace

QString normalizeWalletName(const QString& rawName) {
  QString filtered;
  filtered.reserve(rawName.size());

  for (const QChar ch : rawName) {
    if (ch.isLetterOrNumber() || ch == QChar(' ') || ch == QChar('_') || ch == QChar('-')) {
      filtered.append(ch);
    }
  }

  filtered = filtered.simplified();
  if (filtered.isEmpty()) {
    return filtered;
  }

  if (reservedWalletNames().contains(filtered.toUpper())) {
    filtered.append(QChar('_'));
  }

  return filtered;
}

QString validateWalletNameInput(const QString& rawName, QString* normalizedName) {
  const QString normalized = normalizeWalletName(rawName);
  if (normalizedName) {
    *normalizedName = normalized;
  }

  if (rawName.trimmed().isEmpty()) {
    return QStringLiteral("Enter a wallet name.");
  }

  if (normalized.isEmpty()) {
    return QStringLiteral("Wallet name must contain letters, numbers, spaces, '-' or '_'.");
  }

  if (normalized.size() > 64) {
    return QStringLiteral("Wallet name must be 64 characters or fewer.");
  }

  return {};
}
