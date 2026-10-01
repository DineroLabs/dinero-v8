#include "chaintiming.h"

QString formatApproxSeconds(qint64 seconds) {
    if (seconds <= 0) return QStringLiteral("~0 min");
    if (seconds < 60) return QStringLiteral("~%1 sec").arg(seconds);
    const qint64 minutes = (seconds + 30) / 60;
    if (minutes < 60) return QStringLiteral("~%1 min").arg(minutes);
    const qint64 hours = minutes / 60, remMin = minutes % 60;
    if (hours < 24)
        return remMin ? QStringLiteral("~%1 h %2 min").arg(hours).arg(remMin)
                      : QStringLiteral("~%1 h").arg(hours);
    const qint64 days = hours / 24, remH = hours % 24;
    if (days >= 365) {
        const double years = double(seconds) / 31'557'600.0;  // Julian year
        const QString value = QString::number(years, 'f', 1);
        return value == QLatin1String("1.0") ? QStringLiteral("~1.0 year") : QStringLiteral("~%1 years").arg(value);
    }
    const QString d = days == 1 ? QStringLiteral("1 day") : QStringLiteral("%1 days").arg(days);
    return remH ? QStringLiteral("~%1 %2 h").arg(d).arg(remH) : QStringLiteral("~%1").arg(d);
}

ChainTiming ChainTiming::fromEconomics(const QJsonObject& o) {
    ChainTiming t;
    const QJsonValue v = o.value(QStringLiteral("block_time_seconds"));
    if (v.isDouble()) {
        const double s = v.toDouble();
        if (s >= 1 && s <= 3600) { t.blockSeconds = static_cast<int>(s); t.fromNode = true; }
    }
    return t;
}
