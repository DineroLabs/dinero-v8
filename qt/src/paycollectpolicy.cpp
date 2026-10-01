#include "paycollectpolicy.h"
#include "shieldedtransferpolicy.h"

#include <QRegularExpression>

namespace PayCollectPolicy {

QString amountBlocker(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return QStringLiteral("Enter an amount");
    // parseDinToUna accepts positive amounts only, so name the zero case first.
    if (QRegularExpression(QStringLiteral("^0*(\\.0{0,8})?$")).match(trimmed).hasMatch())
        return QStringLiteral("Amount must be more than 0");
    qint64 una = 0;
    if (!ShieldedTransferPolicy::parseDinToUna(trimmed, &una))
        return QStringLiteral("Enter a valid amount (up to 8 decimals)");
    return {};
}

Badge paymentStatus(int tier, int confirmations) {
    const QString confs = QString("%1 confirmation%2").arg(confirmations).arg(confirmations == 1 ? "" : "s");
    switch (tier) {
    case 0: return {"Failed", Tone::Bad, "T0: the payment could not be verified"};
    case 1: return {QString::fromUtf8("Verified \xC2\xB7 waiting for a block"), Tone::Warn,
                    "T1: valid and seen by the network, not yet in a block"};
    case 2: return {QString::fromUtf8("Confirmed \xC2\xB7 ") + confs, Tone::Info,
                    "T2: included in a block"};
    case 3: return {QString::fromUtf8("Final \xC2\xB7 ") + confs, Tone::Good,
                    "T3: 6 or more confirmations"};
    default: return {"Unknown", Tone::Neutral, {}};
    }
}

Badge verifyResult(const QString& tier) {
    if (tier == "T1")
        return {"Payment verified", Tone::Good, "T1: the package matches this invoice and the network has seen it"};
    return {"Not verified", Tone::Warn, "T0: the package could not be verified yet"};
}

// The app's status palette (see MainWindow): green, amber, red, slate, grey.
QString toneColor(Tone tone) {
    switch (tone) {
    case Tone::Good: return "#51cf66";
    case Tone::Warn: return "#f0b429";
    case Tone::Bad: return "#ff6b6b";
    case Tone::Info: return "#9fb3c8";
    case Tone::Neutral: break;
    }
    return "#868e96";
}

static QString toneBackground(Tone tone) {
    switch (tone) {
    case Tone::Good: return "#1f2a22";
    case Tone::Warn: return "#2c2618";
    case Tone::Bad: return "#2d1f21";
    case Tone::Info: return "#1f2630";
    case Tone::Neutral: break;
    }
    return "#272c33";
}

QString pillStyle(Tone tone) {
    return QString("QLabel { color: %1; background: %2; padding: 8px; border-radius: 6px; }")
        .arg(toneColor(tone), toneBackground(tone));
}

QString badgeHtml(const Badge& badge) {
    return QString("<span style='color:%1; background:%2; padding:4px 10px; font-weight:600;'>%3</span>")
        .arg(toneColor(badge.tone), toneBackground(badge.tone), badge.text.toHtmlEscaped());
}

}  // namespace PayCollectPolicy
