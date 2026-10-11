#pragma once
#include <QJsonObject>
#include <QString>
#include <optional>

// Decides whether to tell the user about a network upgrade. Inputs come from
// the published network-upgrade.json notice (docs/release/NETWORK_UPGRADE_NOTICE.md),
// the latest GitHub release tag and the node's scheduled release height.
// Behaviour is pinned by qt/tests/vectors/network_upgrade_policy_v1.json, which
// DineroDPI shares byte for byte.
namespace UpgradePolicy {
struct Version {
    int major = -1, minor = -1, patch = -1;
    bool valid() const { return major >= 0; }
};
Version parseVersion(const QString& s);           // "v8.1.12-metal-fix1" / "8.1.12.1" → 8.1.12; "unknown" → invalid
int compare(const Version& a, const Version& b);  // -1/0/1; an invalid side compares equal (never nags)

enum class State { None, UpdateAvailable, UpdateRequired, RequiredOverdue, ScheduledReady };
struct Notice {
    bool present = false;
    quint32 activationHeight = 0;
    QString minVersion;
};
Notice parseNotice(const QJsonObject& n, const QString& component, const QString& network = QStringLiteral("mainnet"));

struct Result {
    State state = State::None;
    qint64 blocksLeft = -1;  // -1 = not applicable
};
Result evaluate(const QString& appVersion, const QString& latestTag /* empty = unknown */,
                const Notice& notice, quint32 tip, std::optional<quint32> nodeReleaseHeight,
                bool compareLatestTag = true);
QString stateName(State s);

// getconsensusinfo.release_activation_height; nullopt when absent, malformed,
// zero or the node's "unset" sentinel (4294967295).
std::optional<quint32> parseReleaseActivationHeight(const QJsonObject& consensusInfo);
}  // namespace UpgradePolicy
