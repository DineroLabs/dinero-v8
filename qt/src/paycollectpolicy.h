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

// What the user pasted into Pay: a DPI invoice, a dinero: payment link
// (dinero:<address>?amount=<DIN>&rid=<id>&exp=<unix>&desc=<text>, as the
// DineroDPI phone app writes them) or a bare public address.
struct PayTarget {
    enum class Kind { Empty, Invoice, Link, Address, Invalid };
    Kind kind = Kind::Empty;
    QString address;
    QString amount;     // DIN as written in the link; empty when not given
    QString requestId;
    qint64 expiresAt = 0;
    QString label;      // desc, memo or merchant from the link
    QString error;      // why an Invalid input cannot be paid
};
PayTarget classifyPayInput(const QString& text, qint64 nowSecs);
}
