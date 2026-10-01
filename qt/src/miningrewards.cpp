#include "miningrewards.h"
#include <QJsonObject>
#include <cmath>

namespace {
bool isMiningReward(const QJsonObject& tx) {
    const QString category = tx.value("category").toString();
    if (category == QLatin1String("orphan")) return false;  // orphaned blocks pay nothing
    return tx.value("type").toString() == QLatin1String("mined") || tx.value("is_coinbase").toBool(false) ||
           category == QLatin1String("generate") || category == QLatin1String("immature");
}
}  // namespace

MiningRewardsSummary summarizeMiningRewards(const QJsonArray& transactions, qint64 nowSecs,
                                            int requestedCount, int maturity) {
    MiningRewardsSummary s;
    s.truncated = requestedCount > 0 && transactions.size() >= requestedCount;
    int oldestMaturingConfirmations = -1;
    qint64 newestTime = -1;
    for (const QJsonValue& v : transactions) {
        const QJsonObject tx = v.toObject();
        if (!isMiningReward(tx)) continue;
        s.any = true;
        const qint64 una = std::llround(std::fabs(tx.value("amount").toDouble()) * 100'000'000.0);
        const qint64 time = qint64(tx.value("time").toDouble());
        const int confirmations = tx.value("confirmations").toInt();
        if (time > 0 && nowSecs - time <= 86'400) {
            ++s.blocks24h;
            s.una24h += una;
        }
        if (confirmations >= 1 && confirmations < maturity) {
            s.maturingUna += una;
            oldestMaturingConfirmations = qMax(oldestMaturingConfirmations, confirmations);
        }
        if (time > newestTime) newestTime = time;
    }
    if (oldestMaturingConfirmations >= 1) s.nextUnlockBlocks = maturity - oldestMaturingConfirmations;
    if (newestTime > 0) s.lastFoundSecsAgo = qMax<qint64>(0, nowSecs - newestTime);
    return s;
}

QString formatDinAmount(qint64 una) {
    const bool negative = una < 0;
    const qint64 abs = negative ? -una : una;
    QString whole = QString::number(abs / 100'000'000);
    for (int i = whole.size() - 3; i > 0; i -= 3) whole.insert(i, ',');
    QString frac = QString::number(abs % 100'000'000).rightJustified(8, '0');
    while (frac.endsWith('0')) frac.chop(1);
    return QString("%1%2%3 DIN").arg(negative ? "-" : "", whole, frac.isEmpty() ? QString() : "." + frac);
}
