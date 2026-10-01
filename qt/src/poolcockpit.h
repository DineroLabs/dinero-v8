// Copyright (c) 2026 Dinero Labs.
//
// Formatting for the Pool cockpit: readable durations and counts, contributor
// scripts as addresses, and honest coverage of the locally stored history.

#pragma once

#include <QString>

namespace poolcockpit {

/// "45 s", "12 min", "4 h 20 min", "9 days 5 h".
QString formatDuration(qint64 seconds);
/// 302343 -> "302,343".
QString groupDigits(qint64 value);
/// Groups a decimal string such as a window weight; anything else is returned unchanged.
QString groupDigitsText(const QString& text);
/// A witness-program scriptPubKey (OP_0..OP_16, push 2..40 bytes) as a bech32 /
/// bech32m address with the given HRP. Empty when the script is not one.
QString scriptToAddress(const QString& scriptHex, const QString& hrp);
/// The human-readable part of a bech32 address ("din", "tdin", "rdin"); "din" if unknown.
QString hrpOf(const QString& address);
/// Empty when samples starting at `first_sample_at` cover the whole window;
/// otherwise "collecting since HH:mm" in local time.
QString historyCoverage(qint64 first_sample_at, qint64 now, qint64 window_secs);

}  // namespace poolcockpit
