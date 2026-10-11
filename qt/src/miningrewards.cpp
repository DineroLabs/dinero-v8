#include "miningrewards.h"
#include <QJsonObject>
#include <cmath>
#include "chaintiming.h"

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

MiningRewardsText miningRewardsText(const MiningRewardsSummary& s, const ChainTiming& timing) {
    MiningRewardsText t;
    if (!s.any) {
        t.headline = QStringLiteral("No mining rewards yet");
        t.period = QStringLiteral("in this wallet");
        return t;
    }
    // A full page means more rewards may exist than were fetched: say "at least".
    const QString plus = s.truncated ? QStringLiteral("+") : QString();
    QString amount = formatDinAmount(s.una24h);
    amount.insert(amount.size() - 4, plus);  // before " DIN"
    t.headline = QString("%1%2 block%3 · %4")
                     .arg(s.blocks24h).arg(plus).arg(s.blocks24h == 1 && !s.truncated ? "" : "s").arg(amount);
    t.period = QStringLiteral("found in the last 24 hours");
    if (s.maturingUna > 0 && s.nextUnlockBlocks) {
        const int n = *s.nextUnlockBlocks;
        t.maturing = QString("Maturing: %1 · next unlock in %2 block%3 (%4)")
                         .arg(formatDinAmount(s.maturingUna)).arg(n).arg(n == 1 ? "" : "s")
                         .arg(timing.approxDuration(n));
    } else {
        t.maturing = QStringLiteral("Nothing maturing");
    }
    if (s.lastFoundSecsAgo) t.lastFound = QString("Last block found %1 ago").arg(formatApproxSeconds(*s.lastFoundSecsAgo));
    return t;
}
