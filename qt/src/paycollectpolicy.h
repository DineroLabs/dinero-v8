#pragma once
#include <QString>

// User-facing wording and status colours for the Pay/Collect tab. Kept free of
// widgets so the rules can be tested directly.
namespace PayCollectPolicy {
enum class Tone { Good, Warn, Bad, Info, Neutral };
struct Badge {
    QString text;
    Tone tone = Tone::Neutral;
    QString tooltip;
};
QString amountBlocker(const QString& text);
Badge paymentStatus(int tier, int confirmations);
Badge verifyResult(const QString& tier);
QString toneColor(Tone tone);
QString pillStyle(Tone tone);
QString badgeHtml(const Badge& badge);
}
