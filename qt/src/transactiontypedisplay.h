#pragma once

#include <QString>

namespace dinero::qt {

// Label for the Transaction History "Type" column of a private-pool move.
// The wallet reports these as "shield" (public -> private) and "unshield"
// (private -> public); without a label they fell through to the grey
// unknown-type placeholder. Returns an empty string for any other type.
inline QString privacyMoveTypeLabel(const QString& type) {
  if (type == QLatin1String("shield")) {
    return QString::fromUtf8("\xF0\x9F\x9B\xA1\xEF\xB8\x8F Shield");  // 🛡️
  }
  if (type == QLatin1String("unshield")) {
    return QString::fromUtf8("\xF0\x9F\x94\x93 Unshield");  // 🔓
  }
  return QString();
}

}  // namespace dinero::qt
