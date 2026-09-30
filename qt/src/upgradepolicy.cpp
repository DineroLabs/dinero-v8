#include "upgradepolicy.h"
#include <QRegularExpression>

namespace UpgradePolicy {
Version parseVersion(const QString& s) {
    static const QRegularExpression re(QStringLiteral("^v?(\\d+)\\.(\\d+)\\.(\\d+)(?:[-+].*)?$"));
    const auto m = re.match(s.trimmed());
    if (!m.hasMatch()) return {};
    return {m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt()};
}

int compare(const Version& a, const Version& b) {
    if (!a.valid() || !b.valid()) return 0;
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

Notice parseNotice(const QJsonObject& n, const QString& component, const QString& network) {
    Notice out;
    if (n.value(QStringLiteral("schema")).toString() != QLatin1String("dinero.network-upgrade.v1")) return out;
    if (n.value(QStringLiteral("network")).toString() != network) return out;
    const double h = n.value(QStringLiteral("activation_height")).toDouble(0);
    if (!(h >= 1 && h < 4294967295.0)) return out;
    const QString minV = n.value(QStringLiteral("min_versions")).toObject().value(component).toString();
    if (!parseVersion(minV).valid()) return out;
    out.present = true;
    out.activationHeight = quint32(h);
    out.minVersion = minV;
    return out;
}

Result evaluate(const QString& appVersion, const QString& latestTag, const Notice& notice,
                quint32 tip, std::optional<quint32> nodeReleaseHeight, bool compareLatestTag) {
    const Version app = parseVersion(appVersion);
    if (!app.valid()) return {};  // development builds never nag
    if (notice.present && compare(app, parseVersion(notice.minVersion)) < 0) {
        const qint64 left = qint64(notice.activationHeight) - qint64(tip);
        return left > 0 ? Result{State::UpdateRequired, left} : Result{State::RequiredOverdue, 0};
    }
    std::optional<quint32> scheduled;
    if (nodeReleaseHeight && *nodeReleaseHeight != 0xFFFFFFFFu) scheduled = nodeReleaseHeight;
    else if (notice.present) scheduled = notice.activationHeight;
    if (scheduled && *scheduled > tip) return {State::ScheduledReady, qint64(*scheduled) - qint64(tip)};
    if (compareLatestTag && !latestTag.isEmpty() && compare(app, parseVersion(latestTag)) < 0)
        return {State::UpdateAvailable, -1};
    return {};
}

QString stateName(State s) {
    switch (s) {
    case State::UpdateAvailable: return QStringLiteral("update_available");
    case State::UpdateRequired: return QStringLiteral("update_required");
    case State::RequiredOverdue: return QStringLiteral("required_overdue");
    case State::ScheduledReady: return QStringLiteral("scheduled_ready");
    case State::None: break;
    }
    return QStringLiteral("none");
}
}  // namespace UpgradePolicy
