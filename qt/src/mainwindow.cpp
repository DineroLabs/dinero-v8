#include "mainwindow.h"
#include "responsiveuipolicy.h"
#include "rpcclient.h"
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QStandardItemModel>
#include "changeaddressmanager.h"
#include "transactiontracker.h"
#include "advisorybanner.h"
#include "walletwizard.h"
#include "qrcodegen.h"
#include "hardwarewalletwidget.h"  // Hardware wallet support (production-ready)
#include "dpiwidget.h"             // DPI Pay/Collect
#include "aipanel.h"
#include "cmdkpanel.h"
#include "overviewconnectivitycard.h"
#include "scrollsupport.h"
// AI panel uses ClaudeProcess (no more AiTools)
#include "aistatusstrip.h"
#include <solo_miner/chain_identity.h>

#ifdef DIN_EXPERIMENTAL_FEATURES
#include "websocketclient.h"
// #include "bridgewidget.h"  // DISABLED - not ready for production
#include "paymentswidget.h"
#include "escrowwidget.h"
#include "marketplacewidget.h"
#endif

// Track C — production, included unconditionally.
#include "vaultpanel.h"
#include "shieldedwidget.h"

// NOTE: Lightning Network moved to separate lightning-main branch (L2)
// #include "lightningwidget.h"  // REMOVED for L1 purity
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QPointer>
#include <QThread>
#include <QGroupBox>
#include <QScrollArea>
#include <QClipboard>
#include <QCursor>
#include <QApplication>
#include <QScreen>
#include <QGuiApplication>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpSocket>
#include <QSpinBox>
#include <QProgressBar>
#include <QStandardPaths>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTextCursor>
#include <QTextBlock>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QInputDialog>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QGridLayout>
#include <QFileDialog>
#include <QDateTime>
#include <QTimeZone>
#include <QDesktopServices>
#include <QSettings>
#include <QShortcut>
#include <QLocale>
#include <QUrl>
#include <QFileInfo>
#include <QGraphicsColorizeEffect>
#include <QHash>
#include <QSet>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QFontDatabase>
#include <QThread>
#include <QStackedWidget>
#include <cmath>
#include <algorithm>
#include <limits>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef HAVE_QT_QUICK
#include "rpchelper.h"
#include <QQuickWidget>
#include <QQmlContext>
#endif

namespace {

// Temporary product gate: Cmd+K should expose only My Node Dashboard while
// the AI assistant surface is parked for release polish.
constexpr bool kShowAiAssistantPanel = false;

QString defaultDineroDataDir() {
#if defined(Q_OS_WIN)
  const QString appdata = qEnvironmentVariable("APPDATA");
  if (!appdata.isEmpty()) {
    return QDir::fromNativeSeparators(appdata + "/Dinero");
  }
  return QDir::homePath() + "/Dinero";
#elif defined(Q_OS_MAC)
  return QDir::homePath() + "/Library/Application Support/Dinero";
#else
  return QDir::homePath() + "/.dinero";
#endif
}

bool isTransparentDineroAddress(const QString& address) {
  return address.startsWith("din1q") || address.startsWith("tdin1q") || address.startsWith("rdin1q") ||
         address.startsWith("din1p") || address.startsWith("tdin1p") || address.startsWith("rdin1p") ||
         address.startsWith("din1r") || address.startsWith("tdin1r") || address.startsWith("rdin1r");
}

bool isConfidentialDineroAddress(const QString& address) {
  return address.startsWith("dinc1") || address.startsWith("tdinc1") || address.startsWith("rdinc1") ||
         address.startsWith("dina1") || address.startsWith("tdina1") || address.startsWith("rdina1");
}

bool isShieldedDineroAddress(const QString& address) {
  return address.startsWith("dins1") || address.startsWith("tdins1") || address.startsWith("rdins1");
}

bool miningModeNeedsDaemon(const QString& mode) {
  return mode != "pool" && mode != "sv2_pool";
}

constexpr int kPublicSendEstimateVbytes = 250;
constexpr int kPrivateSendEstimateVbytes = 1000;

QString hardwareWalletPsbtTooltip() {
  return QStringLiteral(
      "<b>Dinero transaction in PSBT format</b><br>"
      "A partially signed Dinero transaction encoded using the BIP174 PSBT signing container.");
}

double estimatedFeeDin(double feeRateUnaPerVb, int txSizeVbytes) {
  if (!std::isfinite(feeRateUnaPerVb) || feeRateUnaPerVb <= 0.0 || txSizeVbytes <= 0) {
    return 0.0;
  }
  return (feeRateUnaPerVb * static_cast<double>(txSizeVbytes)) / 1e8;
}

qint64 privateModeFeeUna(double feeRateUnaPerVb) {
  if (!std::isfinite(feeRateUnaPerVb) || feeRateUnaPerVb <= 0.0) {
    feeRateUnaPerVb = 1.0;
  }
  return std::max<qint64>(
    1000,
    static_cast<qint64>(std::ceil(feeRateUnaPerVb * kPrivateSendEstimateVbytes)));
}

bool isPoolProcessMinerType(const QString& minerType) {
  return minerType == "stratum_worker" ||
         minerType == "sv2_pool" ||
         minerType == "sv2_pool_gpu";
}

double jsonToDouble(const QJsonValue& value) {
  if (value.isString()) {
    return value.toString().toDouble();
  }
  return value.toDouble();
}

QString peerClientLabel(const QString& rawSubver) {
  QString normalized = rawSubver.trimmed();
  if (normalized.startsWith('/')) normalized.remove(0, 1);
  if (normalized.endsWith('/')) normalized.chop(1);

  if (normalized.isEmpty() || normalized.compare("unknown", Qt::CaseInsensitive) == 0) {
    return "Peer client";
  }

  const QString lower = normalized.toLower();
  if (lower.startsWith("dinerod")) {
    QString version = normalized.section(':', 1).trimmed();
    if (version.isEmpty() || version.compare("unknown", Qt::CaseInsensitive) == 0) {
      return "Dinero Core";
    }
    return QString("Dinero Core %1").arg(version);
  }

  normalized.replace(':', ' ');
  return normalized;
}

QString peerClientTooltip(const QString& rawSubver) {
  const QString raw = rawSubver.trimmed();
  if (raw.isEmpty() || raw.compare("unknown", Qt::CaseInsensitive) == 0) {
    return "This peer did not advertise a client version.";
  }
  if (raw.toLower().contains("dinerod:unknown")) {
    return QString("Peer identified as Dinero Core but did not advertise an exact version.\nRaw user agent: %1").arg(raw);
  }
  return QString("Raw peer user agent: %1").arg(raw);
}

QString peerHostFromEndpoint(const QString& endpoint) {
  const QString trimmed = endpoint.trimmed();
  if (trimmed.startsWith('[')) {
    const int close = trimmed.indexOf(']');
    if (close > 1) return trimmed.mid(1, close - 1);
  }

  const int colonCount = trimmed.count(':');
  if (colonCount == 1) {
    return trimmed.section(':', 0, 0);
  }
  return trimmed;
}

bool isDefaultBootstrapPeerHost(const QString& host) {
  static const QSet<QString> bootstrapPeers{
      "173.249.200.59",
      "172.93.167.32",
      "92.118.190.62",
  };
  return bootstrapPeers.contains(host.trimmed());
}

QString peerLocationLabel(const QString& endpoint, int peerIndex) {
  const QString host = peerHostFromEndpoint(endpoint);
  static const QHash<QString, QString> knownSeedRegions{
      {"173.249.200.59", "US-West"},
      {"172.93.167.32", "North America"},
      {"92.118.190.62", "Europe"},
  };

  const QString known = knownSeedRegions.value(host);
  if (!known.isEmpty()) return known;

  if (host == "127.0.0.1" || host == "::1" || host.compare("localhost", Qt::CaseInsensitive) == 0) {
    return "Local Node";
  }
  if (host.startsWith("10.") || host.startsWith("192.168.") || host.startsWith("172.16.") ||
      host.startsWith("172.17.") || host.startsWith("172.18.") || host.startsWith("172.19.") ||
      host.startsWith("172.20.") || host.startsWith("172.21.") || host.startsWith("172.22.") ||
      host.startsWith("172.23.") || host.startsWith("172.24.") || host.startsWith("172.25.") ||
      host.startsWith("172.26.") || host.startsWith("172.27.") || host.startsWith("172.28.") ||
      host.startsWith("172.29.") || host.startsWith("172.30.") || host.startsWith("172.31.")) {
    return "Private Peer";
  }

  return QString("Network Peer %1").arg(peerIndex + 1);
}

QString peerLocationTooltip(const QString& endpoint, const QString& label) {
  return QString("%1\nEndpoint: %2").arg(label, endpoint);
}

QString detectExternalMinerProfileFromHelp(const QString& helpText) {
  const QString lower = helpText.toLower();
  const bool hasRpc = lower.contains("--rpc");
  const bool hasAddress = lower.contains("--address");
  const bool hasStratum = lower.contains("--stratum");
  const bool hasUser = lower.contains("--user");

  if (hasStratum && hasUser && !hasRpc) {
    return "stratum";
  }
  if (hasRpc && hasAddress) {
    return "rpc";
  }
  if (hasStratum && hasUser) {
    return "stratum";
  }
  return "unknown";
}

QString probeExternalMinerProfile(const QString& minerPath, QString* helpTextOut, QString* probeErrorOut) {
  QProcess probe;
  probe.setProcessChannelMode(QProcess::MergedChannels);
  probe.start(minerPath, QStringList() << "--help");

  if (!probe.waitForStarted(2000)) {
    if (probeErrorOut) {
      *probeErrorOut = probe.errorString();
    }
    return "unknown";
  }

  if (!probe.waitForFinished(2500)) {
    probe.kill();
    probe.waitForFinished(500);
  }

  const QString output = QString::fromUtf8(probe.readAllStandardOutput()).trimmed();
  if (helpTextOut) {
    *helpTextOut = output;
  }
  if (probeErrorOut && output.isEmpty()) {
    *probeErrorOut = "No help output from miner";
  }

  return detectExternalMinerProfileFromHelp(output);
}

QString externalMinerSettingsKey(bool poolMode) {
  return poolMode ? "mining/stratum_worker_path" : "mining/external_miner_path";
}

QString externalMinerEnvVar(bool poolMode) {
  return poolMode ? "DINERO_STRATUM_WORKER_PATH" : "DINERO_MINER_PATH";
}

QString externalMinerBinaryName(bool poolMode) {
#ifdef Q_OS_WIN
  return poolMode ? "dinero-stratum-worker.exe" : "dinero-miner.exe";
#else
  return poolMode ? "dinero-stratum-worker" : "dinero-miner";
#endif
}

QString externalMinerDisplayName(bool poolMode) {
  return poolMode ? "dinero-stratum-worker" : "dinero-miner";
}

QString stratumServerBinaryName() {
#ifdef Q_OS_WIN
  return "dinero-stratum.exe";
#else
  return "dinero-stratum";
#endif
}

QString localStratumEndpoint() {
  return QStringLiteral("127.0.0.1:3333");
}

constexpr int kDineroMainnetP2PPort = 20999;
QString p2pAccessPromptAcceptedKey() {
  return QStringLiteral("network/p2p_access_prompt_accepted_v1");
}

QString p2pPortMapAllowedKey() {
  return QStringLiteral("network/p2p_portmap_auto_allowed_v1");
}

// ─────── SV2 pool miner (dinero-sv2-miner) ───────
// Defaults point at the reference pool on SJ. Users override via Settings
// or via DINERO_SV2_* env vars for custom pools.

QString sv2MinerBinaryName() {
#ifdef Q_OS_WIN
  return "dinero-sv2-miner.exe";
#else
  return "dinero-sv2-miner";
#endif
}

QString sv2MinerSettingsKey() { return "mining/sv2_miner_path"; }
QString sv2MinerEnvVar()      { return "DINERO_SV2_MINER_PATH"; }

QString sv2PoolEndpointDefault()       { return "173.249.200.59:4444"; }
QString sv2PoolServerPubkeyDefault()   {
  return "3c879d90c9bb430493dfbf02cecbb93c3ae0d9d6c31d0757595e353fbe927417";
}

QString sv2PoolEndpoint() {
  const QString envOverride = qEnvironmentVariable("DINERO_SV2_POOL");
  if (!envOverride.isEmpty()) return envOverride.trimmed();
  QSettings settings;
  QString saved = settings.value("mining/sv2_endpoint").toString().trimmed();
  if (saved.startsWith("172.93.160.131")) {
    settings.remove("mining/sv2_endpoint");
    saved.clear();
  }
  return saved.isEmpty() ? sv2PoolEndpointDefault() : saved;
}

QString sv2PoolServerPubkey() {
  const QString envOverride = qEnvironmentVariable("DINERO_SV2_SERVER_PUBKEY");
  if (!envOverride.isEmpty()) return envOverride.trimmed();
  QSettings settings;
  QString saved = settings.value("mining/sv2_server_pubkey").toString().trimmed();
  if (saved.startsWith("17fc0efc") ||
      saved == QStringLiteral("bcaa90dba639e2d57baa4c6de8c88647a82f02669cb0395f0d9a44c0e4ec2931")) {
    settings.remove("mining/sv2_server_pubkey");
    saved.clear();
  }
  return saved.isEmpty() ? sv2PoolServerPubkeyDefault() : saved;
}

QString sv2GpuMinerBinaryName() {
#ifdef Q_OS_WIN
  return "dinero-sv2-gpu-miner.exe";
#else
  return "dinero-sv2-gpu-miner";
#endif
}
QString sv2GpuMinerSettingsKey() { return "mining/sv2_gpu_miner_path"; }
QString sv2GpuMinerEnvVar()      { return "DINERO_SV2_GPU_MINER_PATH"; }

QString sv2MinerBinaryNameForBackend(bool useGpu) {
  return useGpu ? sv2GpuMinerBinaryName() : sv2MinerBinaryName();
}

QString sv2MinerSettingsKeyForBackend(bool useGpu) {
  return useGpu ? sv2GpuMinerSettingsKey() : sv2MinerSettingsKey();
}

QString sv2MinerEnvVarForBackend(bool useGpu) {
  return useGpu ? sv2GpuMinerEnvVar() : sv2MinerEnvVar();
}

QString sv2MinerDisplayNameForBackend(bool useGpu) {
  return useGpu ? QStringLiteral("dinero-sv2-gpu-miner")
                : QStringLiteral("dinero-sv2-miner");
}

QString sv2MinerCargoPackageForBackend(bool useGpu) {
  return useGpu ? QStringLiteral("dinero-sv2-gpu-miner")
                : QStringLiteral("dinero-sv2-miner");
}

bool sv2PathMatchesBackend(const QString& path, bool useGpu) {
  return QFileInfo(path).fileName() == sv2MinerBinaryNameForBackend(useGpu);
}

QString discoverSv2MinerPath(bool useGpu, bool allowSavedPath, bool persistDiscovered) {
  const QString binaryName = sv2MinerBinaryNameForBackend(useGpu);
  const QString settingsKey = sv2MinerSettingsKeyForBackend(useGpu);
  const QString envVarName = sv2MinerEnvVarForBackend(useGpu);

  const QString appDir = QCoreApplication::applicationDirPath();
  QStringList candidates;
  auto add = [&candidates](const QString& p) {
    if (p.isEmpty()) return;
    const QString abs = QFileInfo(p).absoluteFilePath();
    if (QFile::exists(abs) && !candidates.contains(abs)) candidates << abs;
  };

  add(qEnvironmentVariable(envVarName.toUtf8().constData()));
#if defined(Q_OS_MAC)
  add(QDir(appDir).absoluteFilePath(binaryName));
  add(QDir(appDir).absoluteFilePath("../Resources/" + binaryName));
#endif

  if (!candidates.isEmpty()) {
    if (persistDiscovered) {
      QSettings().remove(settingsKey);
    }
    return candidates.first();
  }

  if (allowSavedPath) {
    const QString savedPath = QSettings().value(settingsKey).toString().trimmed();
    if (!savedPath.isEmpty() &&
        QFile::exists(savedPath) &&
        sv2PathMatchesBackend(savedPath, useGpu)) {
      return QFileInfo(savedPath).absoluteFilePath();
    }
  }

#if defined(Q_OS_MAC)
  add(QDir(appDir).absoluteFilePath("../../../" + binaryName));
  add(QDir(appDir).absoluteFilePath("../../../bin/" + binaryName));
  add(QDir(appDir).absoluteFilePath("../../../../../../dinero-sv2/target/release/" + binaryName));
#elif defined(Q_OS_WIN)
  add(QDir(appDir).absoluteFilePath(binaryName));
  add(QDir(appDir).absoluteFilePath("../" + binaryName));
#else
  add(QDir(appDir).absoluteFilePath(binaryName));
  add(QDir(appDir).absoluteFilePath("../" + binaryName));
  add(QDir(appDir).absoluteFilePath("../../dinero-sv2/target/release/" + binaryName));
#endif
  add(QDir::homePath() + "/src/dinero-sv2/target/release/" + binaryName);
  add(QStandardPaths::findExecutable(binaryName));

  if (candidates.isEmpty()) {
    return QString();
  }

  if (persistDiscovered) {
    QSettings().setValue(settingsKey, candidates.first());
  }
  return candidates.first();
}

// Minimal bech32m decoder — enough to turn a `din1p…` (Taproot) or
// `din1r…` (P2MR) address into its scriptPubKey hex for SV2 coinbase.
// Based on BIP-350. Returns empty string on any parsing failure.
QString addressToScriptPubKeyHex(const QString& addr_in) {
  static const QString CHARSET = QStringLiteral("qpzry9x8gf2tvdw0s3jn54khce6mua7l");
  const QString addr = addr_in.trimmed().toLower();
  const int sep = addr.lastIndexOf('1');
  if (sep < 1 || sep + 7 > addr.length()) return QString();

  const QString hrp = addr.left(sep);
  if (hrp != "din" && hrp != "tdin" && hrp != "rdin") return QString();

  // 5-bit data + 6-char checksum; discard the checksum but verify length.
  QVector<int> data5;
  data5.reserve(addr.length() - sep - 1);
  for (int i = sep + 1; i < addr.length(); ++i) {
    const int v = CHARSET.indexOf(addr.at(i));
    if (v < 0) return QString();
    data5.append(v);
  }
  if (data5.size() < 7) return QString();  // must hold at least version + program + checksum

  const int version = data5.first();
  if (version != 1 && version != 2) return QString();  // only Taproot / P2MR

  // Drop witness version (1 char) and checksum (6 chars), convert 5-bit → 8-bit.
  const int progLen5 = data5.size() - 1 - 6;
  QVector<uint8_t> program;
  {
    int acc = 0;
    int bits = 0;
    for (int i = 0; i < progLen5; ++i) {
      acc = (acc << 5) | data5.at(1 + i);
      bits += 5;
      while (bits >= 8) {
        bits -= 8;
        program.append(static_cast<uint8_t>((acc >> bits) & 0xff));
      }
    }
    // Leftover bits must be zero per BIP-173/350.
    if (bits >= 5 || ((acc << (8 - bits)) & 0xff) != 0) return QString();
  }
  if (program.size() != 32) return QString();  // Dinero v7: both surfaces are 32-byte keys

  // scriptPubKey: OP_<version> (0x50 + version) + 0x20 push + 32 bytes.
  const uint8_t opVersion = static_cast<uint8_t>(0x50 + version);
  QByteArray script;
  script.append(static_cast<char>(opVersion));
  script.append(static_cast<char>(0x20));
  for (uint8_t b : program) script.append(static_cast<char>(b));
  return QString::fromLatin1(script.toHex());
}

int localStratumPort() {
  return 3333;
}

QString findLocalStratumServerBinary() {
  const QString binaryName = stratumServerBinaryName();
  QStringList candidates;
  auto addCandidate = [&candidates](const QString& path) {
    if (path.isEmpty()) return;
    const QString abs = QFileInfo(path).absoluteFilePath();
    if (QFile::exists(abs) && !candidates.contains(abs)) {
      candidates << abs;
    }
  };

  const QString appDir = QCoreApplication::applicationDirPath();
  addCandidate(qEnvironmentVariable("DINERO_STRATUM_PATH"));

#if defined(Q_OS_MAC)
  addCandidate(QDir(appDir).absoluteFilePath(binaryName));
  addCandidate(QDir(appDir).absoluteFilePath("../Resources/" + binaryName));
  addCandidate(QDir(appDir).absoluteFilePath("../../../" + binaryName));
  addCandidate(QDir(appDir).absoluteFilePath("../../../bin/" + binaryName));
  addCandidate(QDir(appDir).absoluteFilePath("../../../../../../stratum/build/bin/" + binaryName));
#else
  addCandidate(QDir(appDir).absoluteFilePath(binaryName));
  addCandidate(QDir(appDir).absoluteFilePath("../build/" + binaryName));
  addCandidate(QDir(appDir).absoluteFilePath("../build-clean/" + binaryName));
#endif

  addCandidate(QStandardPaths::findExecutable(binaryName));
  return candidates.isEmpty() ? QString() : candidates.first();
}

bool rpcMinerRequiresInsecureAck(const QString& helpText) {
  return helpText.contains("--i-know-this-is-insecure", Qt::CaseInsensitive);
}

bool walletUnlockedFromRpc(const QJsonObject& obj) {
  // Prefer the explicit "unlocked" field when present (canonical source).
  if (obj.contains("unlocked")) {
    return obj.value("unlocked").toBool(false);
  }

  // Fallback: derive from "locked" field.  An unencrypted wallet is always
  // considered unlocked (no passphrase needed), but an encrypted wallet
  // respects the daemon's locked state.
  if (obj.contains("locked")) {
    const bool locked = obj.value("locked").toBool(true);
    const bool encrypted = obj.value("encrypted").toBool(false);
    if (!encrypted) {
      return true;  // unencrypted wallet is always unlocked
    }
    return !locked;
  }

  if (obj.contains("encrypted")) {
    return !obj.value("encrypted").toBool(false);
  }

  return false;
}

QString miningStatusActiveText() {
  return QStringLiteral("Mining - Dinero: Real Money For Free People - Post-Quantum Native");
}

QString miningStatusInactiveText() {
  return QStringLiteral("Not-Mining - Dinero: Real Money For Free People - Post-Quantum Native");
}

QString chromePillStyle() {
  return QStringLiteral(
    "QLabel { padding: 5px 10px; background: #272c33; color: #d6dde6; "
    "border: 1px solid #3a4048; border-radius: 6px; font-weight: 600; }");
}

QString chromeButtonStyle() {
  return QStringLiteral(
    "QPushButton { padding: 6px 12px; background: #2b3037; color: #e6ebf1; "
    "border: 1px solid #3c434d; border-radius: 7px; font-weight: 600; } "
    "QPushButton:hover { background: #333942; } "
    "QPushButton:pressed { background: #262b31; } "
    "QPushButton:disabled { background: #21252a; color: #7f8893; border: 1px solid #30353d; }");
}

QString chromeSectionLabelStyle() {
  return QStringLiteral(
    "QLabel { font-size: 16px; font-weight: 600; color: #d6dde6; }");
}

QString chromeTableStyle() {
  return QStringLiteral(
    "QTableWidget { gridline-color: #3a4048; background: #1d2126; color: #d5dde6; "
    "alternate-background-color: #22272f; selection-background-color: #3e4550; "
    "selection-color: #eef2f6; border: 1px solid #373d46; border-radius: 8px; } "
    "QTableWidget::item { padding: 4px; } "
    "QHeaderView::section { background: #272c33; color: #d5dde6; padding: 5px; "
    "font-weight: 600; border: 1px solid #373d46; }");
}

QString chromeProgressBarStyle(const QString& chunkColor = QStringLiteral("#3a4048")) {
  return QString(
    "QProgressBar { border: 1px solid #3a4048; border-radius: 5px; background: #1f2328; } "
    "QProgressBar::chunk { background: %1; border-radius: 4px; }")
      .arg(chunkColor);
}

QString headerButtonStyle() {
  return QStringLiteral(
    "QPushButton { min-height: 30px; max-height: 30px; padding: 0 10px; "
    "background: #2b3037; color: #e6ebf1; border: 1px solid #3c434d; "
    "border-radius: 8px; font-size: 12px; font-weight: 600; } "
    "QPushButton:hover { background: #333942; } "
    "QPushButton:pressed { background: #262b31; } "
    "QPushButton:disabled { background: #21252a; color: #7f8893; border: 1px solid #30353d; }");
}

QString headerPillStyle() {
  return QStringLiteral(
    "QLabel { min-height: 30px; max-height: 30px; padding: 0 9px; "
    "background: #272c33; color: #d6dde6; border: 1px solid #3a4048; "
    "border-radius: 8px; font-size: 12px; font-weight: 600; }");
}

QScrollArea* makeScrollableTab(QWidget* page) {
  return ScrollSupport::wrapInScrollArea(page);
}

QString miningControlFieldStyle() {
  return QStringLiteral(
    "QLineEdit { min-height: 30px; max-height: 30px; background: #1f2328; color: #d7dde5; "
    "border: 1px solid #353b44; border-radius: 8px; padding: 0 10px; font-size: 13px; }");
}

QString miningConsoleFontFamily() {
  // Prefer Space Mono (user-selected), then deterministic monospace fallbacks.
  const QStringList preferred = {
    QStringLiteral("Space Mono"),
    QStringLiteral("SpaceMono-Regular"),
    QStringLiteral("SpaceMono"),
    QStringLiteral("SF Mono"),
    QStringLiteral("Menlo"),
    QStringLiteral("Monaco")
  };
  const QStringList installed = QFontDatabase::families();
  for (const QString& family : preferred) {
    if (installed.contains(family, Qt::CaseInsensitive)) {
      return family;
    }
  }
  return QStringLiteral("monospace");
}

QFont miningConsoleFont(int pointSize, QFont::Weight weight = QFont::Medium) {
  QFont font(miningConsoleFontFamily());
  font.setStyleHint(QFont::TypeWriter);
  font.setFixedPitch(true);
  font.setPointSize(pointSize);
  font.setWeight(weight);
  return font;
}

QString formatHashrateText(double hashrateHps) {
  double value = hashrateHps;
  QString unit = "H/s";

  if (value >= 1e12) {
    value /= 1e12;
    unit = "TH/s";
  } else if (value >= 1e9) {
    value /= 1e9;
    unit = "GH/s";
  } else if (value >= 1e6) {
    value /= 1e6;
    unit = "MH/s";
  } else if (value >= 1e3) {
    value /= 1e3;
    unit = "KH/s";
  }

  const int precision = value >= 100.0 ? 1 : 2;
  return QString("%1 %2").arg(value, 0, 'f', precision).arg(unit);
}

bool explorerIsHex64(const QString& value) {
  static const QRegularExpression hex64(QStringLiteral("^[0-9a-fA-F]{64}$"));
  return hex64.match(value).hasMatch();
}

bool explorerIsAddress(const QString& value) {
  const QString lower = value.toLower();
  return lower.startsWith("din1") || lower.startsWith("tdin1") || lower.startsWith("rdin1");
}

QString explorerAddressType(const QString& address) {
  const QString lower = address.toLower();
  if (lower.startsWith("din1r") || lower.startsWith("tdin1r") || lower.startsWith("rdin1r")) {
    return QStringLiteral("P2MR Address");
  }
  if (lower.startsWith("din1p") || lower.startsWith("tdin1p") || lower.startsWith("rdin1p")) {
    return QStringLiteral("Taproot Address");
  }
  if (lower.startsWith("din1q") || lower.startsWith("tdin1q") || lower.startsWith("rdin1q")) {
    return QStringLiteral("SegWit Address");
  }
  return QStringLiteral("Address");
}

QString explorerShortValue(const QString& value, int leading = 16, int trailing = 8) {
  if (value.isEmpty()) {
    return QStringLiteral("-");
  }
  if (value.size() <= leading + trailing + 3) {
    return value;
  }
  return value.left(leading) + QStringLiteral("...") + value.right(trailing);
}

QString explorerJsonString(const QJsonValue& value) {
  if (value.isString()) {
    return value.toString();
  }
  if (value.isDouble()) {
    const double number = value.toDouble();
    const qint64 asInt = static_cast<qint64>(number);
    if (qFuzzyCompare(number + 1.0, static_cast<double>(asInt) + 1.0)) {
      return QString::number(asInt);
    }
    return QString::number(number, 'f', 8);
  }
  if (value.isBool()) {
    return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  }
  return QString();
}

qint64 explorerJsonInt64(const QJsonValue& value, bool* okOut = nullptr) {
  bool ok = false;
  qint64 result = 0;
  if (value.isString()) {
    result = value.toString().toLongLong(&ok);
    if (!ok) {
      const double asDouble = value.toString().toDouble(&ok);
      if (ok) {
        result = static_cast<qint64>(std::llround(asDouble));
      }
    }
  } else if (value.isDouble()) {
    result = static_cast<qint64>(std::llround(value.toDouble()));
    ok = true;
  }
  if (okOut) {
    *okOut = ok;
  }
  return result;
}

double explorerJsonDouble(const QJsonValue& value, bool* okOut = nullptr) {
  bool ok = false;
  double result = 0.0;
  if (value.isString()) {
    result = value.toString().toDouble(&ok);
  } else if (value.isDouble()) {
    result = value.toDouble();
    ok = true;
  }
  if (okOut) {
    *okOut = ok;
  }
  return result;
}

QString explorerIntegerText(qint64 value) {
  return QLocale().toString(value);
}

QString explorerTrimDecimal(QString text) {
  while (text.contains('.') && text.endsWith('0')) {
    text.chop(1);
  }
  if (text.endsWith('.')) {
    text.chop(1);
  }
  return text.isEmpty() ? QStringLiteral("0") : text;
}

QString explorerDinFromUna(qint64 una) {
  return explorerTrimDecimal(QString::number(static_cast<double>(una) / 100000000.0, 'f', 8));
}

QString explorerDinString(const QString& value) {
  bool ok = false;
  const double din = value.toDouble(&ok);
  if (!ok) {
    return value.isEmpty() ? QStringLiteral("-") : value;
  }
  return explorerTrimDecimal(QString::number(din, 'f', 8));
}

QString overviewDinText(const QJsonValue& value) {
  bool ok = false;
  const double din = explorerJsonDouble(value, &ok);
  if (!ok) {
    return {};
  }
  return explorerTrimDecimal(QLocale().toString(din, 'f', 8));
}

QString overviewDinTextFromUna(const QJsonValue& value) {
  bool ok = false;
  const qint64 una = explorerJsonInt64(value, &ok);
  if (!ok) {
    return {};
  }
  return overviewDinText(QString::number(static_cast<double>(una) / 100000000.0, 'f', 8));
}

QString explorerRelativeTime(qint64 unixTime) {
  if (unixTime <= 0) {
    return QStringLiteral("-");
  }
  const qint64 now = QDateTime::currentSecsSinceEpoch();
  const qint64 diff = std::max<qint64>(0, now - unixTime);
  if (diff < 60) {
    return QStringLiteral("%1s ago").arg(diff);
  }
  if (diff < 3600) {
    return QStringLiteral("%1m ago").arg(diff / 60);
  }
  if (diff < 86400) {
    return QStringLiteral("%1h ago").arg(diff / 3600);
  }
  return QStringLiteral("%1d ago").arg(diff / 86400);
}

QString explorerFullDate(qint64 unixTime) {
  if (unixTime <= 0) {
    return QStringLiteral("-");
  }
  return QDateTime::fromSecsSinceEpoch(unixTime, Qt::UTC)
      .toString(QStringLiteral("MMM d yyyy, HH:mm:ss 'UTC'"));
}

QString explorerScriptPubKey(const QJsonObject& output) {
  const QJsonValue scriptValue = output.value("scriptPubKey");
  if (scriptValue.isString()) {
    return scriptValue.toString();
  }
  if (scriptValue.isObject()) {
    const QJsonObject script = scriptValue.toObject();
    if (script.value("hex").isString()) {
      return script.value("hex").toString();
    }
  }
  return output.value("script_pubkey").toString();
}

QString explorerOutputAddress(const QJsonObject& output) {
  if (output.value("address").isString()) {
    return output.value("address").toString();
  }
  const QJsonValue scriptValue = output.value("scriptPubKey");
  if (scriptValue.isObject()) {
    const QJsonObject script = scriptValue.toObject();
    if (script.value("address").isString()) {
      return script.value("address").toString();
    }
    const QJsonArray addresses = script.value("addresses").toArray();
    if (!addresses.isEmpty() && addresses.first().isString()) {
      return addresses.first().toString();
    }
  }
  return QString();
}

QString explorerOutputAmount(const QJsonObject& output, qint64* unaOut = nullptr) {
  if (unaOut) {
    *unaOut = 0;
  }
  if (output.value("is_confidential").toBool(false) ||
      output.value("amount_hidden").toBool(false)) {
    return QStringLiteral("hidden");
  }

  const QJsonValue valueDin = output.contains("value_din")
      ? output.value("value_din")
      : output.value("display_amount");
  if (!valueDin.isUndefined()) {
    const QString text = explorerJsonString(valueDin);
    return text.isEmpty() ? QStringLiteral("-") : explorerDinString(text) + QStringLiteral(" DIN");
  }

  const QJsonValue unaValue = output.contains("value_una")
      ? output.value("value_una")
      : output.contains("amount_una") ? output.value("amount_una") : output.value("value");
  bool ok = false;
  const qint64 una = explorerJsonInt64(unaValue, &ok);
  if (ok) {
    if (unaOut) {
      *unaOut = una;
    }
    return explorerDinFromUna(una) + QStringLiteral(" DIN");
  }

  return QStringLiteral("-");
}

QString explorerAddressAmount(const QJsonValue& value, bool valueIsDin) {
  const QString text = explorerJsonString(value).trimmed();
  if (text.isEmpty()) {
    return QStringLiteral("-");
  }
  if (valueIsDin || text.contains('.')) {
    return explorerDinString(text) + QStringLiteral(" DIN");
  }

  bool ok = false;
  const qint64 una = explorerJsonInt64(value, &ok);
  return ok ? explorerDinFromUna(una) + QStringLiteral(" DIN") : text;
}

QString explorerOutputType(const QJsonObject& output) {
  const QString address = explorerOutputAddress(output).toLower();
  const QString script = explorerScriptPubKey(output).toLower();
  const QString rawType = output.value("type").toString();

  if (output.value("is_confidential").toBool(false) ||
      output.value("amount_hidden").toBool(false)) {
    return QStringLiteral("Confidential");
  }
  if (address.startsWith("din1r") || address.startsWith("tdin1r") || address.startsWith("rdin1r") ||
      script.startsWith("5320")) {
    return QStringLiteral("P2MR");
  }
  if (address.startsWith("din1p") || address.startsWith("tdin1p") || address.startsWith("rdin1p") ||
      script.startsWith("5120")) {
    return QStringLiteral("Taproot");
  }
  if (script.startsWith("6a")) {
    return QStringLiteral("OP_RETURN");
  }
  return rawType.isEmpty() || rawType == "legacy" ? QStringLiteral("Standard") : rawType;
}

QString explorerTransactionType(const QJsonObject& tx) {
  if (tx.value("is_coinbase").toBool(false) || tx.value("coinbase").toBool(false)) {
    return QStringLiteral("Coinbase");
  }
  const QJsonArray outputs = tx.contains("outputs") ? tx.value("outputs").toArray() : tx.value("vout").toArray();
  bool hasTaproot = false;
  bool hasP2mr = false;
  for (const QJsonValue& value : outputs) {
    const QString type = explorerOutputType(value.toObject());
    hasP2mr = hasP2mr || type == "P2MR";
    hasTaproot = hasTaproot || type == "Taproot";
  }
  if (hasP2mr) {
    return QStringLiteral("P2MR");
  }
  if (hasTaproot) {
    return QStringLiteral("Taproot");
  }
  if (tx.value("has_confidential_inputs").toBool(false) ||
      tx.value("has_confidential_outputs").toBool(false)) {
    return QStringLiteral("Confidential");
  }
  const QString classification = tx.value("classification").toString(tx.value("type").toString());
  return classification.isEmpty() ? QStringLiteral("Standard") : classification;
}

QTableWidgetItem* explorerItem(const QString& text, const QString& tooltip = QString()) {
  auto* item = new QTableWidgetItem(text);
  item->setToolTip(tooltip.isEmpty() ? text : tooltip);
  item->setFlags(item->flags() & ~Qt::ItemIsEditable);
  return item;
}

QString formatBytesText(qint64 bytes) {
  const double value = static_cast<double>(std::max<qint64>(0, bytes));
  const double kib = 1024.0;
  const double mib = kib * 1024.0;
  const double gib = mib * 1024.0;

  if (value >= gib) {
    return QString("%1 GiB").arg(value / gib, 0, 'f', 2);
  }
  if (value >= mib) {
    return QString("%1 MiB").arg(value / mib, 0, 'f', 1);
  }
  if (value >= kib) {
    return QString("%1 KiB").arg(value / kib, 0, 'f', 1);
  }
  return QString("%1 B").arg(static_cast<qlonglong>(value));
}

double parseHashrateToHps(const QString& valueText, const QString& unitText) {
  bool ok = false;
  const double value = valueText.toDouble(&ok);
  if (!ok) {
    return 0.0;
  }

  const QString unit = unitText.trimmed().toUpper();
  if (unit == "TH/S") return value * 1e12;
  if (unit == "GH/S") return value * 1e9;
  if (unit == "MH/S") return value * 1e6;
  if (unit == "KH/S") return value * 1e3;
  return value;
}

QString titleCaseWord(const QString& text) {
  if (text.isEmpty()) {
    return text;
  }
  QString normalized = text.trimmed().toLower();
  normalized[0] = normalized[0].toUpper();
  return normalized;
}

QStringList buildRpcMinerArgs(const QString& cookiePath,
                              const QString& miningAddress,
                              const QString& threads,
                              bool includeInsecureAck) {
  QStringList args;
  args << "--rpc" << "http://127.0.0.1:20998/"
       << "--cookie" << cookiePath
       << "--address" << miningAddress
       << "--threads" << threads;
  if (includeInsecureAck) {
    args << "--i-know-this-is-insecure";
  }
  return args;
}

QStringList buildStratumMinerArgs(const QString& miningAddress,
                                  const QString& endpoint,
                                  const QString& threads,
                                  QString* userOut) {
  // Stratum identity policy:
  // username = payout Taproot address, password = static dummy ("x")
  const QString user = miningAddress;
  const QString password = "x";

  if (userOut) {
    *userOut = user;
  }

  QStringList args;
  args << "--stratum" << endpoint
       << "--user" << user
       << "--password" << password
       << "--threads" << threads;
  return args;
}

QString explicitStratumEndpoint() {
  const QString envEndpoint = qEnvironmentVariable("DINERO_STRATUM_ENDPOINT").trimmed();
  if (!envEndpoint.isEmpty()) {
    return envEndpoint;
  }
  return QSettings().value("mining/stratum_endpoint").toString().trimmed();
}

QString compactProcessOutput(const QString& rawOutput) {
  QString out = rawOutput.trimmed();
  if (out.length() > 500) {
    out = out.left(500) + "...";
  }
  return out;
}

QString peerHeightDisplayText(int peerHeight, int localTip) {
  if (peerHeight < 0) {
    return QStringLiteral("N/A");
  }
  if (localTip > 0 && peerHeight + 2 < localTip) {
    return QString("%1 seen").arg(peerHeight);
  }
  return QString::number(peerHeight);
}

QString peerHeightBreakdownTooltip(int startHeight,
                                   int syncedHeaders,
                                   int syncedBlocks,
                                   int bestKnown,
                                   int localTip) {
  auto fmt = [](int value) -> QString {
    return value >= 0 ? QString::number(value) : QStringLiteral("N/A");
  };

  QString tooltip =
      QString("Last-seen peer height from local P2P telemetry.\n"
              "This is not a live RPC query to that server.\n\n"
              "Breakdown\n"
              "startingheight: %1\n"
              "synced_headers: %2\n"
              "synced_blocks: %3\n"
              "best_known_height: %4")
          .arg(fmt(startHeight), fmt(syncedHeaders), fmt(syncedBlocks), fmt(bestKnown));
  if (localTip > 0) {
    tooltip += QString("\nlocal node tip: %1").arg(localTip);
  }
  return tooltip;
}

QString miningReadinessSummaryText(const QJsonObject& readiness) {
  if (readiness.isEmpty()) {
    return QStringLiteral("Readiness: waiting for daemon mining state");
  }

  const bool ready = readiness.value("ready").toBool(false);
  const QString reason = readiness.value("reason").toString(QStringLiteral("unknown"));
  const qint64 local = readiness.value("local_height").toInteger(-1);
  const qint64 network = readiness.value("network_height_estimate").toInteger(-1);
  const qint64 peers = readiness.value("peer_count").toInteger(-1);

  const QString state = ready ? QStringLiteral("Ready")
                              : QStringLiteral("Paused (%1)").arg(reason);
  return QStringLiteral("Readiness: %1 | local %2 | network %3 | peers %4")
      .arg(state)
      .arg(local >= 0 ? QString::number(local) : QStringLiteral("N/A"))
      .arg(network >= 0 ? QString::number(network) : QStringLiteral("N/A"))
      .arg(peers >= 0 ? QString::number(peers) : QStringLiteral("N/A"));
}

QString miningReadinessTooltipText(const QJsonObject& readiness) {
  if (readiness.isEmpty()) {
    return QStringLiteral("No mining readiness data received from the daemon yet.");
  }

  auto readInt = [&readiness](const char* key) -> QString {
    const qint64 value = readiness.value(QString::fromUtf8(key)).toInteger(-1);
    return value >= 0 ? QString::number(value) : QStringLiteral("N/A");
  };

  return QStringLiteral(
      "Daemon mining readiness\n"
      "ready: %1\n"
      "reason: %2\n"
      "message: %3\n"
      "local_height: %4\n"
      "network_height_estimate: %5\n"
      "peer_best_height: %6\n"
      "peer_median_height: %7\n"
      "peer_count: %8\n"
      "min_peers: %9\n"
      "peer_freshest_age_seconds: %10\n"
      "max_peer_staleness_seconds: %11")
      .arg(readiness.value("ready").toBool(false) ? QStringLiteral("true") : QStringLiteral("false"))
      .arg(readiness.value("reason").toString(QStringLiteral("unknown")))
      .arg(readiness.value("message").toString(QStringLiteral("N/A")))
      .arg(readInt("local_height"))
      .arg(readInt("network_height_estimate"))
      .arg(readInt("peer_best_height"))
      .arg(readInt("peer_median_height"))
      .arg(readInt("peer_count"))
      .arg(readInt("min_peers"))
      .arg(readInt("peer_freshest_age_seconds"))
      .arg(readInt("max_peer_staleness_seconds"));
}

QString miningReadinessFingerprint(const QJsonObject& readiness) {
  if (readiness.isEmpty()) {
    return QStringLiteral("no-readiness");
  }

  return QStringLiteral("%1|%2|%3|%4|%5|%6")
      .arg(readiness.value("ready").toBool(false) ? 1 : 0)
      .arg(readiness.value("reason").toString())
      .arg(readiness.value("local_height").toInteger(-1))
      .arg(readiness.value("network_height_estimate").toInteger(-1))
      .arg(readiness.value("peer_best_height").toInteger(-1))
      .arg(readiness.value("peer_count").toInteger(-1));
}

QString probeBinaryVersion(const QString& binaryPath, QString* probeErrorOut = nullptr) {
  QProcess probe;
  probe.setProcessChannelMode(QProcess::MergedChannels);
  probe.start(binaryPath, QStringList() << "--version");

  if (!probe.waitForStarted(2000)) {
    if (probeErrorOut) {
      *probeErrorOut = probe.errorString();
    }
    return QString();
  }

  if (!probe.waitForFinished(2500)) {
    probe.kill();
    probe.waitForFinished(500);
  }

  const QString output = QString::fromUtf8(probe.readAllStandardOutput()).trimmed();
  if (output.isEmpty() && probeErrorOut) {
    *probeErrorOut = "No version output from miner";
  }
  return compactProcessOutput(output).replace('\n', " | ");
}

QString binarySha256(const QString& binaryPath) {
  QFile file(binaryPath);
  if (!file.open(QIODevice::ReadOnly)) {
    return QString();
  }

  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!file.atEnd()) {
    hash.addData(file.read(1024 * 1024));
  }
  return QString::fromLatin1(hash.result().toHex());
}

void appendBinaryIdentityToMiningLog(QTextEdit* output, const QString& label, const QString& binaryPath) {
  if (!output || binaryPath.isEmpty()) {
    return;
  }

  const QFileInfo info(binaryPath);
  output->append(QString("[%1] Path: %2").arg(label, info.absoluteFilePath()));

  QString versionProbeError;
  const QString version = probeBinaryVersion(info.absoluteFilePath(), &versionProbeError);
  if (!version.isEmpty()) {
    output->append(QString("[%1] Version: %2").arg(label, version));
  } else if (!versionProbeError.isEmpty()) {
    output->append(QString("[%1] Version probe failed: %2").arg(label, versionProbeError));
  }

  const QString sha256 = binarySha256(info.absoluteFilePath());
  if (!sha256.isEmpty()) {
    output->append(QString("[%1] SHA256: %2").arg(label, sha256.left(16)));
  }

  output->append(QString("[%1] Size: %2 bytes | Modified: %3")
                   .arg(label)
                   .arg(info.size())
                   .arg(info.lastModified().toString(Qt::ISODate)));
}

QString resolveBundledAssetPath(const QString& fileName) {
  const QString appDir = QCoreApplication::applicationDirPath();
  QStringList candidates;
#if defined(Q_OS_MAC)
  candidates << QDir(appDir).absoluteFilePath(QStringLiteral("../Resources/") + fileName);
#endif
  candidates << QDir(appDir).absoluteFilePath(fileName)
             << QDir(appDir).absoluteFilePath(QStringLiteral("../") + fileName)
             << QDir::current().absoluteFilePath(fileName);

  for (const QString& candidate : candidates) {
    const QFileInfo info(candidate);
    if (info.exists() && info.isFile()) {
      return info.absoluteFilePath();
    }
  }
  return QString();
}

QString ensureDineroTemplateGlyphPath() {
  const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  const QString basePath = cacheRoot.isEmpty() ? QDir::tempPath() : cacheRoot;
  QDir cacheDir(basePath);
  if (!cacheDir.exists()) {
    cacheDir.mkpath(QStringLiteral("."));
  }

  const QString iconPath = cacheDir.filePath(QStringLiteral("dinero-template-symbol.png"));

  const int iconSize = 20;
  QPixmap icon(iconSize, iconSize);
  icon.fill(Qt::transparent);

  QPainter painter(&icon);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);

  QFont font(QStringLiteral("Avenir Next"));
  font.setWeight(QFont::DemiBold);
  font.setPixelSize(17);
  painter.setFont(font);

  const QString glyph = QStringLiteral("D");
  const QFontMetrics fm(font);
  const int glyphWidth = fm.horizontalAdvance(glyph);
  const int x = (iconSize - glyphWidth) / 2;
  const int baseline = (iconSize + fm.ascent() - fm.descent()) / 2;

  QPainterPath glyphPath;
  glyphPath.addText(x, baseline, font, glyph);
  painter.fillPath(glyphPath, QColor(118, 114, 110));

  painter.save();
  painter.setClipPath(glyphPath);
  QPen linePen(QColor(64, 62, 59));
  linePen.setWidthF(1.7);
  linePen.setCapStyle(Qt::RoundCap);
  painter.setPen(linePen);
  const int top = 1;
  const int bottom = iconSize - 1;
  const int xLeft = (iconSize / 2) - 2;
  const int xRight = (iconSize / 2) + 2;
  painter.drawLine(xLeft, top, xLeft, bottom);
  painter.drawLine(xRight, top, xRight, bottom);
  painter.restore();

  QPen edgePen(QColor(92, 88, 84, 170));
  edgePen.setWidthF(0.8);
  painter.setPen(edgePen);
  painter.setBrush(Qt::NoBrush);
  painter.drawPath(glyphPath);
  painter.end();

  if (icon.save(iconPath, "PNG")) {
    return QFileInfo(iconPath).absoluteFilePath();
  }
  return QString();
}

QString ensureDineroBlockCoinGlyphPath() {
  const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  const QString basePath = cacheRoot.isEmpty() ? QDir::tempPath() : cacheRoot;
  QDir cacheDir(basePath);
  if (!cacheDir.exists()) {
    cacheDir.mkpath(QStringLiteral("."));
  }

  const QString iconPath = cacheDir.filePath(QStringLiteral("dinero-block-coin-symbol.png"));
  const int iconSize = 20;
  QPixmap icon(iconSize, iconSize);
  icon.fill(Qt::transparent);

  QPainter painter(&icon);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);

  // Perfect circular coin.
  const QRectF circleRect(1.0, 1.0, iconSize - 2.0, iconSize - 2.0);
  painter.setBrush(QColor(245, 195, 52));
  QPen rimPen(QColor(216, 158, 22));
  rimPen.setWidthF(1.0);
  painter.setPen(rimPen);
  painter.drawEllipse(circleRect);

  QFont font(QStringLiteral("Avenir Next"));
  font.setWeight(QFont::DemiBold);
  font.setPixelSize(13);
  painter.setFont(font);

  const QString glyph = QStringLiteral("D");
  const QFontMetrics fm(font);
  const int glyphWidth = fm.horizontalAdvance(glyph);
  const int x = (iconSize - glyphWidth) / 2;
  const int baseline = (iconSize + fm.ascent() - fm.descent()) / 2 + 1;

  QPainterPath glyphPath;
  glyphPath.addText(x, baseline, font, glyph);
  painter.fillPath(glyphPath, QColor(95, 74, 33));

  painter.save();
  painter.setClipPath(glyphPath);
  QPen linePen(QColor(61, 45, 15));
  linePen.setWidthF(1.25);
  linePen.setCapStyle(Qt::RoundCap);
  painter.setPen(linePen);
  const int top = 2;
  const int bottom = iconSize - 2;
  const int xLeft = (iconSize / 2) - 2;
  const int xRight = (iconSize / 2) + 2;
  painter.drawLine(xLeft, top, xLeft, bottom);
  painter.drawLine(xRight, top, xRight, bottom);
  painter.restore();

  QPen edgePen(QColor(79, 61, 25, 180));
  edgePen.setWidthF(0.8);
  painter.setPen(edgePen);
  painter.setBrush(Qt::NoBrush);
  painter.drawPath(glyphPath);
  painter.end();

  if (icon.save(iconPath, "PNG")) {
    return QFileInfo(iconPath).absoluteFilePath();
  }
  return QString();
}

constexpr char kMiningBlockCardProperty[] = "_dineroMiningBlockCardLines";
constexpr char kMiningOutputTextColor[] = "#f1f4f7";
constexpr char kMiningOutputIdleBackground[] = "#181c22";
constexpr char kMiningOutputMatrixBackground[] = "#14181e";

bool isMiningBlockFoundHeader(const QString& line) {
  return line.contains(QStringLiteral("BLOCK FOUND"), Qt::CaseInsensitive) ||
         line.contains(QStringLiteral("Block found"), Qt::CaseInsensitive);
}

bool isMiningBlockFoundTerminalLine(const QString& line) {
  return line.contains(QStringLiteral("BLOCK ACCEPTED"), Qt::CaseInsensitive) ||
         line.contains(QStringLiteral("BLOCK REJECTED"), Qt::CaseInsensitive) ||
         line.contains(QStringLiteral("accepted"), Qt::CaseInsensitive) ||
         line.contains(QStringLiteral("rejected"), Qt::CaseInsensitive);
}

bool isMiningBlockFoundBoundaryLine(const QString& line) {
  return line.startsWith(QStringLiteral("===")) ||
         line.contains(QStringLiteral("Mining stopped"), Qt::CaseInsensitive) ||
         line.contains(QStringLiteral("New template"), Qt::CaseInsensitive);
}

bool isMiningNativeBoxDecorationLine(const QString& line) {
  const QString trimmed = line.trimmed();
  return trimmed.startsWith(QStringLiteral("╔")) ||
         trimmed.startsWith(QStringLiteral("╚"));
}

QString normalizeMiningOutputLine(const QString& rawLine) {
  QString line = rawLine.trimmed();
  if (line.startsWith(QStringLiteral("📦"))) {
    line.remove(0, QStringLiteral("📦").size());
    line = line.trimmed();
  }
  if (line.startsWith(QStringLiteral("🎉"))) {
    line.remove(0, QStringLiteral("🎉").size());
    line = line.trimmed();
  }
  while (line.startsWith(QStringLiteral("║")) ||
         line.startsWith(QStringLiteral("|")) ||
         line.startsWith(QStringLiteral("||"))) {
    if (line.startsWith(QStringLiteral("||"))) {
      line.remove(0, 2);
    } else {
      line.remove(0, 1);
    }
    line = line.trimmed();
  }
  return line;
}

QStringList miningBlockCardLines(QTextEdit* output) {
  if (!output) {
    return {};
  }
  return output->property(kMiningBlockCardProperty).toStringList();
}

void setMiningBlockCardLines(QTextEdit* output, const QStringList& lines) {
  if (!output) {
    return;
  }
  if (lines.isEmpty()) {
    output->setProperty(kMiningBlockCardProperty, QVariant());
    return;
  }
  output->setProperty(kMiningBlockCardProperty, lines);
}

QString padMiningCardLine(const QString& line, int width) {
  if (line.size() >= width) {
    return line;
  }
  return line + QString(width - line.size(), QLatin1Char(' '));
}

QString renderMiningBlockFoundCard(const QStringList& lines) {
  if (lines.isEmpty()) {
    return QString();
  }
  int maxChars = 0;
  for (const QString& line : lines) {
    maxChars = std::max(maxChars, static_cast<int>(line.size()));
  }

  const int innerWidth = maxChars + 3;
  QStringList textLines;

  for (const QString& line : lines) {
    const QString paddedLine = padMiningCardLine(line, innerWidth);
    textLines << paddedLine;
  }
  return textLines.join(QLatin1Char('\n'));
}

void appendMiningConsoleText(QTextEdit* output, const QString& text) {
  if (!output) {
    return;
  }

  QTextCursor cursor(output->document());
  cursor.movePosition(QTextCursor::End);
  QTextCharFormat format = cursor.charFormat();
  format.setForeground(QColor(kMiningOutputTextColor));
  cursor.setCharFormat(format);

  const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
  bool needsBlock = !output->document()->isEmpty();
  for (const QString& line : lines) {
    if (needsBlock) {
      cursor.insertBlock();
    }
    cursor.insertText(line);
    needsBlock = true;
  }
  output->setTextCursor(cursor);
}

void flushMiningBlockFoundCard(QTextEdit* output) {
  if (!output) {
    return;
  }

  const QStringList pendingLines = miningBlockCardLines(output);
  if (pendingLines.isEmpty()) {
    return;
  }

  const QString block = renderMiningBlockFoundCard(pendingLines);
  if (!block.isEmpty()) {
    appendMiningConsoleText(output, block);
  } else {
    for (const QString& line : pendingLines) {
      appendMiningConsoleText(output, line);
    }
  }
  setMiningBlockCardLines(output, {});
}

void appendMiningOutputLine(QTextEdit* output, const QString& rawLine) {
  if (!output) {
    return;
  }

  const QString line = normalizeMiningOutputLine(rawLine);
  if (line.isEmpty() || isMiningNativeBoxDecorationLine(rawLine)) {
    return;
  }
  const bool isBlockFoundLine = isMiningBlockFoundHeader(line);
  const bool isTerminalBlockFoundLine = isMiningBlockFoundTerminalLine(line);
  QStringList pendingLines = miningBlockCardLines(output);
  const bool hasPendingBlockFound = !pendingLines.isEmpty();

  if (isBlockFoundLine) {
    if (hasPendingBlockFound) {
      flushMiningBlockFoundCard(output);
    }
    setMiningBlockCardLines(output, QStringList{line});
    return;
  }

  if (hasPendingBlockFound) {
    if (isMiningBlockFoundBoundaryLine(line)) {
      flushMiningBlockFoundCard(output);
    } else {
      pendingLines.append(line);
      setMiningBlockCardLines(output, pendingLines);
      if (isTerminalBlockFoundLine || pendingLines.size() >= 16) {
        flushMiningBlockFoundCard(output);
      }
      return;
    }
  }

  appendMiningConsoleText(output, line);
}

void removeMiningOutputLine(QTextEdit* output, const QString& rawLine) {
  if (!output) {
    return;
  }

  const QString line = normalizeMiningOutputLine(rawLine);
  QTextBlock block = output->document()->begin();
  while (block.isValid()) {
    const QTextBlock next = block.next();
    if (block.text() == line) {
      QTextCursor cursor(block);
      cursor.select(QTextCursor::BlockUnderCursor);
      cursor.removeSelectedText();
      if (cursor.position() < output->document()->characterCount() - 1) {
        cursor.deleteChar();
      } else if (cursor.position() > 0) {
        cursor.deletePreviousChar();
      }
    }
    block = next;
  }
}

QString compactSv2Value(const QString& value, int head = 18, int tail = 10) {
  const QString trimmed = value.trimmed();
  if (trimmed.size() <= head + tail + 1) {
    return trimmed;
  }
  return trimmed.left(head) + QStringLiteral("...") + trimmed.right(tail);
}

QString sv2HashTexture(const QString& seed, int groups = 4) {
  QString texture;
  for (int i = 0; i < groups; ++i) {
    const QByteArray digest = QCryptographicHash::hash(
      (seed + QStringLiteral(":") + QString::number(i)).toUtf8(),
      QCryptographicHash::Sha256).toHex();
    if (!texture.isEmpty()) {
      texture += QLatin1Char(' ');
    }
    texture += QString::fromLatin1(digest.left(16));
  }
  return texture;
}

void appendMiningHtmlBlock(QTextEdit* output, const QString& html) {
  if (!output) {
    return;
  }

  flushMiningBlockFoundCard(output);
  QTextCursor cursor(output->document());
  cursor.movePosition(QTextCursor::End);
  if (!output->document()->isEmpty()) {
    cursor.insertBlock();
  }
  cursor.insertHtml(html);
  output->setTextCursor(cursor);
}

void appendSv2EventLine(QTextEdit* output,
                        const QString& label,
                        const QString& detail,
                        const QString& accent,
                        const QString& seed = QString()) {
  const QString time = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
  const QString texture = sv2HashTexture(seed.isEmpty() ? detail : seed, 1).left(12).toHtmlEscaped();
  const QString html =
    QStringLiteral(
      "<div style='white-space:pre; font-family:\"SF Mono\", Menlo, monospace; font-size:10px;'>"
      "<span style='color:#66737d;'>%1</span> "
      "<span style='color:%2; font-weight:700;'>%3</span> "
      "<span style='color:#e5edf1;'>%4</span>"
      " <span style='color:#303b40;'>%5</span>"
      "</div>")
      .arg(time.toHtmlEscaped(),
           accent,
           label.toHtmlEscaped(),
           detail.toHtmlEscaped(),
           texture);
  appendMiningHtmlBlock(output, html);
}

void appendSv2SessionHeader(QTextEdit* output,
                            const QString& backend,
                            const QString& pool,
                            const QString& pubkey,
                            const QString& payout,
                            const QString& payoutScript,
                            const QString& workHint) {
  const QString accent = backend.contains(QStringLiteral("GPU"), Qt::CaseInsensitive)
    ? QStringLiteral("#6ee7f2")
    : QStringLiteral("#95d66b");
  const QString seed = pool + payoutScript + backend;
  const QString html =
    QStringLiteral(
      "<div style='white-space:pre; font-family:\"SF Mono\", Menlo, monospace; font-size:10px;'>"
      "<span style='color:%1; font-weight:700;'>SV2 POOL SESSION</span>"
      " <span style='color:#7d8b94;'>%2</span><br>"
      "<span style='color:#66737d;'>pool</span>   <span style='color:#edf6f2;'>%3</span><br>"
      "<span style='color:#66737d;'>pubkey</span> <span style='color:#edf6f2;'>%4</span><br>"
      "<span style='color:#66737d;'>payout</span> <span style='color:#edf6f2;'>%5</span><br>"
      "<span style='color:#66737d;'>script</span> <span style='color:#b9c9d0;'>%6</span><br>"
      "<span style='color:#66737d;'>work</span>   <span style='color:#edf6f2;'>%7</span><br>"
      "<span style='color:#334047;'>%8</span>"
      "</div>")
      .arg(accent,
           backend.toHtmlEscaped(),
           pool.toHtmlEscaped(),
           compactSv2Value(pubkey, 14, 8).toHtmlEscaped(),
           compactSv2Value(payout, 24, 14).toHtmlEscaped(),
           compactSv2Value(payoutScript, 28, 18).toHtmlEscaped(),
           workHint.toHtmlEscaped(),
           sv2HashTexture(seed, 3).toHtmlEscaped());
  appendMiningHtmlBlock(output, html);
}

}  // namespace

qint64 MainWindow::appUptimeSeconds() const {
    if (app_started_at_ms_ == 0) return 0;
    return (QDateTime::currentMSecsSinceEpoch() - app_started_at_ms_) / 1000;
}

MainWindow::MainWindow(dinero::qt::DaemonBootstrapOwner daemonBootstrapOwner,
                       QWidget* parent)
    : QMainWindow(parent)
    , rpc_(new RpcClient(this))
    , changeAddrMgr_(new ChangeAddressManager(QString(), this))
    , txTracker_(new TransactionTracker(QString(), rpc_, this))
    , bannerQueue_(nullptr)
    , connectionMgr_(new ConnectionManager(this))
#ifdef DIN_EXPERIMENTAL_FEATURES
    , ws_(new WebSocketClient("ws://127.0.0.1:21000", this))
#endif
#ifdef HAVE_QT_QUICK
    , rpcHelper_(new RpcHelper(this))
#endif
    , refreshTimer_(new QTimer(this))
    , miningProcess_(nullptr)
    , localStratumProcess_(nullptr)
    , daemonProcess_(nullptr)
    , suppressErrorDialogs_(false)
#ifdef HAVE_QT_QUICK
    , miningWidget_(nullptr)
#endif
    , miningStatsTimer_(new QTimer(this))
    , miningFocusDimTimer_(new QTimer(this))
    , miningCinematicTimer_(new QTimer(this))
    , btnRescanWallet_(nullptr)
    , singleWalletMode_(false)
    , autoLoadDefaultAttempted_(false)
    , walletUnlocked_(false)
    , walletRescanning_(false)
    , walletSwitchInFlight_(false)
    , shuttingDown_(false)
    , safeModeRescanRetryTimer_(new QTimer(this))
    , safeModeRescanRetryAttempts_(0)
    , safeModeRescanRetryScheduled_(false)
    , unlockCountdownTimer_(new QTimer(this))
    , unlockSecondsRemaining_(0)
    , currentWalletName_("")
    , pendingWalletOpenName_("")
    , lblMiningPhase_(nullptr)
    , lblNextReward_(nullptr)
    , lblDifficulty_(nullptr)
    , txtMiningInfo_(nullptr)
{
  
  setWindowTitle("Dinero Cryptocurrency Wallet - Real Money For Free People");
  setWindowIcon(QIcon(resolveBundledAssetPath(QStringLiteral("Dinero-Coin.png"))));

  // Fit window to screen — never taller than available space
  QRect screenRect = QGuiApplication::primaryScreen()->availableGeometry();
  int maxH = screenRect.height() - 60;
  resize(qMin(1200, screenRect.width()), qMin(maxH, 900));
  setMaximumHeight(maxH); // hard cap — prevents layout from pushing window off screen
  move(screenRect.x() + qMax(0, (screenRect.width() - width()) / 2),
       screenRect.y() + qMax(0, (screenRect.height() - height()) / 2));

  // RPC client auto-discovers and loads cookie in its constructor
#ifdef DIN_EXPERIMENTAL_FEATURES
  // Share the same datadir with WebSocket client for authentication
  ws_->setDatadir(rpc_->datadir());
  bool wsCookieLoaded = ws_->loadCookie();
  if (wsCookieLoaded) {
    qDebug() << "MainWindow: WebSocket cookie loaded successfully";
  } else {
#else
  if (false) {  // Keep same structure when WebSockets disabled
#endif
    qWarning() << "MainWindow: WebSocket cookie loading failed - live updates may not work";
  }

  setupUI();
  
  // ═══════════════════════════════════════════════════════════════
  // 🛡️ ConnectionManager Setup (Bulletproof Connection Management)
  // ═══════════════════════════════════════════════════════════════
  connectionMgr_->setDaemonUrl("http://127.0.0.1:20998");
  connectionMgr_->setCookiePath(QDir(rpc_->datadir()).filePath(".cookie"));
  connectionMgr_->setHealthCheckInterval(5000);   // Check every 5 seconds
  // Windows slow-start: a node with many wallets at high height can take ~150s
  // to serve RPC. 10 retries gave up before that, leaving the GUI disconnected
  // even after dinerod came up. Keep trying well past the init window.
  connectionMgr_->setMaxRetries(40);
  
  // Connect ConnectionManager signals
  QObject::connect(connectionMgr_, &ConnectionManager::connected,
                   this, &MainWindow::onDaemonConnected);
  QObject::connect(connectionMgr_, &ConnectionManager::disconnected,
                   this, &MainWindow::onDaemonDisconnected);
  QObject::connect(connectionMgr_, &ConnectionManager::stateChanged,
                   this, &MainWindow::updateConnectionStatus);
  QObject::connect(connectionMgr_, &ConnectionManager::statusMessage,
                   this, &MainWindow::onConnectionStatusMessage);
  QObject::connect(connectionMgr_, &ConnectionManager::blockchainSynced,
                   this, &MainWindow::onBlockchainSyncUpdate);
  QObject::connect(connectionMgr_, &ConnectionManager::reconnecting,
                   this, [this](int attempt, int maxAttempts) {
    lblConnectionStatus_->setText(QString("Reconnecting (%1/%2)").arg(attempt).arg(maxAttempts));
    lblConnectionStatus_->setStyleSheet(headerPillStyle());
  });
  
  // Auto-connect ConnectionManager for wallet/address operations
  connectionMgr_->connectToDaemon();

  // Friendly startup countdown. On Windows a node with many wallets at high
  // height can take up to ~3 minutes to serve RPC. Without visible feedback,
  // users assume the app hung and force-quit + relaunch mid-init — which is
  // exactly what wedged startup historically. A live mm:ss countdown shows
  // progress so they wait it out. Auto-dismisses the instant we connect.
  //
  // MUST be a plain widget-based QDialog, NOT a QMessageBox. On macOS a
  // QMessageBox is presented as a native NSAlert that spins its own nested
  // modal run loop (runModalForWindow:), and Qt QTimers do NOT fire inside that
  // loop — so a QMessageBox-based countdown parks the main thread in the modal
  // loop and its own tick timer never runs, freezing the display at 3:00. A
  // non-modal QDialog uses no native modal loop, so the tick fires and the
  // countdown actually counts down.
  QTimer::singleShot(2000, this, [this]() {
    if (shuttingDown_ || (connectionMgr_ && connectionMgr_->isConnected())) {
      return;  // already connected (fast start / adopted a warm daemon)
    }
    auto* box = new QDialog(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->setWindowTitle("Starting Dinero…");

    auto* layout = new QVBoxLayout(box);
    auto* titleLabel = new QLabel(box);
    titleLabel->setTextFormat(Qt::PlainText);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 1);
    titleLabel->setFont(titleFont);
    auto* infoLabel = new QLabel(box);
    infoLabel->setTextFormat(Qt::PlainText);
    infoLabel->setWordWrap(true);
    layout->addWidget(titleLabel);
    layout->addWidget(infoLabel);
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto* hideBtn = new QPushButton("Hide", box);  // optional dismiss; waiting is fine
    btnRow->addWidget(hideBtn);
    layout->addLayout(btnRow);
    QObject::connect(hideBtn, &QPushButton::clicked, box, &QDialog::close);

    auto remaining = std::make_shared<int>(180);  // 3:00
    auto render = [titleLabel, infoLabel](int secs) {
      if (secs > 0) {
        titleLabel->setText(QString("Starting the Dinero node — ready in about %1:%2")
                                .arg(secs / 60)
                                .arg(secs % 60, 2, 10, QChar('0')));
        infoLabel->setText(
            "This can take up to ~3 minutes on first start.\n"
            "Please wait — do NOT close or restart. The wallet opens "
            "automatically once the node is ready.");
      } else {
        titleLabel->setText("Almost there — the node is taking a little longer than usual.");
        infoLabel->setText(
            "Still starting… please keep waiting and do NOT close or restart.");
      }
    };
    render(*remaining);

    auto* tick = new QTimer(box);
    tick->setInterval(1000);
    QObject::connect(tick, &QTimer::timeout, box, [this, box, remaining, render]() {
      if (connectionMgr_ && connectionMgr_->isConnected()) {
        box->close();
        return;
      }
      if (*remaining > 0) {
        --(*remaining);
      }
      render(*remaining);
    });
    tick->start();

    // Close the moment the daemon connects, even between ticks.
    QObject::connect(connectionMgr_, &ConnectionManager::connected, box,
                     [box]() { box->close(); });

    box->show();  // non-modal: countdown updates while the app keeps working
  });


  // Connect RPC signals (legacy - will be phased out)
  connect(rpc_, &RpcClient::rpcResult, this, &MainWindow::onRpcResult);
  connect(rpc_, &RpcClient::rpcResult, txTracker_, &TransactionTracker::onPollRpcResult);

  // Phase 4: Release change reservation on terminal events (evicted/failed/replaced)
  connect(txTracker_, &TransactionTracker::terminalEvent, this,
      [this](const TerminalAdvisoryEvent& event) {
    if (changeAddrMgr_ && !event.reservationId.isEmpty()) {
      changeAddrMgr_->release(event.reservationId);
    }
  });

  connect(rpc_, &RpcClient::rpcError, this, &MainWindow::onRpcError);
  connect(rpc_, &RpcClient::serverChanged, this, [this](const QString& newServer) {
    lblConnectionStatus_->setText("Connected");
    lblConnectionStatus_->setStyleSheet(headerPillStyle());
    lblConnectionStatus_->setToolTip(newServer);
  });
  connect(rpc_, &RpcClient::connectionOk, this, [this]() {
    lblConnectionStatus_->setText("Connected");
    lblConnectionStatus_->setStyleSheet(headerPillStyle());
    lblConnectionStatus_->setToolTip(rpc_->currentServer());
  });
  connect(rpc_, &RpcClient::connectionFailed, this, [this](const QString& reason) {
    lblConnectionStatus_->setText("Connection issue");
    lblConnectionStatus_->setStyleSheet(headerPillStyle());
    lblConnectionStatus_->setToolTip(reason);
  });

  // Wallet unlock countdown timer (ticks every second)
  connect(unlockCountdownTimer_, &QTimer::timeout, this, &MainWindow::onUnlockCountdownTick);

  // Auto-retry wallet rescan when node safe mode (deep reorg) temporarily blocks it.
  safeModeRescanRetryTimer_->setSingleShot(true);
  connect(safeModeRescanRetryTimer_, &QTimer::timeout, this, [this]() {
    safeModeRescanRetryScheduled_ = false;
    if (!walletRescanning_) {
      return;
    }
    if (lblSyncProgress_) {
      lblSyncProgress_->setText("🔄 Retrying blockchain scan...");
    }
    rpc_->call("wallet.rescanblockchain", QJsonArray());
  });

#ifdef DIN_EXPERIMENTAL_FEATURES
  // Connect WebSocket signals
  connect(ws_, &WebSocketClient::connected, this, &MainWindow::onWsConnected);
  connect(ws_, &WebSocketClient::disconnected, this, &MainWindow::onWsDisconnected);
  connect(ws_, &WebSocketClient::connectionError, this, &MainWindow::onWsError);
  connect(ws_, &WebSocketClient::newBlockReceived, this, &MainWindow::onWsNewBlock);
  connect(ws_, &WebSocketClient::newTransactionReceived, this, &MainWindow::onWsNewTransaction);
  connect(ws_, &WebSocketClient::miningInfoReceived, this, &MainWindow::onWsMiningInfo);
  connect(ws_, &WebSocketClient::networkInfoReceived, this, &MainWindow::onWsNetworkInfo);
  connect(ws_, &WebSocketClient::mempoolUpdateReceived, this, &MainWindow::onWsMempoolUpdate);
  connect(ws_, &WebSocketClient::syncProgressReceived, this, &MainWindow::onWsSyncProgress);
#endif
  
  // Auto-refresh every 5 seconds (start after window is shown)
  connect(refreshTimer_, &QTimer::timeout, this, &MainWindow::refresh);
  
  // Mining stats update. Process miners own their own live status row;
  // daemon-backed solo mining still polls mining.info RPC.
  connect(miningStatsTimer_, &QTimer::timeout, this, [this]() {
    if (miningProcess_ && miningProcess_->state() == QProcess::Running) {
      updateMiningStats();
      return;
    }
    rpc_->miningInfo();
  });

  miningFocusDimTimer_->setSingleShot(true);
  connect(miningFocusDimTimer_, &QTimer::timeout, this, [this]() {
    const bool miningTabActive =
      mainTabs_ && miningTabWidget_ && (mainTabs_->currentWidget() == miningTabWidget_);
    if (isMining_ && miningTabActive) {
      applyMiningFocusDim(true);
    }
  });

  // Live Hash Engine refresh. Ten real samples per second keeps the display
  // visibly alive without competing with the mining hot path or the UI loop.
  connect(miningCinematicTimer_, &QTimer::timeout,
          this, &MainWindow::updateMiningOutputCinematicFrame);
  miningCinematicTimer_->setInterval(dinero::qt::kHashEngineIntervalMs);
  
  // Delayed initial refresh (after GUI is fully loaded)
  QTimer::singleShot(3000, this, [this]() {
    refreshTimer_->start(5000);
    refresh();
  });

  // Production main.cpp already bootstraps dinerod before constructing this
  // window. Scheduling another launch while that daemon is still rebuilding
  // wallet state races two processes against the same datadir; the losing
  // process's retry cleanup can then kill the legitimate first daemon. Only a
  // standalone MainWindow owner may schedule this fallback.
  if (dinero::qt::ShouldScheduleWindowDaemonStart(daemonBootstrapOwner)) {
    QTimer::singleShot(1500, this, &MainWindow::maybeAutoStartDaemon);
  }

  // #295: visible timeout on the startup wait. If RPC never comes up the
  // user previously stared at "Connecting..." forever with no explanation.
  // Bumped 60s -> 100s -> 180s: on Windows a node with many wallets at high
  // height can take ~150s to be RPC-ready, and the old 100s watchdog nagged
  // with "Still Waiting for Daemon" before it connected. The handler no-ops if
  // already connected, so this only delays the alert for a GENUINE hang.
  QTimer::singleShot(180000, this, &MainWindow::onStartupWatchdogTimeout);

  // Cmd+K dashboard. The AI assistant surface is temporarily hidden by
  // kShowAiAssistantPanel, but the shortcut remains the dashboard entry point.
  aiToggleShortcut_ = new QShortcut(QKeySequence("Ctrl+K"), this);
  connect(aiToggleShortcut_, &QShortcut::activated, this, &MainWindow::onToggleAiPanel);

  // Initial state before auto-start logic settles.
  btnStartDaemon_->setVisible(true);
  btnStopDaemon_->setVisible(false);
  lblConnectionStatus_->setText("Daemon stopped");
  lblConnectionStatus_->setStyleSheet(headerPillStyle());
}

MainWindow::~MainWindow() {
  shuttingDown_ = true;

  if (minerCtrl_) {
    QObject::disconnect(minerCtrl_, nullptr, this, nullptr);
    minerCtrl_->shutdownSilently();
    delete minerCtrl_;
    minerCtrl_ = nullptr;
    isMining_ = false;
  }

  if (connectionMgr_) {
    QObject::disconnect(connectionMgr_, nullptr, this, nullptr);
    connectionMgr_->disconnectFromDaemon();
  }
  if (rpc_) {
    QObject::disconnect(rpc_, nullptr, this, nullptr);
  }

  // CRITICAL: Stop mining stats timer to prevent callbacks on deleted widgets
  if (miningStatsTimer_) {
    miningStatsTimer_->stop();
    miningStatsTimer_->disconnect(); // Disconnect all signal handlers
  }
  if (miningFocusDimTimer_) {
    miningFocusDimTimer_->stop();
    miningFocusDimTimer_->disconnect();
  }
  if (miningCinematicTimer_) {
    miningCinematicTimer_->stop();
    miningCinematicTimer_->disconnect();
  }
  
  // CRITICAL: Clean up mining process before widgets are destroyed
  if (miningProcess_) {
    // FIRST: Block all signals to prevent ANY callbacks during destruction
    miningProcess_->blockSignals(true);

    // THEN: Disconnect ALL signals to prevent callbacks during destruction
    miningProcess_->disconnect();

    // FINALLY: Stop the process if it's running
    if (miningProcess_->state() == QProcess::Running) {
      miningProcess_->terminate();
      if (!miningProcess_->waitForFinished(2000)) {
        miningProcess_->kill();
        miningProcess_->waitForFinished(1000);
      }
    }

    // Delete the process
    delete miningProcess_;
    miningProcess_ = nullptr;
  }

  if (localStratumProcess_) {
    localStratumProcess_->blockSignals(true);
    localStratumProcess_->disconnect();
    if (localStratumProcess_->state() == QProcess::Running) {
      localStratumProcess_->terminate();
      if (!localStratumProcess_->waitForFinished(2000)) {
        localStratumProcess_->kill();
        localStratumProcess_->waitForFinished(1000);
      }
    }
    delete localStratumProcess_;
    localStratumProcess_ = nullptr;
  }

  // CRITICAL: Shutdown daemon if it was started by this GUI
  if (daemonProcess_) {
    qDebug() << "Shutting down daemon...";

    // Try graceful shutdown via RPC stop command first
    rpc_->call("daemon.stop", QJsonArray());

    // Give it 5 seconds to shutdown gracefully
    if (daemonProcess_->state() == QProcess::Running) {
      if (!daemonProcess_->waitForFinished(5000)) {
        qWarning() << "Daemon didn't shutdown gracefully, sending SIGTERM...";
        daemonProcess_->terminate();

        // Wait 3 more seconds
        if (!daemonProcess_->waitForFinished(3000)) {
          qWarning() << "Daemon didn't respond to SIGTERM, force killing...";
          daemonProcess_->kill();
          daemonProcess_->waitForFinished(1000);
        }
      }
    }

    delete daemonProcess_;
    daemonProcess_ = nullptr;
    qDebug() << "Daemon shutdown complete";
  }
}

void MainWindow::setupUI() {
  auto *central = new QWidget;
  setCentralWidget(central);

  // Help menu with About Dinero action. Shows the release tag baked in
  // at build time via DINERO_RELEASE_TAG (set by CMake) so users can
  // identify which preview/rc they are running. Added in rc6;
  // compact stylesheet added in rc7.1 to close the visual gap with
  // the Windows title bar.
  auto *menuBar = new QMenuBar(this);
  menuBar->setStyleSheet(
    "QMenuBar { background: #181b20; color: #d6dde6; "
    "padding: 0px; spacing: 8px; "
    "min-height: 0px; max-height: 22px; "
    "border-bottom: 1px solid #2f343c; } "
    "QMenuBar::item { padding: 2px 10px; background: transparent; color: #c8d0d9; } "
    "QMenuBar::item:selected { background: #2d323b; color: #eef2f6; } "
    "QMenuBar::item:pressed { background: #353b45; }"
  );
  // rc8+: Network menu with a checkable router-port-mapping toggle.
  // Writes the same QSettings key the daemon launcher reads. Takes
  // effect on the next daemon restart (File → Quit, then reopen).
  auto *networkMenu = menuBar->addMenu(tr("&Network"));
  auto *portmapAction = networkMenu->addAction(tr("Enable router port mapping (UPnP/NAT-PMP)"));
  portmapAction->setCheckable(true);
  portmapAction->setChecked(QSettings().value(p2pPortMapAllowedKey(), true).toBool());
  connect(portmapAction, &QAction::toggled, this, [this](bool enabled) {
    QSettings().setValue(p2pPortMapAllowedKey(), enabled);
    QMessageBox::information(this, tr("Router port mapping"),
        enabled
          ? tr("Router port mapping will be enabled on the next daemon restart "
               "(File → Quit, then reopen Dinero).")
          : tr("Router port mapping will be disabled on the next daemon restart "
               "(File → Quit, then reopen Dinero)."));
  });

  auto *helpMenu = menuBar->addMenu(tr("&Help"));
  auto *aboutAction = helpMenu->addAction(tr("&About Dinero"));
  connect(aboutAction, &QAction::triggered, this, [this]() {
    const QString version = QCoreApplication::applicationVersion();
    const QString aboutText = tr(
        "<h3>Dinero Wallet</h3>"
        "<p>Version: <b>%1</b></p>"
        "<p>Real Money For Free People.<br>"
        "Post-quantum, utreexo-native, fair-launched.</p>"
        "<hr>"
        "<p style='color:#888;font-size:90%;'>"
        "Built from <code>DineroLabs/Dinero-Coin</code> + "
        "<code>DineroLabs/dinero-qt</code>."
        "</p>"
        "<p><a href='https://dinero-coin.com'>dinero-coin.com</a> &nbsp;·&nbsp; "
        "<a href='https://github.com/DineroLabs'>github.com/DineroLabs</a></p>"
    ).arg(version);
    QMessageBox::about(this, tr("About Dinero"), aboutText);
  });
  setMenuBar(menuBar);

  auto *mainLayout = new QVBoxLayout(central);
  central->setStyleSheet(
    "QWidget { background: #181b20; color: #d6dde6; } "
    "QTabWidget::pane { border: 1px solid #2f343c; background: #1a1d22; border-radius: 8px; margin-top: 6px; } "
    "QTabBar::tab { background: #242932; color: #c8d0d9; border: 1px solid #353b45; border-bottom: none; "
    "padding: 6px 10px; border-top-left-radius: 6px; border-top-right-radius: 6px; margin-right: 3px; } "
    "QTabBar::tab:selected { background: #2d323b; color: #eef2f6; } "
    "QGroupBox { border: 1px solid #30353d; border-radius: 10px; margin-top: 10px; padding-top: 8px; background: #20242a; font-weight: 600; } "
    "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: #cad2db; } "
    "QLineEdit, QComboBox, QSpinBox, QTextEdit, QPlainTextEdit { background: #1f2328; color: #d7dde5; "
    "border: 1px solid #353b44; border-radius: 6px; padding: 6px; selection-background-color: #3e4550; } "
    "QPushButton { background: #2b3037; color: #e6ebf1; border: 1px solid #3c434d; border-radius: 7px; padding: 6px 12px; font-weight: 600; } "
    "QPushButton:hover { background: #333942; } "
    "QPushButton:pressed { background: #262b31; } "
    "QPushButton:disabled { background: #21252a; color: #7f8893; border: 1px solid #30353d; }");
  
  // Wallet name indicator kept for internal status updates only.
  lblWalletName_ = new QLabel("Wallet: none");
  lblWalletName_->setStyleSheet(headerPillStyle());
  lblWalletName_->setMinimumWidth(0);
  lblWalletName_->setMaximumWidth(190);
  lblWalletName_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
  lblWalletName_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  lblWalletName_->setToolTip("No wallet loaded. Create or restore a wallet to get started.");
  lblWalletName_->setVisible(false);

  cmbWalletSelector_ = new QComboBox;
  cmbWalletSelector_->setMinimumWidth(145);
  cmbWalletSelector_->setMaximumWidth(200);
  cmbWalletSelector_->setFixedHeight(30);
  cmbWalletSelector_->setStyleSheet(
    "QComboBox { background: #1f2328; color: #d7dde5; border: 1px solid #353b44; "
    "border-radius: 8px; padding: 0 8px; font-size: 12px; } "
    "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 22px; border-left: 1px solid #353b44; background: #21262c; border-top-right-radius: 8px; border-bottom-right-radius: 8px; } QComboBox::down-arrow { width: 0; height: 0; margin-right: 7px; border-left: 5px solid transparent; border-right: 5px solid transparent; border-top: 6px solid #c4cdd9; } QComboBox::down-arrow:disabled { border-top: 6px solid #5b6470; }");
  cmbWalletSelector_->setToolTip("Select a wallet to load");
  cmbWalletSelector_->setEnabled(false);
  cmbWalletSelector_->setVisible(!singleWalletMode_);

  btnLoadWallet_ = new QPushButton("Load Wallet");
  btnLoadWallet_->setStyleSheet(chromeButtonStyle());
  btnLoadWallet_->setFixedWidth(128);
  btnLoadWallet_->setToolTip("Load selected wallet");
  btnLoadWallet_->setEnabled(false);
  btnLoadWallet_->setVisible(!singleWalletMode_);
  connect(btnLoadWallet_, &QPushButton::clicked, this, &MainWindow::onLoadSelectedWallet);
  connect(cmbWalletSelector_, &QComboBox::currentTextChanged, this, [this]() {
    updateWalletSwitcherState();
  });

  // Single toggle button for wallet lock/unlock (status shown in button text)
  btnWalletLock_ = new QPushButton("Locked | Unlock");
  btnWalletLock_->setStyleSheet(chromeButtonStyle());
  btnWalletLock_->setMinimumWidth(0);
  btnWalletLock_->setMaximumWidth(205);
  btnWalletLock_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
  btnWalletLock_->setToolTip("Unlock wallet to enable Taproot signing and transactions");
  connect(btnWalletLock_, &QPushButton::clicked, this, &MainWindow::onWalletLockToggle);

  btnEncryptWallet_ = new QPushButton("Encrypt Wallet");
  btnEncryptWallet_->setStyleSheet(chromeButtonStyle());
  btnEncryptWallet_->setFixedWidth(122);
  connect(btnEncryptWallet_, &QPushButton::clicked, this, [this]() { onEncryptWallet(); });

  // Phase 5: Advisory banner queue (popup-based)
  bannerQueue_ = new AdvisoryBannerQueue(QString(), this, this);
  connect(txTracker_, &TransactionTracker::terminalEvent,
          bannerQueue_, &AdvisoryBannerQueue::enqueue);

  // Tab widget
  auto *tabs = new QTabWidget;
  mainTabs_ = tabs;
  connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
    updateMiningFocusDimState();
    setMiningOutputCinematicEnabled(isMining_);
    if (isUtxoTabActive()) {
      renderUtxoPage();
      requestUtxoRefresh();
    }
    // Auto-refresh contracts when the Contracts tab is selected
    if (mainTabs_ && mainTabs_->tabText(index).contains("Contracts")) {
        refreshContractsList();
    }
  });

  // Content area: tabs + Cmd+K dashboard side panel.
  auto *contentArea = new QWidget;
  auto *contentLayout = new QHBoxLayout(contentArea);
  contentLayout->setContentsMargins(0, 0, 0, 0);
  contentLayout->setSpacing(0);
  contentLayout->addWidget(tabs, 1);

  if (app_started_at_ms_ == 0) {
      app_started_at_ms_ = QDateTime::currentMSecsSinceEpoch();
  }
  if (kShowAiAssistantPanel) {
    aiPanel_ = new AiPanel(rpc_->datadir(), nullptr);  // re-parented by CmdKPanel below
  }
  cmdKPanel_ = new dinero::qt::dashboard::CmdKPanel(rpc_, aiPanel_, contentArea);
  cmdKPanel_->setPanelWidth(0);
  // Feed qt-side local state into the dashboard: mining flags the daemon
  // doesn't see + app uptime since the daemon has no getuptime method.
  cmdKPanel_->setLocalMiningProvider([this]() {
      dinero::qt::dashboard::LocalMiningState s;
      s.active     = isMiningLocal();
      s.miner_type = activeMinerType();
      s.hashrate   = currentHashrate();
      s.app_uptime = std::chrono::seconds(appUptimeSeconds());
      return s;
  });
  contentLayout->addWidget(cmdKPanel_);

  mainLayout->addWidget(contentArea, 1);

  // AI Status Strip: parked with the AI assistant surface. The dashboard
  // remains available through Ctrl+K.
  if (kShowAiAssistantPanel) {
    aiStatusStrip_ = new AiStatusStrip(central);
    connect(aiStatusStrip_, &AiStatusStrip::clicked, this, &MainWindow::onToggleAiPanel);
    mainLayout->addWidget(aiStatusStrip_);
  }
  
  // === Overview Tab ===
  {
    auto *overview = new QWidget;
    auto *layout = new QVBoxLayout(overview);
    auto *topRow = new QHBoxLayout;
    topRow->setSpacing(12);
    
    auto *infoGroup = new QGroupBox("Network Info");
    auto *infoLayout = new QVBoxLayout(infoGroup);
    infoGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    
    lblHeight_ = new QLabel("Height: -");
    lblHeaders_ = new QLabel("Headers: -");
    lblConnections_ = new QLabel("Connections: -");
    lblMempool_ = new QLabel("Mempool: -");
    lblPhase_ = new QLabel("Halving Epoch: -");
    lblSupply_ = new QLabel("Supply: -");
    lblReward_ = new QLabel("Next Reward: -");
    lblSyncProgress_ = new QLabel("");
    lblSyncProgress_->setStyleSheet("QLabel { color: #cbd3dc; font-weight: 600; background: #262b32; border: 1px solid #373d46; border-radius: 6px; padding: 5px; }");
    
    infoLayout->addWidget(lblHeight_);
    infoLayout->addWidget(lblHeaders_);
    infoLayout->addWidget(lblSyncProgress_);
    infoLayout->addWidget(lblConnections_);
    infoLayout->addWidget(lblMempool_);
    infoLayout->addWidget(lblPhase_);
    infoLayout->addWidget(lblSupply_);
    infoLayout->addWidget(lblReward_);
    
    topRow->addWidget(infoGroup, 3);
    
    // ═══════════════════════════════════════════════════════════════════
    // 🛡️ V7 CONSENSUS HEALTH
    // ═══════════════════════════════════════════════════════════════════
    {
      auto *v7Group = new QGroupBox("v7 Consensus Health");
      auto *v7Column = new QVBoxLayout;
      v7Column->setContentsMargins(10, 12, 10, 10);
      v7Column->setSpacing(8);
      v7Group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      v7Group->setMaximumHeight(245);

      // Utreexo box
      auto *utreexoBox = new QGroupBox("Utreexo");
      auto *utreexoLay = new QVBoxLayout(utreexoBox);
      utreexoLay->setContentsMargins(10, 12, 10, 10);
      utreexoLay->setSpacing(4);
      utreexoBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      utreexoBox->setMaximumHeight(105);
      lblUtreexoHealth_ = new QLabel("Health: --");
      lblUtreexoHealth_->setStyleSheet("QLabel { font-size: 12px; font-weight: bold; color: #2d8a4e; }");
      lblUtreexoLeaves_ = new QLabel("Leaves: --");
      lblUtreexoLeaves_->setStyleSheet("QLabel { font-size: 11px; color: #c5ced8; }");
      lblUtreexoRoot_ = new QLabel("Root: --");
      lblUtreexoRoot_->setStyleSheet("QLabel { font-size: 10px; color: #868e96; font-family: monospace; }");
      utreexoLay->addWidget(lblUtreexoHealth_);
      utreexoLay->addWidget(lblUtreexoLeaves_);
      utreexoLay->addWidget(lblUtreexoRoot_);
      v7Column->addWidget(utreexoBox);

      // PQ Status box
      auto *pqBox = new QGroupBox("Post-Quantum");
      auto *pqLay = new QVBoxLayout(pqBox);
      pqLay->setContentsMargins(10, 12, 10, 10);
      pqLay->setSpacing(4);
      pqBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      pqBox->setMaximumHeight(105);
      lblPqOverviewRatio_ = new QLabel("PQ Ratio: --");
      lblPqOverviewRatio_->setStyleSheet("QLabel { font-size: 12px; font-weight: bold; color: #d6dde6; }");
      lblPqOverviewUtxos_ = new QLabel("P2MR UTXOs: --");
      lblPqOverviewUtxos_->setStyleSheet("QLabel { font-size: 11px; color: #c5ced8; }");
      lblPqOverviewScheme_ = new QLabel("Active: ML-DSA-65");
      lblPqOverviewScheme_->setStyleSheet("QLabel { font-size: 11px; color: #868e96; }");
      pqLay->addWidget(lblPqOverviewRatio_);
      pqLay->addWidget(lblPqOverviewUtxos_);
      pqLay->addWidget(lblPqOverviewScheme_);
      v7Column->addWidget(pqBox);

      v7Group->setLayout(v7Column);
      topRow->addWidget(v7Group, 2);
    }
    layout->addLayout(topRow);

    // ═══════════════════════════════════════════════════════════════════
    // 📊 MONITORING DASHBOARD (bottom half of Overview)
    // ═══════════════════════════════════════════════════════════════════
    
    auto *monitoringGroup = new QGroupBox("Node operation");
    auto *monitoringLayout = new QVBoxLayout(monitoringGroup);
    monitoringLayout->setContentsMargins(10, 12, 10, 10);
    monitoringLayout->setSpacing(10);
    monitoringGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    monitoringGroup->setMaximumHeight(650);
    
    // Row 1: resilient network controls + compact resource telemetry.
    auto *row1 = new QHBoxLayout;
    row1->setSpacing(12);

    auto* connectivityBox = new QGroupBox("Connectivity & contribution");
    auto* connectivityLayout = new QVBoxLayout(connectivityBox);
    connectivityLayout->setContentsMargins(4, 6, 4, 4);
    overviewConnectivityCard_ = new dinero::qt::OverviewConnectivityCard(connectivityBox);
    connectivityLayout->addWidget(overviewConnectivityCard_);
    connectivityBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    connectivityBox->setMaximumHeight(235);
    connect(overviewConnectivityCard_, &dinero::qt::OverviewConnectivityCard::torModeRequested,
            this, [this](const QString& mode) {
      rpc_->callNamed("network.setonionservice", QJsonObject{{"mode", mode}});
    });
    connect(overviewConnectivityCard_, &dinero::qt::OverviewConnectivityCard::relayModeRequested,
            this, [this](const QString& mode) {
      rpc_->callNamed("network.setrelayservice", QJsonObject{{"mode", mode}});
    });
    row1->addWidget(connectivityBox, 2);

    auto *cpuBox = new QGroupBox("Resources & mining");
    auto *cpuLayout = new QVBoxLayout(cpuBox);
    cpuLayout->setContentsMargins(10, 10, 10, 8);
    cpuLayout->setSpacing(3);
    cpuBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    cpuBox->setMaximumHeight(235);
    lblCpuUsage_ = new QLabel("0%");
    lblCpuUsage_->setAlignment(Qt::AlignLeft);
    lblCpuUsage_->setStyleSheet("QLabel { font-size: 14px; font-weight: bold; }");
    cpuProgressBar_ = new QProgressBar;
    cpuProgressBar_->setRange(0, 100);
    cpuProgressBar_->setValue(0);
    cpuProgressBar_->setTextVisible(false);
    cpuProgressBar_->setFixedHeight(18);
    cpuProgressBar_->setStyleSheet(chromeProgressBarStyle());
    lblCpuTemp_ = new QLabel("Temp: --");
    lblPowerStatus_ = new QLabel("Power: --");
    lblCpuTemp_->setStyleSheet("QLabel { font-size: 11px; color: #c5ced8; }");
    lblPowerStatus_->setStyleSheet("QLabel { font-size: 11px; color: #868e96; }");
    auto* cpuHeader = new QHBoxLayout;
    cpuHeader->addWidget(new QLabel("CPU", cpuBox));
    cpuHeader->addStretch();
    cpuHeader->addWidget(lblCpuUsage_);
    cpuLayout->addLayout(cpuHeader);
    cpuLayout->addWidget(cpuProgressBar_);
    cpuLayout->addWidget(lblCpuTemp_);
    cpuLayout->addWidget(lblPowerStatus_);
    lblLocalHashrate_ = new QLabel("Local: 0 H/s");
    lblNetworkHashrate_ = new QLabel("Network: 0 H/s");
    lblLocalHashrate_->setStyleSheet("QLabel { font-size: 12px; }");
    lblNetworkHashrate_->setStyleSheet("QLabel { font-size: 12px; color: #868e96; }");
    cpuLayout->addWidget(lblLocalHashrate_);
    cpuLayout->addWidget(lblNetworkHashrate_);
    lblMinerModeOverview_ = new QLabel("Miner: Idle");
    lblGpuBackendOverview_ = new QLabel("Mining: --");
    lblGpuDeviceOverview_ = new QLabel("GPU: --");
    lblGpuLoadOverview_ = new QLabel("GPU Load: --");
    lblGpuMemoryOverview_ = new QLabel("GPU Mem: --");
    lblGpuThermalsOverview_ = new QLabel("GPU Temp/Fan: --");
    lblMinerModeOverview_->setStyleSheet("QLabel { font-size: 12px; }");
    lblGpuBackendOverview_->setStyleSheet("QLabel { font-size: 11px; color: #c5ced8; }");
    lblGpuDeviceOverview_->setStyleSheet("QLabel { font-size: 11px; color: #868e96; }");
    lblGpuLoadOverview_->setStyleSheet("QLabel { font-size: 11px; color: #c5ced8; }");
    lblGpuMemoryOverview_->setStyleSheet("QLabel { font-size: 11px; color: #868e96; }");
    lblGpuThermalsOverview_->setStyleSheet("QLabel { font-size: 11px; color: #868e96; }");
    cpuLayout->addWidget(lblMinerModeOverview_);
    cpuLayout->addWidget(lblGpuBackendOverview_);
    cpuLayout->addWidget(lblGpuDeviceOverview_);
    cpuLayout->addWidget(lblGpuLoadOverview_);
    cpuLayout->addWidget(lblGpuMemoryOverview_);
    cpuLayout->addWidget(lblGpuThermalsOverview_);
    row1->addWidget(cpuBox, 1);
    
    monitoringLayout->addLayout(row1);
    
    // Row 2: Mempool + Peers Summary
    auto *row2 = new QHBoxLayout;
    
    // Mempool Size (enhanced with sparkline simulation)
    auto *mempoolBox = new QGroupBox("📦 Mempool");
    auto *mempoolLayout = new QVBoxLayout(mempoolBox);
    mempoolBox->setMaximumHeight(125);
    lblMempoolSize_ = new QLabel("0 txs");
    lblMempoolSize_->setStyleSheet("QLabel { font-size: 18px; font-weight: bold; color: #d6dde6; }");
    lblMempoolBytes_ = new QLabel("0 bytes");
    lblMempoolBytes_->setStyleSheet("QLabel { font-size: 11px; color: #868e96; }");
    mempoolLayout->addWidget(lblMempoolSize_);
    mempoolLayout->addWidget(lblMempoolBytes_);
    mempoolLayout->addStretch();
    row2->addWidget(mempoolBox, 1, Qt::AlignTop);
    
    // Peers Summary + compact connected peers table
    auto *peersBox = new QGroupBox("🌐 Peers");
    auto *peersLayout = new QVBoxLayout(peersBox);
    lblPeersCount_ = new QLabel("0 peers");
    lblPeersCount_->setStyleSheet("QLabel { font-size: 18px; font-weight: bold; color: #d6dde6; }");
    lblPeersStatus_ = new QLabel("Disconnected");
    lblPeersStatus_->setStyleSheet("QLabel { font-size: 11px; color: #868e96; }");
    peersLayout->addWidget(lblPeersCount_);
    peersLayout->addWidget(lblPeersStatus_);

    tblPeersOverview_ = new QTableWidget(0, 5);  // 5 columns
    tblPeersOverview_->setHorizontalHeaderLabels({"Location", "Activity", "Last Seen", "Seen Height", "Client"});
    if (auto* advertisedHeader = tblPeersOverview_->horizontalHeaderItem(3)) {
      advertisedHeader->setToolTip(
          "Last height this local node observed for the peer.\n"
          "This is P2P telemetry, not a live RPC query to that server.");
    }
    tblPeersOverview_->horizontalHeader()->setStretchLastSection(true);
    tblPeersOverview_->verticalHeader()->setVisible(false);
    tblPeersOverview_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblPeersOverview_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tblPeersOverview_->setSortingEnabled(true);
    tblPeersOverview_->setMinimumHeight(118);
    tblPeersOverview_->setMaximumHeight(145);  // Compact view
    tblPeersOverview_->setStyleSheet(
      "QTableWidget { gridline-color: #3a4048; background: #1d2126; color: #d5dde6; } "
      "QHeaderView::section { background: #272c33; color: #d5dde6; padding: 4px; font-weight: bold; border: 1px solid #373d46; }"
    );
    peersLayout->addWidget(tblPeersOverview_);
    row2->addWidget(peersBox, 2);

    monitoringLayout->addLayout(row2);
    
    // Row 3: Alerts (last 5 events)
    auto *alertsBox = new QGroupBox("⚠️ Recent Alerts");
    auto *alertsLayout = new QVBoxLayout(alertsBox);
    txtAlerts_ = new QTextEdit;
    txtAlerts_->setReadOnly(true);
    txtAlerts_->setMaximumHeight(48);
    txtAlerts_->setStyleSheet(
      "QTextEdit { background: #1d2126; border: 1px solid #373d46; color: #cfd7df; font-family: monospace; font-size: 11px; }"
    );
    txtAlerts_->setPlaceholderText("No recent alerts");
    alertsLayout->addWidget(txtAlerts_);
    monitoringLayout->addWidget(alertsBox);
    
    // Row 5: Export Button
    auto *exportLayout = new QHBoxLayout;
    exportLayout->addStretch();
    auto *btnExportMetrics = new QPushButton("📊 Export Metrics (JSON/CSV)");
    btnExportMetrics->setStyleSheet(chromeButtonStyle());
    connect(btnExportMetrics, &QPushButton::clicked, this, &MainWindow::onExportMetrics);
    exportLayout->addWidget(btnExportMetrics);
    monitoringLayout->addLayout(exportLayout);
    
    layout->addWidget(monitoringGroup);
    
    // ═══════════════════════════════════════════════════════════════════
    // END MONITORING DASHBOARD
    // ═══════════════════════════════════════════════════════════════════
    
    overview->setMinimumHeight(1120); // Scroll area still handles smaller screens.
    overview->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    tabs->addTab(makeScrollableTab(overview), "Overview");
  }

  // === Wallet Tab ===
  {
    auto *wallet = new QWidget;
    auto *layout = new QVBoxLayout(wallet);
    
    // HD Wallet Setup Banner
    auto *walletSetupGroup = new QGroupBox("🔐 HD Wallet");
    auto *walletSetupLayout = new QVBoxLayout(walletSetupGroup);
    walletSetupLayout->setSpacing(8);
    auto *walletIntroLayout = new QHBoxLayout;
    walletIntroLayout->setContentsMargins(0, 0, 0, 0);
    walletIntroLayout->setSpacing(8);
    auto *lblWalletInfo = new QLabel(
      singleWalletMode_
        ? "Single wallet mode: use one default wallet. Restore is emergency-only."
        : "Create a secure BIP-39 HD wallet with Taproot and quantum-safe address support");
    lblWalletInfo->setWordWrap(true);
    auto *btnCreateWallet = new QPushButton(singleWalletMode_
      ? "🆕 Create Wallet / Emergency Restore"
      : "🆕 Create/Restore Wallet");
    btnCreateWallet->setStyleSheet(chromeButtonStyle());
    connect(btnCreateWallet, &QPushButton::clicked, this, &MainWindow::onCreateWallet);
    btnRescanWallet_ = new QPushButton("🔄 Rescan Wallet");
    btnRescanWallet_->setStyleSheet(chromeButtonStyle());
    btnRescanWallet_->setToolTip("If balance/history looks wrong, rescan blockchain for this wallet.");
    connect(btnRescanWallet_, &QPushButton::clicked, this, &MainWindow::onRescanWallet);
    walletIntroLayout->addWidget(lblWalletInfo, 1);
    walletIntroLayout->addWidget(btnCreateWallet);
    walletSetupLayout->addLayout(walletIntroLayout);

    auto *walletControlLayout = new QHBoxLayout;
    walletControlLayout->setContentsMargins(0, 0, 0, 0);
    walletControlLayout->setSpacing(8);
    auto *lblWalletSelector = new QLabel("Wallet:");
    lblWalletSelector->setVisible(!singleWalletMode_);
    walletControlLayout->addWidget(lblWalletSelector);
    walletControlLayout->addWidget(cmbWalletSelector_, 1);
    walletControlLayout->addWidget(btnLoadWallet_);
    walletControlLayout->addSpacing(4);
    walletControlLayout->addWidget(btnWalletLock_);
    walletControlLayout->addWidget(btnEncryptWallet_);
    walletControlLayout->addWidget(btnRescanWallet_);
    walletSetupLayout->addLayout(walletControlLayout);
    layout->addWidget(walletSetupGroup);
    
    // Cross-Platform Compatibility Banner
    auto *compatGroup = new QGroupBox("📱 Seed & Address Compatibility");
    auto *compatLayout = new QVBoxLayout(compatGroup);
    compatGroup->setStyleSheet("QGroupBox { background: #20252c; border: 1px solid #343b45; border-radius: 10px; }");
    
    auto *lblCompat = new QLabel(
        "<b>One seed, two address lanes.</b><br><br>"
        "<b>BIP39 seed phrase</b> restores the same wallet across Dinero Qt and mobile. "
        "<b>BIP86 Taproot</b> addresses (<code>din1p...</code>) are the mobile-friendly payment lane. "
        "<b>Purpose 88 P2MR</b> addresses (<code>din1r...</code>) are the quantum-safe lane using ML-DSA-65 signatures.<br><br>"
        "✅ Desktop (Qt Wallet) - full node, mining, Taproot, P2MR quantum-safe receive/spend<br>"
        "✅ Mobile (iOS Wallet) - seed-compatible Taproot payments; P2MR keys derive from the same seed as mobile support expands"
    );
    lblCompat->setWordWrap(true);
    compatLayout->addWidget(lblCompat);
    
    auto *btnExportSeed = new QPushButton("🧾 Seed Backup / Mobile Restore");
    btnExportSeed->setStyleSheet(chromeButtonStyle());
    btnExportSeed->setToolTip("Show seed-backup guidance. Hardened wallets may not allow seed phrase re-export after setup.");
    connect(btnExportSeed, &QPushButton::clicked, this, &MainWindow::onExportSeed);
    compatLayout->addWidget(btnExportSeed);
    
    layout->addWidget(compatGroup);
    
    // Enhanced Balance Display
    auto *balanceGroup = new QGroupBox("💰 Balance");
    auto *balanceLayout = new QVBoxLayout(balanceGroup);

    // Main balance: public transparent + private shielded spendable funds.
    lblBalance_ = new QLabel("0.00 DIN");
    lblBalance_->setStyleSheet("QLabel { font-size: 32px; font-weight: bold; color: #e6ecf2; }");
    lblBalance_->setAlignment(Qt::AlignCenter);
    lblBalance_->setToolTip("Total spendable balance: transparent public funds plus shielded private funds");
    balanceLayout->addWidget(lblBalance_);

    // Total label (used for combined balance updates)
    lblTotalWalletBalance_ = new QLabel("0.00000000 DIN");
    lblTotalWalletBalance_->setObjectName("lblTotalWalletBalance");
    lblTotalWalletBalance_->setVisible(false); // hidden, used for data only

    // Balance breakdown - visible, compact. Taproot and P2MR are both public
    // transparent outputs; shielded notes are the private bucket.
    auto *breakdownWidget = new QWidget;
    auto *breakdownLayout = new QGridLayout(breakdownWidget);
    breakdownLayout->setContentsMargins(20, 4, 20, 0);
    breakdownLayout->setVerticalSpacing(2);

    auto *lblPublicHeader = new QLabel("Transparent / public");
    lblPublicHeader->setStyleSheet("QLabel { font-weight: 600; color: #d6dde6; }");
    breakdownLayout->addWidget(lblPublicHeader, 0, 0, 1, 2);

    breakdownLayout->addWidget(new QLabel("Taproot:"), 1, 0);
    lblTransparentTaprootBalance_ = new QLabel("0.00000000 DIN");
    lblTransparentTaprootBalance_->setObjectName("lblTransparentTaprootBalance");
    lblTransparentTaprootBalance_->setToolTip("Public Taproot spendable balance");
    breakdownLayout->addWidget(lblTransparentTaprootBalance_, 1, 1);

    breakdownLayout->addWidget(new QLabel("P2MR quantum-safe:"), 2, 0);
    auto *pqRow = new QHBoxLayout;
    barPqRatio_ = new QProgressBar;
    barPqRatio_->setRange(0, 100);
    barPqRatio_->setValue(0);
    barPqRatio_->setMaximumHeight(16);
    barPqRatio_->setMaximumWidth(120);
    barPqRatio_->setFormat("%p%");
    barPqRatio_->setStyleSheet(
        "QProgressBar { border: 1px solid #343b45; border-radius: 4px; "
        "background: #1a1f27; text-align: center; color: #9fb3c8; font-size: 10px; }"
        "QProgressBar::chunk { background: #2d8a4e; border-radius: 3px; }");
    barPqRatio_->setToolTip("Percentage of transparent spendable funds held in P2MR outputs");
    pqRow->addWidget(barPqRatio_);
    lblPqRatio_ = new QLabel("0.00000000 DIN");
    lblPqRatio_->setStyleSheet("QLabel { font-size: 11px; color: #9fb3c8; }");
    pqRow->addWidget(lblPqRatio_);
    pqRow->addStretch();
    auto *pqWidget = new QWidget;
    pqWidget->setLayout(pqRow);
    breakdownLayout->addWidget(pqWidget, 2, 1);
    lblTransparentP2mrBalance_ = lblPqRatio_;

    auto *lblPrivateHeader = new QLabel("Shielded / private");
    lblPrivateHeader->setStyleSheet("QLabel { font-weight: 600; color: #d6dde6; margin-top: 6px; }");
    breakdownLayout->addWidget(lblPrivateHeader, 3, 0, 1, 2);

    breakdownLayout->addWidget(new QLabel("Private:"), 4, 0);
    lblShieldedBalance_ = new QLabel("0.00000000 DIN");
    lblShieldedBalance_->setObjectName("lblShieldedBalance");
    lblShieldedBalance_->setToolTip("Confirmed shielded note balance");
    breakdownLayout->addWidget(lblShieldedBalance_, 4, 1);

    breakdownLayout->addWidget(new QLabel("Pending:"), 5, 0);
    auto *lblUnconfirmed = new QLabel("0.00 DIN");
    lblUnconfirmed->setObjectName("lblUnconfirmed");
    breakdownLayout->addWidget(lblUnconfirmed, 5, 1);

    breakdownLayout->addWidget(new QLabel("Mining:"), 6, 0);
    auto *lblImmature = new QLabel("0.00 DIN");
    lblImmature->setObjectName("lblImmature");
    lblImmature->setToolTip("Recently mined coins (available after 100 confirmations)");
    breakdownLayout->addWidget(lblImmature, 6, 1);

    balanceLayout->addWidget(breakdownWidget);

    auto switchToTabWithMode = [this](const QString& tabLabelFragment,
                                      QComboBox* combo,
                                      const QString& mode,
                                      QWidget* focusWidget) {
      if (combo) {
        const int index = combo->findData(mode);
        if (index >= 0) {
          combo->setCurrentIndex(index);
        }
      }
      if (mainTabs_) {
        for (int i = 0; i < mainTabs_->count(); ++i) {
          if (mainTabs_->tabText(i).contains(tabLabelFragment)) {
            mainTabs_->setCurrentIndex(i);
            break;
          }
        }
      }
      if (focusWidget) {
        focusWidget->setFocus();
      }
    };

    // v7: Receive Public/Private and Shield/Unshield buttons REMOVED.
    // Privacy is a send mode, not a receive action. Shield/unshield are
    // implicit routing decisions inside the wallet engine.
    // Consolidate button - shown when UTXO count > 50
    // Consolidate button — created here but added to receive tab button row below
    btnConsolidate_ = new QPushButton("Consolidate");
    btnConsolidate_->setStyleSheet(chromeButtonStyle());
    btnConsolidate_->setToolTip("Combine many small UTXOs into fewer larger ones to reduce fees and improve performance");
    btnConsolidate_->setVisible(true);  // Always visible in receive tab
    connect(btnConsolidate_, &QPushButton::clicked, this, &MainWindow::onConsolidateUTXOs);

    // Phase 35: Multi-asset balance display (Taproot assets)
    auto *lblAssets = new QLabel("");
    lblAssets->setObjectName("lblAssets");
    lblAssets->setWordWrap(true);
    lblAssets->setStyleSheet("QLabel { color: #666; font-size: 11px; margin-top: 5px; }");
    lblAssets->setToolTip("Taproot assets held in this wallet");
    balanceLayout->addWidget(lblAssets);

    layout->addWidget(balanceGroup);
    
    auto *addressGroup = new QGroupBox("Receive Address");
    auto *addressLayout = new QVBoxLayout(addressGroup);

    auto *addressModeRow = new QHBoxLayout;
    addressModeRow->setContentsMargins(0, 0, 0, 0);
    addressModeRow->setSpacing(8);
    addressModeRow->addWidget(new QLabel("Mode:"));
    cmbWalletAddressMode_ = new QComboBox;
    cmbWalletAddressMode_->addItem(QString::fromUtf8("\xF0\x9F\x8C\x90 Taproot"), "standard");
    cmbWalletAddressMode_->addItem(QString::fromUtf8("\xF0\x9F\x94\x92 Quantum-Safe (P2MR)"), "p2mr");
    cmbWalletAddressMode_->setFixedHeight(34);
    // NOTE: "Private" is NOT an address type — it's a send mode.
    // Privacy is selected on the Send tab, not here. Both Taproot and
    // P2MR addresses can receive funds transparently or privately.
    cmbWalletAddressMode_->setToolTip(
        "Taproot: fast, small signatures (secp256k1)\n"
        "Quantum-Safe: ML-DSA-65 post-quantum signatures\n\n"
        "Privacy is a send mode, not an address type.\n"
        "Select private/transparent on the Send tab.");
    addressModeRow->addWidget(cmbWalletAddressMode_);
    lblReceivePathHint_ = new QLabel;
    lblReceivePathHint_->setFixedHeight(28);
    lblReceivePathHint_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    lblReceivePathHint_->setStyleSheet(
        "QLabel { color: #9da8b6; font-size: 11px; "
        "padding: 3px 8px; background: #171c23; border: 1px solid #303743; "
        "border-radius: 5px; }");
    addressModeRow->addWidget(lblReceivePathHint_);
    addressModeRow->addStretch();
    addressLayout->addLayout(addressModeRow);
    
    edtAddress_ = new QLineEdit;
    edtAddress_->setPlaceholderText("din1...");
    edtAddress_->setReadOnly(true);
    
    auto *btnRow = new QHBoxLayout;
    btnNewAddress_ = new QPushButton("Generate Public Address");
    btnValidate_ = new QPushButton("Validate");
    btnCopy_ = new QPushButton("Copy");
    
    btnRow->addWidget(btnNewAddress_);
    btnRow->addWidget(btnValidate_);
    btnRow->addWidget(btnCopy_);
    
    addressLayout->addWidget(edtAddress_);
    addressLayout->addLayout(btnRow);
    
    txtValidation_ = new QTextEdit;
    txtValidation_->setMaximumHeight(100);
    txtValidation_->setReadOnly(true);
    addressLayout->addWidget(txtValidation_);
    
    layout->addWidget(addressGroup);
    layout->addStretch();
    
    connect(btnNewAddress_, &QPushButton::clicked, this, &MainWindow::onNewAddress);
    connect(btnValidate_, &QPushButton::clicked, this, &MainWindow::onValidateAddress);
    connect(btnCopy_, &QPushButton::clicked, this, &MainWindow::onCopyAddress);
    connect(cmbWalletAddressMode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateWalletAddressModeUi(); });
    updateWalletAddressModeUi();
    
    wallet->setMinimumHeight(1800); // v7: must be tall enough for all sections to scroll
    wallet->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    tabs->addTab(makeScrollableTab(wallet), "\xF0\x9F\x92\xB0 Wallet");
  }

  // === Contracts Tab (Phase 4) ===
  {
    auto *contracts = new QWidget;
    auto *layout = new QVBoxLayout(contracts);

    // Header with summary
    auto *headerGroup = new QGroupBox("Active Contracts");
    auto *headerLayout = new QVBoxLayout(headerGroup);

    lblContractsSummary_ = new QLabel("Loading...");
    lblContractsSummary_->setWordWrap(true);
    headerLayout->addWidget(lblContractsSummary_);

    layout->addWidget(headerGroup);

    // Contract list table
    tblContracts_ = new QTableWidget;
    tblContracts_->setColumnCount(5);
    tblContracts_->setHorizontalHeaderLabels({
        "Type", "Amount", "Created", "Status", "Actions"
    });
    tblContracts_->horizontalHeader()->setStretchLastSection(true);
    tblContracts_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblContracts_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tblContracts_->verticalHeader()->setVisible(false);
    tblContracts_->setAlternatingRowColors(true);
    layout->addWidget(tblContracts_);

    // Action buttons at bottom
    auto *btnRow = new QHBoxLayout;
    auto *btnRefreshContracts = new QPushButton("Refresh");
    btnRefreshContracts->setStyleSheet(chromeButtonStyle());
    connect(btnRefreshContracts, &QPushButton::clicked, this, [this]() {
        refreshContractsList();
    });
    btnRow->addWidget(btnRefreshContracts);

    auto *btnNewContract = new QPushButton("+ New Contract");
    btnNewContract->setStyleSheet(chromeButtonStyle());
    btnNewContract->setToolTip("Switch to Send tab with Contract mode selected");
    connect(btnNewContract, &QPushButton::clicked, this, [this]() {
        // Switch to Send tab and select Contract action
        if (mainTabs_) {
            for (int i = 0; i < mainTabs_->count(); i++) {
                if (mainTabs_->tabText(i).contains("Send")) {
                    mainTabs_->setCurrentIndex(i);
                    break;
                }
            }
        }
        if (cmbSendAction_) {
            const int contractIndex = cmbSendAction_->findData("public_contract");
            if (contractIndex >= 0) {
                cmbSendAction_->setCurrentIndex(contractIndex);
            }
        }
    });
    btnRow->addWidget(btnNewContract);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    // Info label at bottom
    auto *lblInfo = new QLabel(
        "Contracts are programmable spending rules attached to your funds. "
        "Vaults lock funds to a specific template. Timelocks release after a duration. "
        "Private contracts hide the rules \xe2\x80\x94 only the ZK proof of satisfaction is visible on-chain."
    );
    lblInfo->setWordWrap(true);
    lblInfo->setStyleSheet("color: #888; font-size: 11px; padding: 8px;");
    layout->addWidget(lblInfo);

    tabs->addTab(makeScrollableTab(contracts), "\xF0\x9F\x93\x9C Contracts");
  }

  // === Send Tab ===
  {
    auto *send = new QWidget;
    auto *layout = new QVBoxLayout(send);
    
    auto *sendGroup = new QGroupBox("📤 Send / Convert");
    auto *sendLayout = new QGridLayout(sendGroup);

    // Pick the spend source explicitly. Taproot and P2MR are public
    // transparent sends; shielded notes use the private RPC surface.
    sendLayout->addWidget(new QLabel("Mode:"), 0, 0);
    cmbSendAction_ = new QComboBox;
    cmbSendAction_->addItem("Send publicly", "public_transfer");
    // Shielded send modes are withheld while kShieldedUiLockedOut is set (see
    // shieldedwidget.h). The Send tab is a SECOND entry point into the shielded
    // RPCs, independent of the shielded tab's own buttons: "Spend privately"
    // (private_transfer) is the addressed transfer that carries the
    // sender-retained spend-authority bug, and shield_to / unshield move value
    // in and out of the pool. Locking only the shielded tab would leave all
    // three reachable from here.
    //
    // Withheld rather than shown-disabled because a QComboBox entry cannot
    // carry its own explanation; the shielded tab states the reason.
    if (!kShieldedUiLockedOut) {
        cmbSendAction_->addItem("Spend privately", "private_transfer");
        cmbSendAction_->addItem("Send to shielded", "shield_to");
        cmbSendAction_->addItem("Convert to public", "unshield");
    }
    cmbSendAction_->addItem("Contracts", "public_contract");
    cmbSendAction_->setToolTip(
        kShieldedUiLockedOut
        ? "Send publicly: transparent Taproot or P2MR transfer\n"
          "Contracts: create an on-chain lock or batch spending rule\n"
          "\n"
          "Shielded modes are temporarily unavailable — see the Shielded tab."
        : "Send publicly: transparent Taproot or P2MR transfer\n"
          "Spend privately: shielded note transfer to a private address\n"
          "Send to shielded: fund a shielded dins1 address from your transparent balance\n"
          "Convert to public: unshield to a fresh wallet Taproot address\n"
          "Contracts: create an on-chain lock or batch spending rule");
    sendLayout->addWidget(cmbSendAction_, 0, 1);

    // Hidden cmbSendMode_ kept so legacy code paths that read it stay valid;
    // it mirrors the selected mode.
    cmbSendMode_ = new QComboBox;
    cmbSendMode_->hide();
    auto recomputeMode = [this]() {
        const QString mode = cmbSendAction_->currentData().toString();
        cmbSendMode_->clear();
        cmbSendMode_->addItem(mode, mode);
        cmbSendMode_->setCurrentIndex(0);
        updateSendModeUi();
    };
    connect(cmbSendAction_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [recomputeMode](int) { recomputeMode(); });
    recomputeMode();
    
    // Recipient address
    sendLayout->addWidget(new QLabel("Recipient:"), 1, 0);
    edtRecipient_ = new QLineEdit;
    edtRecipient_->setPlaceholderText("din1p... (Taproot) or din1r... (Quantum-Safe)");
    sendLayout->addWidget(edtRecipient_, 1, 1);
    
    // Amount
    sendLayout->addWidget(new QLabel("Amount (DIN):"), 2, 0);
    auto *amountLayout = new QHBoxLayout;
    edtAmount_ = new QLineEdit;
    edtAmount_->setPlaceholderText("0.00000000");
    // Validate decimal syntax only. Spendability and amount-range limits are
    // determined from the wallet balance, fees, and daemon consensus checks;
    // the obsolete 99-million-DIN UI cap was not a consensus rule.
    edtAmount_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^(?:0|[1-9][0-9]*)(?:\\.[0-9]{0,8})?$")),
        this));
    amountLayout->addWidget(edtAmount_);
    
    btnUseMax_ = new QPushButton("Max");
    btnUseMax_->setStyleSheet("QPushButton { padding: 5px; }");
    connect(btnUseMax_, &QPushButton::clicked, this, &MainWindow::onUseMaxAmount);
    amountLayout->addWidget(btnUseMax_);
    sendLayout->addLayout(amountLayout, 2, 1);
    
    // Fee priority selector (Phase 35)
    sendLayout->addWidget(new QLabel("Fee Priority:"), 3, 0);
    auto *feeLayout = new QHBoxLayout;
    cmbFeePreset_ = new QComboBox;
    cmbFeePreset_->addItem("Low (25+ blocks)", 25);      // ~25 blocks to confirm
    cmbFeePreset_->addItem("Normal (6 blocks)", 6);      // ~6 blocks to confirm (default)
    cmbFeePreset_->addItem("High (2 blocks)", 2);        // ~2 blocks to confirm
    cmbFeePreset_->addItem("Custom", -1);                // Manual fee entry
    cmbFeePreset_->setCurrentIndex(1);  // Default to Normal
    cmbFeePreset_->setToolTip("Select transaction priority (confirmation target)");
    connect(cmbFeePreset_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onFeePresetChanged);
    feeLayout->addWidget(cmbFeePreset_);
    // rc8: setCurrentIndex(1) above doesn't fire currentIndexChanged because
    // it ran before the connect, so the estimate cache stays empty. Trigger
    // an initial estimate ~3 s after construction (gives the daemon RPC time
    // to come up); collectSendForm() then has a real rate to apply.
    QTimer::singleShot(3000, this, [this]() { updateFeeEstimate(); });

    // Custom fee input (hidden by default)
    edtFee_ = new QLineEdit;
    edtFee_->setPlaceholderText("una/vB");
    edtFee_->setValidator(new QDoubleValidator(1.0, 10000.0, 2, this));
    edtFee_->setMaximumWidth(100);
    edtFee_->setVisible(false);  // Show only when "Custom" is selected
    feeLayout->addWidget(edtFee_);

    // Estimated fee display
    lblEstimatedFee_ = new QLabel("Est: ~0.00001 DIN");
    lblEstimatedFee_->setStyleSheet("QLabel { color: #666; font-size: 11px; }");
    feeLayout->addWidget(lblEstimatedFee_);
    feeLayout->addStretch();
    sendLayout->addLayout(feeLayout, 3, 1);

    // === Contract template section (Phase 2: Private Contract path) ===
    contractGroup_ = new QGroupBox(QString::fromUtf8("\xF0\x9F\x93\x9C Contract Template"));
    auto *contractLayout = new QVBoxLayout(contractGroup_);

    auto *templateRow = new QHBoxLayout;
    templateRow->addWidget(new QLabel("Template:"));
    cmbContractTemplate_ = new QComboBox;
    cmbContractTemplate_->addItem("Simple Lock", "vault");
    cmbContractTemplate_->addItem("Lock with Recovery Key", "conditional");
    cmbContractTemplate_->addItem("Time Lock", "timelock");
    cmbContractTemplate_->addItem("Batch Payment", "payroll");
    cmbContractTemplate_->addItem("Custom (Advanced)", "custom");
    cmbContractTemplate_->setToolTip("Simple Lock: funds locked to a spending template\n"
                                     "Timelock: funds locked for N blocks/hours/days\n"
                                     "Payroll: batch payment to multiple recipients (CTV)\n"
                                     "Custom Script: enter raw Tapscript hex");
    templateRow->addWidget(cmbContractTemplate_);
    templateRow->addStretch();
    contractLayout->addLayout(templateRow);

    // Stacked widget for template-specific fields
    contractTemplateStack_ = new QStackedWidget;

    // Page 0: Vault — no extra fields
    contractVaultPage_ = new QWidget;
    auto *vaultPageLayout = new QVBoxLayout(contractVaultPage_);
    auto *vaultInfo = new QLabel("Your funds will be locked to a specific withdrawal destination.\n"
                                 "Only the preset recipient can receive them.");
    vaultInfo->setWordWrap(true);
    vaultInfo->setStyleSheet("QLabel { color: #9fb3c8; padding: 4px; }");
    vaultPageLayout->addWidget(vaultInfo);
    contractTemplateStack_->addWidget(contractVaultPage_);

    // Page 1: Conditional Vault (CTV + CHECKSIG recovery)
    contractConditionalPage_ = new QWidget;
    auto *condPageLayout = new QVBoxLayout(contractConditionalPage_);
    auto *condInfo = new QLabel(
        "Lock with a backup recovery key.\n\n"
        "Primary: funds go to the preset destination automatically.\n"
        "Backup: if something goes wrong, use the recovery key to rescue funds.");
    condInfo->setWordWrap(true);
    condInfo->setStyleSheet("QLabel { color: #9fb3c8; padding: 4px; }");
    condPageLayout->addWidget(condInfo);

    auto *recoveryRow = new QHBoxLayout;
    recoveryRow->addWidget(new QLabel("Recovery key:"));
    edtRecoveryPubkey_ = new QLineEdit;
    edtRecoveryPubkey_->setPlaceholderText("Paste your recovery key here (64 characters)");
    edtRecoveryPubkey_->setMaxLength(64);
    edtRecoveryPubkey_->setStyleSheet("QLineEdit { font-family: monospace; font-size: 11px; }");
    recoveryRow->addWidget(edtRecoveryPubkey_);
    condPageLayout->addLayout(recoveryRow);
    condPageLayout->addStretch();
    contractTemplateStack_->addWidget(contractConditionalPage_);

    // Page 2: Timelock
    contractTimelockPage_ = new QWidget;
    auto *timelockPageLayout = new QHBoxLayout(contractTimelockPage_);
    timelockPageLayout->addWidget(new QLabel("Lock duration:"));
    spnTimelockDuration_ = new QSpinBox;
    spnTimelockDuration_->setMinimum(1);
    spnTimelockDuration_->setMaximum(100000);
    spnTimelockDuration_->setValue(144);
    timelockPageLayout->addWidget(spnTimelockDuration_);
    cmbTimelockUnit_ = new QComboBox;
    cmbTimelockUnit_->addItem("blocks", "blocks");
    cmbTimelockUnit_->addItem("hours", "hours");
    cmbTimelockUnit_->addItem("days", "days");
    timelockPageLayout->addWidget(cmbTimelockUnit_);
    timelockPageLayout->addStretch();
    contractTemplateStack_->addWidget(contractTimelockPage_);

    // Page 2: Payroll — batch payment to multiple recipients
    contractPayrollPage_ = new QWidget;
    auto *payrollPageLayout = new QVBoxLayout(contractPayrollPage_);

    auto *payrollInfo = new QLabel(
        "Payroll: batch payment locked to multiple recipients.\n"
        "The CTV template commits to the exact output set.\n"
        "In Private mode: all amounts and recipients are hidden in ZK.");
    payrollInfo->setWordWrap(true);
    payrollInfo->setStyleSheet("QLabel { color: #9fb3c8; padding: 4px; }");
    payrollPageLayout->addWidget(payrollInfo);

    // Recipient table
    tblPayrollRecipients_ = new QTableWidget;
    tblPayrollRecipients_->setColumnCount(2);
    tblPayrollRecipients_->setHorizontalHeaderLabels({"Address", "Amount (DIN)"});
    tblPayrollRecipients_->horizontalHeader()->setStretchLastSection(true);
    tblPayrollRecipients_->setRowCount(3); // Start with 3 empty rows
    for (int r = 0; r < 3; ++r) {
        tblPayrollRecipients_->setItem(r, 0, new QTableWidgetItem(""));
        tblPayrollRecipients_->setItem(r, 1, new QTableWidgetItem(""));
    }
    tblPayrollRecipients_->setMaximumHeight(180);
    payrollPageLayout->addWidget(tblPayrollRecipients_);

    // Add/Remove row buttons
    auto *payrollBtnRow = new QHBoxLayout;
    auto *btnAddRecipient = new QPushButton("+ Add Recipient");
    connect(btnAddRecipient, &QPushButton::clicked, this, [this]() {
        int row = tblPayrollRecipients_->rowCount();
        tblPayrollRecipients_->insertRow(row);
        tblPayrollRecipients_->setItem(row, 0, new QTableWidgetItem(""));
        tblPayrollRecipients_->setItem(row, 1, new QTableWidgetItem(""));
    });
    payrollBtnRow->addWidget(btnAddRecipient);

    auto *btnRemoveRecipient = new QPushButton("- Remove Last");
    connect(btnRemoveRecipient, &QPushButton::clicked, this, [this]() {
        int rows = tblPayrollRecipients_->rowCount();
        if (rows > 1) tblPayrollRecipients_->removeRow(rows - 1);
    });
    payrollBtnRow->addWidget(btnRemoveRecipient);

    lblPayrollTotal_ = new QLabel("Total: 0.00000000 DIN");
    lblPayrollTotal_->setStyleSheet("QLabel { font-weight: bold; }");
    payrollBtnRow->addWidget(lblPayrollTotal_);
    payrollBtnRow->addStretch();
    payrollPageLayout->addLayout(payrollBtnRow);

    // Update total when cells change
    connect(tblPayrollRecipients_, &QTableWidget::cellChanged, this, [this](int, int col) {
        if (col != 1) return;
        double total = 0;
        for (int r = 0; r < tblPayrollRecipients_->rowCount(); ++r) {
            auto *item = tblPayrollRecipients_->item(r, 1);
            if (item) total += item->text().toDouble();
        }
        if (lblPayrollTotal_)
            lblPayrollTotal_->setText("Total: " + QString::number(total, 'f', 8) + " DIN");
    });

    contractTemplateStack_->addWidget(contractPayrollPage_);

    // Page 3: Custom Script (Advanced)
    contractCustomPage_ = new QWidget;
    auto *customPageLayout = new QVBoxLayout(contractCustomPage_);
    lblCustomScriptWarning_ = new QLabel(QString::fromUtf8(
      "\xE2\x9A\xA0\xEF\xB8\x8F Advanced: Enter raw Tapscript hex. "
      "Incorrect scripts will fail verification."));
    lblCustomScriptWarning_->setWordWrap(true);
    lblCustomScriptWarning_->setStyleSheet("QLabel { color: #ffa94d; padding: 4px; font-weight: bold; }");
    customPageLayout->addWidget(lblCustomScriptWarning_);
    edtCustomScript_ = new QLineEdit;
    edtCustomScript_->setPlaceholderText("Enter Tapscript hex...");
    edtCustomScript_->setStyleSheet("QLineEdit { font-family: monospace; }");
    customPageLayout->addWidget(edtCustomScript_);
    contractTemplateStack_->addWidget(contractCustomPage_);

    contractLayout->addWidget(contractTemplateStack_);

    connect(cmbContractTemplate_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
      if (contractTemplateStack_) contractTemplateStack_->setCurrentIndex(index);
    });
    cmbContractTemplate_->setCurrentIndex(0);  // Default: Vault
    contractTemplateStack_->setCurrentIndex(0);

    contractGroup_->setVisible(false);  // Hidden by default, shown when contract mode selected
    sendLayout->addWidget(contractGroup_, 4, 0, 1, 2);

    // Send buttons row
    auto *sendBtnLayout = new QHBoxLayout;

    btnSend_ = new QPushButton("📤 Send Transaction");
    btnSend_->setStyleSheet(chromeButtonStyle());
    btnSend_->setToolTip("Sign and broadcast with software wallet");
    connect(btnSend_, &QPushButton::clicked, this, &MainWindow::onSendTransaction);
    sendBtnLayout->addWidget(btnSend_);

    // Create PSBT for hardware wallet
    btnHardwareWalletSend_ = new QPushButton("🔐 Hardware Wallet PSBT");
    btnHardwareWalletSend_->setStyleSheet(chromeButtonStyle());
    btnHardwareWalletSend_->setToolTip(hardwareWalletPsbtTooltip());
    connect(btnHardwareWalletSend_, &QPushButton::clicked, this, &MainWindow::onCreatePSBT);
    sendBtnLayout->addWidget(btnHardwareWalletSend_);

    sendLayout->addLayout(sendBtnLayout, 5, 0, 1, 2);

    layout->addWidget(sendGroup);
    
    // Status label
    lblSendStatus_ = new QLabel();
    lblSendStatus_->setWordWrap(true);
    lblSendStatus_->setStyleSheet("QLabel { padding: 10px; }");
    layout->addWidget(lblSendStatus_);
    
    // Result display
    auto *resultGroup = new QGroupBox("Transaction Result");
    auto *resultLayout = new QVBoxLayout(resultGroup);
    txtSendResult_ = new QTextEdit;
    txtSendResult_->setReadOnly(true);
    txtSendResult_->setMaximumHeight(150);
    txtSendResult_->setPlaceholderText("Transaction details will appear here after sending...");
    resultLayout->addWidget(txtSendResult_);
    layout->addWidget(resultGroup);
    
    layout->addStretch();
    
    tabs->addTab(makeScrollableTab(send), "\xF0\x9F\x93\xA4 Send");
  }
  
  // === Receive Tab (HD Address List) ===
  {
    auto *receive = new QWidget;
    auto *layout = new QVBoxLayout(receive);
    
    auto *headerLayout = new QHBoxLayout;
    auto *lblHeader = new QLabel("📥 Receive");
    lblHeader->setStyleSheet(chromeSectionLabelStyle());
    headerLayout->addWidget(lblHeader);
    headerLayout->addStretch();

    cmbReceiveMode_ = new QComboBox;
    cmbReceiveMode_->addItem(QString::fromUtf8("\xF0\x9F\x93\x8B All"), "all");
    cmbReceiveMode_->addItem(QString::fromUtf8("\xF0\x9F\x8C\x90 Taproot"), "taproot");
    cmbReceiveMode_->addItem(QString::fromUtf8("\xF0\x9F\x94\x92 Quantum-Safe (P2MR)"), "p2mr");
    connect(cmbReceiveMode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
              updateReceiveModeUi();
              rpc_->callNamed("wallet.listaddresses", QJsonObject{{"count", 200}});
            });
    headerLayout->addWidget(cmbReceiveMode_);

    btnDeriveAddress_ = new QPushButton(QString::fromUtf8("\xF0\x9F\x86\x95 New Address"));
    btnDeriveAddress_->setStyleSheet(chromeButtonStyle());
    btnDeriveAddress_->setToolTip("Derive a new address of the currently selected type");
    connect(btnDeriveAddress_, &QPushButton::clicked, this, &MainWindow::onDeriveNewAddress);
    headerLayout->addWidget(btnDeriveAddress_);
    headerLayout->addWidget(btnConsolidate_);

    layout->addLayout(headerLayout);

    // Single unified address table. Filter is applied in the listaddresses
    // response handler based on cmbReceiveMode_'s current value.
    tblAddresses_ = new QTableWidget(0, 6);
    tblAddresses_->setHorizontalHeaderLabels({"Index", "Address", "Label", "Balance (DIN)", "Path", "Actions"});
    tblAddresses_->horizontalHeader()->setStretchLastSection(false);
    tblAddresses_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    tblAddresses_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    tblAddresses_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    tblAddresses_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblAddresses_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tblAddresses_->setSortingEnabled(true);
    tblAddresses_->setAlternatingRowColors(true);
    tblAddresses_->setStyleSheet(chromeTableStyle());
    tblAddresses_->setColumnWidth(2, 120);

    labelEditInProgress_ = false;
    connect(tblAddresses_, &QTableWidget::cellDoubleClicked, this, &MainWindow::onAddressLabelDoubleClicked);
    connect(tblAddresses_, &QTableWidget::itemChanged, this, &MainWindow::onAddressLabelChanged);

    layout->addWidget(tblAddresses_, 1);

    btnLoadAllAddresses_ = new QPushButton("📋 Reload");
    btnLoadAllAddresses_->setStyleSheet(chromeButtonStyle());
    connect(btnLoadAllAddresses_, &QPushButton::clicked, [this]() {
      rpc_->callNamed("wallet.listaddresses", QJsonObject{{"count", 200}});
    });
    layout->addWidget(btnLoadAllAddresses_);

    tabs->addTab(makeScrollableTab(receive), "📥 Receive");
  }
  
  // === Transactions Tab (History) ===
  {
    auto *transactions = new QWidget;
    auto *layout = new QVBoxLayout(transactions);
    
    auto *headerLayout = new QHBoxLayout;
    auto *lblHeader = new QLabel("📜 Transaction History");
    lblHeader->setStyleSheet(chromeSectionLabelStyle());
    headerLayout->addWidget(lblHeader);
    headerLayout->addStretch();

    auto *lblTypeFilter = new QLabel("View:");
    headerLayout->addWidget(lblTypeFilter);

    cmbTxTypeFilter_ = new QComboBox;
    cmbTxTypeFilter_->addItem("All", "all");
    cmbTxTypeFilter_->addItem("Mined", "mined");
    cmbTxTypeFilter_->addItem("Sent", "sent");
    cmbTxTypeFilter_->addItem("Received", "received");
    cmbTxTypeFilter_->addItem("Contract", "contract");
    headerLayout->addWidget(cmbTxTypeFilter_);
    connect(cmbTxTypeFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { loadTransactionHistory(); });
    
    auto *btnRefreshTxs = new QPushButton("🔄 Refresh");
    btnRefreshTxs->setStyleSheet(chromeButtonStyle());
    connect(btnRefreshTxs, &QPushButton::clicked, this, &MainWindow::loadTransactionHistory);
    headerLayout->addWidget(btnRefreshTxs);
    
    layout->addLayout(headerLayout);
    
    // Transaction table
    tblTransactions_ = new QTableWidget(0, 7);
    tblTransactions_->setHorizontalHeaderLabels({"Date", "Time", "Type", "Amount (DIN)", "Address", "Confirmations", "TxID"});
    tblTransactions_->horizontalHeader()->setStretchLastSection(false);
    tblTransactions_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch); // Address column
    tblTransactions_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch); // TxID column
    tblTransactions_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblTransactions_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tblTransactions_->setSortingEnabled(true);
    tblTransactions_->setAlternatingRowColors(true);
    tblTransactions_->setStyleSheet(chromeTableStyle());
    layout->addWidget(tblTransactions_);
    
    // Auto-load transaction history after 5 seconds
    QTimer::singleShot(5000, this, &MainWindow::loadTransactionHistory);
    
    tabs->addTab(transactions, "📜 Transactions");
  }
  
  // === UTXOs Tab (Advanced) ===
  {
    auto *utxos = new QWidget;
    utxoTabWidget_ = utxos;
    auto *layout = new QVBoxLayout(utxos);
    
    auto *headerLayout = new QHBoxLayout;
    auto *lblHeader = new QLabel("🔗 Unspent Outputs (UTXOs)");
    lblHeader->setStyleSheet(chromeSectionLabelStyle());
    headerLayout->addWidget(lblHeader);
    headerLayout->addStretch();
    
    auto *btnRefreshUTXOs = new QPushButton("🔄 Refresh");
    btnRefreshUTXOs->setStyleSheet(chromeButtonStyle());
    connect(btnRefreshUTXOs, &QPushButton::clicked, this,
            [this]() { requestUtxoRefresh(true); });
    headerLayout->addWidget(btnRefreshUTXOs);
    
    layout->addLayout(headerLayout);
    
    // UTXO table (added "Maturity" column for coinbase tracking)
    tblUTXOs_ = new QTableWidget(0, 7);
    tblUTXOs_->setHorizontalHeaderLabels({"TxID", "Vout", "Amount", "Confirmations", "Maturity", "Address", "Spendable"});
    tblUTXOs_->horizontalHeader()->setStretchLastSection(true);
    tblUTXOs_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    tblUTXOs_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblUTXOs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tblUTXOs_->setAlternatingRowColors(true);
    tblUTXOs_->setStyleSheet(chromeTableStyle());
    tblUTXOs_->setObjectName("tblUTXOs");
    layout->addWidget(tblUTXOs_);

    auto* pageLayout = new QHBoxLayout;
    lblUtxoPage_ = new QLabel("Open this tab to load unspent outputs.");
    btnPrevUtxoPage_ = new QPushButton("Previous");
    btnNextUtxoPage_ = new QPushButton("Next");
    btnPrevUtxoPage_->setEnabled(false);
    btnNextUtxoPage_->setEnabled(false);
    connect(btnPrevUtxoPage_, &QPushButton::clicked, this, [this]() {
      currentUtxoPage_ = std::max(0, currentUtxoPage_ - 1);
      renderUtxoPage();
    });
    connect(btnNextUtxoPage_, &QPushButton::clicked, this, [this]() {
      ++currentUtxoPage_;
      renderUtxoPage();
    });
    pageLayout->addWidget(lblUtxoPage_);
    pageLayout->addStretch();
    pageLayout->addWidget(btnPrevUtxoPage_);
    pageLayout->addWidget(btnNextUtxoPage_);
    layout->addLayout(pageLayout);
    
    tabs->addTab(utxos, "🔗 UTXOs");
  }

  // === Hardware Wallet Tab (Production-Ready) ===
  // Supports: Coldcard (SD card), Ledger, Trezor, Keystone, Passport (QR)
  {
    hardwareWalletWidget_ = new HardwareWalletWidget(rpc_);
    connect(hardwareWalletWidget_, &HardwareWalletWidget::transactionBroadcasted,
            this, &MainWindow::handleHardwareWalletBroadcast);
    tabs->addTab(hardwareWalletWidget_, "🔐 Hardware Wallet");
  }

  // === DPI Pay/Collect Tab ===
  {
    dpiWidget_ = new DpiWidget(rpc_, this);
    tabs->addTab(dpiWidget_, "💳 Pay/Collect");
  }

#ifdef DIN_EXPERIMENTAL_FEATURES
  // ═══════════════════════════════════════════════════════════════════
  // 🧪 EXPERIMENTAL FEATURES (disabled in production builds)
  // ═══════════════════════════════════════════════════════════════════
  // These tabs are under development and not ready for mainnet deployment.
  // To enable: cmake -DDIN_EXPERIMENTAL_FEATURES=ON
  // ═══════════════════════════════════════════════════════════════════

  // === Payments Tab ===
  {
    paymentsWidget_ = new PaymentsWidget(rpc_, ws_, this);
    tabs->addTab(paymentsWidget_, "💳 Payments");
  }

  // === Escrow Tab ===
  {
    escrowWidget_ = new EscrowWidget(rpc_, ws_, this);
    tabs->addTab(escrowWidget_, "⚖️ Escrow");
  }

  // === Marketplace Tab ===
  {
    marketplaceWidget_ = new MarketplaceWidget(rpc_, ws_, this);
    tabs->addTab(marketplaceWidget_, "🛒 Marketplace");
  }

  // ═══════════════════════════════════════════════════════════════════
  // END EXPERIMENTAL FEATURES
  // ═══════════════════════════════════════════════════════════════════
#endif // DIN_EXPERIMENTAL_FEATURES

  // === Liquidity Vault Tab (Track C — daemon-side custodial vault) ===
  // Production feature, not gated behind DIN_EXPERIMENTAL_FEATURES.
  // Talks to vault.* RPC family on the embedded dinerod (boots in
  // shadow mode by default; flip vault.shadow=0 once you're past
  // Stage 0).
  {
    vaultPanel_ = new VaultPanel(rpc_, this);
    tabs->addTab(vaultPanel_, "🏦 Liquidity Vault");
  }

  // === Shielded Tab (Phase 5 — daemon shielded pool, gated by
  // chainparams.shielded_activation_height). On mainnet/testnet the
  // RPCs return shielded_not_active and the widget surfaces that as a
  // banner; on regtest it is fully functional.
  {
    shieldedWidget_ = new ShieldedWidget(rpc_, this);
    tabs->addTab(shieldedWidget_, "🛡 Shielded");
  }

  // === ⚡ Lightning Network Tab (Phase 7) ===
  // NOTE: Lightning Network moved to separate lightning-main branch (L2)
  // REMOVED for L1 purity - uncomment on lightning-main branch only
  // {
  //   lightningWidget_ = new dinero::LightningWidget(rpc_, this);
  //   tabs->addTab(lightningWidget_, "⚡ Lightning");
  // }

  // === Bridge Tab (DISABLED - Not ready for production) ===
  // TODO: Re-enable when custodial provider APIs are implemented
  // {
  //   bridgeWidget_ = new BridgeWidget(rpc_, ws_, this);
  //   tabs->addTab(bridgeWidget_, "💱 Bridge");
  // }

  // === Explorer Tab ===
  {
    auto *explorer = new QWidget;
    auto *layout = new QVBoxLayout(explorer);
    layout->setSpacing(12);

    auto styleExplorerTable = [](QTableWidget* table) {
      table->setSelectionBehavior(QTableWidget::SelectRows);
      table->setEditTriggers(QTableWidget::NoEditTriggers);
      table->setAlternatingRowColors(true);
      table->verticalHeader()->setVisible(false);
      table->horizontalHeader()->setStretchLastSection(true);
      table->setStyleSheet(
          "QTableWidget { gridline-color: #343a43; background: #1d2126; "
          "alternate-background-color: #20252b; color: #d8dee7; } "
          "QHeaderView::section { background: #272c33; color: #d8dee7; padding: 5px; "
          "font-weight: bold; border: 1px solid #373d46; }");
    };

    auto makeExplorerStat = [](const QString& label, QLabel** valueOut) {
      auto* box = new QWidget;
      auto* boxLayout = new QVBoxLayout(box);
      boxLayout->setContentsMargins(12, 9, 12, 9);
      boxLayout->setSpacing(2);
      auto* title = new QLabel(label);
      title->setStyleSheet("QLabel { color: #99a4b3; font-size: 11px; }");
      auto* value = new QLabel("-");
      value->setTextInteractionFlags(Qt::TextSelectableByMouse);
      value->setStyleSheet("QLabel { color: #eef3f8; font-size: 15px; font-weight: 700; }");
      boxLayout->addWidget(title);
      boxLayout->addWidget(value);
      box->setStyleSheet("QWidget { background: #242a31; border: 1px solid #353c46; border-radius: 8px; }");
      *valueOut = value;
      return box;
    };

    auto *searchGroup = new QGroupBox("Chain Explorer");
    auto *searchLayout = new QVBoxLayout(searchGroup);

    auto *searchRow = new QHBoxLayout;
    edtBlockHash_ = new QLineEdit;
    edtBlockHash_->setPlaceholderText("Search block height, transaction hash, block hash, or din1/tdin1/rdin1 address...");
    edtBlockHash_->setMinimumHeight(34);
    btnGetBlock_ = new QPushButton("Search");
    btnGetBlock_->setMinimumHeight(34);

    searchRow->addWidget(edtBlockHash_, 1);
    searchRow->addWidget(btnGetBlock_);
    searchLayout->addLayout(searchRow);

    lblExplorerStatus_ = new QLabel("Search a block, transaction, or address. Latest blocks load from the connected daemon.");
    lblExplorerStatus_->setWordWrap(true);
    lblExplorerStatus_->setStyleSheet(
        "QLabel { color: #aab4c2; background: #20252b; border: 1px solid #333a43; "
        "border-radius: 8px; padding: 8px 10px; }");
    searchLayout->addWidget(lblExplorerStatus_);

    auto *statsGrid = new QGridLayout;
    statsGrid->setHorizontalSpacing(8);
    statsGrid->setVerticalSpacing(8);
    statsGrid->addWidget(makeExplorerStat("Block Height", &lblExplorerHeight_), 0, 0);
    statsGrid->addWidget(makeExplorerStat("Difficulty", &lblExplorerDifficulty_), 0, 1);
    statsGrid->addWidget(makeExplorerStat("Hashrate", &lblExplorerHashrate_), 0, 2);
    statsGrid->addWidget(makeExplorerStat("Money Supply", &lblExplorerSupply_), 0, 3);
    searchLayout->addLayout(statsGrid);

    lblBestBlock_ = new QLabel("Best Block: -");
    lblBestBlock_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lblBestBlock_->setStyleSheet("QLabel { color: #9da8b6; font-family: monospace; padding-top: 2px; }");
    searchLayout->addWidget(lblBestBlock_);

    layout->addWidget(searchGroup);

    auto *blocksGroup = new QGroupBox("Latest Blocks");
    auto *blocksLayout = new QVBoxLayout(blocksGroup);
    tblRecentBlocks_ = new QTableWidget(0, 5);
    tblRecentBlocks_->setHorizontalHeaderLabels({"Height", "Hash", "Time", "Txns", "Nonce"});
    styleExplorerTable(tblRecentBlocks_);
    tblRecentBlocks_->setColumnWidth(0, 85);
    tblRecentBlocks_->setColumnWidth(1, 230);
    tblRecentBlocks_->setColumnWidth(2, 125);
    tblRecentBlocks_->setColumnWidth(3, 60);
    tblRecentBlocks_->setColumnWidth(4, 120);
    tblRecentBlocks_->setMaximumHeight(270);
    tblRecentBlocks_->setToolTip("Double-click a row to open block detail.");
    blocksLayout->addWidget(tblRecentBlocks_);

    connect(tblRecentBlocks_, &QTableWidget::cellDoubleClicked, this,
        [this](int row, int) {
          auto *item = tblRecentBlocks_->item(row, 1);
          if (!item) return;
          const QString hash = item->data(Qt::UserRole).toString();
          if (hash.isEmpty()) return;
          edtBlockHash_->setText(hash);
          pendingExplorerBlockHash_ = hash;
          pendingExplorerTxLookup_.clear();
          pendingExplorerTxFallbackBlockHash_.clear();
          resetExplorerDetailTables();
          setExplorerStatus(QString("Loading block %1...").arg(explorerShortValue(hash)));
          rpc_->call("blockchain.getblock", QJsonArray{hash, 1});
        });

    layout->addWidget(blocksGroup);

    auto *detailGroup = new QGroupBox("Explorer Detail");
    auto *detailLayout = new QVBoxLayout(detailGroup);

    lblExplorerSummary_ = new QLabel;
    lblExplorerSummary_->setWordWrap(true);
    lblExplorerSummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lblExplorerSummary_->setStyleSheet(
        "QLabel { color: #d8dee7; background: #232932; border: 1px solid #343c47; "
        "border-radius: 8px; padding: 10px; }");
    lblExplorerSummary_->hide();
    detailLayout->addWidget(lblExplorerSummary_);

    txtBlockData_ = new QTextEdit;
    txtBlockData_->setReadOnly(true);
    txtBlockData_->setMinimumHeight(180);
    txtBlockData_->setStyleSheet(
        "QTextEdit { background: #1d2126; color: #d8dee7; border: 1px solid #343c47; "
        "border-radius: 8px; padding: 10px; font-family: monospace; }");
    txtBlockData_->setHtml("<p style='color:#9da8b6'>Select a latest block or search for a transaction, block, or address.</p>");
    detailLayout->addWidget(txtBlockData_);

    tblExplorerTransactions_ = new QTableWidget(0, 5);
    tblExplorerTransactions_->setHorizontalHeaderLabels({"TXID", "Type", "Inputs", "Outputs", "Total Out"});
    styleExplorerTable(tblExplorerTransactions_);
    tblExplorerTransactions_->setColumnWidth(0, 320);
    tblExplorerTransactions_->setColumnWidth(1, 110);
    tblExplorerTransactions_->setColumnWidth(2, 70);
    tblExplorerTransactions_->setColumnWidth(3, 70);
    tblExplorerTransactions_->setColumnWidth(4, 120);
    tblExplorerTransactions_->hide();
    detailLayout->addWidget(tblExplorerTransactions_);

    connect(tblExplorerTransactions_, &QTableWidget::cellDoubleClicked, this,
        [this](int row, int) {
          auto *item = tblExplorerTransactions_->item(row, 0);
          if (!item) return;
          const QString txid = item->data(Qt::UserRole).toString();
          if (txid.isEmpty()) return;
          edtBlockHash_->setText(txid);
          pendingExplorerTxLookup_ = txid;
          pendingExplorerTxFallbackBlockHash_.clear();
          pendingExplorerBlockHash_.clear();
          pendingExplorerTxAliasTried_ = false;
          resetExplorerDetailTables();
          setExplorerStatus(QString("Loading transaction %1...").arg(explorerShortValue(txid)));
          rpc_->call("gettransaction", QJsonArray{txid});
        });

    auto *ioRow = new QHBoxLayout;
    tblExplorerInputs_ = new QTableWidget(0, 3);
    tblExplorerInputs_->setHorizontalHeaderLabels({"Input", "Type", "Amount"});
    styleExplorerTable(tblExplorerInputs_);
    tblExplorerInputs_->setColumnWidth(0, 320);
    tblExplorerInputs_->setColumnWidth(1, 120);
    tblExplorerInputs_->setColumnWidth(2, 120);
    tblExplorerInputs_->hide();
    ioRow->addWidget(tblExplorerInputs_);

    tblExplorerOutputs_ = new QTableWidget(0, 4);
    tblExplorerOutputs_->setHorizontalHeaderLabels({"Output", "Type", "Address / Script", "Amount"});
    styleExplorerTable(tblExplorerOutputs_);
    tblExplorerOutputs_->setColumnWidth(0, 60);
    tblExplorerOutputs_->setColumnWidth(1, 110);
    tblExplorerOutputs_->setColumnWidth(2, 320);
    tblExplorerOutputs_->setColumnWidth(3, 120);
    tblExplorerOutputs_->hide();
    ioRow->addWidget(tblExplorerOutputs_);
    detailLayout->addLayout(ioRow);

    tblExplorerUTXOs_ = new QTableWidget(0, 5);
    tblExplorerUTXOs_->setHorizontalHeaderLabels({"TXID", "Block", "Status", "Amount", "Extra"});
    styleExplorerTable(tblExplorerUTXOs_);
    tblExplorerUTXOs_->setColumnWidth(0, 340);
    tblExplorerUTXOs_->setColumnWidth(1, 90);
    tblExplorerUTXOs_->setColumnWidth(2, 110);
    tblExplorerUTXOs_->setColumnWidth(3, 130);
    tblExplorerUTXOs_->hide();
    detailLayout->addWidget(tblExplorerUTXOs_);

    connect(tblExplorerUTXOs_, &QTableWidget::cellDoubleClicked, this,
        [this](int row, int) {
          auto *item = tblExplorerUTXOs_->item(row, 0);
          if (!item) return;
          const QString txid = item->data(Qt::UserRole).toString();
          if (txid.isEmpty()) return;
          edtBlockHash_->setText(txid);
          pendingExplorerTxLookup_ = txid;
          pendingExplorerTxFallbackBlockHash_.clear();
          pendingExplorerBlockHash_.clear();
          pendingExplorerTxAliasTried_ = false;
          resetExplorerDetailTables();
          setExplorerStatus(QString("Loading transaction %1...").arg(explorerShortValue(txid)));
          rpc_->call("gettransaction", QJsonArray{txid});
        });

    layout->addWidget(detailGroup);

    connect(btnGetBlock_, &QPushButton::clicked, this, &MainWindow::onRefreshBlocks);
    connect(edtBlockHash_, &QLineEdit::returnPressed, this, &MainWindow::onRefreshBlocks);

    tabs->addTab(makeScrollableTab(explorer), "Explorer");
  }
  
  // === Mining Tab ===
  {
    miningTabWidget_ = nullptr;
    miningInfoGroup_ = nullptr;
    miningControlsGroup_ = nullptr;
    lblMiningOutputSection_ = nullptr;

#ifdef HAVE_QT_QUICK
    // QML mining widget (if Qt Quick is available)
    try {
        miningWidget_ = new QQuickWidget;
        miningWidget_->setResizeMode(QQuickWidget::SizeRootObjectToView);
        
        // Set context properties for paths
        QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (dataDir.isEmpty()) {
#if defined(Q_OS_MAC)
            dataDir = QDir::homePath() + "/Library/Application Support/Dinero";
#elif defined(Q_OS_WIN)
            dataDir = QDir::homePath() + "/AppData/Roaming/Dinero";
#else
            dataDir = QDir::homePath() + "/.local/share/dinero";
#endif
        }
        
        QString minerPath;
#if defined(Q_OS_MAC)
        minerPath = QCoreApplication::applicationDirPath() + "/../Resources/dinero-miner";
#elif defined(Q_OS_WIN)
        minerPath = QCoreApplication::applicationDirPath() + "/dinero-miner.exe";
#else
        minerPath = QCoreApplication::applicationDirPath() + "/dinero-miner";
#endif
        
        miningWidget_->rootContext()->setContextProperty("DefaultDataDir", dataDir);
        miningWidget_->rootContext()->setContextProperty("DefaultMinerPath", minerPath);
        
        // Load QML
        miningWidget_->setSource(QUrl("qrc:/MinerPane.qml"));
        
        if (miningWidget_->status() == QQuickWidget::Error) {
            qWarning() << "QML Errors:";
            for (const auto& err : miningWidget_->errors()) {
                qWarning() << err.toString();
            }
            // Fall back to message tab
            delete miningWidget_;
            miningWidget_ = nullptr;
            throw std::runtime_error("QML failed to load");
        }
        
        tabs->addTab(miningWidget_, "⛏️ Mining");
        miningTabWidget_ = miningWidget_;
    } catch (...) {
        // Fallback to simple message if QML fails
        qWarning() << "QML mining tab failed, using fallback";
        auto *mining = new QWidget;
        auto *layout = new QVBoxLayout(mining);
        auto *label = new QLabel(
          "<h2>⛏️ Mining</h2>"
          "<p><b>Use command-line miner:</b></p>"
          "<p>1. Generate address in Wallet tab</p>"
          "<p>2. Run in terminal:</p>"
          "<pre>./build/dinero-miner --rpc http://127.0.0.1:20998/ \\\n"
          "  --address YOUR_ADDRESS --threads 8</pre>"
        );
        label->setWordWrap(true);
        label->setTextFormat(Qt::RichText);
        layout->addWidget(label);
        layout->addStretch();
        auto* miningPage = makeScrollableTab(mining);
        tabs->addTab(miningPage, "⛏️ Mining");
        miningTabWidget_ = miningPage;
    }
#else
    // Widgets-based mining tab
    auto *mining = new QWidget;
    auto *layout = new QVBoxLayout(mining);
    layout->setSpacing(6); // Compact spacing

    // Quick Start Mining - SUPER COMPACT
    auto *quickMineGroup = new QGroupBox("");
    miningControlsGroup_ = quickMineGroup;
    quickMineGroup->setStyleSheet(
      "QGroupBox { border: 1px solid #30353d; border-radius: 10px; margin-top: 0px; "
      "padding-top: 8px; background: #20242a; }");
    quickMineGroup->setMaximumHeight(150); // Increased height for miner type row
    auto *quickMineLayout = new QVBoxLayout(quickMineGroup);
    quickMineLayout->setSpacing(6);
    quickMineLayout->setContentsMargins(8, 8, 8, 8);

    // Row 0: Mining mode selection (solo default, pool optional)
    auto *modeRow = new QHBoxLayout;
    modeRow->setSpacing(6);
    modeRow->addWidget(new QLabel("Mode:"));
    cmbMiningMode_ = new QComboBox;
    cmbMiningMode_->addItem("Solo Mining (Default)", "solo");
    cmbMiningMode_->addItem("Pool Mining (Stratum V1)", "pool");
    cmbMiningMode_->addItem("Pool Mining (SV2)", "sv2_pool");
    cmbMiningMode_->setCurrentIndex(0);
    cmbMiningMode_->setFixedHeight(30);
    cmbMiningMode_->setStyleSheet(
      "QComboBox { background: #1f2328; color: #d7dde5; border: 1px solid #353b44; "
      "border-radius: 8px; padding: 0 8px; font-size: 12px; } "
      "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 22px; border-left: 1px solid #353b44; background: #21262c; border-top-right-radius: 8px; border-bottom-right-radius: 8px; } QComboBox::down-arrow { width: 0; height: 0; margin-right: 7px; border-left: 5px solid transparent; border-right: 5px solid transparent; border-top: 6px solid #c4cdd9; } QComboBox::down-arrow:disabled { border-top: 6px solid #5b6470; }");
    cmbMiningMode_->setToolTip(
      "Solo = mine directly with your node.\n"
      "Pool (Stratum V1) = submit shares to a V1 pool (legacy, cleartext).\n"
      "Pool (SV2) = Noise-encrypted pool mining. Choose Shared rewards\n"
      "for PPLNS payouts or Solo rewards for a miner-owned coinbase.");
    modeRow->addWidget(cmbMiningMode_);

    lblStratumEndpoint_ = new QLabel("Pool Endpoint:");
    lblStratumEndpoint_->setVisible(false);
    modeRow->addWidget(lblStratumEndpoint_);

    edtStratumEndpoint_ = new QLineEdit;
    edtStratumEndpoint_->setPlaceholderText("127.0.0.1:3333");
    edtStratumEndpoint_->setStyleSheet(miningControlFieldStyle());
    edtStratumEndpoint_->setFixedHeight(30);
    edtStratumEndpoint_->setVisible(false);
    const QString savedOrEnvEndpoint = explicitStratumEndpoint();
    if (!savedOrEnvEndpoint.isEmpty()) {
      edtStratumEndpoint_->setText(savedOrEnvEndpoint);
    }
    if (qEnvironmentVariableIsSet("DINERO_STRATUM_ENDPOINT")) {
      edtStratumEndpoint_->setToolTip(
        "Endpoint currently overridden by DINERO_STRATUM_ENDPOINT environment variable.");
    }
    connect(edtStratumEndpoint_, &QLineEdit::editingFinished, this, [this]() {
      if (!edtStratumEndpoint_) {
        return;
      }
      const QString endpoint = edtStratumEndpoint_->text().trimmed();
      QSettings().setValue("mining/stratum_endpoint", endpoint);
      updateStratumIdentityLabel();
    });
    modeRow->addWidget(edtStratumEndpoint_, 2);

    btnLocalStratum_ = new QPushButton("Start Local");
    btnLocalStratum_->setStyleSheet(headerButtonStyle());
    btnLocalStratum_->setFixedSize(104, 30);
    btnLocalStratum_->setVisible(false);
    btnLocalStratum_->setToolTip(
      "Start a localhost dinero-stratum server and use it as this pool endpoint.");
    connect(btnLocalStratum_, &QPushButton::clicked,
            this, &MainWindow::onToggleLocalStratumServer);
    modeRow->addWidget(btnLocalStratum_);

    // ── SV2 pool endpoint (separate from V1, own settings keys) ──
    lblSv2Endpoint_ = new QLabel("SV2 Pool:");
    lblSv2Endpoint_->setVisible(false);
    modeRow->addWidget(lblSv2Endpoint_);

    edtSv2Endpoint_ = new QLineEdit;
    edtSv2Endpoint_->setPlaceholderText(sv2PoolEndpointDefault());
    edtSv2Endpoint_->setStyleSheet(miningControlFieldStyle());
    edtSv2Endpoint_->setFixedHeight(30);
    edtSv2Endpoint_->setVisible(false);
    edtSv2Endpoint_->setText(sv2PoolEndpoint());
    if (qEnvironmentVariableIsSet("DINERO_SV2_POOL")) {
      edtSv2Endpoint_->setToolTip(
        "SV2 endpoint currently overridden by DINERO_SV2_POOL environment variable.");
    }
    connect(edtSv2Endpoint_, &QLineEdit::editingFinished, this, [this]() {
      if (!edtSv2Endpoint_) return;
      QSettings().setValue("mining/sv2_endpoint", edtSv2Endpoint_->text().trimmed());
    });
    modeRow->addWidget(edtSv2Endpoint_, 2);
    quickMineLayout->addLayout(modeRow);

    // SV2 noise pubkey lives on its own row to avoid crowding the endpoint row.
    auto *sv2PubkeyRow = new QHBoxLayout;
    sv2PubkeyRow->setSpacing(6);
    lblSv2Pubkey_ = new QLabel("SV2 Pubkey:");
    lblSv2Pubkey_->setVisible(false);
    sv2PubkeyRow->addWidget(lblSv2Pubkey_);

    edtSv2Pubkey_ = new QLineEdit;
    edtSv2Pubkey_->setPlaceholderText(sv2PoolServerPubkeyDefault());
    edtSv2Pubkey_->setStyleSheet(miningControlFieldStyle());
    edtSv2Pubkey_->setFixedHeight(30);
    edtSv2Pubkey_->setVisible(false);
    edtSv2Pubkey_->setText(sv2PoolServerPubkey());
    edtSv2Pubkey_->setToolTip(
      "64-hex-char static public key of the SV2 pool. Pinned on connect — "
      "leave blank only for first-contact TOFU (not recommended).");
    if (qEnvironmentVariableIsSet("DINERO_SV2_SERVER_PUBKEY")) {
      edtSv2Pubkey_->setToolTip(edtSv2Pubkey_->toolTip() +
        "\nCurrently overridden by DINERO_SV2_SERVER_PUBKEY environment variable.");
    }
    connect(edtSv2Pubkey_, &QLineEdit::editingFinished, this, [this]() {
      if (!edtSv2Pubkey_) return;
      QSettings().setValue("mining/sv2_server_pubkey", edtSv2Pubkey_->text().trimmed());
    });
    sv2PubkeyRow->addWidget(edtSv2Pubkey_, 2);

    // Backend: CPU multi-thread or GPU (Metal on Apple Silicon).
    lblSv2Backend_ = new QLabel("Backend:");
    lblSv2Backend_->setVisible(false);
    sv2PubkeyRow->addWidget(lblSv2Backend_);

    cmbSv2Backend_ = new QComboBox;
    cmbSv2Backend_->addItem("CPU (multi-thread)", "cpu");
#ifdef Q_OS_MAC
    cmbSv2Backend_->addItem("GPU (Metal)", "metal");
#endif
    {
      const QString saved =
        QSettings().value("mining/sv2_backend").toString();
      const int idx = cmbSv2Backend_->findData(saved);
      cmbSv2Backend_->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    cmbSv2Backend_->setFixedHeight(30);
    cmbSv2Backend_->setStyleSheet(
      "QComboBox { background: #1f2328; color: #d7dde5; border: 1px solid #353b44; "
      "border-radius: 8px; padding: 0 8px; font-size: 12px; } "
      "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 22px; border-left: 1px solid #353b44; background: #21262c; border-top-right-radius: 8px; border-bottom-right-radius: 8px; } QComboBox::down-arrow { width: 0; height: 0; margin-right: 7px; border-left: 5px solid transparent; border-right: 5px solid transparent; border-top: 6px solid #c4cdd9; } QComboBox::down-arrow:disabled { border-top: 6px solid #5b6470; }");
    cmbSv2Backend_->setToolTip(
      "CPU = dinero-sv2-miner (all cores hashing).\n"
      "GPU (Metal) = dinero-sv2-gpu-miner (Apple Silicon, ~500 MH/s).");
    cmbSv2Backend_->setVisible(false);
    connect(cmbSv2Backend_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
      if (!cmbSv2Backend_) return;
      QSettings().setValue(
        "mining/sv2_backend",
        cmbSv2Backend_->currentData().toString());
      onMinerTypeChanged(cmbMinerType_ ? cmbMinerType_->currentIndex() : 0);
    });
    sv2PubkeyRow->addWidget(cmbSv2Backend_);

    lblSv2RewardMode_ = new QLabel("Rewards:");
    lblSv2RewardMode_->setVisible(false);
    sv2PubkeyRow->addWidget(lblSv2RewardMode_);

    cmbSv2RewardMode_ = new QComboBox;
    cmbSv2RewardMode_->addItem("Pool Shared", "shared");
    cmbSv2RewardMode_->addItem("Pool Solo", "solo");
    {
      const QString saved =
        QSettings().value("mining/sv2_reward_mode", "shared").toString();
      const int idx = cmbSv2RewardMode_->findData(saved);
      cmbSv2RewardMode_->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    cmbSv2RewardMode_->setFixedHeight(30);
    cmbSv2RewardMode_->setStyleSheet(cmbSv2Backend_->styleSheet());
    cmbSv2RewardMode_->setToolTip(
      "Pool Shared = each accepted share contributes to the pool's PPLNS window.\n"
      "Pool Solo = the miner owns the block coinbase, but receives nothing unless it finds a block.");
    cmbSv2RewardMode_->setVisible(false);
    connect(cmbSv2RewardMode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
      if (!cmbSv2RewardMode_) return;
      QSettings().setValue(
        "mining/sv2_reward_mode",
        cmbSv2RewardMode_->currentData().toString());
    });
    sv2PubkeyRow->addWidget(cmbSv2RewardMode_);
    quickMineLayout->addLayout(sv2PubkeyRow);

    // Row 1: Solo miner engine, or pool worker binary when Pool mode is selected.
    auto *row0 = new QHBoxLayout;
    row0->setSpacing(6);
    lblMinerType_ = new QLabel("Miner:");
    row0->addWidget(lblMinerType_);
    cmbMinerType_ = new QComboBox;
    cmbMinerType_->addItem("Solo CPU", "internal");
#ifdef Q_OS_WIN
    cmbMinerType_->addItem("Solo GPU (NVIDIA CUDA)", "internal_gpu");
#endif
#ifdef Q_OS_MAC
    cmbMinerType_->addItem("Solo GPU (Metal)", "internal_gpu");
#endif
    // "External (RPC Solo Miner)" dropdown option removed 2026-04-20.
    // It launched `dinero-miner` as a subprocess, which is another CPU miner
    // using a different codebase than the in-process CPU miner — so the UI
    // effectively offered two CPU miner entries with different names and
    // no user-visible difference in function. The in-process "CPU Mining
    // (Built-in)" path (via the dinero-solo-miner library) is the modern,
    // maintained path. Existing code paths that check minerType == "external"
    // in handlers below become unreachable after this removal but are left
    // in place — they're dead-code-safe and removing them is a separate
    // cleanup slice. The "external" value cannot be produced from the UI
    // anymore and the setting isn't persisted between launches.
    cmbMinerType_->setCurrentIndex(0); // Default to internal
    cmbMinerType_->setFixedHeight(30);
    cmbMinerType_->setStyleSheet(
      "QComboBox { background: #1f2328; color: #d7dde5; border: 1px solid #353b44; "
      "border-radius: 8px; padding: 0 8px; font-size: 12px; } "
      "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 22px; border-left: 1px solid #353b44; background: #21262c; border-top-right-radius: 8px; border-bottom-right-radius: 8px; } QComboBox::down-arrow { width: 0; height: 0; margin-right: 7px; border-left: 5px solid transparent; border-right: 5px solid transparent; border-top: 6px solid #c4cdd9; } QComboBox::down-arrow:disabled { border-top: 6px solid #5b6470; }");
    cmbMinerType_->setToolTip("Choose the solo mining engine");
    connect(cmbMinerType_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onMinerTypeChanged);
    row0->addWidget(cmbMinerType_);

    lblMinerPath_ = new QLabel("Worker:");
    lblMinerPath_->setVisible(false);
    row0->addWidget(lblMinerPath_);

    edtMinerPath_ = new QLineEdit;
    edtMinerPath_->setPlaceholderText("Path to dinero-stratum-worker binary...");
    edtMinerPath_->setStyleSheet(miningControlFieldStyle());
    edtMinerPath_->setFixedHeight(30);
    edtMinerPath_->setVisible(false); // Hidden by default (solo built-in miner)
    row0->addWidget(edtMinerPath_, 2);

    btnBrowseMiner_ = new QPushButton("Browse...");
    btnBrowseMiner_->setStyleSheet(headerButtonStyle());
    btnBrowseMiner_->setFixedSize(84, 30);
    btnBrowseMiner_->setVisible(false); // Hidden by default
    connect(btnBrowseMiner_, &QPushButton::clicked, this, &MainWindow::onBrowseMinerBinary);
    row0->addWidget(btnBrowseMiner_);
    quickMineLayout->addLayout(row0);

    // Row 1: Address and threads
    auto *row1 = new QHBoxLayout;
    row1->setSpacing(6);
    row1->addWidget(new QLabel("Address:"));
    edtMiningAddress_ = new QLineEdit;
    edtMiningAddress_->setPlaceholderText("din1p... (Taproot only)");
    edtMiningAddress_->setStyleSheet(miningControlFieldStyle());
    edtMiningAddress_->setFixedHeight(30);
    row1->addWidget(edtMiningAddress_, 2);
    btnUseWalletAddr_ = new QPushButton("Use Wallet");
    btnUseWalletAddr_->setStyleSheet(headerButtonStyle());
    btnUseWalletAddr_->setFixedSize(100, 30);
    btnUseWalletAddr_->setToolTip("Fill mining address from your wallet (requires unlocked wallet)");
    connect(btnUseWalletAddr_, &QPushButton::clicked, this, &MainWindow::onSetMiningAddress);
    row1->addWidget(btnUseWalletAddr_);
    row1->addWidget(new QLabel("Threads:"));
    edtMiningThreads_ = new QLineEdit("8");
    edtMiningThreads_->setStyleSheet(miningControlFieldStyle());
    edtMiningThreads_->setAlignment(Qt::AlignCenter);
    edtMiningThreads_->setFixedSize(56, 30);
    row1->addWidget(edtMiningThreads_);
    quickMineLayout->addLayout(row1);

    lblStratumIdentity_ = new QLabel;
    lblStratumIdentity_->setStyleSheet("QLabel { font-size: 10px; color: #6e7781; }");
    lblStratumIdentity_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lblStratumIdentity_->setVisible(false);
    quickMineLayout->addWidget(lblStratumIdentity_);

    connect(edtMiningAddress_, &QLineEdit::textChanged, this, [this](const QString&) {
      updateStratumIdentityLabel();
    });
    updateStratumIdentityLabel();
    onMiningModeChanged(cmbMiningMode_ ? cmbMiningMode_->currentIndex() : 0);
    
    auto *row2 = new QHBoxLayout;
    // v0.14.0.4: Single toggle button for Start/Stop mining
    btnStartMining_ = new QPushButton("Start Mining");
    btnStartMining_->setStyleSheet(headerButtonStyle());
    // CRITICAL: Start disabled - will be enabled when daemon connects
    btnStartMining_->setEnabled(false);
    btnStartMining_->setToolTip("Start daemon first to enable mining");
    const int miningControlHeight = 30;
    btnStartMining_->setFixedHeight(miningControlHeight);
    btnStartMining_->setMinimumWidth(170);
    // v0.14.0.4: Toggle button handler - toggles between start and stop
    connect(btnStartMining_, &QPushButton::clicked, this, &MainWindow::onToggleMining);
    row2->addWidget(btnStartMining_);
    // Legacy stop button - hidden but kept for internal use
    btnStopMining_ = new QPushButton("Stop");
    btnStopMining_->setVisible(false);  // Hidden - we use toggle button instead

    lblMiningStatus_ = new QLabel(miningStatusInactiveText());
    lblMiningStatus_->setStyleSheet(chromePillStyle());
    lblMiningStatus_->setFixedHeight(miningControlHeight);
    lblMiningStatus_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    lblMiningStatus_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    row2->addWidget(lblMiningStatus_, 1);

    // Connect mining mode AFTER all dependent widgets exist
    connect(cmbMiningMode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onMiningModeChanged);

    // Tiny stats
    lblBlocksFound_ = new QLabel("0");
    lblBlocksFound_->setStyleSheet("QLabel { font-weight: bold; color: #d6dde6; }");
    row2->addWidget(new QLabel("Blocks:"));
    row2->addWidget(lblBlocksFound_);

    row2->addWidget(new QLabel("Height:"));
    lblMiningHeight_ = new QLabel("-");
    lblMiningHeight_->setStyleSheet("QLabel { font-weight: bold; color: #d6dde6; }");
    row2->addWidget(lblMiningHeight_);

    row2->addWidget(new QLabel("Difficulty:"));
    lblMiningDifficulty_ = new QLabel("-");
    lblMiningDifficulty_->setStyleSheet(
      "QLabel { font-family: monospace; font-weight: bold; color: #d6dde6; }");
    row2->addWidget(lblMiningDifficulty_);
    
    lblCurrentHash_ = new QLabel("0.00");
    lblCurrentHash_->setStyleSheet("QLabel { font-weight: bold; color: #339af0; font-size: 10px; }");
    row2->addWidget(new QLabel("MH/s:"));
    row2->addWidget(lblCurrentHash_);
    
    lblMiningUptime_ = new QLabel("-");
    lblMiningUptime_->setStyleSheet("QLabel { font-size: 10px; }");
    lblMiningUptimeCaption_ = new QLabel("Run:");
    row2->addWidget(lblMiningUptimeCaption_);
    row2->addWidget(lblMiningUptime_);

    btnMiningSessionFinds_ = new QPushButton("Session finds");
    btnMiningSessionFinds_->setStyleSheet(headerButtonStyle());
    btnMiningSessionFinds_->setFixedHeight(miningControlHeight);
    btnMiningSessionFinds_->setEnabled(false);
    connect(btnMiningSessionFinds_, &QPushButton::clicked, this, [this]() {
      if (miningSessionFinds_.isEmpty()) return;

      auto* dialog = new QDialog(this);
      dialog->setAttribute(Qt::WA_DeleteOnClose);
      dialog->setWindowTitle("Blocks found this session");
      auto* dialogLayout = new QVBoxLayout(dialog);
      auto* table = new QTableWidget(miningSessionFinds_.size(), 6, dialog);
      table->setHorizontalHeaderLabels(
        {"Height", "Hash", "Merkle root", "Utreexo root", "Nonce", "Difficulty"});
      table->setEditTriggers(QAbstractItemView::NoEditTriggers);
      table->setSelectionBehavior(QAbstractItemView::SelectRows);
      table->setWordWrap(false);
      table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
      table->verticalHeader()->setVisible(false);
      table->verticalHeader()->setDefaultSectionSize(28);
      table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
      for (int row = 0; row < miningSessionFinds_.size(); ++row) {
        const auto& found = miningSessionFinds_.at(row);
        const QStringList values = {
          QString::number(found.height), found.hash, found.merkleRoot,
          found.utreexoRoot,
          QString("0x%1").arg(found.nonce, 8, 16, QChar('0')),
          QString("0x%1").arg(found.difficultyBits, 8, 16, QChar('0'))};
        for (int column = 0; column < values.size(); ++column) {
          auto* item = new QTableWidgetItem(values.at(column));
          item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
          table->setItem(row, column, item);
        }
      }
      dialogLayout->addWidget(table);
      auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
      connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
      dialogLayout->addWidget(buttons);
      const QRect available = screen() ? screen()->availableGeometry() : QRect(0, 0, 1440, 900);
      dialog->resize(qMin(1500, available.width() - 80),
                     qMin(300, 150 + 28 * qMin(5, miningSessionFinds_.size())));
      dialog->show();
    });
    row2->addWidget(btnMiningSessionFinds_);
    
    lblTotalHashes_ = new QLabel("0");
    lblTotalHashes_->setStyleSheet("QLabel { font-size: 10px; }");
    lblHashrate_ = new QLabel("-");
    lblHashrate_->setStyleSheet("QLabel { font-size: 10px; }");

    // SV2-only: live share counter. Updated in place so we never spam
    // the mining-output panel with one line per accept.
    lblSv2Shares_ = new QLabel("Shares: 0");
    lblSv2Shares_->setStyleSheet(
      "QLabel { font-size: 10px; color: #9ddf9d; font-weight: bold; }");
    lblSv2Shares_->setVisible(false);
    row2->addWidget(lblSv2Shares_);

    quickMineLayout->addLayout(row2);
    // Mining readiness is surfaced only when it is actionable. The previous
    // permanent placeholder contradicted a visibly running embedded miner.
    lblMiningReadiness_ = nullptr;
    layout->addWidget(quickMineGroup);
    
    // Mining output (label intentionally removed for cleaner layout)
    
    txtMiningOutput_ = new QTextEdit;
    txtMiningOutput_->setReadOnly(true);
    txtMiningOutput_->setMinimumHeight(250); // Ensure minimum decent size
    txtMiningOutput_->setLineWrapMode(QTextEdit::NoWrap);
    const QString miningFontFamily = miningConsoleFontFamily();
#if defined(Q_OS_WIN)
    txtMiningOutput_->setStyleSheet(QString(
      "QTextEdit { color: %2; font-family: \"%1\", \"SF Mono\", Menlo, monospace; "
      "font-size: 10px; background: palette(base); }")
      .arg(miningFontFamily, QString::fromLatin1(kMiningOutputTextColor)));
#else
    txtMiningOutput_->setStyleSheet(QString(
      "QTextEdit { color: %2; font-family: \"%1\", \"SF Mono\", Menlo, monospace; "
      "font-size: 10px; background: transparent; }")
      .arg(miningFontFamily, QString::fromLatin1(kMiningOutputTextColor)));
#endif
    txtMiningOutput_->setFont(miningConsoleFont(10, QFont::Medium));
    txtMiningOutput_->setTextColor(QColor(kMiningOutputTextColor));
    txtMiningOutput_->viewport()->setAutoFillBackground(true);
#if defined(Q_OS_WIN)
    txtMiningOutput_->viewport()->setAttribute(Qt::WA_StyledBackground, false);
    txtMiningOutput_->viewport()->setBackgroundRole(QPalette::Base);
#endif
    txtMiningOutput_->setPlaceholderText("Mining output will appear here when you start mining...");
    // QTextEdit's styled document paints above its viewport palette on macOS.
    // Keep a dedicated transparent paint layer for the live Hash Engine so
    // candidate rows remain visible without replacing the preserved log.
    miningHashOverlay_ = new QLabel(txtMiningOutput_->viewport());
    miningHashOverlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
    miningHashOverlay_->setStyleSheet("background: transparent;");
    miningHashOverlay_->hide();
    layout->addWidget(txtMiningOutput_, 10); // HUGE stretch factor = takes all remaining space!
    setMiningOutputCinematicEnabled(false);
    
    auto* miningPage = makeScrollableTab(mining);
    tabs->addTab(miningPage, "⛏️ Mining");
    miningTabWidget_ = miningPage;

    // Create embedded miner controller (in-process via dinero-solo-miner library)
    minerCtrl_ = new MinerController(this);

    // Wire MinerController signals to existing Widgets UI
    connect(minerCtrl_, &MinerController::statsChanged, this, [this]() {
        // Keep mining_stats_.current_hashrate fresh so the Cmd+K dashboard's
        // currentHashrate() accessor returns live data (the internal-miner
        // path doesn't go through the stdout-regex updater that other miner
        // types use to write this field).
        if (minerCtrl_) {
            mining_stats_.current_hashrate = minerCtrl_->hashrate();
        }
        if (lblCurrentHash_) {
            double hr = minerCtrl_->hashrate();
            lblCurrentHash_->setText(QString::number(hr / 1e6, 'f', 2));
        }
        if (lblBlocksFound_) {
            lblBlocksFound_->setText(QString::number(minerCtrl_->accepted()));
        }
        if (lblHashrate_ && minerCtrl_->currentHeight() > 0) {
            lblHashrate_->setText(QString("Height: %1").arg(minerCtrl_->currentHeight()));
        }
        if (lblMiningHeight_ && minerCtrl_->currentHeight() > 0) {
            lblMiningHeight_->setText(QString::number(minerCtrl_->currentHeight()));
        }
        if (lblMiningDifficulty_ && minerCtrl_->currentDifficultyBits() != 0) {
            lblMiningDifficulty_->setText(
              dinero::qt::compactDifficultyText(minerCtrl_->currentDifficultyBits()));
        }
        setOverviewLocalHashrate(minerCtrl_->hashrate(), "Embedded CPU miner hashrate");
        updateMiningRuntimeLabel();
    });

    connect(minerCtrl_, &MinerController::logLine, this, [this](const QString& line) {
        const bool transientError = dinero::qt::isTransientMiningError(line);
        const QString displayLine = dinero::qt::miningOutputDisplayText(line);
        if (txtMiningOutput_) {
            // Keep a recurring recoverable template failure to one compact
            // status line. The miner continues retrying and the Hash Engine
            // remains live; internal exception chains belong in daemon logs.
            if (transientError) {
                removeMiningOutputLine(txtMiningOutput_, displayLine);
            }
            appendMiningOutputLine(txtMiningOutput_, displayLine);
            QTextCursor c = txtMiningOutput_->textCursor();
            c.movePosition(QTextCursor::End);
            txtMiningOutput_->setTextCursor(c);
        }
        if (transientError) {
            const quint64 generation =
              transientMiningErrorGenerations_.value(displayLine, 0) + 1;
            transientMiningErrorGenerations_.insert(displayLine, generation);
            QTimer::singleShot(dinero::qt::kTransientMiningErrorMs, this,
              [this, displayLine, generation]() {
                if (transientMiningErrorGenerations_.value(displayLine) != generation) {
                  return;
                }
                transientMiningErrorGenerations_.remove(displayLine);
                removeMiningOutputLine(txtMiningOutput_, displayLine);
              });
        }
    });

    connect(minerCtrl_, &MinerController::blockFound, this, [this](const QString& /*hash*/, int /*height*/) {
        mining_stats_.blocks_found++;
        if (lblBlocksFound_) {
            lblBlocksFound_->setText(QString::number(mining_stats_.blocks_found));
        }
        // Refresh balance after block found
        QTimer::singleShot(1000, this, [this]() { rpc_->getBalance(); });
    });

    connect(minerCtrl_, &MinerController::templateChanged, this,
            [this](int height, quint32 difficultyBits) {
      // A fresh template is authoritative evidence that the miner recovered.
      // Remove prior recoverable rejection/template errors from the live view;
      // their full diagnostics remain available in the daemon log.
      const auto recoveredErrors = transientMiningErrorGenerations_.keys();
      transientMiningErrorGenerations_.clear();
      for (const QString& displayLine : recoveredErrors) {
        removeMiningOutputLine(txtMiningOutput_, displayLine);
      }
      if (lblMiningHeight_) lblMiningHeight_->setText(QString::number(height));
      if (lblMiningDifficulty_) {
        lblMiningDifficulty_->setText(dinero::qt::compactDifficultyText(difficultyBits));
      }
    });

    connect(minerCtrl_, &MinerController::blockFoundDetailed, this,
            [this](const QString& hash, int height, quint32 nonce,
                   const QString& merkleRoot, const QString& utreexoRoot,
                   quint32 difficultyBits) {
      miningSessionFinds_.append(
        {height, hash, merkleRoot, utreexoRoot, nonce, difficultyBits});
      if (btnMiningSessionFinds_) {
        btnMiningSessionFinds_->setEnabled(true);
        btnMiningSessionFinds_->setText(
          QString("Session finds (%1)").arg(miningSessionFinds_.size()));
      }
      const QString liveRecord = QString(
        "BLOCK_FOUND height=%1 hash=%2 merkle=%3 utreexo=%4 "
        "bits=0x%5 nonce=0x%6")
          .arg(height).arg(hash, merkleRoot, utreexoRoot)
          .arg(difficultyBits, 8, 16, QChar('0'))
          .arg(nonce, 8, 16, QChar('0'));
      miningHashSamples_.append({nonce, hash, liveRecord, true,
        QDateTime::currentMSecsSinceEpoch() +
          dinero::qt::kBlockFoundHighlightMs});
    });

    // Forward embedded miner state into the daemon's relay auto-mode so
    // p2p.relay=auto advertises NODE_RELAY while we're mining. The daemon
    // handler is idempotent and p2p.relay=0/1 overrides still win.
    connect(minerCtrl_, &MinerController::miningRelayStateRequested, this, [this](bool active) {
        if (rpc_) {
            rpc_->call(QStringLiteral("mining.setrelayactive"), QJsonArray{active});
        }
    });

    connect(minerCtrl_, &MinerController::runningChanged, this, [this]() {
        isMining_ = minerCtrl_->running();
        if (isMining_) {
            miningSessionFinds_.clear();
            miningHashSamples_.clear();
            if (btnMiningSessionFinds_) {
                btnMiningSessionFinds_->setText("Session finds");
                btnMiningSessionFinds_->setEnabled(false);
            }
            if (mining_stats_.mining_started <= 0) {
                mining_stats_.mining_started = QDateTime::currentMSecsSinceEpoch();
            }
            updateMiningRuntimeLabel();
            if (btnStartMining_) {
                btnStartMining_->setText("Stop Mining");
                btnStartMining_->setStyleSheet(headerButtonStyle());
                btnStartMining_->setToolTip("Click to stop mining");
            }
            if (lblMiningStatus_) {
                lblMiningStatus_->setText(miningStatusActiveText());
                lblMiningStatus_->setStyleSheet(chromePillStyle());
            }
            updateOverviewHardwareTelemetry();
        } else {
            if (btnStartMining_) {
                btnStartMining_->setText("Start Mining");
                btnStartMining_->setStyleSheet(headerButtonStyle());
                btnStartMining_->setToolTip("Click to start mining");
            }
            if (lblMiningStatus_) {
                lblMiningStatus_->setText(miningStatusInactiveText());
                lblMiningStatus_->setStyleSheet(chromePillStyle());
            }
            activeMinerType_ = "none";
            mining_stats_.mining_started = 0;
            if (lblMiningUptime_) {
                lblMiningUptime_->setText("-");
                lblMiningUptime_->setStyleSheet("QLabel { color: #868e96; }");
            }
            setMiningModeControlsLocked(false);
            resetOverviewMiningTelemetry();
            miningHashSamples_.clear();
        }
        setMiningOutputCinematicEnabled(isMining_);
    });

#endif
  }

  // === Utreexo proof-service diagnostics ===
  {
    auto *bridge = new QWidget;
    auto *layout = new QVBoxLayout(bridge);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 20, 20, 20);

    auto *lblHeader = new QLabel("Utreexo Proof Service");
    lblHeader->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; color: #d6dde6; }");
    layout->addWidget(lblHeader);

    auto *lblDesc = new QLabel(
        "Live daemon diagnostics for Utreexo proof caching and proof serving. "
        "This is not the asset bridge; it shows whether this node can serve "
        "compact proof data to stateless/mobile peers.");
    lblDesc->setWordWrap(true);
    lblDesc->setStyleSheet("QLabel { color: #868e96; font-size: 12px; margin-bottom: 8px; }");
    layout->addWidget(lblDesc);

    lblBridgeSummary_ = new QLabel("Waiting for Utreexo proof metrics...");
    lblBridgeSummary_->setWordWrap(true);
    lblBridgeSummary_->setStyleSheet(
        "QLabel { background: #232930; color: #cdd6e0; border: 1px solid #353c46; "
        "padding: 10px 12px; border-radius: 8px; font-size: 12px; }");
    layout->addWidget(lblBridgeSummary_);

    QString formStyle = QStringLiteral(
        "QLabel { color: #d6dde6; font-size: 13px; font-family: \"Space Mono\", \"SF Mono\", Menlo, monospace; }");

    auto *grid = new QFormLayout;
    grid->setSpacing(8);
    grid->setLabelAlignment(Qt::AlignRight);

    auto makeValueLabel = [&]() {
        auto *lbl = new QLabel("--");
        lbl->setStyleSheet(formStyle);
        return lbl;
    };

    lblBridgeStatus_ = new QLabel("--");
    lblBridgeStatus_->setStyleSheet("QLabel { font-size: 13px; font-weight: bold; }");
    grid->addRow(new QLabel("Status:"), lblBridgeStatus_);

    lblBridgeRequests_ = makeValueLabel();
    grid->addRow(new QLabel("Requests:"), lblBridgeRequests_);

    lblBridgeQueue_ = makeValueLabel();
    grid->addRow(new QLabel("Queue:"), lblBridgeQueue_);

    lblBridgeCacheHits_ = makeValueLabel();
    grid->addRow(new QLabel("Cache Hits:"), lblBridgeCacheHits_);

    lblBridgeCacheMisses_ = makeValueLabel();
    grid->addRow(new QLabel("Cache Misses:"), lblBridgeCacheMisses_);

    lblBridgeHitRate_ = makeValueLabel();
    grid->addRow(new QLabel("Hit Rate:"), lblBridgeHitRate_);

    lblBridgeBlockEntries_ = makeValueLabel();
    grid->addRow(new QLabel("Block Cache:"), lblBridgeBlockEntries_);

    lblBridgeTxEntries_ = makeValueLabel();
    grid->addRow(new QLabel("Tx Cache:"), lblBridgeTxEntries_);

    lblBridgeIndexed_ = makeValueLabel();
    grid->addRow(new QLabel("Indexed History:"), lblBridgeIndexed_);

    lblBridgeEvictions_ = makeValueLabel();
    grid->addRow(new QLabel("Evictions / TTL:"), lblBridgeEvictions_);

    lblBridgeWorkers_ = makeValueLabel();
    grid->addRow(new QLabel("Proof Workers:"), lblBridgeWorkers_);

    lblBridgeActiveGens_ = makeValueLabel();
    grid->addRow(new QLabel("Active Generations:"), lblBridgeActiveGens_);

    lblBridgeLatency_ = makeValueLabel();
    grid->addRow(new QLabel("Proof Latency:"), lblBridgeLatency_);

    lblBridgeQueueWait_ = makeValueLabel();
    grid->addRow(new QLabel("Queue Wait:"), lblBridgeQueueWait_);

    lblBridgePriority_ = makeValueLabel();
    grid->addRow(new QLabel("Priority Routing:"), lblBridgePriority_);

    lblBridgeTasks_ = makeValueLabel();
    grid->addRow(new QLabel("Tasks:"), lblBridgeTasks_);

    // Style the row labels
    for (int i = 0; i < grid->rowCount(); ++i) {
        auto *label = qobject_cast<QLabel*>(grid->itemAt(i, QFormLayout::LabelRole)->widget());
        if (label) {
            label->setStyleSheet("QLabel { color: #868e96; font-size: 13px; }");
        }
    }

    layout->addLayout(grid);
    layout->addStretch();

    const int bridgeTabIndex = tabs->addTab(makeScrollableTab(bridge), "Utreexo Proofs");
    tabs->setTabVisible(bridgeTabIndex, false);  // Advanced diagnostics; surfaced on Overview.
  }

  // === Peers Tab (Network Monitoring) ===
  {
    auto *peers = new QWidget;
    auto *layout = new QVBoxLayout(peers);

    // Header with refresh button
    auto *headerLayout = new QHBoxLayout;
    auto *lblHeader = new QLabel("🌐 Connected Peers");
    lblHeader->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; }");
    headerLayout->addWidget(lblHeader);
    headerLayout->addStretch();

    btnRefreshPeers_ = new QPushButton("🔄 Refresh");
    btnRefreshPeers_->setToolTip("Refresh peer list from daemon");
    connect(btnRefreshPeers_, &QPushButton::clicked, [this]() {
      rpc_->call("getpeerinfo", QJsonArray{});
      rpc_->call("getnetworkinfo", QJsonArray{});
    });
    headerLayout->addWidget(btnRefreshPeers_);
    layout->addLayout(headerLayout);

    auto *statusGroup = new QGroupBox("Network Status");
    auto *statusGrid = new QGridLayout(statusGroup);
    lblPeerSummary_ = new QLabel("Connections: -");
    lblPeerReachability_ = new QLabel("Listening: -");
    lblPeerPortMapping_ = new QLabel("Port mapping: -");
    lblPeerRelay_ = new QLabel("Relay: -");
    lblPeerAdvertised_ = new QLabel("Advertised: -");
    lblPeerReachabilityAdvice_ = new QLabel("Reachability: checking");
    lblPeerReachabilityAdvice_->setWordWrap(true);
    lblPeerReachabilityAdvice_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lblPeerReachabilityAdvice_->setStyleSheet("QLabel { color: #aeb8c2; font-size: 12px; }");
    for (auto *label : {lblPeerSummary_, lblPeerReachability_, lblPeerPortMapping_, lblPeerRelay_, lblPeerAdvertised_}) {
      label->setTextInteractionFlags(Qt::TextSelectableByMouse);
      label->setStyleSheet("QLabel { color: #cfd7df; font-size: 12px; }");
    }
    statusGrid->addWidget(lblPeerSummary_, 0, 0);
    statusGrid->addWidget(lblPeerReachability_, 0, 1);
    statusGrid->addWidget(lblPeerPortMapping_, 1, 0);
    statusGrid->addWidget(lblPeerRelay_, 1, 1);
    statusGrid->addWidget(lblPeerAdvertised_, 2, 0, 1, 2);
    statusGrid->addWidget(lblPeerReachabilityAdvice_, 3, 0, 1, 2);
    statusGrid->setColumnStretch(0, 1);
    statusGrid->setColumnStretch(1, 1);
    layout->addWidget(statusGroup);

    // Peer table
    tblPeers_ = new QTableWidget(0, 6);
    tblPeers_->setHorizontalHeaderLabels({"ID", "Location", "Type", "Client", "Seen Height", "Direction"});
    if (auto* advertisedHeader = tblPeers_->horizontalHeaderItem(4)) {
      advertisedHeader->setToolTip(
          "Last height this local node observed for the peer.\n"
          "This is P2P telemetry, not a live RPC query to that server.");
    }
    tblPeers_->horizontalHeader()->setStretchLastSection(true);
    tblPeers_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblPeers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tblPeers_->setSortingEnabled(true);
    layout->addWidget(tblPeers_);

    // Action buttons
    auto *btnLayout = new QHBoxLayout;
    btnDisconnectPeer_ = new QPushButton("❌ Disconnect");
    btnDisconnectPeer_->setToolTip("Disconnect from selected peer");
    connect(btnDisconnectPeer_, &QPushButton::clicked, this, &MainWindow::onDisconnectPeer);
    btnLayout->addWidget(btnDisconnectPeer_);

    btnBanPeer_ = new QPushButton("🚫 Ban");
    btnBanPeer_->setToolTip("Ban selected peer (blocks reconnection)");
    btnBanPeer_->setStyleSheet(chromeButtonStyle());
    connect(btnBanPeer_, &QPushButton::clicked, this, &MainWindow::onBanPeer);
    btnLayout->addWidget(btnBanPeer_);

    btnReconnectAllPeers_ = new QPushButton("🔄 Reconnect All");
    btnReconnectAllPeers_->setToolTip("Disconnect and reconnect all peers (refreshes peer heights)");
    btnReconnectAllPeers_->setStyleSheet(chromeButtonStyle());
    connect(btnReconnectAllPeers_, &QPushButton::clicked, this, &MainWindow::onReconnectAllPeers);
    btnLayout->addWidget(btnReconnectAllPeers_);

    btnCopyNetworkDiagnostics_ = new QPushButton("📋 Copy Diagnostics");
    btnCopyNetworkDiagnostics_->setToolTip("Copy P2P status, port mapping, advertised addresses, and peers");
    btnCopyNetworkDiagnostics_->setStyleSheet(chromeButtonStyle());
    connect(btnCopyNetworkDiagnostics_, &QPushButton::clicked, this, &MainWindow::onCopyNetworkDiagnostics);
    btnLayout->addWidget(btnCopyNetworkDiagnostics_);

    btnLayout->addStretch();
    layout->addLayout(btnLayout);

    const int peersTabIndex = tabs->addTab(makeScrollableTab(peers), "🌐 Peers");
    tabs->setTabVisible(peersTabIndex, false);  // Advanced diagnostics; surfaced on Overview.
  }

  // === Block Template Tab (Mining Info) ===
  {
    auto *templateWidget = new QWidget;
    auto *layout = new QVBoxLayout(templateWidget);

    // Header with stats
    auto *headerLayout = new QHBoxLayout;
    auto *lblHeader = new QLabel("📋 Block Template");
    lblHeader->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; }");
    headerLayout->addWidget(lblHeader);
    headerLayout->addStretch();

    btnRefreshTemplate_ = new QPushButton("🔄 Refresh");
    btnRefreshTemplate_->setToolTip("Fetch latest block template from daemon");
    connect(btnRefreshTemplate_, &QPushButton::clicked, this, &MainWindow::onRefreshTemplate);
    headerLayout->addWidget(btnRefreshTemplate_);
    layout->addLayout(headerLayout);

    // Template stats
    auto *statsGroup = new QGroupBox("Template Stats");
    auto *statsLayout = new QGridLayout(statsGroup);

    statsLayout->addWidget(new QLabel("Height:"), 0, 0);
    lblTemplateHeight_ = new QLabel("-");
    lblTemplateHeight_->setStyleSheet("QLabel { font-weight: bold; }");
    statsLayout->addWidget(lblTemplateHeight_, 0, 1);

    statsLayout->addWidget(new QLabel("Transactions:"), 0, 2);
    lblTemplateTxCount_ = new QLabel("-");
    lblTemplateTxCount_->setStyleSheet("QLabel { font-weight: bold; }");
    statsLayout->addWidget(lblTemplateTxCount_, 0, 3);

    statsLayout->addWidget(new QLabel("Total Fees:"), 1, 0);
    lblTemplateFees_ = new QLabel("-");
    lblTemplateFees_->setStyleSheet("QLabel { font-weight: bold; color: #d6dde6; }");
    statsLayout->addWidget(lblTemplateFees_, 1, 1);

    statsLayout->addWidget(new QLabel("Difficulty:"), 1, 2);
    lblTemplateDifficulty_ = new QLabel("-");
    lblTemplateDifficulty_->setStyleSheet("QLabel { font-weight: bold; }");
    statsLayout->addWidget(lblTemplateDifficulty_, 1, 3);

    statsLayout->setColumnStretch(1, 1);
    statsLayout->setColumnStretch(3, 1);
    layout->addWidget(statsGroup);

    // Template JSON display
    auto *jsonGroup = new QGroupBox("Raw Template (JSON)");
    auto *jsonLayout = new QVBoxLayout(jsonGroup);
    txtBlockTemplate_ = new QTextEdit;
    txtBlockTemplate_->setReadOnly(true);
    txtBlockTemplate_->setFont(QFont("Courier", 10));
    txtBlockTemplate_->setPlaceholderText("Click 'Refresh' to fetch block template...");
    jsonLayout->addWidget(txtBlockTemplate_);
    layout->addWidget(jsonGroup);

    const int templateTabIndex = tabs->addTab(makeScrollableTab(templateWidget), "📋 Template");
    tabs->setTabVisible(templateTabIndex, false);  // Advanced mining diagnostics; Mining tab covers normal use.
  }

  // === Settings Tab (Backup & Maintenance) ===
  {
    auto *settings = new QWidget;
    auto *layout = new QVBoxLayout(settings);
    layout->setSpacing(12);

    auto actualDataDir = [this]() {
      QString dataDir = rpc_ ? rpc_->datadir() : QString();
      if (dataDir.trimmed().isEmpty()) {
        dataDir = defaultDineroDataDir();
      }
      return QDir::cleanPath(dataDir);
    };

    auto backupPanelStyle = []() {
      return QStringLiteral(
        "QLabel { padding: 8px; background: #171b20; color: #d6dde6; "
        "border: 1px solid #2f363f; border-radius: 6px; }");
    };

    auto mutedLabelStyle = []() {
      return QStringLiteral("QLabel { font-size: 11px; color: #9aa4af; }");
    };

    auto copyFileIntoBackup = [](const QString& srcPath, const QString& dstPath, QStringList& errors) {
      if (!QFile::exists(srcPath)) {
        return false;
      }
      QDir().mkpath(QFileInfo(dstPath).absolutePath());
      if (QFile::exists(dstPath) && !QFile::remove(dstPath)) {
        errors << QString("Could not replace %1").arg(dstPath);
        return false;
      }
      if (!QFile::copy(srcPath, dstPath)) {
        errors << QString("Could not copy %1").arg(srcPath);
        return false;
      }
      return true;
    };

    auto copyDirectoryIntoBackup = [copyFileIntoBackup](auto&& self,
                                                        const QString& srcPath,
                                                        const QString& dstPath,
                                                        QStringList& errors) -> int {
      QDir srcDir(srcPath);
      if (!srcDir.exists()) {
        return 0;
      }
      if (!QDir().mkpath(dstPath)) {
        errors << QString("Could not create %1").arg(dstPath);
        return 0;
      }

      int copied = 0;
      const QFileInfoList entries = srcDir.entryInfoList(
        QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
      for (const QFileInfo& entry : entries) {
        const QString dst = QDir(dstPath).filePath(entry.fileName());
        if (entry.isDir()) {
          copied += self(self, entry.absoluteFilePath(), dst, errors);
        } else if (copyFileIntoBackup(entry.absoluteFilePath(), dst, errors)) {
          ++copied;
        }
      }
      return copied;
    };

    // Header
    auto *headerLayout = new QHBoxLayout;
    auto *lblHeader = new QLabel("⚙️ Settings & Backup");
    lblHeader->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; }");
    headerLayout->addWidget(lblHeader);
    headerLayout->addStretch();
    layout->addLayout(headerLayout);

    // Node runtime section
    auto *runtimeGroup = new QGroupBox("📁 Node Runtime");
    auto *runtimeLayout = new QGridLayout(runtimeGroup);
    runtimeLayout->setColumnStretch(1, 1);
    runtimeLayout->setHorizontalSpacing(10);
    runtimeLayout->setVerticalSpacing(8);

    const QString initialDataDir = actualDataDir();
    QString savedDaemonPath = QSettings().value("daemon/custom_path").toString().trimmed();
    const QString currentAppDir = QCoreApplication::applicationDirPath();
    const QString bundledResourceDaemon = QDir(currentAppDir).absoluteFilePath("../Resources/dinerod");
    const QString bundledMacDaemon = QDir(currentAppDir).absoluteFilePath("dinerod");
    if (!savedDaemonPath.isEmpty() &&
        (savedDaemonPath.contains("/AppTranslocation/", Qt::CaseInsensitive) ||
         !QFile::exists(savedDaemonPath))) {
      QSettings().remove("daemon/custom_path");
      savedDaemonPath.clear();
    }
    const QString currentAppContentsDir = QFileInfo(
      QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("..")).absoluteFilePath();
    const bool savedDaemonIsBundled = !savedDaemonPath.isEmpty() &&
      QFileInfo(savedDaemonPath).absoluteFilePath().startsWith(currentAppContentsDir + "/", Qt::CaseInsensitive);
    const QString daemonText = (savedDaemonPath.isEmpty() || savedDaemonIsBundled)
      ? QStringLiteral("Bundled dinerod (inside app)")
      : savedDaemonPath;

    btnStartDaemon_ = new QPushButton("Start Daemon");
    btnStartDaemon_->setStyleSheet(chromeButtonStyle());
    btnStartDaemon_->setFixedWidth(136);
    btnStartDaemon_->setToolTip("Start local dinerod daemon");
    connect(btnStartDaemon_, &QPushButton::clicked, this, &MainWindow::onStartDaemon);

    btnStopDaemon_ = new QPushButton("Stop Daemon");
    btnStopDaemon_->setStyleSheet(chromeButtonStyle());
    btnStopDaemon_->setFixedWidth(136);
    btnStopDaemon_->setToolTip("Stop local dinerod daemon");
    btnStopDaemon_->setVisible(false);
    connect(btnStopDaemon_, &QPushButton::clicked, this, &MainWindow::onStopDaemon);

    edtDaemonPath_ = new QLineEdit(settings);
    edtDaemonPath_->setPlaceholderText("dinerod path (optional)...");
    edtDaemonPath_->setStyleSheet(
      "QLineEdit { background: #1f2328; color: #d7dde5; border: 1px solid #353b44; "
      "border-radius: 8px; padding: 0 10px; font-size: 12px; }");
    edtDaemonPath_->setToolTip("Custom path to dinerod binary (optional)");
    edtDaemonPath_->setVisible(false);

    btnBrowseDaemon_ = new QPushButton("Browse Daemon…");
    btnBrowseDaemon_->setStyleSheet(chromeButtonStyle());
    btnBrowseDaemon_->setFixedWidth(148);
    btnBrowseDaemon_->setToolTip("Select a custom dinerod binary");
    connect(btnBrowseDaemon_, &QPushButton::clicked, this, &MainWindow::onBrowseDaemonBinary);

    lblConnectionStatus_ = new QLabel("Disconnected");
    lblConnectionStatus_->setStyleSheet(headerPillStyle());
    lblConnectionStatus_->setMinimumWidth(120);
    lblConnectionStatus_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    lblNetworkInfo_ = new QLabel("Network: -");
    lblNetworkInfo_->setStyleSheet(headerPillStyle());
    lblNetworkInfo_->setMinimumWidth(210);
    lblNetworkInfo_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    lblDaemonVersion_ = new QLabel("Core: -");
    lblDaemonVersion_->setStyleSheet(headerPillStyle());
    lblDaemonVersion_->setMinimumWidth(110);
    lblDaemonVersion_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    lblDbHealth_ = new QLabel("DB: -");
    lblDbHealth_->setStyleSheet(headerPillStyle());
    lblDbHealth_->setMinimumWidth(150);
    lblDbHealth_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    if (!savedDaemonPath.isEmpty() && QFile::exists(savedDaemonPath)) {
      edtDaemonPath_->setText(savedDaemonPath);
      btnBrowseDaemon_->setToolTip(QString("Daemon binary: %1").arg(QFileInfo(savedDaemonPath).fileName()));
      qDebug() << "Loaded saved daemon path:" << savedDaemonPath;
    } else if (QFile::exists(bundledResourceDaemon)) {
      edtDaemonPath_->setText(bundledResourceDaemon);
    } else if (QFile::exists(bundledMacDaemon)) {
      edtDaemonPath_->setText(bundledMacDaemon);
    }

    auto addRuntimeRow = [&](int row, const QString& title, const QString& value) {
      auto *titleLabel = new QLabel(title);
      titleLabel->setStyleSheet("QLabel { color: #9aa4af; font-weight: 600; }");
      auto *valueLabel = new QLabel(value);
      valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
      valueLabel->setWordWrap(true);
      valueLabel->setStyleSheet(
        "QLabel { padding: 6px 8px; background: #171b20; color: #d6dde6; "
        "border: 1px solid #2f363f; border-radius: 6px; }");
      runtimeLayout->addWidget(titleLabel, row, 0);
      runtimeLayout->addWidget(valueLabel, row, 1);
    };

    addRuntimeRow(0, "Data directory", initialDataDir);
    addRuntimeRow(1, "Chain database", QDir(initialDataDir).filePath("blockchain/chaindb"));
    addRuntimeRow(2, "Daemon binary", daemonText);

    auto *runtimeButtons = new QHBoxLayout;
    auto *btnOpenDataDir = new QPushButton("📂 Open Data Directory");
    btnOpenDataDir->setStyleSheet(chromeButtonStyle());
    connect(btnOpenDataDir, &QPushButton::clicked, [actualDataDir]() {
      QDesktopServices::openUrl(QUrl::fromLocalFile(actualDataDir()));
    });
    runtimeButtons->addWidget(btnOpenDataDir);

    auto *btnOpenChainDir = new QPushButton("⛓️ Open Chain Database");
    btnOpenChainDir->setStyleSheet(chromeButtonStyle());
    connect(btnOpenChainDir, &QPushButton::clicked, [actualDataDir]() {
      QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(actualDataDir()).filePath("blockchain/chaindb")));
    });
    runtimeButtons->addWidget(btnOpenChainDir);

    if (!savedDaemonPath.isEmpty() && !savedDaemonIsBundled) {
      auto *btnClearDaemonPath = new QPushButton("Reset Daemon Path");
      btnClearDaemonPath->setStyleSheet(chromeButtonStyle());
      btnClearDaemonPath->setToolTip("Return to the bundled or system dinerod on the next daemon start.");
      connect(btnClearDaemonPath, &QPushButton::clicked, [this]() {
        QSettings().remove("daemon/custom_path");
        QMessageBox::information(this, "Daemon Path Reset",
          "Dinero-Qt will use the bundled or system dinerod the next time the daemon is started.");
      });
      runtimeButtons->addWidget(btnClearDaemonPath);
    }

    runtimeButtons->addStretch();
    runtimeLayout->addLayout(runtimeButtons, 3, 0, 1, 2);
    layout->addWidget(runtimeGroup);

    auto *developerGroup = new QGroupBox("🧪 Developer Menu");
    auto *developerLayout = new QVBoxLayout(developerGroup);
    developerLayout->setSpacing(10);

    auto *developerSummary = new QLabel(
      "Advanced local daemon controls, reconnect tools, and runtime diagnostics. "
      "Hidden by default so the main wallet stays calm."
    );
    developerSummary->setWordWrap(true);
    developerSummary->setStyleSheet(backupPanelStyle());
    developerLayout->addWidget(developerSummary);

    const QString developerToggleStyle =
      chromeButtonStyle() +
      "QPushButton:checked { background: #343b45; border: 1px solid #4a5461; }";
    auto *btnDeveloperToggle = new QPushButton;
    btnDeveloperToggle->setCheckable(true);
    btnDeveloperToggle->setStyleSheet(developerToggleStyle);
    btnDeveloperToggle->setFixedWidth(156);
    developerLayout->addWidget(btnDeveloperToggle, 0, Qt::AlignLeft);

    auto *developerPanel = new QWidget(developerGroup);
    auto *developerPanelLayout = new QVBoxLayout(developerPanel);
    developerPanelLayout->setContentsMargins(0, 0, 0, 0);
    developerPanelLayout->setSpacing(10);

    auto *developerActions = new QHBoxLayout;
    developerActions->setSpacing(8);
    developerActions->addWidget(btnStartDaemon_);
    developerActions->addWidget(btnStopDaemon_);
    developerActions->addWidget(btnBrowseDaemon_);

    auto *btnReconnect = new QPushButton("Reconnect");
    btnReconnect->setToolTip("Force reconnection (reload cookie & retry)");
    btnReconnect->setFixedWidth(108);
    btnReconnect->setStyleSheet(chromeButtonStyle());
    connect(btnReconnect, &QPushButton::clicked, this, [this]() {
      lblConnectionStatus_->setText("Reconnecting...");
      lblConnectionStatus_->setStyleSheet(headerPillStyle());
      rpc_->reconnect();
      QTimer::singleShot(2000, this, &MainWindow::refresh);
    });
    developerActions->addWidget(btnReconnect);
    developerActions->addStretch();
    developerPanelLayout->addLayout(developerActions);

    auto *developerStatusGrid = new QGridLayout;
    developerStatusGrid->setHorizontalSpacing(8);
    developerStatusGrid->setVerticalSpacing(8);
    developerStatusGrid->addWidget(lblConnectionStatus_, 0, 0);
    developerStatusGrid->addWidget(lblNetworkInfo_, 0, 1);
    developerStatusGrid->addWidget(lblDaemonVersion_, 1, 0);
    developerStatusGrid->addWidget(lblDbHealth_, 1, 1);
    auto *lblVaultDeveloperMetrics = new QLabel(
      vaultPanel_ ? vaultPanel_->developerSummary()
                  : QStringLiteral("Vault raw metrics: unavailable"));
    lblVaultDeveloperMetrics->setWordWrap(true);
    lblVaultDeveloperMetrics->setStyleSheet(mutedLabelStyle());
    developerStatusGrid->addWidget(lblVaultDeveloperMetrics, 2, 0, 1, 2);
    developerPanelLayout->addLayout(developerStatusGrid);

    if (vaultPanel_) {
      connect(vaultPanel_, &VaultPanel::developerSummaryChanged, this,
              [lblVaultDeveloperMetrics](const QString& summary) {
        lblVaultDeveloperMetrics->setText(summary);
      });
    }

    auto *developerNote = new QLabel(
      "Use these controls when testing daemon startup, connection recovery, or local runtime health."
    );
    developerNote->setWordWrap(true);
    developerNote->setStyleSheet(mutedLabelStyle());
    developerPanelLayout->addWidget(developerNote);

    const bool showDeveloperMenu = QSettings().value("ui/show_settings_developer_menu", false).toBool();
    developerPanel->setVisible(showDeveloperMenu);
    btnDeveloperToggle->setChecked(showDeveloperMenu);
    auto updateDeveloperToggle = [btnDeveloperToggle](bool expanded) {
      btnDeveloperToggle->setText(expanded ? QStringLiteral("Hide Developer Menu")
                                           : QStringLiteral("Show Developer Menu"));
    };
    updateDeveloperToggle(showDeveloperMenu);
    connect(btnDeveloperToggle, &QPushButton::toggled, this,
            [developerPanel, updateDeveloperToggle](bool checked) {
      developerPanel->setVisible(checked);
      QSettings().setValue("ui/show_settings_developer_menu", checked);
      updateDeveloperToggle(checked);
    });

    developerLayout->addWidget(developerPanel);
    layout->addWidget(developerGroup);

    // Backup section
    auto *backupGroup = new QGroupBox("💾 Backup");
    auto *backupLayout = new QVBoxLayout(backupGroup);
    backupLayout->setSpacing(10);

    auto *backupInfoLabel = new QLabel(
      "<b>Wallet backup:</b> copies wallet databases, HD wallet state, and wallet registry from the live daemon data directory. "
      "<b>Chain backup:</b> optional; it can be large and can always be rebuilt by syncing again."
    );
    backupInfoLabel->setWordWrap(true);
    backupInfoLabel->setStyleSheet(backupPanelStyle());
    backupLayout->addWidget(backupInfoLabel);

    auto *backupGrid = new QGridLayout;
    backupGrid->setColumnStretch(1, 1);
    backupGrid->setHorizontalSpacing(12);
    backupGrid->setVerticalSpacing(10);

    auto *btnBackupWallet = new QPushButton("💼 Backup Wallet Data");
    btnBackupWallet->setStyleSheet(chromeButtonStyle());
    btnBackupWallet->setToolTip("Backup wallets/, hd_wallet/, and wallet_registry.db files from the daemon data directory.");
    connect(btnBackupWallet, &QPushButton::clicked,
            [this, actualDataDir, copyFileIntoBackup, copyDirectoryIntoBackup]() {
      const QString backupDir = QFileDialog::getExistingDirectory(this, "Select Wallet Backup Location");
      if (backupDir.isEmpty()) return;

      const QString dataDir = actualDataDir();
      const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
      const QString backupPath = QDir(backupDir).filePath("dinero_wallet_backup_" + timestamp);
      if (!QDir().mkpath(backupPath)) {
        QMessageBox::warning(this, "Backup Failed", QString("Could not create:\n%1").arg(backupPath));
        return;
      }

      QStringList errors;
      int copiedCount = 0;
      copiedCount += copyDirectoryIntoBackup(copyDirectoryIntoBackup,
                                             QDir(dataDir).filePath("wallets"),
                                             QDir(backupPath).filePath("wallets"),
                                             errors);
      copiedCount += copyDirectoryIntoBackup(copyDirectoryIntoBackup,
                                             QDir(dataDir).filePath("hd_wallet"),
                                             QDir(backupPath).filePath("hd_wallet"),
                                             errors);

      const QStringList walletFiles = {
        "wallet_registry.db", "wallet_registry.db-wal", "wallet_registry.db-shm",
        "dinero-sv2-pool.key"
      };
      for (const QString& file : walletFiles) {
        if (copyFileIntoBackup(QDir(dataDir).filePath(file), QDir(backupPath).filePath(file), errors)) {
          ++copiedCount;
        }
      }

      if (copiedCount > 0) {
        QString message = QString("Wallet backup complete.\n\nItems copied: %1\nLocation: %2")
          .arg(copiedCount)
          .arg(backupPath);
        if (!errors.isEmpty()) {
          message += QString("\n\nSkipped: %1 item(s)").arg(errors.size());
        }
        QMessageBox::information(this, "Backup Complete", message);
      } else {
        QMessageBox::warning(this, "Backup Failed",
          QString("No wallet data was found in:\n%1").arg(dataDir));
      }
    });
    backupGrid->addWidget(btnBackupWallet, 0, 0);

    auto *lblWalletBackup = new QLabel("Copies wallets/, hd_wallet/, wallet registry, and the local SV2 pool key.");
    lblWalletBackup->setWordWrap(true);
    lblWalletBackup->setStyleSheet(mutedLabelStyle());
    backupGrid->addWidget(lblWalletBackup, 0, 1);

    auto *btnBackupChain = new QPushButton("⛓️ Backup Chain Data");
    btnBackupChain->setStyleSheet(chromeButtonStyle());
    btnBackupChain->setToolTip("Backup blockchain/, blocks/, and headers/ from the daemon data directory.");
    connect(btnBackupChain, &QPushButton::clicked,
            [this, actualDataDir, copyDirectoryIntoBackup]() {
      const auto reply = QMessageBox::question(this, "Backup Chain Data",
        "Chain data can be large and may take several minutes to copy.\n\n"
        "This is optional because the node can resync from the network.\n\n"
        "Continue?",
        QMessageBox::Yes | QMessageBox::No);
      if (reply != QMessageBox::Yes) return;

      const QString backupDir = QFileDialog::getExistingDirectory(this, "Select Chain Backup Location");
      if (backupDir.isEmpty()) return;

      const QString dataDir = actualDataDir();
      const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
      const QString backupPath = QDir(backupDir).filePath("dinero_chain_backup_" + timestamp);
      if (!QDir().mkpath(backupPath)) {
        QMessageBox::warning(this, "Backup Failed", QString("Could not create:\n%1").arg(backupPath));
        return;
      }

      QStringList errors;
      int copiedCount = 0;
      const QStringList chainDirs = {"blockchain", "blocks", "headers"};
      for (const QString& dir : chainDirs) {
        copiedCount += copyDirectoryIntoBackup(copyDirectoryIntoBackup,
                                               QDir(dataDir).filePath(dir),
                                               QDir(backupPath).filePath(dir),
                                               errors);
      }

      if (copiedCount > 0) {
        QString message = QString("Chain data backup complete.\n\nItems copied: %1\nLocation: %2")
          .arg(copiedCount)
          .arg(backupPath);
        if (!errors.isEmpty()) {
          message += QString("\n\nSkipped: %1 item(s)").arg(errors.size());
        }
        QMessageBox::information(this, "Backup Complete", message);
      } else {
        QMessageBox::warning(this, "Backup Failed",
          QString("No chain data was found in:\n%1").arg(dataDir));
      }
    });
    backupGrid->addWidget(btnBackupChain, 1, 0);

    auto *lblChainBackup = new QLabel("Copies blockchain/, blocks/, and headers/. Wallet backup is the critical one.");
    lblChainBackup->setWordWrap(true);
    lblChainBackup->setStyleSheet(mutedLabelStyle());
    backupGrid->addWidget(lblChainBackup, 1, 1);

    backupLayout->addLayout(backupGrid);
    layout->addWidget(backupGroup);

    auto *note = new QLabel(
      "Backups are created as timestamped folders. For the cleanest filesystem snapshot, stop mining first and avoid closing the app during the copy."
    );
    note->setWordWrap(true);
    note->setStyleSheet(backupPanelStyle());
    layout->addWidget(note);

    layout->addStretch();
    tabs->addTab(makeScrollableTab(settings), "\xE2\x9A\x99\xEF\xB8\x8F Settings");
  }

  // === Error Message Bar (at very bottom) ===
  lblErrorMessage_ = new QLabel("");
  lblErrorMessage_->setVisible(false);  // Hidden by default, shown when error occurs
  lblErrorMessage_->setStyleSheet(
    "QLabel { "
    "  padding: 8px; "
    "  background: #2c3036; "
    "  color: #d6dde6; "
    "  font-weight: bold; "
    "  border-top: 1px solid #3d434d; "
    "}"
  );
  lblErrorMessage_->setWordWrap(true);
  mainLayout->addWidget(lblErrorMessage_);
}

void MainWindow::refresh() {
  // DON'T make RPC calls if daemon is not connected - this prevents error spam
  // Use ConnectionManager state (authoritative connection verifier)
  if (!connectionMgr_ || !connectionMgr_->isConnected()) {
    qDebug() << "⏸️ Skipping refresh - daemon not connected";
    return;
  }

  // Check wallet status on first refresh
  static bool firstRun = true;
  if (firstRun) {
    firstRun = false;
    rpc_->getWalletInfo(); // Check if wallet exists
  }

  // Check rescan status periodically (affects wallet lock/unlock)
  checkRescanStatus();

  // Get comprehensive network info
  rpc_->getBlockCount();
  rpc_->call("getnetworkinfo", QJsonArray());     // Get P2P listen/port-map status
  if (overviewRelayRpcSupported_) {
    rpc_->callNamed("network.getrelayservice", QJsonObject{}); // Overview relay preference/state
  }
  rpc_->call("getpeerinfo", QJsonArray());        // Get connection count
  rpc_->call("economics.getinfo", QJsonArray());       // Get phase & reward
  rpc_->call("economics.getsupply", QJsonArray());          // Get total supply
  rpc_->call("mempool.getinfo", QJsonArray());     // Get mempool stats
  rpc_->call("blockchain.getinfo", QJsonArray());  // Get headers vs blocks (headers-first sync)
  rpc_->call("blockchain.getmininginfo", QJsonArray());  // Get current difficulty & network mining stats

  // Status bar info: daemon version and DB health
  rpc_->call("rpc.version", QJsonArray());        // Get daemon version
  rpc_->call("consensus.checkdb", QJsonArray());  // Get database health

  // Phase X.1: CPU monitoring and resource pressure
  rpc_->call("node.getcpustats", QJsonArray());        // Get real CPU usage
  rpc_->call("node.getresourcepressure", QJsonArray()); // Get resource health

  // v7: recent blocks for Overview/Explorer tabs
  rpc_->call("getblockcount", QJsonArray());

  // Phase X.2: Disk space monitoring
  rpc_->call("node.getdiskstats", QJsonArray());       // Get disk space stats

  // Utreexo proof-service cache/serving stats
  rpc_->getUtreexoCacheStats();

  // Phase X.3: Enhanced wallet sync status
  rpc_->call("wallet.getsyncstatus", QJsonArray());    // Get comprehensive sync status
  if (walletReorgInfoSupported_) {
    rpc_->call("wallet.getreorginfo", QJsonArray());   // Optional: older daemons may not expose this
  }

  // Get wallet balance (real HD wallet balance)
  rpc_->getBalance();
  refreshShieldedBalanceSummary();
  rpc_->call("wallet.listwallets", QJsonArray());

  // ✅ Load all addresses to populate Receive tab (regardless of balance)
  rpc_->callNamed("wallet.listaddresses", QJsonObject{{"count", 200}});
  // Large mining wallets may contain thousands of UTXOs. Fetch them only
  // while their panel is visible instead of rebuilding a hidden table every
  // five seconds on Qt's UI thread.
  requestUtxoRefresh();
  rpc_->getBestBlockHash();  // Load explorer data

  // Auto-refresh block template when mining is active
  if (isMining_ && edtMiningAddress_ && !edtMiningAddress_->text().trimmed().isEmpty()) {
    QJsonArray tplParams;
    QJsonObject tplReq;
    tplReq["address"] = edtMiningAddress_->text().trimmed();
    tplParams.append(tplReq);
    rpc_->call("mining.getblocktemplate", tplParams);
  }
}

QString MainWindow::currentWalletAddressMode() const {
  if (!cmbWalletAddressMode_) {
    return "standard";
  }
  const QString mode = cmbWalletAddressMode_->currentData().toString();
  return mode.isEmpty() ? QStringLiteral("standard") : mode;
}

QString MainWindow::currentReceiveMode() const {
  if (!cmbReceiveMode_) {
    return "all";
  }
  const QString mode = cmbReceiveMode_->currentData().toString();
  return mode.isEmpty() ? QStringLiteral("all") : mode;
}

QString MainWindow::currentSendMode() const {
  if (!cmbSendMode_) {
    return "public_transfer";
  }
  const QString mode = cmbSendMode_->currentData().toString();
  return mode.isEmpty() ? QStringLiteral("public_transfer") : mode;
}

// Helper: does the current send mode behave like the old "standard" transparent send?
static bool isSendModePublic(const QString& mode) {
  return mode == "public_transfer" || mode == "public_contract";
}
// Helper: does the current send mode behave like the old "confidential" private send?
static bool isSendModeConfidential(const QString& mode) {
  return mode == "confidential_transfer" || mode == "confidential_contract";
}
// Helper: does the current send mode behave like the old private send?
static bool isSendModePrivate(const QString& mode) {
  return mode == "private_transfer" || mode == "private_contract";
}
// Helper: is this a contract mode?
static bool isSendModeContract(const QString& mode) {
  return mode == "public_contract" || mode == "confidential_contract" || mode == "private_contract" || mode == "shield_covenant";
}

void MainWindow::updateWalletAddressModeUi() {
  const QString mode = currentWalletAddressMode();
  const bool confidentialMode = (mode == "confidential");
  const bool p2mrMode = (mode == "p2mr");

  if (edtAddress_) {
    edtAddress_->setPlaceholderText(
        confidentialMode ? "dina1..." : p2mrMode ? "din1r..." : "din1p...");
  }
  if (lblReceivePathHint_) {
    if (p2mrMode) {
      lblReceivePathHint_->setText("Path: m/88'/1448'/0'/0/i | din1r");
      lblReceivePathHint_->setToolTip(
          "Purpose 88 P2MR lane.\n"
          "Quantum-safe ML-DSA-65 receive addresses derive from the same BIP39 seed.");
    } else {
      lblReceivePathHint_->setText("Path: m/86'/1448'/0'/0/i | din1p");
      lblReceivePathHint_->setToolTip(
          "BIP86 Taproot lane.\n"
          "Mobile-friendly receive addresses derive from the same BIP39 seed.");
    }
  }
  if (btnNewAddress_) {
    btnNewAddress_->setText(
        confidentialMode ? "Generate Private Address"
        : p2mrMode       ? "Generate Quantum-Safe Address"
        :                   "Generate Taproot Address");
    btnNewAddress_->setToolTip(
        confidentialMode ? "Generate a new private receive address"
        : p2mrMode       ? "Generate a new ML-DSA-65 quantum-resistant address"
        :                   "Generate a new Taproot (secp256k1) address");
  }
  if (btnValidate_) {
    btnValidate_->setToolTip(confidentialMode
      ? "Private addresses are wallet-generated and not validated with the standard address RPC"
      : "Validate the current receive address");
  }
  if (txtValidation_ && txtValidation_->toPlainText().isEmpty()) {
    txtValidation_->setText(
        confidentialMode
        ? QString::fromUtf8("\xF0\x9F\x9B\xA1\xEF\xB8\x8F Private receive addresses hide amounts on-chain while still locking funds to Taproot keys.")
        : p2mrMode
        ? QString::fromUtf8("\xF0\x9F\x94\x92 Quantum-safe addresses use ML-DSA-65 (NIST FIPS 204). Larger signatures (~5 KB) but resistant to quantum computing.")
        : "Generate a receive address, then copy or validate it here.");
  }
}

void MainWindow::applyPrimaryAddressesToReceiveTab() {
  // v7: only transparent address types (Taproot din1p, P2MR din1r). No private
  // dina1 cache. The standard receive page covers both.
  if (!cachedPrimaryAddress_.isEmpty()) {
    if (edtAddress_)
      edtAddress_->setText(cachedPrimaryAddress_);
  }
}

void MainWindow::updateReceiveModeUi() {
  const QString mode = currentReceiveMode();  // "all" | "taproot" | "p2mr"
  if (btnDeriveAddress_) {
    if (mode == "p2mr") {
      btnDeriveAddress_->setText("New P2MR Address");
      btnDeriveAddress_->setToolTip("Generate a quantum-safe ML-DSA-65 receive address (din1r...)");
    } else {
      // "all" and "taproot" both default-derive to Taproot
      btnDeriveAddress_->setText("New Taproot Address");
      btnDeriveAddress_->setToolTip("Generate a new Taproot receive address (din1p...)");
    }
  }
  applyPrimaryAddressesToReceiveTab();
}

void MainWindow::updateSendModeUi() {
  // Guard: called during setupUI before widgets are fully created
  if (!btnSend_ || !edtRecipient_ || !edtAmount_) return;

  const QString mode = currentSendMode();
  const bool publicMode = isSendModePublic(mode);
  const bool confidentialMode = isSendModeConfidential(mode);
  const bool privateMode = isSendModePrivate(mode);
  const bool shieldMode = mode == "shield";
  const bool shieldToMode = mode == "shield_to";
  const bool unshieldMode = mode == "unshield";
  const bool shieldCovenantMode = mode == "shield_covenant";
  const bool contractMode = isSendModeContract(mode);
  const bool inputsEnabled = !btnSend_ || btnSend_->isEnabled();

  // Show/hide the contract template section
  if (contractGroup_) {
    contractGroup_->setVisible(contractMode);
  }

  if (edtRecipient_) {
    edtRecipient_->setEnabled(inputsEnabled && !unshieldMode);
    if (publicMode) {
      edtRecipient_->setPlaceholderText("din1p... (Taproot) or din1r... (P2MR public)");
    } else if (privateMode) {
      edtRecipient_->setPlaceholderText("dins1... (shielded)");
    } else if (shieldToMode) {
      edtRecipient_->setPlaceholderText("dins1... (shielded destination)");
    } else if (unshieldMode) {
      edtRecipient_->setPlaceholderText("Fresh wallet Taproot address will be generated");
    } else {
      edtRecipient_->setPlaceholderText("din1p... or din1r...");
    }
  }

  if (btnSend_) {
    if (contractMode) {
      btnSend_->setText("Create Contract");
      btnSend_->setToolTip("Create an on-chain contract lock with spending rules");
    } else if (privateMode) {
      btnSend_->setText("Send Privately");
      btnSend_->setToolTip("Spend shielded notes to a shielded address");
    } else if (shieldToMode) {
      btnSend_->setText("Send to Shielded");
      btnSend_->setToolTip("Fund a shielded dins1 address from your transparent balance");
    } else if (unshieldMode) {
      btnSend_->setText("Convert to Public");
      btnSend_->setToolTip("Unshield selected private value to a fresh wallet Taproot address");
    } else {
      btnSend_->setText("Send");
      btnSend_->setToolTip("Send DIN transparently from public Taproot/P2MR funds");
    }
  }

  if (btnHardwareWalletSend_) {
    const bool hwSupported = (mode == "public_transfer");
    btnHardwareWalletSend_->setEnabled(hwSupported && btnSend_ && btnSend_->isEnabled());
    btnHardwareWalletSend_->setToolTip(hwSupported
      ? hardwareWalletPsbtTooltip()
      : "Hardware-wallet signing is currently wired only for public Taproot transfers");
  }

  if (lblSendStatus_) {
    const QString status = lblSendStatus_->text();
    const bool isModeHint =
      status.isEmpty() ||
      status.startsWith(QString::fromUtf8("\xE2\x9C\x85 Wallet unlocked. Ready to send transactions.")) ||
      status.startsWith(QString::fromUtf8("\xE2\x84\xB9\xEF\xB8\x8F Create or restore a wallet")) ||
      status.startsWith(QString::fromUtf8("\xF0\x9F\x94\x84 Blockchain rescan in progress")) ||
      status.startsWith(QString::fromUtf8("\xF0\x9F\x94\x92 Wallet is locked")) ||
      status.startsWith(QString::fromUtf8("\xF0\x9F\x93\x9C Contract options"));
    if (isModeHint) {
      if (contractMode) {
        lblSendStatus_->setText("Create an on-chain contract with spending rules.");
      } else if (privateMode) {
        lblSendStatus_->setText(
          "Spend shielded balance privately. Recipient must be a dins1 shielded address.");
      } else if (shieldToMode) {
        lblSendStatus_->setText(
          "Send transparent balance into a shielded address. Recipient must be a dins1 shielded address.");
      } else if (unshieldMode) {
        lblSendStatus_->setText(
          "Convert shielded balance to public Taproot. The daemon sends it to a fresh wallet address.");
      } else {
        lblSendStatus_->setText("Send DIN publicly from transparent Taproot/P2MR funds.");
      }
      lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
    }
  }
}

void MainWindow::updateWalletBalanceDisplay() {
  const double transparent = std::max(0.0, cachedTransparentBalance_);
  const double p2mr = std::clamp(cachedP2mrBalance_, 0.0, transparent);
  const double taproot = std::max(0.0, transparent - p2mr);
  const double shielded = std::max(0.0, cachedShieldedBalance_);
  const double total = transparent + shielded;

  if (lblBalance_) {
    lblBalance_->setText(QString("%1 DIN").arg(total, 0, 'f', 8));
  }
  if (lblTotalWalletBalance_) {
    lblTotalWalletBalance_->setText(QString("%1 DIN").arg(total, 0, 'f', 8));
  }
  if (lblTransparentTaprootBalance_) {
    lblTransparentTaprootBalance_->setText(QString("%1 DIN").arg(taproot, 0, 'f', 8));
  }
  if (lblTransparentP2mrBalance_) {
    lblTransparentP2mrBalance_->setText(QString("%1 DIN").arg(p2mr, 0, 'f', 8));
  }
  if (lblShieldedBalance_) {
    lblShieldedBalance_->setText(QString("%1 DIN").arg(shielded, 0, 'f', 8));
  }

  auto lblUnconfirmed = findChild<QLabel*>("lblUnconfirmed");
  if (lblUnconfirmed) {
    if (cachedPendingBalance_ == 0.0) {
      lblUnconfirmed->setText("0.00000000 DIN");
    } else {
      const QString prefix = cachedPendingBalance_ > 0 ? "+" : "";
      lblUnconfirmed->setText(QString("%1%2 DIN").arg(prefix).arg(cachedPendingBalance_, 0, 'f', 8));
    }
  }

  auto lblImmature = findChild<QLabel*>("lblImmature");
  if (lblImmature) {
    lblImmature->setText(QString("%1 DIN").arg(cachedMiningBalance_, 0, 'f', 8));
  }

  if (barPqRatio_) {
    const double pqRatio = transparent > 0.0 ? (p2mr / transparent) : 0.0;
    barPqRatio_->setValue(static_cast<int>(std::round(pqRatio * 100.0)));
    QString chunkColor = (pqRatio < 0.10) ? "#c0392b"
                     : (pqRatio < 0.50) ? "#d4a017"
                     :                     "#2d8a4e";
    barPqRatio_->setStyleSheet(
        "QProgressBar { border: 1px solid #343b45; border-radius: 4px; "
        "background: #1a1f27; text-align: center; color: #9fb3c8; font-size: 10px; }"
        "QProgressBar::chunk { background: " + chunkColor + "; border-radius: 3px; }");
  }
}

void MainWindow::refreshShieldedBalanceSummary() {
  if (!rpc_ || currentWalletName_.isEmpty()) {
    cachedShieldedBalance_ = 0.0;
    updateWalletBalanceDisplay();
    return;
  }
  rpc_->call("wallet.shieldedbalance", QJsonArray());
}

void MainWindow::refreshConfidentialWalletState() {
  if (!rpc_ || currentWalletName_.isEmpty()) {
    cachedTransparentBalance_ = 0.0;
    cachedP2mrBalance_ = 0.0;
    cachedShieldedBalance_ = 0.0;
    cachedPendingBalance_ = 0.0;
    cachedMiningBalance_ = 0.0;
    updateWalletBalanceDisplay();
    return;
  }
  rpc_->getBalance();
  refreshShieldedBalanceSummary();
}

void MainWindow::onNewAddress() {

  // Disable button to prevent double-clicks
  if (btnNewAddress_) {
    btnNewAddress_->setEnabled(false);
    btnNewAddress_->setText("Generating...");
  }

  // P2MR mode: use the direct RPC path with positional params (matches
  // onDeriveNewAddress pattern). Falls through to the same rpcResult
  // handler that refreshes the address table.
  if (currentWalletAddressMode() == "p2mr") {
    rpc_->call("wallet.getnewaddress", QJsonArray{"p2mr"});
    auto pqConn = std::make_shared<QMetaObject::Connection>();
    *pqConn = connect(rpc_, &RpcClient::rpcResult, this,
      [this, pqConn](const QString& method, const QJsonValue& result) {
        if (method != "wallet.getnewaddress") return;
        QObject::disconnect(*pqConn);
        if (result.isObject()) {
          QString addr = result.toObject().value("address").toString();
          if (!addr.isEmpty()) {
            edtAddress_->setText(addr);
            txtValidation_->setText(QString::fromUtf8(
                "\xF0\x9F\x94\x92 Quantum-safe address generated (ML-DSA-65)"));
            rpc_->callNamed("wallet.listaddresses", QJsonObject{{"count", 200}});
          }
        }
        if (btnNewAddress_) { btnNewAddress_->setEnabled(true); updateWalletAddressModeUi(); }
      });
    return;
  }

  connectionMgr_->call("getnewaddress", QJsonObject{},
    // Success callback
    [this](QJsonObject result) {
      QString address = result["address"].toString();
      
      if (!address.isEmpty()) {
        edtAddress_->setText(address);
        txtValidation_->setText("✅ New address generated successfully!");
        txtValidation_->setStyleSheet("QTextEdit { color: #d6dde6; }");
        qDebug() << "MainWindow: Generated new address:" << address;
        
        // ✅ Auto-refresh the address table to show the new address
        rpc_->callNamed("wallet.listaddresses", QJsonObject{{"count", 200}});
        
        // ✅ Also update mining address if empty
        if (edtMiningAddress_ && edtMiningAddress_->text().isEmpty()) {
          edtMiningAddress_->setText(address);
          qDebug() << "MainWindow: Set mining address to new address:" << address;
        }
      } else {
        txtValidation_->setText("⚠️ Address generation returned empty result");
        txtValidation_->setStyleSheet("QTextEdit { color: #f59f00; }");
      }
      
      // Re-enable button
      if (btnNewAddress_) {
        btnNewAddress_->setEnabled(true);
        updateWalletAddressModeUi();
      }
    },
    // Error callback
    [this](QString error) {
      txtValidation_->setText("❌ Error generating address: " + error);
      txtValidation_->setStyleSheet("QTextEdit { color: #c2cad3; }");
      qWarning() << "MainWindow: Failed to generate address:" << error;
      
      // Re-enable button with error tooltip
      if (btnNewAddress_) {
        btnNewAddress_->setEnabled(true);
        updateWalletAddressModeUi();
        btnNewAddress_->setToolTip("Previous attempt failed: " + error + " (click to retry)");
      }
    }
  );
}

void MainWindow::onValidateAddress() {
  if (currentWalletAddressMode() == "confidential") {
    if (txtValidation_) {
      txtValidation_->setText("🕶️ Private receive addresses are wallet-generated private addresses. Copy the address directly or view its underlying Taproot details above.");
    }
    return;
  }
  if (!edtAddress_->text().isEmpty()) {
    rpc_->validateAddress(edtAddress_->text());
  }
}

void MainWindow::onCopyAddress() {
  if (!edtAddress_->text().isEmpty()) {
    QApplication::clipboard()->setText(edtAddress_->text());
    txtValidation_->setText("Address copied to clipboard!");
  }
}

void MainWindow::onRefreshBlocks() {
  QString input = edtBlockHash_->text().trimmed();

  resetExplorerDetailTables();

  if (input.isEmpty()) {
    setExplorerStatus("Refreshing best block and latest blocks...");
    rpc_->getBestBlockHash();
    if (cachedHeight_ > 0) {
      updateExplorerRecentBlocks(cachedHeight_);
    }
    return;
  }

  if (explorerIsAddress(input)) {
    pendingExplorerAddress_ = input;
    pendingExplorerAddressBalance_ = QJsonValue();
    pendingExplorerAddressHistory_ = QJsonValue();
    pendingExplorerAddressBalanceReady_ = false;
    pendingExplorerAddressHistoryReady_ = false;
    explorerAddressBalanceAliasTried_ = false;
    explorerAddressHistoryAliasTried_ = false;
    explorerAddressScantxFallbackTried_ = false;
    setExplorerStatus(QString("Loading address %1...").arg(explorerShortValue(input, 22)));
    rpc_->call("blockchain.getaddressbalance", QJsonArray{input});
    rpc_->call("blockchain.getaddresshistory", QJsonArray{input});
    return;
  }

  bool isNumber = false;
  const int height = input.toInt(&isNumber);
  if (isNumber && height >= 0) {
    pendingExplorerHeightLookup_ = input;
    pendingExplorerBlockHash_.clear();
    pendingExplorerTxLookup_.clear();
    pendingExplorerTxFallbackBlockHash_.clear();
    setExplorerStatus(QString("Loading block height %1...").arg(input));
    rpc_->call("blockchain.getblockhash", QJsonArray{height});
    return;
  }

  if (explorerIsHex64(input)) {
    pendingExplorerTxLookup_ = input.toLower();
    pendingExplorerTxFallbackBlockHash_ = input;
    pendingExplorerTxAliasTried_ = false;
    pendingExplorerBlockHash_.clear();
    setExplorerStatus(QString("Loading transaction %1...").arg(explorerShortValue(input)));
    rpc_->call("gettransaction", QJsonArray{input});
    return;
  }

  setExplorerStatus("Enter a block height, 64-character transaction/block hash, or Dinero address.", true);
}

void MainWindow::onRpcResult(const QString& method, const QJsonValue& result) {
  // Guard: if RPC client has been destroyed (app shutting down), bail out
  if (!rpc_) return;

  if (shouldIgnoreWalletScopedResult(method)) {
    qDebug() << "Ignoring wallet-scoped RPC result during wallet switch:" << method
             << "pending=" << pendingWalletOpenName_;
    return;
  }

  // WebSocket discovery
  if (method == "server.getinfo") {
    if (result.isObject()) {
      auto obj = result.toObject();

      // Extract WebSocket port from result
      if (obj.contains("websocket") && obj["websocket"].isObject()) {
        auto wsObj = obj["websocket"].toObject();
        int wsPort = wsObj["port"].toInt();
        QString wsBind = wsObj["bind"].toString("127.0.0.1");

        if (wsPort > 0) {
          QString wsUrl = QString("ws://%1:%2").arg(wsBind).arg(wsPort);
          qDebug() << "MainWindow: Discovered WebSocket at" << wsUrl;

#ifdef DIN_EXPERIMENTAL_FEATURES
          // Update WebSocket URL and connect
          ws_->setServerUrl(wsUrl);
          ws_->connectToServer();
          ws_->subscribe("newblock");
          ws_->subscribe("newtx");
          ws_->subscribe("mining");
          ws_->subscribe("network");
          ws_->subscribe("mempool");
          ws_->subscribe("sync");
#endif

          return;  // Early return after handling discovery
        }
      }

      // Fallback: If discovery failed, try convention (RPC port + 3)
      qWarning() << "MainWindow: getserverinfo missing WebSocket info, using fallback";
      QString fallbackUrl = "ws://127.0.0.1:21000";  // Default WebSocket port
      qDebug() << "MainWindow: Using fallback WebSocket URL:" << fallbackUrl;

#ifdef DIN_EXPERIMENTAL_FEATURES
      ws_->setServerUrl(fallbackUrl);
      ws_->connectToServer();
      ws_->subscribe("newblock");
      ws_->subscribe("newtx");
      ws_->subscribe("mining");
      ws_->subscribe("network");
      ws_->subscribe("mempool");
      ws_->subscribe("sync");
#endif
    }
  }
  // Status bar: Daemon version (git hash is the version)
  else if (method == "rpc.version") {
    if (result.isObject()) {
      auto obj = result.toObject();
      QString hash = obj["version"].toString();
      lblDaemonVersion_->setText(QString("Core %1").arg(hash));
      lblDaemonVersion_->setStyleSheet(headerPillStyle());
      lblDaemonVersion_->setToolTip(QString("Daemon commit %1").arg(hash));
    }
  }
  // Status bar: Database health
  else if (method == "consensus.checkdb") {
    if (result.isObject()) {
      auto obj = result.toObject();
      bool healthy = obj["healthy"].toBool();
      if (healthy) {
        int tipHeight = obj["tip_height"].toInt();
        lblDbHealth_->setText(QString("DB healthy | H:%1").arg(tipHeight));
        lblDbHealth_->setStyleSheet(headerPillStyle());
        lblDbHealth_->setToolTip("Database is healthy.");
      } else {
        QString error = obj["error"].toString();
        lblDbHealth_->setText("DB issue");
        lblDbHealth_->setStyleSheet(headerPillStyle());
        lblDbHealth_->setToolTip(error);
      }
    }
  }
  else if (method == "blockchain.getinfo") {
    if (result.isObject()) {
      auto obj = result.toObject();
      updateStatus(obj);
      cachedHeight_ = obj["blocks"].toInt();
      cachedHeaders_ = obj["headers"].toInt();
      refreshAiStatusStrip();

      // v7 Overview: Utreexo health from blockchain info
      if (lblUtreexoHealth_) {
        int blocks = obj["blocks"].toInt();
        int headers = obj["headers"].toInt();
        bool synced = (blocks >= headers && headers > 0);
        lblUtreexoHealth_->setText(synced ? "Health: HEALTHY" : "Health: SYNCING");
        lblUtreexoHealth_->setStyleSheet(synced
            ? "QLabel { font-size: 12px; font-weight: bold; color: #2d8a4e; }"
            : "QLabel { font-size: 12px; font-weight: bold; color: #d4a017; }");
      }
      if (lblUtreexoLeaves_) {
        lblUtreexoLeaves_->setText(QString("Height: %1").arg(obj["blocks"].toInt()));
      }
      if (lblUtreexoRoot_) {
        QString hash = obj["bestblockhash"].toString();
        if (hash.length() > 16) hash = hash.left(16) + "...";
        lblUtreexoRoot_->setText(QString("Tip: %1").arg(hash));
      }

      updateExplorerRecentBlocks(cachedHeight_);
    }
  } else if (method == "economics.getinfo") {
    if (result.isObject()) {
      updateEconomics(result.toObject());
    }
  } else if (method == "economics.getsupply") {
    if (result.isObject()) {
      auto obj = result.toObject();
      QString issued = overviewDinText(obj.value("total_issued_din"));
      if (issued.isEmpty()) {
        issued = overviewDinText(obj.value("current_supply_din"));
      }
      if (issued.isEmpty()) {
        issued = overviewDinTextFromUna(obj.value("total_issued_una"));
      }

      if (!issued.isEmpty()) {
        const QString hardCap = overviewDinText(obj.value("hard_cap_din"));
        lblSupply_->setText(hardCap.isEmpty()
            ? QString("Supply: %1 DIN").arg(issued)
            : QString("Supply: %1 / %2 DIN").arg(issued, hardCap));
        lblSupply_->setToolTip(
            QString("Issued supply from economics.getsupply.\nHeight: %1\nReward: %2 DIN\nPolicy: %3")
                .arg(obj.value("height").toVariant().toString(),
                     obj.value("current_block_reward_din").toString("-"),
                     obj.value("monetary_policy").toString("PoW mining")));
        if (lblExplorerSupply_) {
          lblExplorerSupply_->setText(issued + " DIN");
        }
      } else {
        qWarning() << "getsupply missing issued supply fields";
        lblSupply_->setText("Supply: unavailable");
      }
    }
  } else if (method == "wallet.getnewaddress") {
    QString addr = result.toString();
    if (addr.isEmpty() && result.isObject()) {
      addr = result.toObject()["address"].toString();
    }
    updateWallet(addr);
  } else if (method == "wallet.validateaddress") {
    if (result.isObject()) {
      auto obj = result.toObject();
      QJsonDocument doc(obj);
      txtValidation_->setText(doc.toJson(QJsonDocument::Indented));
    }
  } else if (method == "wallet.getbalance") {
    // Handle the new getbalance RPC format (returns object with breakdown)
    if (result.isObject()) {
      auto obj = result.toObject();
      double confirmed = obj["confirmed"].toDouble();
      double unconfirmed = obj["unconfirmed"].toDouble();
      double immature = obj["immature"].toDouble();
      cachedBalance_ = confirmed;  // Cache confirmed-only for AI status strip
      cachedTransparentBalance_ = confirmed;
      cachedPendingBalance_ = unconfirmed;
      cachedMiningBalance_ = immature;
      cachedP2mrBalance_ = obj.contains("pq_balance_din")
          ? obj["pq_balance_din"].toDouble()
          : 0.0;
      refreshAiStatusStrip();
      updateWalletBalanceDisplay();

      // v7 Overview tab: update PQ status from getbalance response
      if (obj.contains("pq_ratio") && lblPqOverviewRatio_) {
        double pqRatio = obj["pq_ratio"].toDouble();
        double pqDin   = obj["pq_balance_din"].toDouble();
        lblPqOverviewRatio_->setText(QString("PQ Ratio: %1%").arg(
            static_cast<int>(pqRatio * 100)));
        if (pqRatio >= 0.50) {
          lblPqOverviewRatio_->setStyleSheet("QLabel { font-size: 12px; font-weight: bold; color: #2d8a4e; }");
        } else if (pqRatio >= 0.10) {
          lblPqOverviewRatio_->setStyleSheet("QLabel { font-size: 12px; font-weight: bold; color: #d4a017; }");
        } else {
          lblPqOverviewRatio_->setStyleSheet("QLabel { font-size: 12px; font-weight: bold; color: #c0392b; }");
        }
        if (lblPqOverviewUtxos_) {
          lblPqOverviewUtxos_->setText(QString("P2MR: %1 DIN").arg(pqDin, 0, 'f', 2));
        }
      }

      // Phase 35: Display multi-asset balances (Taproot assets)
      auto lblAssets = findChild<QLabel*>("lblAssets");
      if (lblAssets && obj.contains("assets")) {
        auto assetsObj = obj["assets"].toObject();
        if (assetsObj.isEmpty()) {
          lblAssets->setText("");  // No assets
        } else {
          QStringList assetLines;
          for (auto it = assetsObj.begin(); it != assetsObj.end(); ++it) {
            QString assetId = it.key();
            double amount = it.value().toDouble();
            // Truncate asset ID for display (first 8 + last 4 chars)
            QString shortId = assetId.length() > 16
                ? assetId.left(8) + "..." + assetId.right(4)
                : assetId;
            assetLines << QString("%1: %2").arg(shortId).arg(amount, 0, 'f', 4);
          }
          lblAssets->setText("Assets: " + assetLines.join(" | "));
        }
      } else if (lblAssets) {
        lblAssets->setText("");  // No assets field in response
      }
    } else {
      // Fallback for old format
      double balance = result.toDouble();
      cachedBalance_ = balance;
      cachedTransparentBalance_ = balance;
      cachedP2mrBalance_ = 0.0;
      updateWalletBalanceDisplay();
    }
  } else if (method == "wallet.shieldedbalance") {
    if (result.isObject()) {
      const auto obj = result.toObject();
      const QString innerError = obj.value("error").toString();
      if (innerError == "shielded_not_active") {
        cachedShieldedBalance_ = 0.0;
      } else if (innerError.isEmpty()) {
        const qint64 balanceUna = obj.value("balance_una").toVariant().toLongLong();
        cachedShieldedBalance_ = static_cast<double>(balanceUna) / 100000000.0;
      }
      updateWalletBalanceDisplay();
    }
  } else if (method == "wallet.gettotalbalance") {
    if (result.isObject()) {
      const auto obj = result.toObject();
      const auto transparent = obj.value("transparent").toObject();
      const double total = jsonToDouble(obj.value("total_balance"));
      if (lblTotalWalletBalance_) {
        lblTotalWalletBalance_->setText(QString("%1 DIN").arg(total, 0, 'f', 8));
      }
    }
  } else if (method == "wallet.rescanblockchain") {
    if (result.isObject()) {
      const auto obj = result.toObject();
      const bool success = obj.value("success").toBool(false);
      if (success) {
        clearSafeModeRescanRetry();
        // Rescan finished (or started, depending on daemon mode). Refresh wallet data.
        walletRescanning_ = false;
        updateWalletUIState();
        if (lblSyncProgress_) {
          lblSyncProgress_->setText("✅ Wallet scan complete");
        }
        rpc_->getBalance();
        rpc_->callNamed("wallet.listaddresses", QJsonObject{{"count", 200}});
        rpc_->call("wallet.listunspent", QJsonArray());
      } else {
        const QString error = obj.value("error").toString("Unknown error");
        if (isRescanSafeModeError(error)) {
          walletRescanning_ = true;
          updateWalletUIState();
          scheduleSafeModeRescanRetry(error);
          return;
        }
        clearSafeModeRescanRetry();
        walletRescanning_ = false;
        updateWalletUIState();
        QMessageBox::warning(this, "Wallet Scan Failed",
          QString("Wallet restore succeeded, but blockchain scan failed:\n\n%1\n\n"
                  "You can retry with RPC method wallet.rescanblockchain.")
            .arg(error));
      }
    }
  } else if (method == "wallet.sendtoaddress" || method == "sendtoaddress") {
    // Handle sendtoaddress response.
    // New daemon behavior: signs + submits and returns txid/status directly.
    // Legacy behavior: may return tx_hex for a second broadcast step.
    if (result.isObject()) {
      auto obj = result.toObject();
      QString txHex = obj["tx_hex"].toString();
      QString txid = obj["txid"].toString();
      QString status = obj["status"].toString();
      bool accepted = obj["accepted"].toBool(true);
      QString rejectReason = obj["reject_reason"].toString();
      QString error = obj["error"].toString();
      
      if (btnSend_) {
        btnSend_->setEnabled(true);
        btnSend_->setText("📤 Send Transaction");
      }
      
      if (!error.isEmpty()) {
        // Phase 6: Release reservation on error
        if (changeAddrMgr_ && !activeReservationId_.isEmpty()) {
          changeAddrMgr_->release(activeReservationId_);
          activeReservationId_.clear();
        }
        if (isInputUtxoMissingError(error)) {
          handleSpendInputMissing(error);
        } else {
          lblSendStatus_->setText(QString("❌ Error: %1").arg(error));
          lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
        }
      } else if (!txid.isEmpty() || status == "signed_and_submitted") {
        if (!accepted) {
          // Phase 6: Release reservation on rejection
          if (changeAddrMgr_ && !activeReservationId_.isEmpty()) {
            changeAddrMgr_->release(activeReservationId_);
            activeReservationId_.clear();
          }
          QString reason = rejectReason.isEmpty() ? "Transaction rejected by mempool policy" : rejectReason;
          if (isInputUtxoMissingError(reason)) {
            handleSpendInputMissing(reason);
          } else {
            lblSendStatus_->setText(QString("❌ Broadcast failed: %1").arg(reason));
            lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
          }
          return;
        }

        // Phase 6: Mark reservation used on success, trust backend change_address
        QString reservationId = activeReservationId_;
        if (changeAddrMgr_ && !reservationId.isEmpty()) {
          changeAddrMgr_->markUsed(reservationId);
          activeReservationId_.clear();
        }

        // Phase 4: Create tracked transaction from enhanced response
        {
          TrackedTransaction tracked;
          tracked.txid = txid;
          tracked.isIncoming = false;
          tracked.address = edtRecipient_ ? edtRecipient_->text().trimmed() : QString();
          tracked.amountUna = static_cast<qint64>(
              (edtAmount_ ? edtAmount_->text().toDouble() : 0.0) * 1e8);
          tracked.status = TxStatus::Pending;
          tracked.createdAt = QDateTime::currentDateTimeUtc();
          tracked.changeAddress = obj["change_address"].toString();
          tracked.changeAmountUna = obj["change_amount_una"].toVariant().toLongLong();
          tracked.feePaidUna = obj["fee_paid_una"].toVariant().toLongLong();
          tracked.reservationId = reservationId;

          QJsonArray selectedInputs = obj["selected_inputs"].toArray();
          for (const auto& inp : selectedInputs) {
            QJsonObject inpObj = inp.toObject();
            QString outpoint = QString("%1:%2")
                .arg(inpObj["txid"].toString())
                .arg(inpObj["vout"].toInt());
            tracked.selectedInputOutpoints.append(outpoint);
            tracked.selectedInputAmountsUna[outpoint] =
                inpObj["amount_una"].toVariant().toLongLong();
          }

          txTracker_->trackSend(tracked);
        }

        lblSendStatus_->setText("✅ Transaction sent successfully!");
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; font-weight: 600; }");

        if (txtSendResult_) {
          txtSendResult_->setHtml(QString(
            "<b>✅ Transaction Broadcast Successfully</b><br><br>"
            "<b>Transaction ID:</b><br>"
            "<span style='font-family: monospace; font-size: 11px;'>%1</span><br><br>"
            "<b>Status:</b> Pending confirmation<br>"
            "<b>Recipient:</b> %2<br>"
            "<b>Amount:</b> %3 DIN<br><br>"
            "<i>Your transaction has been broadcast to the network. "
            "It will appear in your transaction history once confirmed.</i>"
          ).arg(txid).arg(edtRecipient_ ? edtRecipient_->text() : "").arg(edtAmount_ ? edtAmount_->text() : ""));
        }

        // Clear form
        if (edtRecipient_) edtRecipient_->clear();
        if (edtAmount_) edtAmount_->clear();

        // Refresh wallet state
        refresh();
      } else if (!txHex.isEmpty()) {
        // Legacy daemon path: created transaction and requires explicit broadcast.
        lblSendStatus_->setText("✅ Transaction created and signed! Broadcasting...");
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
        
        // Broadcast the transaction
        rpc_->sendRawTransaction(txHex);
      }
    }
  } else if (method == "wallet.transfer" || method == "wallet.unshield" || method == "wallet.shield") {
    if (result.isObject()) {
      const auto obj = result.toObject();
      const QString error = obj.value("error").toString();
      const QString txid = obj.value("txid").toString();

      if (btnSend_) {
        btnSend_->setEnabled(true);
        updateSendModeUi();
      }

      if (!error.isEmpty()) {
        const QString detail = obj.value("error_message").toString(error);
        if (lblSendStatus_) {
          lblSendStatus_->setText(QString("❌ Error: %1").arg(detail));
          lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
        }
        return;
      }

      const bool unshield = method == "wallet.unshield";
      const bool shieldTo = method == "wallet.shield";
      if (lblSendStatus_) {
        lblSendStatus_->setText(unshield
            ? "✅ Converted shielded balance to public Taproot."
            : shieldTo
            ? "✅ Sent to shielded address."
            : "✅ Private spend submitted.");
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; font-weight: 600; }");
      }

      if (txtSendResult_) {
        const QString recipientLine = unshield
            ? QString("<b>Public address:</b> %1<br>").arg(obj.value("recipient_address").toString("fresh wallet Taproot address"))
            : shieldTo
            ? QString("<b>Shielded recipient:</b> %1<br>").arg(edtRecipient_ ? edtRecipient_->text().toHtmlEscaped() : QString())
            : QString("<b>Private recipient:</b> %1<br>").arg(edtRecipient_ ? edtRecipient_->text().toHtmlEscaped() : QString());
        txtSendResult_->setHtml(QString(
          "<b>%1</b><br><br>"
          "<b>Transaction ID:</b><br>"
          "<span style='font-family: monospace; font-size: 11px;'>%2</span><br><br>"
          "%3"
          "<b>Amount:</b> %4 DIN<br>"
          "<b>Fee:</b> 0.00001000 DIN<br><br>"
          "<i>%5</i>"
        ).arg(unshield ? "Convert to Public Submitted" : shieldTo ? "Send to Shielded Submitted" : "Private Spend Submitted",
              txid.toHtmlEscaped(),
              recipientLine,
              edtAmount_ ? edtAmount_->text().toHtmlEscaped() : QString(),
              unshield ? "The output is now transparent/public once confirmed."
                       : shieldTo ? "The transparent balance is now shielded at the destination once confirmed."
                       : "Amounts and sender remain shielded on-chain."));
      }

      if (edtRecipient_) edtRecipient_->clear();
      if (edtAmount_) edtAmount_->clear();
      refreshShieldedBalanceSummary();
      refresh();
    }
  } else if (method == "wallet.sendrawtransaction" || method == "sendrawtransaction") {
    // Handle broadcast response
    if (result.isObject()) {
      auto obj = result.toObject();
      QString txid = obj["txid"].toString();
      QString error = obj["error"].toString();

      if (pendingHardwareWalletSend_.active && error.isEmpty() && !txid.isEmpty()) {
        // Hardware-wallet broadcasts raised from the dedicated widget are
        // completed via transactionBroadcasted() so the send flow can reuse
        // the linked recipient/amount context without double-rendering here.
        return;
      }
      
      if (!error.isEmpty()) {
        if (isInputUtxoMissingError(error)) {
          handleSpendInputMissing(error);
        } else {
          lblSendStatus_->setText(QString("❌ Broadcast failed: %1").arg(error));
          lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
        }
      } else if (!txid.isEmpty()) {
        lblSendStatus_->setText("✅ Transaction sent successfully!");
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; font-weight: 600; }");
        
        if (txtSendResult_) {
          txtSendResult_->setHtml(QString(
            "<b>✅ Transaction Broadcast Successfully</b><br><br>"
            "<b>Transaction ID:</b><br>"
            "<span style='font-family: monospace; font-size: 11px;'>%1</span><br><br>"
            "<b>Status:</b> Pending confirmation<br>"
            "<b>Recipient:</b> %2<br>"
            "<b>Amount:</b> %3 DIN<br><br>"
            "<i>Your transaction has been broadcast to the network. "
            "It will appear in your transaction history once confirmed.</i>"
          ).arg(txid).arg(edtRecipient_ ? edtRecipient_->text() : "").arg(edtAmount_ ? edtAmount_->text() : ""));
        }
        
        // Clear form
        if (edtRecipient_) edtRecipient_->clear();
        if (edtAmount_) edtAmount_->clear();
        
        // Refresh balance
        refresh();
      }
    }
  } else if (method == "estimatesmartfee") {
    // Phase 35: Handle fee estimation response
    if (result.isObject()) {
      auto obj = result.toObject();
      double feeRate = obj["feerate"].toDouble(0.0);
      int confTarget = obj["conf_target"].toInt(6);
      bool fallback = obj["using_fallback"].toBool(false);
      QString errors = obj["errors"].toString();

      if (lblEstimatedFee_ && feeRate > 0) {
        // feeRate is in DIN/kB, convert to una/vB for display
        // DIN/kB * 1e8 / 1000 = una/vB
        double unaPerVb = feeRate * 1e8 / 1000.0;

        // Estimate total fee for typical tx (250 vB)
        double estTotalFee = unaPerVb * 250.0 / 1e8;  // Back to DIN

        QString feeText = QString("Est: %1 una/vB (~%2 DIN)")
            .arg(unaPerVb, 0, 'f', 1)
            .arg(estTotalFee, 0, 'f', 8);

        if (fallback) {
          feeText += " [fallback]";
        }

        lblEstimatedFee_->setText(feeText);
        lblEstimatedFee_->setStyleSheet("QLabel { color: #cfd6de; font-size: 11px; }");
      } else if (lblEstimatedFee_) {
        // No fee estimate available
        QString msg = errors.isEmpty() ? "Insufficient data" : errors;
        lblEstimatedFee_->setText("Est: " + msg);
        lblEstimatedFee_->setStyleSheet("QLabel { color: #b4bec9; font-size: 11px; }");
      }
    }
  } else if (method == "blockchain.getbestblockhash") {
    QString hash = result.toString();
    if (lblBestBlock_) {
      lblBestBlock_->setText("Best Block: " + explorerShortValue(hash));
      lblBestBlock_->setToolTip(hash);
    }
    setExplorerStatus("Best block refreshed.");
  } else if (method == "blockchain.getblock") {
    if (result.isObject()) {
      const QJsonObject block = result.toObject();
      const QString hash = block.value("hash").toString();
      const bool recentOnly =
          explorerRecentBlockRows_.contains(hash) && pendingExplorerBlockHash_.isEmpty();
      const bool mainDetail = !recentOnly && (pendingExplorerBlockHash_.isEmpty() || hash == pendingExplorerBlockHash_);
      displayExplorerBlock(block, mainDetail);
      if (mainDetail) {
        pendingExplorerBlockHash_.clear();
      }
    } else {
      updateExplorer(result);
    }
  } else if (method == "blockchain.getblockhash") {
    QString hash = result.toString();
    if (pendingExplorerRecentHeight_ >= 0) {
      const int row = explorerRecentBlockRows_.value(QString::number(pendingExplorerRecentHeight_), -1);
      if (row >= 0 && tblRecentBlocks_) {
        auto* item = explorerItem(explorerShortValue(hash), hash);
        item->setData(Qt::UserRole, hash);
        tblRecentBlocks_->setItem(row, 1, item);
      }
      explorerRecentBlockRows_.insert(hash, row);
      pendingExplorerRecentHeight_ = -1;
      if (!hash.isEmpty()) {
        rpc_->call("blockchain.getblock", QJsonArray{hash, 1});
      } else {
        requestNextExplorerRecentBlock();
      }
    } else if (!pendingExplorerHeightLookup_.isEmpty()) {
      pendingExplorerBlockHash_ = hash;
      pendingExplorerHeightLookup_.clear();
      if (!hash.isEmpty()) {
        rpc_->call("blockchain.getblock", QJsonArray{hash, 1});
      }
    } else if (!hash.isEmpty()) {
      pendingExplorerBlockHash_ = hash;
      rpc_->call("blockchain.getblock", QJsonArray{hash, 1});
    }
  } else if (method == "gettransaction" || method == "mempool.gettransaction") {
    if (result.isObject()) {
      displayExplorerTransaction(result.toObject());
    }
  } else if (method == "blockchain.getaddressbalance" || method == "getaddressbalance") {
    pendingExplorerAddressBalance_ = result;
    pendingExplorerAddressBalanceReady_ = true;
    if (pendingExplorerAddressHistoryReady_) {
      displayExplorerAddress();
    }
  } else if (method == "blockchain.getaddresshistory" || method == "getaddresshistory") {
    pendingExplorerAddressHistory_ = result;
    pendingExplorerAddressHistoryReady_ = true;
    if (pendingExplorerAddressBalanceReady_) {
      displayExplorerAddress();
    }
  } else if (method == "blockchain.scantxoutset") {
    if (result.isObject()) {
      displayAddressResult(result.toObject());
    }
  } else if (method == "blockchain.getblockcount") {
    int height = result.toInt();
    lblHeight_->setText(QString("Height: %1").arg(height));
    if (lblExplorerHeight_) {
      lblExplorerHeight_->setText(explorerIntegerText(height));
    }
    updateExplorerRecentBlocks(height);
    // Update connection status to green when we get a successful response
    lblConnectionStatus_->setText("Connected");
    lblConnectionStatus_->setStyleSheet(headerPillStyle());
  } else if (method == "blockchain.getmininginfo") {
    if (result.isObject()) {
      auto mobj = result.toObject();
      updateMining(mobj);
      setOverviewNetworkHashrate(
        mobj["networkhashps"].toDouble(0.0),
        "Estimated network hashrate from blockchain.getmininginfo");
      if (lblExplorerHashrate_) {
        lblExplorerHashrate_->setText(formatHashrateText(mobj["networkhashps"].toDouble(0.0)));
      }
      // Cache for AI status strip
      cachedMiningActive_ = mobj["mining"].toBool();
      cachedNetworkHashrate_ = mobj["networkhashps"].toDouble(0.0);
      refreshAiStatusStrip();
    }
  }
  // Phase X.1: CPU Stats Integration
  else if (method == "node.getcpustats") {
    if (result.isObject()) {
      auto obj = result.toObject();

      // Update CPU usage with real data (not simulated!)
      if (cpuProgressBar_ && lblCpuUsage_) {
        double cpuLoad = obj["cpu_load_percent"].toDouble(0.0);
        int cpuUsage = static_cast<int>(cpuLoad);
        cpuProgressBar_->setValue(cpuUsage);
        lblCpuUsage_->setText(QString("%1%").arg(cpuUsage));

        // Update progress bar color based on status
        QString status = obj["status"].toString("OK");
        QString color = "#3a4048"; // Neutral for OK
        if (status == "WARNING") {
          color = "#4a4f57";
        } else if (status == "CRITICAL") {
          color = "#525760";
        } else if (status == "EXHAUSTED") {
          color = "#5a606a";
        }
        cpuProgressBar_->setStyleSheet(chromeProgressBarStyle(color));
      }

      updateOverviewCpuTelemetry(obj);
      updateOverviewGpuTelemetry(obj.value("gpu").toObject());

      // TODO Phase X.1.1: Add timeout stats display
      // auto scriptStats = obj["script_validation"].toObject();
      // auto blockStats = obj["block_validation"].toObject();
      // auto sigStats = obj["signature_verification"].toObject();
    }
  }
  // Phase X.1: Resource Pressure Monitoring
  else if (method == "node.getresourcepressure") {
    if (result.isObject()) {
      auto obj = result.toObject();

      // Phase X.2: Display disk warnings in alerts
      QString diskStatus = obj["disk"].toString("OK");
      QString overallStatus = obj["overall"].toString("OK");

      if (diskStatus == "CRITICAL" || diskStatus == "FULL") {
        if (txtAlerts_) {
          QString alertMsg = QString("⚠️ Disk space %1 - Check available storage")
            .arg(diskStatus == "FULL" ? "FULL" : "CRITICAL");
          txtAlerts_->append(alertMsg);
        }
      }

      // TODO Phase X.1.2: Add resource health traffic light display
      // QString cpuStatus = obj["cpu"].toString("OK");
      // QString memoryStatus = obj["memory"].toString("OK");
      // 🟢 OK / 🟡 WARNING / 🔴 CRITICAL / ⚫ EXHAUSTED
    }
  }
  // Phase X.2: Disk Space Monitoring
  else if (method == "node.getdiskstats") {
    if (result.isObject()) {
      auto obj = result.toObject();

      // Extract disk space information
      qint64 totalBytes = obj["total_bytes"].toVariant().toLongLong();
      qint64 availableBytes = obj["available_bytes"].toVariant().toLongLong();
      double usagePercent = obj["usage_percent"].toDouble(0.0);
      double availablePercent = obj["available_percent"].toDouble(100.0);
      QString status = obj["status"].toString("OK");

      // Log disk stats for monitoring (Phase X.2.1: Add disk widgets later)
      if (status != "OK") {
        qDebug() << "⚠️ Disk Status:" << status
                 << "| Used:" << QString::number(usagePercent, 'f', 1) << "%"
                 << "| Available:" << QString::number(availableBytes / (1024.0 * 1024 * 1024), 'f', 2) << "GB";
      }

      // TODO Phase X.2.1: Add dedicated disk space widgets
      // - QProgressBar for disk usage
      // - QLabel showing available space in GB
      // - Color coding: green (OK), yellow (LOW), orange (CRITICAL), red (FULL)
    }
  }
  // Utreexo proof-service stats
  else if (method == "blockchain.getutreexocachestats") {
    if (result.isObject()) {
      auto obj = result.toObject();
      cachedBridgeActive_ = obj["bridge_enabled"].toBool();
      cachedProofCacheEntries_ =
          obj["block_cache_entries"].toInt() + obj["tx_cache_entries"].toInt();
      refreshAiStatusStrip();
      updateBridgeTab(obj);
    }
  }
  // Phase X.3: Enhanced Wallet Sync Status
  else if (method == "wallet.getsyncstatus") {
    if (result.isObject()) {
      auto obj = result.toObject();

      // Extract comprehensive sync information
      QString phase = obj["phase"].toString("unknown");
      QString phaseName = obj["phase_name"].toString("Unknown");
      double overallProgress = obj["overall_progress"].toDouble(0.0);
      QString progressPercent = obj["overall_progress_percent"].toString("0.0%");
      QString etaFormatted = obj["eta_formatted"].toString("Estimating...");
      bool isSynced = obj["is_synced"].toBool(false);

      // Update sync progress label with enhanced information
      if (lblSyncProgress_) {
        QString syncText;
        if (isSynced) {
          syncText = "✅ Fully Synced";
        } else {
          syncText = QString("%1: %2 (ETA: %3)")
            .arg(phaseName)
            .arg(progressPercent)
            .arg(etaFormatted);
        }
        lblSyncProgress_->setText(syncText);
      }

      // Extract detailed progress breakdown
      if (obj.contains("headers") && obj["headers"].isObject()) {
        auto headers = obj["headers"].toObject();
        int headersSynced = headers["synced"].toInt(0);
        int headersTotal = headers["total"].toInt(0);

        if (lblHeaders_) {
          lblHeaders_->setText(QString("Headers: %1 / %2").arg(headersSynced).arg(headersTotal));
        }
      }

      if (obj.contains("blocks") && obj["blocks"].isObject()) {
        auto blocks = obj["blocks"].toObject();
        int blocksSynced = blocks["synced"].toInt(0);
        int blocksTotal = blocks["total"].toInt(0);

        if (lblHeight_) {
          lblHeight_->setText(QString("Height: %1 / %2").arg(blocksSynced).arg(blocksTotal));
        }
      }

      // Extract slow reason information
      if (obj.contains("slow_reason") && obj["slow_reason"].isObject()) {
        auto slowReason = obj["slow_reason"].toObject();
        QString reason = slowReason["reason"].toString("none");
        QString description = slowReason["description"].toString("");
        QString suggestion = slowReason["suggestion"].toString("");

        // Show slow reason in alerts if not "none"
        if (reason != "none" && !description.isEmpty()) {
          if (txtAlerts_) {
            QString alertMsg = QString("ℹ️ Sync: %1 - %2").arg(description).arg(suggestion);
            // Only append if not already shown (avoid spam)
            QString currentAlerts = txtAlerts_->toPlainText();
            if (!currentAlerts.contains(description)) {
              txtAlerts_->append(alertMsg);
            }
          }
        }
      }

      // Log comprehensive sync status for debugging
      qDebug() << "Phase X.3 Sync Status:" << phase << "|" << progressPercent << "| ETA:" << etaFormatted;
    }
  }
  // Phase X.3: Reorg Detection
  else if (method == "wallet.getreorginfo") {
    if (result.isObject()) {
      auto obj = result.toObject();

      bool inProgress = obj["in_progress"].toBool(false);
      int depth = obj["depth"].toInt(0);
      int affectedTxs = obj["affected_transactions"].toInt(0);

      // Show reorg alert if in progress
      if (inProgress && depth > 0) {
        if (txtAlerts_) {
          QString alertMsg = QString("⚠️ Reorg detected! Depth: %1 blocks, Affected txs: %2")
            .arg(depth).arg(affectedTxs);
          txtAlerts_->append(alertMsg);
        }
        qWarning() << "Phase X.3: Reorg in progress - Depth:" << depth << "Affected:" << affectedTxs;
      }
    }
  }
  // Phase X.4: Fee Estimation
  else if (method == "wallet.estimatefee") {
    if (result.isObject()) {
      auto obj = result.toObject();

      double feerateUnaPerVb = obj["feerate"].toDouble(1.0);
      int blocks = obj["blocks"].toInt(6);
      QString confidence = obj["confidence"].toString("medium");
      QString source = obj["source"].toString("unknown");

      // rc8: persist the rate so collectSendForm() can pass it to the actual
      // send RPC. Without this cache, the preset combo (Low/Normal/High) was
      // purely cosmetic — the fee estimate showed in the label but never
      // reached the transaction.
      currentEstimatedFeeRate_ = feerateUnaPerVb;
      currentEstimatedFeeBlocks_ = blocks;

      // Update estimated fee label in Send tab
      if (lblEstimatedFee_) {
        // rc8: include the absolute fee estimate for the currently-entered
        // amount, not just the rate. Tx size approximation: 250 vB for
        // public sends, 1000 vB for confidential/private modes.
        const QString mode = currentSendMode();
        const int txSizeVb =
          (isSendModeConfidential(mode) || isSendModePrivate(mode) ||
           mode == "shield" || mode == "shield_to" || mode == "unshield" || mode == "shield_covenant")
          ? kPrivateSendEstimateVbytes : kPublicSendEstimateVbytes;
        const double feeDin = estimatedFeeDin(feerateUnaPerVb, txSizeVb);

        QString statusText = QStringLiteral("Estimated fee");
        if (source == "fallback" || confidence == "low") {
          statusText = QStringLiteral("Minimum fee");
        } else if (confidence == "medium") {
          statusText = QStringLiteral("Fee estimate");
        }

        QString feeText = QString("%1: %2 una/vB ~ %3 DIN (%4 blocks)")
          .arg(statusText)
          .arg(QString::number(feerateUnaPerVb, 'f', 2))
          .arg(QString::number(feeDin, 'f', 8))
          .arg(blocks);

        lblEstimatedFee_->setText(feeText);

        // Keep fee confidence readable without saturated warning colors.
        QString color = "#d6dde6";
        if (confidence == "medium") {
          color = "#c5ced8";
        } else if (confidence == "low") {
          color = "#b3bdc8";
        }
        lblEstimatedFee_->setStyleSheet(QString("QLabel { color: %1; font-size: 11px; }").arg(color));
      }

      qDebug() << "rc8 Fee Estimate:" << feerateUnaPerVb << "una/vB |" << confidence << "confidence |" << source;
    }
  }
  else if (method == "mining.info") {
    if (result.isObject()) {
      updateMiningStats(result.toObject());

      // Phase X.1: CPU usage now comes from node.getcpustats (real data)
      // Removed simulated CPU calculation (threads * 10%)
      // Real CPU monitoring provides actual load percentage from CPUBudgetMonitor
    }
  } else if (method == "mining.start") {
    if (result.isObject()) {
      auto obj = result.toObject();
      bool mining = obj["mining"].toBool();
      QString message = obj["message"].toString();

      if (mining && txtMiningOutput_) {
        txtMiningOutput_->append("✅ " + message);
        lblMiningStatus_->setText(miningStatusActiveText());
      }
    }
  } else if (method == "mining.stop") {
    if (result.isObject()) {
      auto obj = result.toObject();
      QString message = obj["message"].toString();

      if (txtMiningOutput_) {
        txtMiningOutput_->append("✅ " + message);
      }
      activeMinerType_ = "none";
      isMining_ = false;
      resetOverviewMiningTelemetry();
    }
  } else if (method == "mining.setaddress") {
    if (result.isObject()) {
      auto obj = result.toObject();
      QString address = obj["address"].toString();
      QString message = obj["message"].toString();

      if (txtMiningOutput_) {
        txtMiningOutput_->append(QString("✅ Mining address set: %1").arg(address));
      }
    }
  } else if (method == "mining.getaddress") {
    if (result.isObject()) {
      auto obj = result.toObject();
      QString address = obj["address"].toString();

      if (!address.isEmpty() && edtMiningAddress_) {
        edtMiningAddress_->setText(address);
      }
    }
  } else if (method == "mining.getstatus") {
    // Phase Y: Integrated CPU miner status updates
    // mining.getstatus reflects daemon-side mining only — skip when a local embedded/process miner owns the UI.
    const bool embeddedMinerActive = minerCtrl_ && minerCtrl_->running();
    const bool processMinerActive = miningProcess_ && miningProcess_->state() == QProcess::Running;
    if (embeddedMinerActive || processMinerActive) {
      return;
    }
    if (result.isObject()) {
      auto obj = result.toObject();
      bool is_mining = obj["is_mining"].toBool();

      // Parse metrics_raw JSON string
      QString metrics_raw = obj["metrics_raw"].toString();
      QJsonDocument metricsDoc = QJsonDocument::fromJson(metrics_raw.toUtf8());
      if (metricsDoc.isObject()) {
        auto metrics = metricsDoc.object();
        double hashrate = metrics["hashrate"].toDouble();
        qint64 total_hashes = metrics["total_hashes"].toVariant().toLongLong();
        int blocks_found = metrics["blocks_found"].toInt();

        // Update UI labels
        if (lblHashrate_) {
          if (hashrate > 1000000) {
            lblHashrate_->setText(QString("%1 MH/s").arg(hashrate / 1000000.0, 0, 'f', 2));
          } else if (hashrate > 1000) {
            lblHashrate_->setText(QString("%1 kH/s").arg(hashrate / 1000.0, 0, 'f', 1));
          } else {
            lblHashrate_->setText(QString("%1 H/s").arg(hashrate, 0, 'f', 0));
          }
        }

        if (lblTotalHashes_) {
          lblTotalHashes_->setText(QString::number(total_hashes));
        }

        if (lblBlocksFound_) {
          lblBlocksFound_->setText(QString::number(blocks_found));
        }

        // v0.14.0.4: Update mining status AND toggle button state from RPC
        if (is_mining != isMining_) {
          isMining_ = is_mining;
          if (btnStartMining_) {
            if (is_mining) {
              btnStartMining_->setText("Stop Mining");
              btnStartMining_->setStyleSheet(headerButtonStyle());
              btnStartMining_->setToolTip("Click to stop mining");
            } else {
              btnStartMining_->setText("Start Mining");
              btnStartMining_->setStyleSheet(headerButtonStyle());
              btnStartMining_->setToolTip("Click to start mining");
            }
          }
        }

        if (lblMiningStatus_) {
          if (is_mining) {
            lblMiningStatus_->setText(miningStatusActiveText());
            lblMiningStatus_->setStyleSheet(chromePillStyle());
          } else {
            lblMiningStatus_->setText(miningStatusInactiveText());
            lblMiningStatus_->setStyleSheet(chromePillStyle());
          }
        }

        setMiningOutputCinematicEnabled(isMining_);
      }
    }
  } else if (method == "mining.getexternalstats") {
    // External miner stats from daemon protocol events
    if (result.isObject()) {
      handleExternalMinerStats(result.toObject());
    }
  } else if (method == "mempool.getinfo") {
    if (result.isObject()) {
      auto obj = result.toObject();
      if (obj.contains("size") && obj.contains("bytes")) {
        int size = obj["size"].toInt();
        int bytes = obj["bytes"].toInt();

        // Phase X.6: Enhanced mempool stats
        double avgFeeRate = obj["avg_fee_rate"].toDouble(0.0);
        int minFeeRate = obj["min_fee_rate"].toInt(0);
        int maxFeeRate = obj["max_fee_rate"].toInt(0);
        int oldestTxAge = obj["oldest_tx_age_seconds"].toInt(0);

        // Format mempool text with fee rate info
        QString mempoolText = QString("Mempool: %1 txs, %2 bytes").arg(size).arg(bytes);
        if (avgFeeRate > 0) {
          mempoolText += QString(" | Avg fee: %1 una/vB").arg(QString::number(avgFeeRate, 'f', 2));
        }
        lblMempool_->setText(mempoolText);

        // Update monitoring dashboard
        if (lblMempoolSize_) {
          QString sizeText = QString("%1 txs").arg(size);
          if (oldestTxAge > 0) {
            // Show oldest tx age
            QString ageText;
            if (oldestTxAge > 3600) {
              ageText = QString("%1h").arg(oldestTxAge / 3600);
            } else if (oldestTxAge > 60) {
              ageText = QString("%1m").arg(oldestTxAge / 60);
            } else {
              ageText = QString("%1s").arg(oldestTxAge);
            }
            sizeText += QString(" (oldest: %1)").arg(ageText);
          }
          lblMempoolSize_->setText(sizeText);
        }
        if (lblMempoolBytes_) {
          // Format bytes nicely with fee rate range
          QString bytesStr;
          if (bytes > 1024 * 1024) {
            bytesStr = QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
          } else if (bytes > 1024) {
            bytesStr = QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 2);
          } else {
            bytesStr = QString("%1 bytes").arg(bytes);
          }
          lblMempoolBytes_->setText(bytesStr);
        }
      } else {
        qWarning() << "getmempoolinfo missing required fields";
        lblMempool_->setText("Mempool: N/A");
      }
    }
  } else if (method == "getnetworkinfo" || method == "network.info") {
    if (result.isObject()) {
      const auto info = result.toObject();
      updateNetworkInfo(info);
      if (overviewConnectivityCard_) overviewConnectivityCard_->setNetworkInfo(info);
    }
  } else if (method == "network.getrelayservice" || method == "network.setrelayservice") {
    if (overviewConnectivityCard_ && result.isObject()) {
      overviewConnectivityCard_->setRelayServiceStatus(result.toObject());
    }
  } else if (method == "network.setonionservice") {
    if (overviewConnectivityCard_ && result.isObject()) {
      overviewConnectivityCard_->setOnionServiceStatus(result.toObject());
    }
  } else if (method == "getpeerinfo" || method == "getpeerinfo") {
    // Week 7: Backend returns { "peers": [...], "connected_peers": N }
    QJsonArray peers;
    int peerCount = 0;

    if (result.isObject()) {
      QJsonObject obj = result.toObject();
      peers = obj["peers"].toArray();
      peerCount = obj["connected_peers"].toInt(peers.size());
      lblConnections_->setText(QString("Connections: %1").arg(peerCount));
      // Update peer table tab
      updatePeerTable(peers);
    } else if (result.isArray()) {
      // Fallback: if backend returns array directly (legacy format)
      peers = result.toArray();
      peerCount = peers.size();
      lblConnections_->setText(QString("Connections: %1").arg(peerCount));
      updatePeerTable(peers);
    }
    
    // Feed real peer count to AI tools + status strip
    cachedPeerCount_ = peerCount;
    refreshAiStatusStrip();

    // Update monitoring dashboard
    if (lblPeersCount_) {
      lblPeersCount_->setText(QString("%1 peers").arg(peerCount));
    }
    if (lblPeersStatus_) {
      if (peerCount == 0) {
        lblPeersStatus_->setText("Disconnected");
        lblPeersStatus_->setStyleSheet("QLabel { font-size: 11px; color: #a9b2bc; }");
      } else if (peerCount < 3) {
        lblPeersStatus_->setText("Poor connectivity");
        lblPeersStatus_->setStyleSheet("QLabel { font-size: 11px; color: #b8c0ca; }");
      } else {
        lblPeersStatus_->setText("Good connectivity");
        lblPeersStatus_->setStyleSheet("QLabel { font-size: 11px; color: #d0d7df; }");
      }
    }
    
    // Update overview peers table (compact view - top 5 peers)
    if (tblPeersOverview_) {
      tblPeersOverview_->setSortingEnabled(false);
      tblPeersOverview_->setRowCount(0);
      
      int maxRows = qMin(5, peers.size());  // Show max 5 peers in overview
      for (int i = 0; i < maxRows; i++) {
        auto peer = peers[i].toObject();
        int row = tblPeersOverview_->rowCount();
        tblPeersOverview_->insertRow(row);
        
        // Location
        QString addr = peer["addr"].toString();
        const QString location = peerLocationLabel(addr, row);
        auto* locationItem = new QTableWidgetItem(location);
        locationItem->setData(Qt::UserRole, addr);
        locationItem->setToolTip(peerLocationTooltip(addr, location));
        tblPeersOverview_->setItem(row, 0, locationItem);
        
        // Activity: the daemon does not expose ping latency, so show recent message activity.
        qint64 lastMsg = peer["lastrecv"].toVariant().toLongLong();
        if (lastMsg <= 0) lastMsg = peer["last_message_at"].toVariant().toLongLong();
        qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
        qint64 agoSecs = (lastMsg > 0 && nowSecs >= lastMsg) ? (nowSecs - lastMsg) : -1;
        QString activity;
        if (agoSecs < 0) {
          activity = "Connected";
        } else if (agoSecs <= 60) {
          activity = "Active";
        } else if (agoSecs <= 300) {
          activity = "Idle";
        } else {
          activity = "Quiet";
        }
        auto* activityItem = new QTableWidgetItem(activity);
        activityItem->setToolTip(
            "Dinero Core currently reports peer activity timestamps, not per-peer ping latency.");
        tblPeersOverview_->setItem(row, 1, activityItem);

        // Last Seen: time since last message from this peer.
        QString lastSeen;
        if (agoSecs < 0) {
          lastSeen = "-";
        } else if (agoSecs >= 3600) {
          lastSeen = QString("%1 h ago").arg(agoSecs / 3600);
        } else if (agoSecs >= 60) {
          lastSeen = QString("%1 min ago").arg(agoSecs / 60);
        } else {
          lastSeen = QString("%1 s ago").arg(agoSecs);
        }
        // Tooltip shows full uptime
        qint64 conntime = peer["conntime"].toVariant().toLongLong();
        qint64 uptimeSecs = (conntime > 0 && nowSecs >= conntime) ? (nowSecs - conntime) : -1;
        auto* lastSeenItem = new QTableWidgetItem(lastSeen);
        if (uptimeSecs >= 0) {
          QString uptimeStr = uptimeSecs >= 3600 ? QString("%1 h").arg(uptimeSecs / 3600) :
                              uptimeSecs >= 60 ? QString("%1 min").arg(uptimeSecs / 60) :
                              QString("%1 s").arg(uptimeSecs);
          lastSeenItem->setToolTip(QString("Connected for %1").arg(uptimeStr));
        }
        tblPeersOverview_->setItem(row, 2, lastSeenItem);
        
        // Peer height (best available from daemon fields)
        const int syncedBlocks = peer["synced_blocks"].toInt(-1);
        const int syncedHeaders = peer["synced_headers"].toInt(-1);
        const int startHeight = peer["startingheight"].toInt(-1);
        const int bestKnown = peer["best_known_height"].toInt(-1);
        int peerHeight = syncedBlocks;
        if (syncedHeaders > peerHeight) peerHeight = syncedHeaders;
        if (startHeight > peerHeight) peerHeight = startHeight;
        if (bestKnown > peerHeight) peerHeight = bestKnown;
        const QString peerHeightText = peerHeightDisplayText(peerHeight, cachedHeight_);
        auto* heightItem = new QTableWidgetItem(peerHeightText);
        heightItem->setToolTip(peerHeightBreakdownTooltip(startHeight,
                                                          syncedHeaders,
                                                          syncedBlocks,
                                                          bestKnown,
                                                          cachedHeight_));
        if (cachedHeight_ > 0 && peerHeight >= 0 && peerHeight + 2 < cachedHeight_) {
          heightItem->setForeground(QColor("#d9b36a"));
        }
        tblPeersOverview_->setItem(row, 3, heightItem);

        // Client
        const QString rawClient = peer["subver"].toString();
        auto* clientItem = new QTableWidgetItem(peerClientLabel(rawClient));
        clientItem->setToolTip(peerClientTooltip(rawClient));
        tblPeersOverview_->setItem(row, 4, clientItem);
      }
      
      tblPeersOverview_->setSortingEnabled(true);
    }
  } else if (method == "mining.getblocktemplate") {
    if (result.isObject()) {
      updateBlockTemplate(result.toObject());
    }
  } else if (method == "blockchain.getinfo") {
    if (result.isObject()) {
      auto obj = result.toObject();
      if (!obj.contains("blocks") || !obj.contains("headers")) {
        qWarning() << "blockchain.getinfo missing required fields";
        return;
      }
      
      int blocks = obj["blocks"].toInt();
      int headers = obj["headers"].toInt();
      
      lblHeight_->setText(QString("Height: %1 blocks").arg(blocks));
      lblHeaders_->setText(QString("Headers: %1").arg(headers));
      
      // Show sync progress if headers > blocks (headers-first sync in progress)
      if (headers > blocks && headers > 0) {
        double progress = (blocks * 100.0) / headers;
        lblSyncProgress_->setText(QString("⏬ Syncing: %1% (%2 / %3)")
          .arg(progress, 0, 'f', 1)
          .arg(blocks)
          .arg(headers));
        lblSyncProgress_->setStyleSheet("QLabel { color: #d0d7df; font-weight: 600; background: #262b32; border: 1px solid #373d46; border-radius: 6px; padding: 5px; }");
      } else if (headers == blocks && blocks > 0) {
        lblSyncProgress_->setText("✅ Fully synced!");
        lblSyncProgress_->setStyleSheet("QLabel { color: #e1e6ec; font-weight: 600; background: #2b3037; border: 1px solid #3a4048; border-radius: 6px; padding: 5px; }");
      } else {
        lblSyncProgress_->setText("");
      }
    }
  } else if (method == "wallet.getinfo" || method == "getwalletinfo") {
    // Check if HD wallet is enabled
    if (result.isObject()) {
      auto obj = result.toObject();
      bool hdEnabled = obj["hd_enabled"].toBool(false);
      bool unlocked = walletUnlockedFromRpc(obj);
      
      // Avoid auto-launching restore/create flow; keep wallet actions explicit.
      if (!hdEnabled && lblConnectionStatus_) {
        lblConnectionStatus_->setText("No wallet loaded");
        lblConnectionStatus_->setStyleSheet(headerPillStyle());
        lblConnectionStatus_->setToolTip("No active wallet is loaded.");
      }
      
      // Keep connection pill stable; expose wallet state in tooltip only.
      if (hdEnabled && lblConnectionStatus_) {
        lblConnectionStatus_->setToolTip(
          QString("Wallet state: %1").arg(unlocked ? "unlocked" : "locked"));
      }

      // Cache primary address from daemon (deterministic from seed)
      QString pa = obj.value("primary_address").toString();
      if (!pa.isEmpty()) cachedPrimaryAddress_ = pa;

      // Apply to receive tab
      applyPrimaryAddressesToReceiveTab();
    }
  } else if (method == "wallet.getviewkeyinfo") {
    // Phase 6: wallet identity = fingerprint:accountIndex (matches DineroDPI model)
    if (result.isObject()) {
      const auto obj = result.toObject();
      const QString fp = obj.value("fingerprint").toString();
      const int accountIndex = obj.value("account_index").toInt(0);
      if (!fp.isEmpty() && changeAddrMgr_) {
        changeAddrMgr_->setWalletIdentityKey(
            QString("%1:%2").arg(fp).arg(accountIndex));
        changeAddrMgr_->reconcileStaleReservations();
      }
    }
  } else if (method == "wallet.listwallets") {
    if (cmbWalletSelector_) {
      QString selectedBefore = cmbWalletSelector_->currentText();
      bool blocked = cmbWalletSelector_->blockSignals(true);
      cmbWalletSelector_->clear();

      QJsonArray wallets;
      if (result.isArray()) {
        wallets = result.toArray();
      } else if (result.isObject()) {
        const auto obj = result.toObject();
        if (obj.value("wallets").isArray()) {
          wallets = obj.value("wallets").toArray();
        }
      }

      if (!wallets.isEmpty()) {
        for (const auto& walletVal : wallets) {
          QString walletName;
          if (walletVal.isObject()) {
            walletName = walletVal.toObject().value("name").toString().trimmed();
          } else if (walletVal.isString()) {
            walletName = walletVal.toString().trimmed();
          }

          if (!walletName.isEmpty() && cmbWalletSelector_->findText(walletName) < 0) {
            cmbWalletSelector_->addItem(walletName);
          }
        }
      }

      QString desiredSelection;
      if (!pendingWalletOpenName_.isEmpty()) {
        desiredSelection = pendingWalletOpenName_;
      } else if (!currentWalletName_.isEmpty()) {
        desiredSelection = currentWalletName_;
      } else {
        desiredSelection = selectedBefore;
      }

      if (!desiredSelection.isEmpty()) {
        int idx = cmbWalletSelector_->findText(desiredSelection);
        if (idx >= 0) {
          cmbWalletSelector_->setCurrentIndex(idx);
        }
      }

      cmbWalletSelector_->blockSignals(blocked);
      updateWalletSwitcherState();

      if (!walletSwitchInFlight_ &&
          currentWalletName_.isEmpty() &&
          !autoLoadDefaultAttempted_ &&
          cmbWalletSelector_->count() == 1) {
        const int defaultIdx = 0;
        if (defaultIdx >= 0) {
          autoLoadDefaultAttempted_ = true;
          cmbWalletSelector_->setCurrentIndex(defaultIdx);
          onLoadSelectedWallet();
        }
      }
    }
  } else if (method == "wallet.open" || method == "wallet.load") {
    QString errorText;
    QString openedWalletName = pendingWalletOpenName_;
    if (result.isObject()) {
      const auto obj = result.toObject();
      bool loadSucceeded = false;
      if (obj.contains("success")) {
        loadSucceeded = obj.value("success").toBool(false);
      } else if (obj.contains("active")) {
        loadSucceeded = obj.value("active").toBool(false);
      }

      if (loadSucceeded) {
        if (obj.contains("wallet_name")) {
          openedWalletName = obj.value("wallet_name").toString(openedWalletName);
        } else if (obj.contains("name")) {
          openedWalletName = obj.value("name").toString(openedWalletName);
        }
        pendingWalletOpenName_ = openedWalletName;
        updateWalletSwitcherState();

        if (lblErrorMessage_) {
          lblErrorMessage_->setVisible(false);
        }

        lblConnectionStatus_->setText("Switching wallet");
        lblConnectionStatus_->setStyleSheet(headerPillStyle());
        lblConnectionStatus_->setToolTip(QString("Binding active wallet: %1").arg(openedWalletName));

        checkRescanStatus();
        rpc_->getBalance();
        rpc_->callNamed("wallet.listaddresses", QJsonObject{{"count", 200}});
        rpc_->call("wallet.listunspent", QJsonArray());
        loadTransactionHistory();
        rpc_->call("wallet.getsyncstatus", QJsonArray());
        rpc_->call("wallet.getviewkeyinfo", QJsonArray()); // Phase 6: get fingerprint for change reservation
        return;
      }

      errorText = obj.value("error").toString("Unknown error while loading wallet");
    } else {
      errorText = "Invalid response while loading wallet";
    }

    walletSwitchInFlight_ = false;
    pendingWalletOpenName_.clear();
    updateWalletSwitcherState();
    if (!currentWalletName_.isEmpty()) {
      checkRescanStatus();
      rpc_->getBalance();
      rpc_->callNamed("wallet.listaddresses", QJsonObject{{"count", 200}});
      rpc_->call("wallet.listunspent", QJsonArray());
      loadTransactionHistory();
      rpc_->call("wallet.getviewkeyinfo", QJsonArray());
    }
    QMessageBox::warning(this, "Wallet Load Failed",
      QString("Could not load selected wallet.\n\n%1").arg(errorText));
  } else if (method == "wallet.unload") {
    QString errorText;
    bool unloadSucceeded = false;

    if (result.isObject()) {
      const auto obj = result.toObject();
      unloadSucceeded = obj.value("success").toBool(false);
      if (!unloadSucceeded) {
        errorText = obj.value("error").toString("Unknown error while unloading wallet");
      }
    } else {
      errorText = QStringLiteral("Invalid response while unloading wallet");
    }

    if (unloadSucceeded) {
      walletSwitchInFlight_ = false;
      pendingWalletOpenName_.clear();
      walletRescanning_ = false;
      walletUnlocked_ = false;
      unlockSecondsRemaining_ = 0;
      if (unlockCountdownTimer_) {
        unlockCountdownTimer_->stop();
      }
      activeReservationId_.clear();
      currentWalletName_.clear();
      autoLoadDefaultAttempted_ = true;

      bindWalletScopedState(QString());
      if (changeAddrMgr_) {
        changeAddrMgr_->setWalletIdentityKey(QString());
      }
      if (lblWalletName_) {
        lblWalletName_->setText("Wallet: none");
        lblWalletName_->setStyleSheet(headerPillStyle());
        lblWalletName_->setToolTip("No wallet loaded. Create or restore a wallet to get started.");
      }

      clearWalletScopedUiState();
      refreshWalletMiningAddress();
      updateWalletUIState();
      updateWalletSwitcherState();

      if (lblErrorMessage_) {
        lblErrorMessage_->setVisible(false);
      }
      lblConnectionStatus_->setText("No wallet loaded");
      lblConnectionStatus_->setStyleSheet(headerPillStyle());
      lblConnectionStatus_->setToolTip("No active wallet is loaded.");
      return;
    }

    lblConnectionStatus_->setText("Wallet unload failed");
    lblConnectionStatus_->setStyleSheet(headerPillStyle());
    lblConnectionStatus_->setToolTip(errorText);
  } else if (method == "sendpubliccovenant" || method == "wallet.sendpubliccovenant") {
    // Handle sendpubliccovenant result (public/auditable covenant)
    if (btnSend_) {
      btnSend_->setEnabled(true);
      updateSendModeUi();
    }

    if (result.isObject()) {
      const auto obj = result.toObject();
      const QString error = obj.value("error").toString();
      if (!error.isEmpty()) {
        if (lblSendStatus_) {
          lblSendStatus_->setText(QString::fromUtf8("\xE2\x9D\x8C Public contract error: %1").arg(error));
          lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
        }
        return;
      }

      const QString txid = obj.value("txid").toString();
      const double amountSent = obj.value("amount_sent").toDouble(0.0);
      const double fee = obj.value("fee").toDouble(0.0);
      const QString covenantScript = obj.value("covenant_script").toString();
      const QString visibility = obj.value("visibility").toString("public");
      const QString status = obj.value("status").toString("broadcast");
      const QString warning = obj.value("warning").toString();

      if (lblSendStatus_) {
        lblSendStatus_->setText(status == "broadcast"
          ? QString::fromUtf8("\xE2\x9C\x85 Public contract broadcast complete.")
          : QString::fromUtf8("\xE2\x9C\x85 Public contract signed but not broadcast."));
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; font-weight: 600; }");
      }

      if (txtSendResult_) {
        txtSendResult_->setHtml(QString(
          "<b>Public Contract Created</b><br><br>"
          "<b>Transaction ID:</b><br>"
          "<span style='font-family: monospace; font-size: 11px;'>%1</span><br><br>"
          "<b>Amount:</b> %2 DIN<br>"
          "<b>Fee:</b> %3 DIN<br>"
          "<b>Visibility:</b> %4<br>"
          "<b>Tx version:</b> v2<br>"
          "<b>Status:</b> %5"
          "%6"
        ).arg(txid,
              QString::number(amountSent, 'f', 8),
              QString::number(fee, 'f', 8),
              visibility,
              status,
              warning.isEmpty() ? QString() : QString("<br><b>Warning:</b> %1").arg(warning)));
      }

      if (edtRecipient_) edtRecipient_->clear();
      if (edtAmount_) edtAmount_->clear();
      if (edtCustomScript_) edtCustomScript_->clear();
      refresh();
      loadTransactionHistory();
    }
  } else if (method == "withdrawfromvault" || method == "wallet.withdrawfromvault") {
    if (result.isObject()) {
      auto obj = result.toObject();
      QString error = obj.value("error").toString();
      if (!error.isEmpty()) {
        QMessageBox::warning(this, "Contract Spend Failed", error);
      } else {
        QString wtxid = obj.value("txid").toString();
        double withdrawn = obj.value("amount_withdrawn").toDouble(0.0);
        double fee = obj.value("fee").toDouble(0.0);
        QString wstatus = obj.value("status").toString("unknown");
        QMessageBox::information(this, "Contract Spend",
          QString("<b>Contract Spend %1</b><br><br>"
                  "<b>Transaction ID:</b><br>"
                  "<span style='font-family: monospace; font-size: 11px;'>%2</span><br><br>"
                  "<b>Amount:</b> %3 DIN<br>"
                  "<b>Fee:</b> %4 DIN<br>"
                  "<b>Status:</b> %5")
            .arg(wstatus == "broadcast" ? "Complete" : "Submitted")
            .arg(wtxid)
            .arg(QString::number(withdrawn, 'f', 8))
            .arg(QString::number(fee, 'f', 8))
            .arg(wstatus));
        refresh();
        loadTransactionHistory();
      }
    }
  } else if (method == "wallet.consolidate") {
    // Re-enable consolidate button
    if (btnConsolidate_) {
      btnConsolidate_->setEnabled(true);
    }

    QJsonObject obj = result.toObject();
    const bool isPreview = obj.value("dry_run").toBool(false) && obj.value("ok").toBool(false);
    const int sel = obj.value("selected_inputs").toInt(0);

    if (isPreview && sel == 0) {
      pendingConsolidateParams_ = {};
      // Daemon found nothing eligible to consolidate.
      QMessageBox::information(this, "Consolidation",
        "Nothing to consolidate — no eligible UTXOs.");
    } else if (isPreview) {
      // Dry-run plan accepted: confirm the real numbers, then broadcast.
      const double fee = obj.value("estimated_fee").toDouble();
      const double out = obj.value("output_value").toDouble();
      QMessageBox box(this);
      box.setWindowTitle("Confirm Consolidation");
      box.setText(QString("Consolidate %1 UTXOs into one output.").arg(sel));
      box.setInformativeText(QString("Estimated fee: %1 DIN\nResulting output: %2 DIN")
                               .arg(fee, 0, 'f', 8).arg(out, 0, 'f', 8));
      QPushButton* go = box.addButton("Consolidate", QMessageBox::AcceptRole);
      box.addButton("Cancel", QMessageBox::RejectRole);
      box.exec();
      if (box.clickedButton() == go) {
        QJsonObject p = pendingConsolidateParams_;
        if (p.isEmpty()) p["address_type"] = "auto";
        p["dry_run"] = false;
        p["broadcast"] = true;
        if (btnConsolidate_) btnConsolidate_->setEnabled(false);
        rpc_->callNamed("wallet.consolidate", p);
      } else {
        pendingConsolidateParams_ = {};
        if (btnConsolidate_ && cachedUtxoCount_ > 50) {
          btnConsolidate_->setText(QString("\xF0\x9F\xA7\xB9 Consolidate (%1 UTXOs)").arg(cachedUtxoCount_));
        }
      }
    } else if (!obj.value("ok").toBool(true)) {
      pendingConsolidateParams_ = {};
      // Fee gate or execution error — surface the daemon's reason.
      QMessageBox::warning(this, "Consolidation Failed",
        obj.value("reason").toString(obj.value("error").toString("Consolidation could not be completed.")));
    } else {
      pendingConsolidateParams_ = {};
      // Executed + broadcast.
      const QString txid = obj.value("txid").toString();
      QMessageBox::information(this, "Consolidation Complete",
        txid.isEmpty() ? QString("Consolidation submitted.")
                       : QString("Consolidation broadcast.\nTXID: %1").arg(txid));
      refresh();
      loadTransactionHistory();
    }
  } else if (method == "wallet.listtransactions") {
    if (result.isArray()) {
      updateTransactionTable(result.toArray());
    } else {
      qWarning() << "listtransactions returned non-array result";
      tblTransactions_->setRowCount(0);
      tblTransactions_->insertRow(0);
      auto *item = new QTableWidgetItem("No transactions found");
      item->setForeground(QBrush(QColor("#868e96")));
      item->setTextAlignment(Qt::AlignCenter);
      tblTransactions_->setItem(0, 0, item);
      tblTransactions_->setSpan(0, 0, 1, 7);
    }
    // Phase 4: also update contracts table if a refresh was requested
    if (pendingContractsRefresh_) {
      pendingContractsRefresh_ = false;
      updateContractsTable(result);
    }
  } else if (method == "wallet.listunspent") {
    utxoRequestPending_ = false;
    if (result.isArray()) {
      const QJsonArray incoming = result.toArray();
      const bool changed = incoming != cachedUtxos_;
      cachedUtxos_ = incoming;
      if (isUtxoTabActive() && (changed || (tblUTXOs_ && tblUTXOs_->rowCount() == 0))) {
        updateUTXOTable(cachedUtxos_);
      }
    } else {
      qWarning() << "listunspent returned non-array result";
      auto *tblUTXOs = tblUTXOs_;
      if (tblUTXOs && isUtxoTabActive()) {
        tblUTXOs->setRowCount(0);
        tblUTXOs->insertRow(0);
        auto *item = new QTableWidgetItem("No UTXOs found");
        item->setForeground(QBrush(QColor("#868e96")));
        item->setTextAlignment(Qt::AlignCenter);
        tblUTXOs->setItem(0, 0, item);
        tblUTXOs->setSpan(0, 0, 1, 6);
      }
    }
  } else if (method == "wallet.listaddresses") {
    if (result.isArray()) {
      tblAddresses_->setSortingEnabled(false);
      tblAddresses_->setUpdatesEnabled(false);
      tblAddresses_->blockSignals(true);
      tblAddresses_->setRowCount(0);

      // Receive-tab filter: "all" / "taproot" / "p2mr"
      const QString receiveFilter = currentReceiveMode();

      auto addresses = result.toArray();
      constexpr int MAX_DISPLAY_ADDRESSES = 200;
      int displayed = 0;
      int skipped_zero = 0;
      for (const auto& addrVal : addresses) {
        if (displayed >= MAX_DISPLAY_ADDRESSES) break;
        if (!addrVal.isObject()) continue;
        auto obj = addrVal.toObject();

        QString address = obj["address"].toString();
        int index = obj["index"].toInt();
        QString label = obj["label"].toString();
        // Skip shieldcoins/consolidate entries entirely — they're internal
        // change addresses from pool bootstrap, not user-facing receive addresses
        if (label == "shieldcoins" || label == "consolidate" ||
            label == "shield_change" || label == "consolidation") {
          skipped_zero++;
          continue;
        }

        // Use daemon-provided path if available; otherwise derive from type.
        int account = obj["account"].toInt();
        int change = obj["change"].toInt();
        QString type = obj["type"].toString().toLower();
        QString path = obj["path"].toString();

        // Apply receive-tab filter by scheme.
        const bool p2mrPrefix = address.startsWith("din1r") ||
                                address.startsWith("tdin1r") ||
                                address.startsWith("rdin1r");
        const bool taprootPrefix = address.startsWith("din1p") ||
                                   address.startsWith("tdin1p") ||
                                   address.startsWith("rdin1p");
        const bool isP2mrRow   = (type == "p2mr")   || p2mrPrefix;
        const bool isTaprootRow= (type == "p2tr" || type == "taproot") || taprootPrefix;
        if (receiveFilter == "p2mr" && !isP2mrRow) continue;
        if (receiveFilter == "taproot" && !isTaprootRow) continue;
        if (path.isEmpty()) {
          if (account < 0) {
            path = "imported";
          } else {
            const bool p2mrPrefix = address.startsWith("din1r") || address.startsWith("tdin1r") || address.startsWith("rdin1r");
            const bool taprootPrefix = address.startsWith("din1p") || address.startsWith("tdin1p") || address.startsWith("rdin1p");
            int purpose;
            if (type == "p2mr" || p2mrPrefix) {
              purpose = 88;  // P2MR (ML-DSA-65)
            } else if (type == "p2tr" || type == "taproot" || taprootPrefix) {
              purpose = 86;  // Taproot
            } else {
              purpose = 84;  // legacy fallback
            }
            path = QString("m/%1'/1448'/%2'/%3/%4").arg(purpose).arg(account).arg(change).arg(index);
          }
        }

        double balance = obj["balance"].toDouble(0.0);
        
        int row = tblAddresses_->rowCount();
        tblAddresses_->insertRow(row);

        // Column 0: Index
        auto *indexItem = new QTableWidgetItem(QString::number(index));
        indexItem->setTextAlignment(Qt::AlignCenter);
        tblAddresses_->setItem(row, 0, indexItem);

        // Column 1: Address
        tblAddresses_->setItem(row, 1, new QTableWidgetItem(address));

        // Column 2: Label (user-defined, editable via double-click)
        auto *labelItem = new QTableWidgetItem(label);
        labelItem->setForeground(label.isEmpty() ? QBrush(QColor("#868e96")) : QBrush(QColor("#c6ced8")));
        if (label.isEmpty()) {
          labelItem->setText("(no label)");
          labelItem->setToolTip("Double-click to add a label");
        }
        tblAddresses_->setItem(row, 2, labelItem);

        // Column 3: Balance
        auto *balanceItem = new QTableWidgetItem(QString("%1").arg(balance, 0, 'f', 8));
        balanceItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (balance > 0.0) {
          balanceItem->setForeground(QBrush(QColor("#d6dde6"))); // Active balance
          QFont font = balanceItem->font();
          font.setBold(true);
          balanceItem->setFont(font);
        } else {
          balanceItem->setForeground(QBrush(QColor("#868e96"))); // Gray for zero
        }
        tblAddresses_->setItem(row, 3, balanceItem);

        // Column 4: Path
        auto *pathItem = new QTableWidgetItem(path);
        pathItem->setForeground(QBrush(QColor("#868e96")));
        tblAddresses_->setItem(row, 4, pathItem);

        // Column 5: Copy button
        auto *btnCopy = new QPushButton("📋 Copy");
        connect(btnCopy, &QPushButton::clicked, [address]() {
          QApplication::clipboard()->setText(address);
        });
        tblAddresses_->setCellWidget(row, 5, btnCopy);
        displayed++;
      }

      tblAddresses_->blockSignals(false);        // Re-enable itemChanged signals
      tblAddresses_->setUpdatesEnabled(true);   // Re-enable UI updates
      tblAddresses_->setSortingEnabled(true);
      tblAddresses_->sortByColumn(0, Qt::AscendingOrder); // Sort by index

      if (addresses.size() > MAX_DISPLAY_ADDRESSES) {
        qDebug() << "Showing" << displayed << "of" << addresses.size() << "addresses (limited to" << MAX_DISPLAY_ADDRESSES << ")";
      } else {
        qDebug() << "Loaded" << addresses.size() << "addresses";
      }
    }
  } else if (method == "wallet.listaddresseswithbalances") {
    // Update address table with balances
    if (result.isArray()) {
      tblAddresses_->setSortingEnabled(false);
      tblAddresses_->blockSignals(true);  // Suppress itemChanged during bulk fill
      tblAddresses_->setRowCount(0);

      auto addresses = result.toArray();
      for (const auto& addrVal : addresses) {
        if (!addrVal.isObject()) continue;
        auto obj = addrVal.toObject();

        QString address = obj["address"].toString();
        int index = obj["index"].toInt();
        QString label = obj["label"].toString();
        double balance = obj["balance"].toDouble();
        int account = obj["account"].toInt();
        int change = obj["change"].toInt();
        QString type = obj["type"].toString().toLower();
        QString path = obj["path"].toString();
        if (path.isEmpty()) {
          if (account < 0) {
            path = "imported";
          } else {
            const bool p2mrPrefix = address.startsWith("din1r") || address.startsWith("tdin1r") || address.startsWith("rdin1r");
            const bool taprootPrefix = address.startsWith("din1p") || address.startsWith("tdin1p") || address.startsWith("rdin1p");
            int purpose;
            if (type == "p2mr" || p2mrPrefix) {
              purpose = 88;  // P2MR (ML-DSA-65)
            } else if (type == "p2tr" || type == "taproot" || taprootPrefix) {
              purpose = 86;  // Taproot
            } else {
              purpose = 84;  // legacy fallback
            }
            path = QString("m/%1'/1448'/%2'/%3/%4").arg(purpose).arg(account).arg(change).arg(index);
          }
        }

        int row = tblAddresses_->rowCount();
        tblAddresses_->insertRow(row);

        // Column 0: Index
        auto *indexItem = new QTableWidgetItem(QString::number(index));
        indexItem->setTextAlignment(Qt::AlignCenter);
        tblAddresses_->setItem(row, 0, indexItem);

        // Column 1: Address
        tblAddresses_->setItem(row, 1, new QTableWidgetItem(address));

        // Column 2: Label
        auto *labelItem = new QTableWidgetItem(label);
        labelItem->setForeground(label.isEmpty() ? QBrush(QColor("#868e96")) : QBrush(QColor("#c6ced8")));
        if (label.isEmpty()) {
          labelItem->setText("(no label)");
          labelItem->setToolTip("Double-click to add a label");
        }
        tblAddresses_->setItem(row, 2, labelItem);

        // Column 3: Balance
        auto *balanceItem = new QTableWidgetItem(QString("%1").arg(balance, 0, 'f', 8));
        balanceItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (balance > 0) {
          balanceItem->setForeground(QBrush(QColor("#d6dde6"))); // Active balance
          QFont font = balanceItem->font();
          font.setBold(true);
          balanceItem->setFont(font);
        } else {
          balanceItem->setForeground(QBrush(QColor("#868e96"))); // Gray
        }
        tblAddresses_->setItem(row, 3, balanceItem);

        // Column 4: Path
        auto *pathItem = new QTableWidgetItem(path);
        pathItem->setForeground(QBrush(QColor("#868e96")));
        tblAddresses_->setItem(row, 4, pathItem);

        // Column 5: Copy button
        auto *btnCopy = new QPushButton("📋 Copy");
        connect(btnCopy, &QPushButton::clicked, [address]() {
          QApplication::clipboard()->setText(address);
        });
        tblAddresses_->setCellWidget(row, 5, btnCopy);
      }

      tblAddresses_->blockSignals(false);  // Re-enable itemChanged signals
      tblAddresses_->setSortingEnabled(true);
      tblAddresses_->sortByColumn(0, Qt::AscendingOrder); // Sort by index

      qDebug() << "Loaded" << addresses.size() << "addresses with balances";
    }
  }
}

void MainWindow::onRpcError(const QString& method, int code, const QString& message) {
  if (!rpc_) return;  // Guard: shutting down

  if (method == "wallet.listunspent") {
    utxoRequestPending_ = false;
  }

  if (method == "network.setonionservice" || method == "network.setrelayservice" ||
      method == "network.getrelayservice") {
    const QString lowerMessage = message.toLower();
    const bool unsupported = code == -32601 || lowerMessage.contains("unknown method") ||
                             (lowerMessage.contains("method") && lowerMessage.contains("not found"));
    if (unsupported && method != "network.setonionservice") {
      overviewRelayRpcSupported_ = false;
    }
    if (overviewConnectivityCard_) {
      if (method == "network.setonionservice") {
        overviewConnectivityCard_->setTorActionError(unsupported);
      } else {
        overviewConnectivityCard_->setRelayActionError(unsupported);
      }
    }
    qWarning() << "Overview network control RPC failed:" << method << code;
    return;
  }

  // ALWAYS log errors to console so user can see them
  qDebug() << "🔴 RPC Error:" << method << "Code:" << code << "Message:" << message;

  // Compatibility: some deployed daemons may not expose wallet.getreorginfo yet.
  // Disable this optional poll after first "method not found" to avoid refresh spam.
  if (method == "wallet.getreorginfo") {
    const QString lowerMessage = message.toLower();
    const bool methodMissing =
      (code == -32601) ||
      lowerMessage.contains("unknown method") ||
      (lowerMessage.contains("method") && lowerMessage.contains("not found"));
    if (methodMissing) {
      if (walletReorgInfoSupported_) {
        walletReorgInfoSupported_ = false;
        qWarning() << "wallet.getreorginfo unsupported by daemon; disabling periodic reorg probe";
      }
      return;
    }
  }

  if (shouldIgnoreWalletScopedResult(method)) {
    qDebug() << "Ignoring wallet-scoped RPC error during wallet switch:" << method
             << "pending=" << pendingWalletOpenName_;
    return;
  }

  if (handleExplorerRpcError(method, code, message)) {
    return;
  }

  // ALWAYS show error in dedicated error bar at bottom (doesn't overlap connection status)
  lblErrorMessage_->setText(QString("⚠️ Error: %1 failed - %2 (code: %3)").arg(method).arg(message).arg(code));
  lblErrorMessage_->setVisible(true);

  // Keep send UI usable after RPC failures.
  if (method == "wallet.sendtoaddress" || method == "sendtoaddress" ||
      method == "sendpubliccovenant" || method == "wallet.sendpubliccovenant" ||
      method == "wallet.transfer" || method == "wallet.unshield" || method == "wallet.shield" ||
      method == "wallet.sendrawtransaction" || method == "sendrawtransaction") {
    if (btnSend_) {
      btnSend_->setEnabled(true);
      updateSendModeUi();
    }
    if (lblSendStatus_) {
      if (isInputUtxoMissingError(message)) {
        handleSpendInputMissing(message);
      } else {
        lblSendStatus_->setText(QString("❌ Error: %1").arg(message));
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
      }
    }
  }

  if (method == "wallet.consolidate") {
    pendingConsolidateParams_ = {};
    // Re-enable the consolidate button on error
    if (btnConsolidate_) {
      btnConsolidate_->setEnabled(true);
      if (cachedUtxoCount_ > 50) {
        btnConsolidate_->setText(QString("\xF0\x9F\xA7\xB9 Consolidate (%1 UTXOs)").arg(cachedUtxoCount_));
      }
    }
    QMessageBox::warning(this, "Consolidation Failed",
      QString("Could not consolidate UTXOs.\n\n%1").arg(message));
    return;
  }

  if (method == "wallet.open") {
    const QString lowerMessage = message.toLower();
    const bool methodMissing =
      (code == -32601) ||
      lowerMessage.contains("unknown method") ||
      (lowerMessage.contains("method") && lowerMessage.contains("not found"));

    // Compatibility fallback for daemons that expose wallet.load instead of wallet.open.
    if (methodMissing && !pendingWalletOpenName_.isEmpty()) {
      const QString walletName = pendingWalletOpenName_;
      qWarning() << "wallet.open unavailable, retrying wallet.load for" << walletName;
      lblConnectionStatus_->setText("Retrying wallet load");
      lblConnectionStatus_->setStyleSheet(headerPillStyle());
      lblConnectionStatus_->setToolTip(QString("Retrying wallet: %1").arg(walletName));
      rpc_->callNamed("wallet.load", QJsonObject{{"name", walletName}});
      return;
    }
  }

  if (method == "wallet.open" || method == "wallet.load") {
    walletSwitchInFlight_ = false;
    pendingWalletOpenName_.clear();
    updateWalletSwitcherState();
    if (!currentWalletName_.isEmpty()) {
      checkRescanStatus();
      rpc_->getBalance();
      rpc_->callNamed("wallet.listaddresses", QJsonObject{{"count", 200}});
      rpc_->call("wallet.listunspent", QJsonArray());
      loadTransactionHistory();
      rpc_->call("wallet.getviewkeyinfo", QJsonArray());
    }
    QMessageBox::warning(this, "Wallet Load Failed",
      QString("Could not load selected wallet.\n\n%1").arg(message));
    return;
  }

  // Deep reorg safe mode can temporarily block rescans; keep trying automatically.
  if (method == "wallet.rescanblockchain" && isRescanSafeModeError(message)) {
    walletRescanning_ = true;
    updateWalletUIState();
    scheduleSafeModeRescanRetry(message);
    return;
  }

  // Don't update connection status here - let ConnectionManager handle it
  // ConnectionManager provides more accurate and consistent state updates

  // Suppress error DIALOGS during daemon startup (but error bar still visible)
  if (suppressErrorDialogs_) {
    qDebug() << "  (Dialog suppressed during daemon startup)";
    return;
  }

  // Handle Taproot-required errors from mining RPCs
  if ((method == "mining.setaddress" || method == "mining.start" || method == "generatetoaddress") &&
      (message.contains("TAPROOT_REQUIRED", Qt::CaseInsensitive) ||
       message.contains("Taproot", Qt::CaseInsensitive) ||
       message.contains("P2TR", Qt::CaseInsensitive) ||
       code == -5)) {
    QMessageBox::warning(this, "Mining Address Not Eligible",
      QString("<h3>Mining Requires a Taproot or P2MR Address</h3>"
              "<p>%1</p>"
              "<hr>"
              "<p><b>Solution:</b></p>"
              "<ol>"
              "<li>Go to Receive tab</li>"
              "<li>Generate a Taproot (din1p...) or Quantum-Safe P2MR (din1r...) address</li>"
              "<li>Return to Mining and click 'Use Wallet'</li>"
              "</ol>").arg(message));

    // Reset mining UI state
    isMining_ = false;
    activeMinerType_ = "none";
    setMiningModeControlsLocked(false);
    setMiningOutputCinematicEnabled(false);
    if (btnStartMining_) btnStartMining_->setEnabled(true);
    if (btnStopMining_) btnStopMining_->setEnabled(false);
    if (btnStartMining_) {
      btnStartMining_->setText("Start Mining");
      btnStartMining_->setStyleSheet(headerButtonStyle());
      btnStartMining_->setToolTip("Click to start mining");
    }
    if (lblMiningStatus_) {
      lblMiningStatus_->setText(miningStatusInactiveText() + " | Taproot address required");
      lblMiningStatus_->setStyleSheet(chromePillStyle());
    }
    return;
  }

  // DISABLED: First-run daemon start dialog (user reported as annoying/duplicate)
  // User should use the "▶️ Start Daemon" toolbar button instead
  if (false) {  // Permanently disabled - keep only toolbar button
    static bool shownFirstRunDialog = false;
    static bool showingDialog = false;

    if (showingDialog) {
      return;
    }

    if (!shownFirstRunDialog &&
        (message.contains("Connection refused", Qt::CaseInsensitive) ||
         message.contains("Unauthorized", Qt::CaseInsensitive) ||
         message.contains("cookie", Qt::CaseInsensitive))) {

    shownFirstRunDialog = true;
    showingDialog = true; // Block re-entry

    // Show friendly first-run dialog
    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Welcome to Dinero");
    msgBox.setIcon(QMessageBox::Information);
    msgBox.setText("<h2>🚀 Welcome to Dinero!</h2>");
    msgBox.setInformativeText(
      "<p>The Dinero daemon (dinerod) is not running yet.</p>"
      "<p><b>Would you like to start it now?</b></p>"
      "<p style='font-size: 11px; color: #666;'>"
      "The daemon syncs the blockchain and processes transactions. "
      "It runs in the background and the GUI connects to it via RPC.</p>"
    );

    QPushButton *startBtn = msgBox.addButton("Start Daemon", QMessageBox::AcceptRole);
    QPushButton *cancelBtn = msgBox.addButton("Not Now", QMessageBox::RejectRole);
    msgBox.setDefaultButton(startBtn);

    msgBox.exec();

    if (msgBox.clickedButton() == startBtn) {
      // Start daemon in background
      QString appDir = QCoreApplication::applicationDirPath();
      QString daemonPath;

#if defined(Q_OS_MAC)
      // Priority 1: Bundled daemon in Resources (production)
      daemonPath = QDir(appDir).absoluteFilePath("../Resources/dinerod");

      // Priority 2: Same directory as GUI (development)
      if (!QFile::exists(daemonPath)) {
        daemonPath = QDir(appDir).absoluteFilePath("dinerod");
      }

      // Priority 3: Parent build directory (build/gui/dinero-qt -> build/dinerod)
      if (!QFile::exists(daemonPath)) {
        daemonPath = QDir(appDir).absoluteFilePath("../dinerod");
      }

      // Priority 4: Go up from app bundle (build/gui/dinero-qt -> ../../dinerod)
      if (!QFile::exists(daemonPath)) {
        daemonPath = QDir(appDir).absoluteFilePath("../../dinerod");
      }

      // Priority 5: Try system PATH
      if (!QFile::exists(daemonPath)) {
        daemonPath = QStandardPaths::findExecutable("dinerod");
      }
#elif defined(Q_OS_WIN)
      // Priority 1: Bundled daemon
      daemonPath = QDir(appDir).absoluteFilePath("dinerod.exe");

      // Priority 2: Parent build directory
      if (!QFile::exists(daemonPath)) {
        daemonPath = QDir(appDir).absoluteFilePath("../dinerod.exe");
      }

      // Priority 3: System PATH
      if (!QFile::exists(daemonPath)) {
        daemonPath = QStandardPaths::findExecutable("dinerod");
      }
#else
      // Priority 1: Bundled daemon
      daemonPath = QDir(appDir).absoluteFilePath("dinerod");

      // Priority 2: Parent build directory
      if (!QFile::exists(daemonPath)) {
        daemonPath = QDir(appDir).absoluteFilePath("../dinerod");
      }

      // Priority 3: System PATH
      if (!QFile::exists(daemonPath)) {
        daemonPath = QStandardPaths::findExecutable("dinerod");
      }
#endif

      if (QFile::exists(daemonPath)) {
        qDebug() << "✅ Found daemon at:" << daemonPath;

        // Store daemon process so we can shut it down cleanly on exit
        daemonProcess_ = new QProcess(this);

        // CRITICAL: use the same datadir that RpcClient expects.
        QString datadir = rpc_ ? rpc_->datadir() : QString();
        if (datadir.trimmed().isEmpty()) {
            datadir = defaultDineroDataDir();
        }

        qDebug() << "🗂️ Using datadir:" << datadir;

        daemonProcess_->setProgram(daemonPath);
        daemonProcess_->setArguments(QStringList()
          << QString("--datadir=%1").arg(datadir)
          << QString("--embedded-parent-pid=%1").arg(QCoreApplication::applicationPid()));

        qDebug() << "🚀 Starting embedded daemon:" << daemonPath
                 << "--datadir=" << datadir
                 << "--embedded-parent-pid=" << QCoreApplication::applicationPid();

        connect(daemonProcess_, &QProcess::errorOccurred, [this](QProcess::ProcessError error) {
          qWarning() << "❌ Daemon process error:" << error;
          lblConnectionStatus_->setText("Daemon start failed");
          lblConnectionStatus_->setStyleSheet(headerPillStyle());
          // Don't show dialog - would create nested dialog crash
        });

        daemonProcess_->start();

        if (!daemonProcess_->waitForStarted(5000)) {
          qWarning() << "❌ Daemon failed to start!";
          lblConnectionStatus_->setText("Daemon start failed");
          lblConnectionStatus_->setStyleSheet(headerPillStyle());
          // Don't show dialog - would create nested dialog crash
          // Error is visible in status bar and user can see terminal output
          showingDialog = false;
          return;
        }

        qDebug() << "✅ Daemon process started successfully, PID:" << daemonProcess_->processId();

        lblConnectionStatus_->setText("Starting daemon...");
        lblConnectionStatus_->setStyleSheet(headerPillStyle());

        // Give daemon time to start and write cookie, then try connecting
        QTimer::singleShot(5000, [this]() {
          qDebug() << "⏰ Attempting to load cookie and connect...";
          rpc_->loadCookie();
          refresh();
          lblConnectionStatus_->setText("Connecting...");
        });
      } else {
        QString appDir = QCoreApplication::applicationDirPath();
        QMessageBox::warning(this, "Daemon Not Found",
          QString("Could not find dinerod.\n\n"
                  "Searched locations:\n"
                  "• %1\n"
                  "• System PATH\n\n"
                  "Please start the daemon manually or install it to your system PATH.\n\n"
                  "You can set DINERO_DAEMON_PATH environment variable to specify the location.").arg(daemonPath));
        lblConnectionStatus_->setText("Daemon not found");
        lblConnectionStatus_->setStyleSheet(headerPillStyle());
      }
    } else {
      lblConnectionStatus_->setText("Daemon not running");
      lblConnectionStatus_->setStyleSheet(headerPillStyle());
    }
    showingDialog = false; // Reset flag before returning
    return;
    }  // End of disabled first-run dialog (if false)
  }

  // Don't update connection status here - let ConnectionManager handle it
  // ConnectionManager provides more accurate and consistent state updates
  // The error is already shown in lblErrorMessage_ at the bottom
}

void MainWindow::updateStatus(const QJsonObject& info) {
  lblConnectionStatus_->setText("Connected");
  lblConnectionStatus_->setStyleSheet(headerPillStyle());
  lblConnectionStatus_->setToolTip("Connected to local daemon.");

  // Clear any previous error messages on successful update
  lblErrorMessage_->setVisible(false);

  // Display network chain and connection info
  QString chain = info["chain"].toString("unknown");

  // Extract RPC/WS ports from current connections
  QString rpcServer = rpc_->currentServer();  // e.g., "http://127.0.0.1:20998/"
#ifdef DIN_EXPERIMENTAL_FEATURES
  QString wsServer = ws_->serverUrl();         // e.g., "ws://127.0.0.1:21001"
#else
  QString wsServer = "ws://disabled";          // WebSockets disabled in production
#endif

  // Parse ports
  QUrl rpcUrl(rpcServer);
  QUrl wsUrl(wsServer);
  int rpcPort = rpcUrl.port(20998);
  int wsPort = wsUrl.port(21001);

  lblNetworkInfo_->setText(QString("%1 | RPC %2 | WS %3")
    .arg(chain.toUpper())
    .arg(rpcPort)
    .arg(wsPort));
  lblNetworkInfo_->setStyleSheet(headerPillStyle());
  lblNetworkInfo_->setToolTip(QString("Chain: %1\nRPC: %2\nWS: %3").arg(chain, QString::number(rpcPort), QString::number(wsPort)));

  const int blocks = info.value("blocks").toInt(-1);
  const int headers = info.contains("headers") ? info.value("headers").toInt(blocks) : -1;
  if (blocks >= 0) {
    lblHeight_->setText(QString("Height: %1").arg(blocks));
    if (lblExplorerHeight_) {
      lblExplorerHeight_->setText(explorerIntegerText(blocks));
    }
  }
  if (headers >= 0) {
    lblHeaders_->setText(QString("Headers: %1").arg(headers));

    // Fallback sync status when wallet.getsyncstatus is unavailable.
    if (headers > blocks && blocks >= 0) {
      double progress = (blocks * 100.0) / headers;
      lblSyncProgress_->setText(QString("⏬ Syncing: %1% (%2 / %3)")
        .arg(progress, 0, 'f', 1)
        .arg(blocks)
        .arg(headers));
      lblSyncProgress_->setStyleSheet("QLabel { color: #d0d7df; font-weight: 600; background: #262b32; border: 1px solid #373d46; border-radius: 6px; padding: 5px; }");
    } else if (headers == blocks && blocks > 0) {
      lblSyncProgress_->setText("✅ Fully synced!");
      lblSyncProgress_->setStyleSheet("QLabel { color: #e1e6ec; font-weight: 600; background: #2b3037; border: 1px solid #3a4048; border-radius: 6px; padding: 5px; }");
    }
  }

  // Do not force connection count from blockchain.getinfo when field is absent.
  // Connections are sourced from getpeerinfo for consistency.
  if (info.contains("connections")) {
    lblConnections_->setText(QString("Connections: %1").arg(info["connections"].toInt()));
  }
  if (lblExplorerDifficulty_) {
    bool ok = false;
    const double difficulty = explorerJsonDouble(info.value("difficulty"), &ok);
    lblExplorerDifficulty_->setText(ok ? QLocale().toString(difficulty, 'f', 3) : "-");
  }
  if (lblExplorerSupply_) {
    const QString supply = explorerJsonString(info.value("moneysupply"));
    if (!supply.isEmpty()) {
      lblExplorerSupply_->setText(explorerDinString(supply) + " DIN");
    }
  }
}

void MainWindow::updateEconomics(const QJsonObject& economics) {
  // Update Overview tab labels
  if (lblPhase_) {
    // Use halving epoch instead of non-existent "phase"
    if (economics.contains("current_halving_epoch")) {
      lblPhase_->setText(QString("Halving Epoch: %1").arg(economics["current_halving_epoch"].toInt()));
    } else {
      lblPhase_->setVisible(false); // Hide if not available
    }
  }
  if (lblReward_) {
    lblReward_->setText(QString("Next Reward: %1 DIN").arg(economics["next_block_reward_din"].toString()));
  }
  if (lblSupply_ && economics.contains("current_supply_din")) {
    const QString supply = overviewDinText(economics.value("current_supply_din"));
    if (!supply.isEmpty()) {
      lblSupply_->setText(QString("Supply: %1 DIN").arg(supply));
      lblSupply_->setToolTip(
          QString("Current issued supply from economics.getinfo.\nMonetary policy: %1")
              .arg(economics.value("monetary_policy").toString("PoW mining")));
    }
  }

  // Update mining tab labels (if mining tab exists)
  if (lblMiningPhase_) {
    // Use halving epoch instead of non-existent "phase"
    if (economics.contains("current_halving_epoch")) {
      lblMiningPhase_->setText(QString("Halving Epoch: %1").arg(economics["current_halving_epoch"].toInt()));
    } else {
      lblMiningPhase_->setVisible(false);
    }
  }
  if (lblNextReward_) {
    lblNextReward_->setText(QString("Next Reward: %1 DIN").arg(economics["next_block_reward_din"].toString()));
  }
  if (txtMiningInfo_) {
    QJsonDocument doc(economics);
    txtMiningInfo_->setText(doc.toJson(QJsonDocument::Indented));
  }
}

int MainWindow::livePeerCountForStatusStrip() const {
  bool ok = false;

  if (lblConnections_) {
    const QString text = lblConnections_->text();
    const int count = text.section(':', 1).trimmed().section(' ', 0, 0).toInt(&ok);
    if (ok) {
      return count;
    }
  }

  if (lblPeersCount_) {
    const int count = lblPeersCount_->text().section(' ', 0, 0).toInt(&ok);
    if (ok) {
      return count;
    }
  }

  if (tblPeers_) {
    return tblPeers_->rowCount();
  }

  return cachedPeerCount_;
}

void MainWindow::refreshAiStatusStrip() {
  if (!aiStatusStrip_) {
    return;
  }

  aiStatusStrip_->updateStatus(cachedHeight_, cachedHeaders_, livePeerCountForStatusStrip(),
                               cachedBalance_, cachedMiningActive_, cachedHashrate_,
                               cachedBridgeActive_, cachedProofCacheEntries_);
}

void MainWindow::updateBridgeTab(const QJsonObject& stats) {
  const bool active = stats["bridge_enabled"].toBool();
  const qint64 proofRequestsTotal = stats["proof_requests_total"].toVariant().toLongLong();
  const qint64 proofRequestsRejected = stats["proof_requests_rejected"].toVariant().toLongLong();
  const qint64 proofRequestsCoalesced = stats["proof_requests_coalesced"].toVariant().toLongLong();
  const qint64 proofQueueDepth = stats["proof_queue_depth"].toVariant().toLongLong();
  const qint64 proofQueueCapacity = stats["proof_queue_capacity"].toVariant().toLongLong();
  const qint64 activeGenerations = stats["active_generations"].toVariant().toLongLong();
  const qint64 blockCacheEntries = stats["block_cache_entries"].toVariant().toLongLong();
  const qint64 txCacheEntries = stats["tx_cache_entries"].toVariant().toLongLong();
  const qint64 indexedHeights = stats["indexed_heights"].toVariant().toLongLong();
  const qint64 indexedBlocks = stats["indexed_blocks"].toVariant().toLongLong();
  const qint64 evictions = stats["evictions"].toVariant().toLongLong();
  const double hitRate = stats["hit_rate"].toDouble();
  const double proofP50 = stats["proof_generation_p50_ms"].toDouble();
  const double proofP95 = stats["proof_generation_p95_ms"].toDouble();
  const double queueP50 = stats["queue_wait_p50_ms"].toDouble();
  const double queueP95 = stats["queue_wait_p95_ms"].toDouble();
  const bool busy = proofQueueDepth > 0 || activeGenerations > 0;
  const bool backpressure = proofRequestsRejected > 0 || (proofQueueCapacity > 0 && proofQueueDepth >= proofQueueCapacity);
  const bool hasTraffic = proofRequestsTotal > 0 || blockCacheEntries > 0 || txCacheEntries > 0;

  if (lblBridgeStatus_) {
    if (!active) {
      lblBridgeStatus_->setText("Disabled");
      lblBridgeStatus_->setStyleSheet("QLabel { color: #868e96; font-size: 13px; font-weight: bold; }");
    } else if (backpressure) {
      lblBridgeStatus_->setText("Enabled / Backpressure");
      lblBridgeStatus_->setStyleSheet("QLabel { color: #ff922b; font-size: 13px; font-weight: bold; }");
    } else if (busy) {
      lblBridgeStatus_->setText("Enabled / Serving");
      lblBridgeStatus_->setStyleSheet("QLabel { color: #51cf66; font-size: 13px; font-weight: bold; }");
    } else if (hasTraffic) {
      lblBridgeStatus_->setText("Enabled / Warm");
      lblBridgeStatus_->setStyleSheet("QLabel { color: #74c0fc; font-size: 13px; font-weight: bold; }");
    } else {
      lblBridgeStatus_->setText("Enabled / Idle");
      lblBridgeStatus_->setStyleSheet("QLabel { color: #74c0fc; font-size: 13px; font-weight: bold; }");
    }
  }

  if (lblBridgeSummary_) {
    QString summary;
    QString style =
        "QLabel { background: #232930; color: #cdd6e0; border: 1px solid #353c46; "
        "padding: 10px 12px; border-radius: 8px; font-size: 12px; }";
    if (!active) {
      summary = "Utreexo proof serving is disabled in this daemon. This node can still validate locally, but it is not advertising proof-serving capacity to stateless peers.";
      style =
          "QLabel { background: #2a2e35; color: #adb5bd; border: 1px solid #3b414b; "
          "padding: 10px 12px; border-radius: 8px; font-size: 12px; }";
    } else if (!hasTraffic) {
      summary = "Proof serving is enabled and ready. No proof requests have been served in this process yet, so zero counters here mean idle traffic, not a broken wallet.";
    } else if (backpressure) {
      summary = QString("Proof serving has seen backpressure: %1 request(s) rejected with queue depth %2/%3.")
          .arg(QLocale().toString(proofRequestsRejected))
          .arg(QLocale().toString(proofQueueDepth))
          .arg(QLocale().toString(proofQueueCapacity));
      style =
          "QLabel { background: #33270f; color: #ffd8a8; border: 1px solid #7a5a1f; "
          "padding: 10px 12px; border-radius: 8px; font-size: 12px; }";
    } else if (busy) {
      summary = QString("Proof service is actively working: queue %1/%2, active generations %3, P50 generation latency %4 ms.")
          .arg(QLocale().toString(proofQueueDepth))
          .arg(QLocale().toString(proofQueueCapacity))
          .arg(QLocale().toString(activeGenerations))
          .arg(QString::number(proofP50, 'f', 1));
    } else {
      summary = QString("Proof service is warm and idle: %1 request(s), %2 cached proof entries, hit rate %3%.")
          .arg(QLocale().toString(proofRequestsTotal))
          .arg(QLocale().toString(blockCacheEntries + txCacheEntries))
          .arg(QString::number(hitRate * 100.0, 'f', 1));
    }
    lblBridgeSummary_->setText(summary);
    lblBridgeSummary_->setStyleSheet(style);
  }

  auto fmtInt = [&](const QString& key) {
    return QLocale().toString(stats[key].toVariant().toLongLong());
  };

  if (lblBridgeRequests_)
    lblBridgeRequests_->setText(QString("%1 total, %2 coalesced, %3 rejected")
        .arg(fmtInt("proof_requests_total"))
        .arg(fmtInt("proof_requests_coalesced"))
        .arg(fmtInt("proof_requests_rejected")));
  if (lblBridgeQueue_)
    lblBridgeQueue_->setText(QString("%1 / %2")
        .arg(fmtInt("proof_queue_depth"))
        .arg(fmtInt("proof_queue_capacity")));
  if (lblBridgeCacheHits_)
    lblBridgeCacheHits_->setText(fmtInt("hits"));
  if (lblBridgeCacheMisses_)
    lblBridgeCacheMisses_->setText(fmtInt("misses"));
  if (lblBridgeHitRate_)
    lblBridgeHitRate_->setText(QString("%1%").arg(hitRate * 100.0, 0, 'f', 1));
  if (lblBridgeBlockEntries_)
    lblBridgeBlockEntries_->setText(QString("%1 / %2").arg(fmtInt("block_cache_entries")).arg(fmtInt("block_cache_capacity")));
  if (lblBridgeTxEntries_)
    lblBridgeTxEntries_->setText(QString("%1 / %2").arg(fmtInt("tx_cache_entries")).arg(fmtInt("tx_cache_capacity")));
  if (lblBridgeIndexed_)
    lblBridgeIndexed_->setText(QString("%1 heights, %2 blocks")
        .arg(fmtInt("indexed_heights"))
        .arg(fmtInt("indexed_blocks")));
  if (lblBridgeEvictions_)
    lblBridgeEvictions_->setText(QString("%1 evictions, TTL %2s / %3s")
        .arg(fmtInt("evictions"))
        .arg(fmtInt("block_cache_ttl_seconds"))
        .arg(fmtInt("tx_cache_ttl_seconds")));
  if (lblBridgeWorkers_)
    lblBridgeWorkers_->setText(fmtInt("proof_workers"));
  if (lblBridgeActiveGens_)
    lblBridgeActiveGens_->setText(fmtInt("active_generations"));
  if (lblBridgeLatency_)
    lblBridgeLatency_->setText(QString("P50: %1ms  P95: %2ms  P99: %3ms")
        .arg(proofP50, 0, 'f', 1)
        .arg(proofP95, 0, 'f', 1)
        .arg(stats["proof_generation_p99_ms"].toDouble(), 0, 'f', 1));
  if (lblBridgeQueueWait_)
    lblBridgeQueueWait_->setText(QString("P50: %1ms  P95: %2ms  P99: %3ms")
        .arg(queueP50, 0, 'f', 1)
        .arg(queueP95, 0, 'f', 1)
        .arg(stats["queue_wait_p99_ms"].toDouble(), 0, 'f', 1));
  if (lblBridgePriority_)
    lblBridgePriority_->setText(QString("tip %1/%2  recent %3/%4  historical %5/%6")
        .arg(fmtInt("tip_priority_accepted"))
        .arg(fmtInt("tip_priority_rejected"))
        .arg(fmtInt("recent_priority_accepted"))
        .arg(fmtInt("recent_priority_rejected"))
        .arg(fmtInt("historical_priority_accepted"))
        .arg(fmtInt("historical_priority_rejected")));
  if (lblBridgeTasks_)
    lblBridgeTasks_->setText(QString("%1 completed, %2 failed")
        .arg(fmtInt("proof_tasks_completed")).arg(fmtInt("proof_tasks_failed")));
}

void MainWindow::updateWallet(const QString& address) {
  edtAddress_->setText(address);
  txtValidation_->setText("New address generated!");
}

void MainWindow::setExplorerStatus(const QString& text, bool warning) {
  if (!lblExplorerStatus_) {
    return;
  }
  lblExplorerStatus_->setText(text);
  lblExplorerStatus_->setStyleSheet(warning
      ? "QLabel { color: #f2d2a0; background: #2f2820; border: 1px solid #5a4630; border-radius: 8px; padding: 8px 10px; }"
      : "QLabel { color: #aab4c2; background: #20252b; border: 1px solid #333a43; border-radius: 8px; padding: 8px 10px; }");
}

void MainWindow::resetExplorerDetailTables() {
  if (lblExplorerSummary_) {
    lblExplorerSummary_->clear();
    lblExplorerSummary_->hide();
  }
  if (tblExplorerTransactions_) {
    tblExplorerTransactions_->setRowCount(0);
    tblExplorerTransactions_->hide();
  }
  if (tblExplorerInputs_) {
    tblExplorerInputs_->setRowCount(0);
    tblExplorerInputs_->hide();
  }
  if (tblExplorerOutputs_) {
    tblExplorerOutputs_->setRowCount(0);
    tblExplorerOutputs_->hide();
  }
  if (tblExplorerUTXOs_) {
    tblExplorerUTXOs_->setRowCount(0);
    tblExplorerUTXOs_->hide();
  }
  if (txtBlockData_) {
    txtBlockData_->show();
    txtBlockData_->clear();
  }
  explorerBlockTransactionRows_.clear();
}

void MainWindow::updateExplorer(const QJsonValue& block) {
  if (block.isObject()) {
    displayExplorerBlock(block.toObject(), true);
    return;
  }

  QJsonDocument doc(QJsonObject{{"result", block}});
  txtBlockData_->setText(doc.toJson(QJsonDocument::Indented));
}

void MainWindow::updateExplorerRecentBlocks(int height) {
  if (!tblRecentBlocks_ || height < 0) {
    return;
  }
  if (explorerLatestBlocksHeight_ == height && tblRecentBlocks_->rowCount() > 0) {
    return;
  }

  explorerLatestBlocksHeight_ = height;
  pendingExplorerRecentHeights_.clear();
  explorerRecentBlockRows_.clear();
  pendingExplorerRecentHeight_ = -1;

  const int rowCount = std::min(10, height + 1);
  tblRecentBlocks_->setRowCount(rowCount);
  for (int row = 0; row < rowCount; ++row) {
    const int blockHeight = height - row;
    explorerRecentBlockRows_.insert(QString::number(blockHeight), row);
    pendingExplorerRecentHeights_.append(blockHeight);
    tblRecentBlocks_->setItem(row, 0, explorerItem(explorerIntegerText(blockHeight)));
    tblRecentBlocks_->setItem(row, 1, explorerItem("loading..."));
    tblRecentBlocks_->setItem(row, 2, explorerItem("-"));
    tblRecentBlocks_->setItem(row, 3, explorerItem("-"));
    tblRecentBlocks_->setItem(row, 4, explorerItem("-"));
  }

  requestNextExplorerRecentBlock();
}

void MainWindow::requestNextExplorerRecentBlock() {
  if (!rpc_ || pendingExplorerRecentHeight_ >= 0 || pendingExplorerRecentHeights_.isEmpty()) {
    return;
  }
  pendingExplorerRecentHeight_ = pendingExplorerRecentHeights_.takeFirst();
  rpc_->call("blockchain.getblockhash", QJsonArray{pendingExplorerRecentHeight_});
}

void MainWindow::displayExplorerBlock(const QJsonObject& block, bool mainDetail) {
  const QString hash = block.value("hash").toString();
  const qint64 height = explorerJsonInt64(block.value("height"));
  const qint64 time = explorerJsonInt64(block.value("time"));
  const QJsonArray txValues = block.value("tx").toArray(block.value("transactions").toArray());

  QStringList txids;
  for (const QJsonValue& value : txValues) {
    if (value.isString()) {
      txids.append(value.toString());
    } else if (value.isObject()) {
      const QJsonObject obj = value.toObject();
      const QString txid = obj.value("txid").toString(obj.value("hash").toString());
      if (!txid.isEmpty()) {
        txids.append(txid);
      }
    }
  }

  const int recentRow = explorerRecentBlockRows_.value(hash, -1);
  if (recentRow >= 0 && tblRecentBlocks_) {
    tblRecentBlocks_->setItem(recentRow, 0, explorerItem(explorerIntegerText(height)));
    auto* hashItem = explorerItem(explorerShortValue(hash), hash);
    hashItem->setData(Qt::UserRole, hash);
    tblRecentBlocks_->setItem(recentRow, 1, hashItem);
    tblRecentBlocks_->setItem(recentRow, 2, explorerItem(explorerRelativeTime(time), explorerFullDate(time)));
    tblRecentBlocks_->setItem(recentRow, 3, explorerItem(QString::number(txids.size())));
    tblRecentBlocks_->setItem(recentRow, 4, explorerItem(explorerJsonString(block.value("nonce"))));
  }

  if (!mainDetail) {
    requestNextExplorerRecentBlock();
    return;
  }

  resetExplorerDetailTables();

  const QString previous = block.value("previousblockhash").toString("genesis");
  const QString bits = explorerJsonString(block.value("bits"));
  const QString merkle = block.value("merkleroot").toString("-");
  const QString utreexo =
      block.value("utreexocommitment").toString(
      block.value("utreexo_commitment").toString(
      block.value("utreexoRoot").toString(
      block.value("utreexo_root").toString("-"))));

  lblExplorerSummary_->setText(
      QString("<b>Block #%1</b><br>"
              "<b>Hash:</b> %2<br>"
              "<b>Previous:</b> %3<br>"
              "<b>Time:</b> %4 (%5)<br>"
              "<b>Transactions:</b> %6 &nbsp;&nbsp; <b>Nonce:</b> %7 &nbsp;&nbsp; <b>Bits:</b> %8<br>"
              "<b>Merkle Root:</b> %9<br>"
              "<b>Utreexo Root:</b> %10")
          .arg(explorerIntegerText(height),
               hash.toHtmlEscaped(),
               previous.toHtmlEscaped(),
               explorerFullDate(time).toHtmlEscaped(),
               explorerRelativeTime(time).toHtmlEscaped())
          .arg(txids.size())
          .arg(explorerJsonString(block.value("nonce")).toHtmlEscaped(),
               bits.toHtmlEscaped(),
               merkle.toHtmlEscaped(),
               utreexo.toHtmlEscaped()));
  lblExplorerSummary_->show();

  txtBlockData_->setText(QJsonDocument(block).toJson(QJsonDocument::Indented));
  setExplorerStatus(QString("Showing block #%1 with %2 transaction%3.")
      .arg(explorerIntegerText(height))
      .arg(txids.size())
      .arg(txids.size() == 1 ? "" : "s"));

  requestExplorerBlockTransactions(block);
}

void MainWindow::requestExplorerBlockTransactions(const QJsonObject& block) {
  if (!tblExplorerTransactions_) {
    return;
  }

  const QJsonArray txValues = block.value("tx").toArray(block.value("transactions").toArray());
  const int shown = std::min<int>(20, static_cast<int>(txValues.size()));
  tblExplorerTransactions_->setRowCount(shown);
  explorerBlockTransactionRows_.clear();

  for (int row = 0; row < shown; ++row) {
    const QJsonValue value = txValues.at(row);
    QString txid;
    QJsonObject txObject;
    if (value.isString()) {
      txid = value.toString();
    } else if (value.isObject()) {
      txObject = value.toObject();
      txid = txObject.value("txid").toString(txObject.value("hash").toString());
    }

    auto* txidItem = explorerItem(explorerShortValue(txid), txid);
    txidItem->setData(Qt::UserRole, txid);
    tblExplorerTransactions_->setItem(row, 0, txidItem);
    tblExplorerTransactions_->setItem(row, 1, explorerItem(txObject.isEmpty() ? "loading..." : explorerTransactionType(txObject)));
    tblExplorerTransactions_->setItem(row, 2, explorerItem(txObject.isEmpty() ? "-" : QString::number(txObject.value("inputs").toArray(txObject.value("vin").toArray()).size())));
    tblExplorerTransactions_->setItem(row, 3, explorerItem(txObject.isEmpty() ? "-" : QString::number(txObject.value("outputs").toArray(txObject.value("vout").toArray()).size())));
    tblExplorerTransactions_->setItem(row, 4, explorerItem("-"));

    if (!txid.isEmpty()) {
      explorerBlockTransactionRows_.insert(txid, row);
      if (txObject.isEmpty()) {
        rpc_->call("gettransaction", QJsonArray{txid});
      } else {
        displayExplorerTransaction(txObject);
      }
    }
  }

  tblExplorerTransactions_->show();
  if (txValues.size() > shown) {
    setExplorerStatus(QString("Showing first %1 of %2 block transactions. Double-click a row for full detail.")
        .arg(shown)
        .arg(txValues.size()));
  }
}

void MainWindow::displayExplorerTransaction(const QJsonObject& tx) {
  const QString txid = tx.value("txid").toString(tx.value("hash").toString(pendingExplorerTxLookup_));
  const QJsonArray inputs = tx.value("inputs").toArray(tx.value("vin").toArray());
  const QJsonArray outputs = tx.value("outputs").toArray(tx.value("vout").toArray());

  qint64 outputTotalUna = 0;
  bool hasOutputUna = false;
  for (const QJsonValue& value : outputs) {
    qint64 una = 0;
    explorerOutputAmount(value.toObject(), &una);
    if (una > 0) {
      outputTotalUna += una;
      hasOutputUna = true;
    }
  }

  QString totalOutput = tx.value("total_output_value_din").isUndefined()
      ? QString()
      : explorerDinString(explorerJsonString(tx.value("total_output_value_din"))) + " DIN";
  if (totalOutput.isEmpty() || totalOutput == "- DIN") {
    bool ok = false;
    const qint64 totalUna = explorerJsonInt64(tx.value("total_output_value"), &ok);
    totalOutput = ok ? explorerDinFromUna(totalUna) + " DIN" :
        hasOutputUna ? explorerDinFromUna(outputTotalUna) + " DIN" : "-";
  }

  const int row = explorerBlockTransactionRows_.value(txid, -1);
  if (row >= 0 && tblExplorerTransactions_) {
    auto* txidItem = explorerItem(explorerShortValue(txid), txid);
    txidItem->setData(Qt::UserRole, txid);
    tblExplorerTransactions_->setItem(row, 0, txidItem);
    tblExplorerTransactions_->setItem(row, 1, explorerItem(explorerTransactionType(tx)));
    tblExplorerTransactions_->setItem(row, 2, explorerItem(QString::number(inputs.size())));
    tblExplorerTransactions_->setItem(row, 3, explorerItem(QString::number(outputs.size())));
    tblExplorerTransactions_->setItem(row, 4, explorerItem(totalOutput));
  }

  const bool isMainLookup =
      !pendingExplorerTxLookup_.isEmpty() &&
      (txid.isEmpty() || txid.compare(pendingExplorerTxLookup_, Qt::CaseInsensitive) == 0);
  if (!isMainLookup) {
    return;
  }

  resetExplorerDetailTables();
  pendingExplorerTxLookup_.clear();
  pendingExplorerTxFallbackBlockHash_.clear();

  const QString type = explorerTransactionType(tx);
  const QString blockHash = tx.value("blockhash").toString();
  const qint64 blockHeight = explorerJsonInt64(tx.value("blockheight").isUndefined()
      ? tx.value("height")
      : tx.value("blockheight"));
  const qint64 confirmations = explorerJsonInt64(tx.value("confirmations"));
  const QString status = tx.value("status").toString(confirmations > 0 ? "Confirmed" : "Pending");

  lblExplorerSummary_->setText(
      QString("<b>%1 Transaction</b><br>"
              "<b>TXID:</b> %2<br>"
              "<b>Block:</b> %3<br>"
              "<b>Confirmations:</b> %4 &nbsp;&nbsp; <b>Status:</b> %5<br>"
              "<b>Version:</b> %6 &nbsp;&nbsp; <b>Type:</b> %7<br>"
              "<b>Total Output:</b> %8")
          .arg(type.toHtmlEscaped(),
               txid.toHtmlEscaped(),
               (blockHash.isEmpty()
                    ? QStringLiteral("unconfirmed")
                    : QString("#%1 %2").arg(explorerIntegerText(blockHeight), explorerShortValue(blockHash))).toHtmlEscaped(),
               explorerIntegerText(confirmations),
               status.toHtmlEscaped(),
               explorerJsonString(tx.value("version")).toHtmlEscaped(),
               tx.value("classification").toString(type).toHtmlEscaped(),
               totalOutput.toHtmlEscaped()));
  lblExplorerSummary_->show();
  txtBlockData_->setText(QJsonDocument(tx).toJson(QJsonDocument::Indented));

  tblExplorerInputs_->setRowCount(inputs.isEmpty() && tx.value("is_coinbase").toBool(false) ? 1 : inputs.size());
  if (inputs.isEmpty() && tx.value("is_coinbase").toBool(false)) {
    tblExplorerInputs_->setItem(0, 0, explorerItem("Coinbase"));
    tblExplorerInputs_->setItem(0, 1, explorerItem("New coins"));
    tblExplorerInputs_->setItem(0, 2, explorerItem("-"));
  } else {
    for (int row = 0; row < inputs.size(); ++row) {
      const QJsonObject input = inputs.at(row).toObject();
      const QString prevTx = input.value("prevout_txid").toString(
          input.value("prev_txid").toString(input.value("txid").toString()));
      const QString prevVout = explorerJsonString(input.value("prevout_vout").isUndefined()
          ? input.value("vout")
          : input.value("prevout_vout"));
      const bool isPrivate = input.value("is_private").toBool(false) || input.contains("ring_size");
      tblExplorerInputs_->setItem(row, 0, explorerItem(prevTx.isEmpty() ? "unknown" : explorerShortValue(prevTx) + ":" + (prevVout.isEmpty() ? "0" : prevVout), prevTx));
      tblExplorerInputs_->setItem(row, 1, explorerItem(isPrivate ? "Private input" : "Transparent"));
      tblExplorerInputs_->setItem(row, 2, explorerItem(isPrivate ? input.value("ring_size").toVariant().toString() : "-"));
    }
  }
  tblExplorerInputs_->show();

  tblExplorerOutputs_->setRowCount(outputs.size());
  for (int row = 0; row < outputs.size(); ++row) {
    const QJsonObject output = outputs.at(row).toObject();
    const QString address = explorerOutputAddress(output);
    const QString script = explorerScriptPubKey(output);
    const QString display = !address.isEmpty()
        ? explorerShortValue(address, 22)
        : script.startsWith("6a", Qt::CaseInsensitive) ? QStringLiteral("OP_RETURN") : explorerShortValue(script, 14);
    tblExplorerOutputs_->setItem(row, 0, explorerItem(QString::number(row)));
    tblExplorerOutputs_->setItem(row, 1, explorerItem(explorerOutputType(output)));
    tblExplorerOutputs_->setItem(row, 2, explorerItem(display, address.isEmpty() ? script : address));
    tblExplorerOutputs_->setItem(row, 3, explorerItem(explorerOutputAmount(output)));
  }
  tblExplorerOutputs_->show();
  setExplorerStatus(QString("Showing transaction %1.").arg(explorerShortValue(txid)));
}

void MainWindow::displayAddressResult(const QJsonObject& result) {
  // Check for error
  if (result.contains("error")) {
    txtBlockData_->setText("Error: " + result["error"].toObject()["message"].toString());
    return;
  }

  QString address = result["address"].toString();
  QString totalDin = result["total_din"].toString();
  int utxoCount = result["total_count"].toInt();
  int height = result["height"].toInt();

  lblExplorerSummary_->setText(
    QString("<b>%1</b><br>"
            "<b>Address:</b> %2<br>"
            "<b>Balance:</b> %3 DIN<br>"
            "<b>UTXOs:</b> %4 &nbsp;&nbsp; <b>Chain Height:</b> %5")
      .arg(explorerAddressType(address), address.toHtmlEscaped(), totalDin.toHtmlEscaped())
      .arg(utxoCount)
      .arg(height));
  lblExplorerSummary_->show();

  QJsonArray utxos = result["utxos"].toArray();
  tblExplorerUTXOs_->setRowCount(utxos.size());

  for (int i = 0; i < utxos.size(); i++) {
    QJsonObject utxo = utxos[i].toObject();
    QString txid = utxo["txid"].toString();
    int vout = utxo["vout"].toInt();
    double amount = utxo["amount"].toDouble() / 100000000.0;
    int ht = utxo["height"].toInt();
    bool coinbase = utxo["coinbase"].toBool();

    auto* txidItem = explorerItem(explorerShortValue(txid), txid);
    txidItem->setData(Qt::UserRole, txid);
    tblExplorerUTXOs_->setItem(i, 0, txidItem);
    tblExplorerUTXOs_->setItem(i, 1, explorerItem(QString("#%1").arg(ht)));
    tblExplorerUTXOs_->setItem(i, 2, explorerItem(ht > 0 ? "Confirmed" : "Pending"));
    tblExplorerUTXOs_->setItem(i, 3, explorerItem(QString::number(amount, 'f', 8) + " DIN"));
    tblExplorerUTXOs_->setItem(i, 4, explorerItem(QString("vout %1%2").arg(vout).arg(coinbase ? " coinbase" : "")));
  }

  tblExplorerUTXOs_->show();
  txtBlockData_->setText(QJsonDocument(result).toJson(QJsonDocument::Indented));
  setExplorerStatus(QString("Showing UTXO scan fallback for %1.").arg(explorerShortValue(address, 22)));
}

void MainWindow::displayExplorerAddress() {
  if (pendingExplorerAddress_.isEmpty()) {
    return;
  }

  auto payloadObject = [](const QJsonValue& value) {
    if (value.isObject()) {
      const QJsonObject obj = value.toObject();
      if (obj.value("result").isObject()) {
        return obj.value("result").toObject();
      }
      return obj;
    }
    return QJsonObject();
  };

  const QJsonObject balance = payloadObject(pendingExplorerAddressBalance_);
  QJsonValue balanceValue = balance.value("estimated_balance");
  if (balanceValue.isUndefined()) balanceValue = balance.value("balance");
  if (balanceValue.isUndefined()) balanceValue = balance.value("confirmed");
  if (balanceValue.isUndefined()) balanceValue = balance.value("total");

  QString balanceDisplay = "-";
  bool balanceOk = false;
  const QString balanceText = explorerJsonString(balanceValue);
  if (!balanceText.isEmpty() && balanceText.contains('.')) {
    balanceDisplay = explorerDinString(balanceText);
    balanceOk = true;
  } else {
    const qint64 una = explorerJsonInt64(balanceValue, &balanceOk);
    if (balanceOk) {
      balanceDisplay = explorerDinFromUna(una);
    }
  }

  QString unconfirmedDisplay;
  bool unconfirmedOk = false;
  const qint64 unconfirmed = explorerJsonInt64(balance.value("unconfirmed"), &unconfirmedOk);
  if (unconfirmedOk && unconfirmed != 0) {
    const qint64 absUnconfirmed = unconfirmed < 0 ? -unconfirmed : unconfirmed;
    unconfirmedDisplay = QString("%1%2 DIN unconfirmed")
        .arg(unconfirmed > 0 ? "+" : "-")
        .arg(explorerDinFromUna(absUnconfirmed));
  }

  QJsonArray history;
  if (pendingExplorerAddressHistory_.isArray()) {
    history = pendingExplorerAddressHistory_.toArray();
  } else if (pendingExplorerAddressHistory_.isObject()) {
    const QJsonObject obj = payloadObject(pendingExplorerAddressHistory_);
    history = obj.value("transactions").toArray();
    if (history.isEmpty()) {
      history = obj.value("txids").toArray();
    }
  }

  if (balance.isEmpty() && history.isEmpty() && !explorerAddressScantxFallbackTried_) {
    explorerAddressScantxFallbackTried_ = true;
    setExplorerStatus("Address-index RPCs were unavailable or empty; scanning UTXO set as fallback...", true);
    rpc_->call("blockchain.scantxoutset", QJsonArray{pendingExplorerAddress_});
    return;
  }

  lblExplorerSummary_->setText(
      QString("<b>%1</b><br>"
              "<b>Address:</b> %2<br>"
              "<b>Balance:</b> %3 DIN%4<br>"
              "<b>Transactions:</b> %5")
          .arg(explorerAddressType(pendingExplorerAddress_).toHtmlEscaped(),
               pendingExplorerAddress_.toHtmlEscaped(),
               balanceDisplay.toHtmlEscaped(),
               unconfirmedDisplay.isEmpty() ? QString() : "<br><b>Unconfirmed:</b> " + unconfirmedDisplay.toHtmlEscaped())
          .arg(history.size()));
  lblExplorerSummary_->show();

  tblExplorerUTXOs_->setRowCount(history.size());
  for (int row = 0; row < history.size(); ++row) {
    const QJsonValue value = history.at(row);
    QJsonObject item;
    if (value.isString()) {
      item.insert("txid", value.toString());
    } else {
      item = value.toObject();
    }
    const QString txid = item.value("txid").toString(item.value("hash").toString());
    bool hasHeight = false;
    const qint64 height = explorerJsonInt64(item.value("height").isUndefined()
        ? item.value("blockheight")
        : item.value("height"), &hasHeight);
    const bool hasDinAmount = !item.value("amount").isUndefined();
    const QJsonValue amountValue = hasDinAmount ? item.value("amount") : item.value("value");
    auto* txidItem = explorerItem(explorerShortValue(txid), txid);
    txidItem->setData(Qt::UserRole, txid);
    tblExplorerUTXOs_->setItem(row, 0, txidItem);
    tblExplorerUTXOs_->setItem(row, 1, explorerItem(hasHeight ? QString("#%1").arg(explorerIntegerText(height)) : "unconfirmed"));
    tblExplorerUTXOs_->setItem(row, 2, explorerItem(hasHeight ? "Confirmed" : "Pending"));
    tblExplorerUTXOs_->setItem(row, 3, explorerItem(explorerAddressAmount(amountValue, hasDinAmount)));
    tblExplorerUTXOs_->setItem(row, 4, explorerItem(item.value("category").toString("-")));
  }
  tblExplorerUTXOs_->show();

  QJsonObject raw;
  raw.insert("address", pendingExplorerAddress_);
  raw.insert("balance", pendingExplorerAddressBalance_);
  raw.insert("history", pendingExplorerAddressHistory_);
  txtBlockData_->setText(QJsonDocument(raw).toJson(QJsonDocument::Indented));
  setExplorerStatus(QString("Showing address %1.").arg(explorerShortValue(pendingExplorerAddress_, 22)));
}

bool MainWindow::handleExplorerRpcError(const QString& method, int code, const QString& message) {
  const QString lowerMessage = message.toLower();
  const bool methodMissing =
      code == -32601 ||
      lowerMessage.contains("unknown method") ||
      (lowerMessage.contains("method") && lowerMessage.contains("not found"));

  if (method == "gettransaction" || method == "mempool.gettransaction") {
    if (!pendingExplorerTxLookup_.isEmpty()) {
      if (method == "gettransaction" && !pendingExplorerTxAliasTried_) {
        pendingExplorerTxAliasTried_ = true;
        setExplorerStatus("Transaction lookup missed gettransaction; trying mempool.gettransaction...");
        rpc_->call("mempool.gettransaction", QJsonArray{pendingExplorerTxLookup_});
        return true;
      }
      if (!pendingExplorerTxFallbackBlockHash_.isEmpty()) {
        const QString hash = pendingExplorerTxFallbackBlockHash_;
        pendingExplorerTxLookup_.clear();
        pendingExplorerTxFallbackBlockHash_.clear();
        pendingExplorerBlockHash_ = hash;
        setExplorerStatus(QString("Transaction not found; trying %1 as a block hash...").arg(explorerShortValue(hash)));
        rpc_->call("blockchain.getblock", QJsonArray{hash, 1});
        return true;
      }
      setExplorerStatus(QString("Transaction lookup failed: %1").arg(message), true);
      pendingExplorerTxLookup_.clear();
      return true;
    }

    if (!explorerBlockTransactionRows_.isEmpty()) {
      setExplorerStatus("Some transaction summaries were unavailable from this daemon.", true);
      return true;
    }
  }

  if (method == "blockchain.getaddressbalance" || method == "getaddressbalance") {
    if (!pendingExplorerAddress_.isEmpty()) {
      if (method == "blockchain.getaddressbalance" && methodMissing && !explorerAddressBalanceAliasTried_) {
        explorerAddressBalanceAliasTried_ = true;
        rpc_->call("getaddressbalance", QJsonArray{pendingExplorerAddress_});
        return true;
      }
      pendingExplorerAddressBalanceReady_ = true;
      pendingExplorerAddressBalance_ = QJsonValue(QJsonObject{});
      if (pendingExplorerAddressHistoryReady_) {
        displayExplorerAddress();
      }
      return true;
    }
  }

  if (method == "blockchain.getaddresshistory" || method == "getaddresshistory") {
    if (!pendingExplorerAddress_.isEmpty()) {
      if (method == "blockchain.getaddresshistory" && methodMissing && !explorerAddressHistoryAliasTried_) {
        explorerAddressHistoryAliasTried_ = true;
        rpc_->call("getaddresshistory", QJsonArray{pendingExplorerAddress_});
        return true;
      }
      pendingExplorerAddressHistoryReady_ = true;
      pendingExplorerAddressHistory_ = QJsonValue(QJsonArray{});
      if (pendingExplorerAddressBalanceReady_) {
        displayExplorerAddress();
      }
      return true;
    }
  }

  if (method == "blockchain.getblockhash" && pendingExplorerRecentHeight_ >= 0) {
    pendingExplorerRecentHeight_ = -1;
    requestNextExplorerRecentBlock();
    return true;
  }

  if (method == "blockchain.getblock" && !pendingExplorerBlockHash_.isEmpty()) {
    setExplorerStatus(QString("Block lookup failed: %1").arg(message), true);
    pendingExplorerBlockHash_.clear();
    return true;
  }

  return false;
}

void MainWindow::updateMining(const QJsonObject& miningInfo) {
  // Check if mining tab exists (it's disabled if QML not available)
  if (lblDifficulty_) {
    lblDifficulty_->setText(QString("Difficulty: %1").arg(miningInfo["difficulty"].toDouble(), 0, 'f', 12));
  }
}

void MainWindow::setOverviewLocalHashrate(double hashrateHps, const QString& tooltip) {
  if (!lblLocalHashrate_) {
    return;
  }
  lblLocalHashrate_->setText(QString("Local: %1").arg(formatHashrateText(std::max(0.0, hashrateHps))));
  lblLocalHashrate_->setToolTip(tooltip.isEmpty() ? QString("Local miner/session hashrate") : tooltip);
  cachedHashrate_ = std::max(0.0, hashrateHps);
  refreshAiStatusStrip();
}

void MainWindow::setOverviewNetworkHashrate(double hashrateHps, const QString& tooltip) {
  if (!lblNetworkHashrate_) {
    return;
  }
  const double safeHashrate = std::max(0.0, hashrateHps);
  lblNetworkHashrate_->setText(QString("Network: %1").arg(formatHashrateText(safeHashrate)));
  lblNetworkHashrate_->setToolTip(tooltip.isEmpty() ? QString("Estimated network hashrate from blockchain difficulty") : tooltip);
  cachedNetworkHashrate_ = safeHashrate;
}

void MainWindow::updateOverviewCpuTelemetry(const QJsonObject& cpuStats) {
  if (!lblCpuTemp_ || !lblPowerStatus_) {
    return;
  }

  const double cpuTemp = cpuStats.value("cpu_temp_c").toDouble(std::numeric_limits<double>::quiet_NaN());
  const bool thermalThrottling = cpuStats.value("thermal_throttling").toBool(false);
  const bool tempAvailable = std::isfinite(cpuTemp) && cpuTemp > 0.0;

  if (tempAvailable) {
    QString tempText = QString("Temp: %1 °C").arg(cpuTemp, 0, 'f', 1);
    if (thermalThrottling) {
      tempText += " • throttling";
      lblCpuTemp_->setStyleSheet("QLabel { font-size: 11px; color: #d6dde6; font-weight: 600; }");
    } else {
      lblCpuTemp_->setStyleSheet("QLabel { font-size: 11px; color: #c5ced8; }");
    }
    lblCpuTemp_->setText(tempText);
  } else {
    lblCpuTemp_->setText("Temp: unavailable");
    lblCpuTemp_->setStyleSheet("QLabel { font-size: 11px; color: #868e96; }");
  }

  const bool onBattery = cpuStats.value("on_battery").toBool(false);
  const int batteryPercent = cpuStats.value("battery_percent").toInt(-1);
  QString powerText = onBattery ? "Power: Battery" : "Power: AC";
  if (batteryPercent >= 0) {
    powerText += QString(" (%1%)").arg(batteryPercent);
  }
  lblPowerStatus_->setText(powerText);
  lblPowerStatus_->setToolTip(cpuStats.value("battery_reason").toString());
}

void MainWindow::updateOverviewGpuTelemetry(const QJsonObject& gpuStats) {
  if (!lblGpuLoadOverview_ || !lblGpuMemoryOverview_ || !lblGpuThermalsOverview_) {
    return;
  }

  overviewGpuStatsAvailable_ = gpuStats.value("supported").toBool(false);

  const QString backend = gpuStats.value("backend").toString();
  if (!backend.isEmpty()) {
    overviewGpuBackend_ = backend;
  }

  const QString model = gpuStats.value("model").toString();
  const int coreCount = gpuStats.value("core_count").toInt(0);
  if (!model.isEmpty()) {
    overviewGpuDevice_ = coreCount > 0
      ? QString("%1 (%2 cores)").arg(model).arg(coreCount)
      : model;
  }

  overviewGpuDeviceUtilization_ = gpuStats.value("device_utilization_percent").toDouble(0.0);
  overviewGpuRendererUtilization_ = gpuStats.value("renderer_utilization_percent").toDouble(0.0);
  overviewGpuTilerUtilization_ = gpuStats.value("tiler_utilization_percent").toDouble(0.0);
  overviewGpuMemoryInUse_ = static_cast<qint64>(gpuStats.value("memory_in_use_bytes").toDouble(0.0));
  overviewGpuMemoryAllocated_ = static_cast<qint64>(gpuStats.value("memory_allocated_bytes").toDouble(0.0));
  overviewGpuTempAvailable_ = gpuStats.value("temp_available").toBool(false);
  overviewGpuTempC_ = gpuStats.value("temp_c").toDouble(0.0);
  overviewGpuFanAvailable_ = gpuStats.value("fan_available").toBool(false);
  overviewGpuFanRpm_ = static_cast<qint64>(gpuStats.value("fan_rpm").toDouble(0.0));
  overviewGpuThermalReason_ = gpuStats.value("temp_reason").toString(gpuStats.value("reason").toString());
  if (overviewGpuThermalReason_.isEmpty()) {
    overviewGpuThermalReason_ = gpuStats.value("fan_reason").toString();
  }

  if (overviewGpuStatsAvailable_) {
    lblGpuLoadOverview_->setText(
      QString("GPU Load: %1% • Render %2% • Tiler %3%")
        .arg(overviewGpuDeviceUtilization_, 0, 'f', 0)
        .arg(overviewGpuRendererUtilization_, 0, 'f', 0)
        .arg(overviewGpuTilerUtilization_, 0, 'f', 0));
    lblGpuLoadOverview_->setToolTip("System-wide GPU telemetry. It may be active even when the selected miner is CPU.");

    QString memoryText = QString("GPU Mem: %1 in use").arg(formatBytesText(overviewGpuMemoryInUse_));
    if (overviewGpuMemoryAllocated_ > 0) {
      memoryText += QString(" / %1 alloc").arg(formatBytesText(overviewGpuMemoryAllocated_));
    }
    lblGpuMemoryOverview_->setText(memoryText);
    lblGpuMemoryOverview_->setToolTip("System-wide GPU memory from macOS AGX PerformanceStatistics");
  } else {
    lblGpuLoadOverview_->setText("GPU Load: unavailable");
    lblGpuMemoryOverview_->setText("GPU Mem: unavailable");
    const QString reason = gpuStats.value("reason").toString();
    lblGpuLoadOverview_->setToolTip(reason);
    lblGpuMemoryOverview_->setToolTip(reason);
  }

  if (overviewGpuTempAvailable_ || overviewGpuFanAvailable_) {
    QStringList parts;
    if (overviewGpuTempAvailable_) {
      parts << QString("GPU Temp %1 °C").arg(overviewGpuTempC_, 0, 'f', 1);
    }
    if (overviewGpuFanAvailable_) {
      parts << QString("GPU Fan %1 RPM").arg(overviewGpuFanRpm_);
    }
    lblGpuThermalsOverview_->setText(parts.join(" • "));
    lblGpuThermalsOverview_->setToolTip("GPU thermal telemetry");
  } else {
    QString thermalSummary = "GPU Temp/Fan: unavailable";
    if (overviewGpuThermalReason_.contains("not GPU temperature or fan RPM", Qt::CaseInsensitive) ||
        overviewGpuThermalReason_.contains("temperature or fan", Qt::CaseInsensitive)) {
      thermalSummary = "GPU Temp/Fan: not exposed by macOS";
    }
    lblGpuThermalsOverview_->setText(thermalSummary);
    lblGpuThermalsOverview_->setToolTip(
      overviewGpuThermalReason_.isEmpty()
        ? QString("GPU temperature/fan telemetry unavailable")
        : overviewGpuThermalReason_);
  }

  updateOverviewHardwareTelemetry();
}

void MainWindow::updateOverviewHardwareTelemetry() {
  if (!lblMinerModeOverview_ || !lblGpuBackendOverview_ || !lblGpuDeviceOverview_) {
    return;
  }

  QString minerText = "Miner: Idle";
  if (isMining_) {
    if (activeMinerType_ == "gpu") {
      minerText = "Miner: Solo GPU";
    } else if (activeMinerType_ == "internal_gpu") {
      minerText = "Miner: Solo GPU (embedded)";
    } else if (activeMinerType_ == "sv2_pool_gpu") {
      minerText = "Miner: SV2 Pool GPU";
    } else if (activeMinerType_ == "sv2_pool") {
      minerText = "Miner: SV2 Pool CPU";
    } else if (activeMinerType_ == "stratum_worker") {
      minerText = "Miner: Pool Worker";
    } else if (activeMinerType_ == "external") {
      minerText = "Miner: Solo CPU";
    } else if (activeMinerType_ == "internal") {
      minerText = "Miner: Solo CPU (embedded)";
    } else if (activeMinerType_ == "daemon") {
      minerText = "Miner: Daemon";
    } else {
      minerText = "Miner: Daemon";
    }
  }
  lblMinerModeOverview_->setText(minerText);

  const bool activeGpuMiner =
      activeMinerType_ == "gpu" || activeMinerType_ == "internal_gpu" ||
      activeMinerType_ == "sv2_pool_gpu";
  const bool activeCpuMiner =
      activeMinerType_ == "internal" || activeMinerType_ == "external" ||
      activeMinerType_ == "sv2_pool";

  QString backendText = "Mining: idle";
  if (activeGpuMiner) {
    backendText = QString("Mining: %1").arg(
        overviewGpuBackend_.isEmpty() ? QStringLiteral("GPU") : titleCaseWord(overviewGpuBackend_));
  } else if (activeCpuMiner) {
    backendText = "Mining: CPU";
  } else if (activeMinerType_ == "stratum_worker") {
    backendText = "Mining: stratum worker";
  } else if (activeMinerType_ == "daemon") {
    backendText = "Mining: daemon";
  }
  if (activeGpuMiner && overviewGpuDeviceCount_ > 0) {
    backendText += QString(" • %1 device%2")
                     .arg(overviewGpuDeviceCount_)
                     .arg(overviewGpuDeviceCount_ == 1 ? "" : "s");
  }

  QString deviceText = "GPU: --";
  if (activeGpuMiner && !overviewGpuDevice_.isEmpty()) {
    deviceText = QString("GPU: %1").arg(overviewGpuDevice_);
  } else if (!overviewGpuDevice_.isEmpty()) {
    deviceText = QString("GPU: %1 (available)").arg(overviewGpuDevice_);
  } else if (!isMining_) {
    deviceText = "GPU: idle";
  } else if (activeGpuMiner) {
    deviceText = "GPU: waiting for device info";
  } else if (activeMinerType_ == "stratum_worker") {
    deviceText = "GPU: not used";
  } else if (activeMinerType_ == "external") {
    deviceText = "GPU: not used by selected miner";
  } else {
    deviceText = "GPU: not used";
  }

  lblGpuBackendOverview_->setText(backendText);
  lblGpuDeviceOverview_->setText(deviceText);
}

void MainWindow::resetOverviewMiningTelemetry() {
  overviewGpuStatsAvailable_ = false;
  overviewGpuDeviceUtilization_ = 0.0;
  overviewGpuRendererUtilization_ = 0.0;
  overviewGpuTilerUtilization_ = 0.0;
  overviewGpuMemoryInUse_ = 0;
  overviewGpuMemoryAllocated_ = 0;
  overviewGpuTempAvailable_ = false;
  overviewGpuTempC_ = 0.0;
  overviewGpuFanAvailable_ = false;
  overviewGpuFanRpm_ = 0;
  overviewGpuThermalReason_.clear();
  overviewGpuBackend_.clear();
  overviewGpuDevice_.clear();
  overviewGpuDeviceCount_ = 0;
  overviewGpuTelemetrySeen_ = false;
  setOverviewLocalHashrate(0.0, "Local miner/session hashrate");
  if (lblGpuLoadOverview_) {
    lblGpuLoadOverview_->setText("GPU Load: --");
    lblGpuLoadOverview_->setToolTip({});
  }
  if (lblGpuMemoryOverview_) {
    lblGpuMemoryOverview_->setText("GPU Mem: --");
    lblGpuMemoryOverview_->setToolTip({});
  }
  if (lblGpuThermalsOverview_) {
    lblGpuThermalsOverview_->setText("GPU Temp/Fan: --");
    lblGpuThermalsOverview_->setToolTip({});
  }
  updateOverviewHardwareTelemetry();
}

void MainWindow::resetMiningReadinessDisplay(const QString& summary, const QString& detail) {
  if (!lblMiningReadiness_) {
    return;
  }
  lblMiningReadiness_->setText(summary);
  lblMiningReadiness_->setToolTip(detail.isEmpty() ? summary : detail);
  lastMiningReadinessFingerprint_.clear();
}

void MainWindow::updateMiningReadinessDisplay(const QJsonObject& readiness) {
  if (!lblMiningReadiness_) {
    return;
  }

  const QString summary = miningReadinessSummaryText(readiness);
  const QString tooltip = miningReadinessTooltipText(readiness);
  lblMiningReadiness_->setText(summary);
  lblMiningReadiness_->setToolTip(tooltip);

  const QString fingerprint = miningReadinessFingerprint(readiness);
  if (fingerprint != lastMiningReadinessFingerprint_) {
    lastMiningReadinessFingerprint_ = fingerprint;
    if (txtMiningOutput_) {
      txtMiningOutput_->append(QString("[Daemon readiness] %1").arg(summary));
      const QString message = readiness.value("message").toString().trimmed();
      if (!message.isEmpty()) {
        txtMiningOutput_->append(QString("[Daemon readiness] %1").arg(message));
      }
    }
  }
}

void MainWindow::updateMiningStats(const QJsonObject& miningInfo) {
  // Update mining status from mining.info RPC result
  if (!lblMiningStatus_ || !lblBlocksFound_ || !lblCurrentHash_) {
    return;
  }

  const QJsonObject readiness = miningInfo.value("mining_readiness").toObject();
  updateMiningReadinessDisplay(readiness);

  // A locally-owned miner (embedded CPU or external process) should drive the UI directly.
  const bool embeddedMinerActive = minerCtrl_ && minerCtrl_->running();
  const bool processMinerActive = miningProcess_ && miningProcess_->state() == QProcess::Running;
  if (embeddedMinerActive || processMinerActive) {
    setMiningOutputCinematicEnabled(true);
    return;
  }

  bool mining = miningInfo["mining"].toBool();
  int threads = miningInfo["threads"].toInt();
  double hashrate = miningInfo["hashrate"].toDouble();
  int blocksFound = miningInfo["blocks_found"].toInt();
  setOverviewLocalHashrate(hashrate, "Local hashrate from mining.info");

  // Update status
  if (mining) {
    lblMiningStatus_->setText(miningStatusActiveText());
    lblMiningStatus_->setStyleSheet(chromePillStyle());
    isMining_ = true;
    if (activeMinerType_ == "none") {
      activeMinerType_ = "daemon";
      setMiningModeControlsLocked(true);
    }
    if (btnStartMining_) {
      btnStartMining_->setText("Stop Mining");
      btnStartMining_->setStyleSheet(headerButtonStyle());
      btnStartMining_->setToolTip("Click to stop mining");
    }
  } else {
    lblMiningStatus_->setText(miningStatusInactiveText());
    lblMiningStatus_->setStyleSheet(chromePillStyle());
    if (activeMinerType_ != "external" && activeMinerType_ != "stratum_worker" && activeMinerType_ != "gpu") {
      isMining_ = false;
      activeMinerType_ = "none";
      setMiningModeControlsLocked(false);
      if (btnStartMining_) {
        btnStartMining_->setText("Start Mining");
        btnStartMining_->setStyleSheet(headerButtonStyle());
        btnStartMining_->setToolTip("Click to start mining");
      }
    }
  }

  // Update stats
  lblBlocksFound_->setText(QString::number(blocksFound));

  // Convert hashrate to MH/s
  double mhs = hashrate / 1000000.0;
  lblCurrentHash_->setText(QString::number(mhs, 'f', 2));

  // Add mining output log
  if (txtMiningOutput_ && mining) {
    QString logLine = QString("[%1] Mining: %2 threads, %3 MH/s, %4 blocks found")
      .arg(QTime::currentTime().toString("HH:mm:ss"))
      .arg(threads)
      .arg(mhs, 0, 'f', 2)
      .arg(blocksFound);
    txtMiningOutput_->append(logLine);

    // Auto-scroll to bottom
    QTextCursor cursor = txtMiningOutput_->textCursor();
    cursor.movePosition(QTextCursor::End);
    txtMiningOutput_->setTextCursor(cursor);
  }

  setMiningOutputCinematicEnabled(isMining_);
}

void MainWindow::setDatadir(const QString& datadir) {
  rpc_->setDatadir(datadir);
  rpc_->loadCookie();

  // Update ConnectionManager with new datadir
  QString cookiePath = QDir(datadir).filePath(".cookie");
  connectionMgr_->setCookiePath(cookiePath);

  // Reconnect to apply new settings
  if (connectionMgr_->isConnected()) {
    connectionMgr_->reconnect();
  }

  // WebSockets disabled - no discovery needed
  // discoverAndConnectWebSocket();

  // Phase 6: Initialize change address manager with wallet datadir
  if (changeAddrMgr_) {
    changeAddrMgr_->deleteLater();
  }
  changeAddrMgr_ = new ChangeAddressManager(datadir, this);

  // Phase 4: Initialize transaction tracker with wallet datadir
  txTracker_->setDataDir(datadir);
  txTracker_->setWalletScope(currentWalletName_);

  // Phase 5: Initialize advisory banner queue and replay persisted events
  if (bannerQueue_) {
    bannerQueue_->setDataDir(datadir);
    bannerQueue_->setWalletScope(currentWalletName_);
    if (!currentWalletName_.isEmpty()) {
      bannerQueue_->replayOnStartup();
    }
  }
}

void MainWindow::onSetMiningAddress() {
  // Check wallet state first
  if (!walletUnlocked_) {
    QMessageBox::StandardButton reply = QMessageBox::warning(this,
      "Wallet Locked",
      "Your wallet must be unlocked to use wallet addresses for mining.\n\n"
      "Would you like to unlock your wallet now?",
      QMessageBox::Yes | QMessageBox::No,
      QMessageBox::Yes);

    if (reply == QMessageBox::Yes) {
      onUnlockWallet();
    }
    return;
  }

  // Use the latest address from the Receive tab
  if (tblAddresses_->rowCount() > 0) {
    int lastRow = tblAddresses_->rowCount() - 1;
    QString address = tblAddresses_->item(lastRow, 1)->text();

    // v7: mining accepts Taproot (din1p...) or P2MR (din1r...)
    const bool isTaproot = address.startsWith("din1p") ||
                           address.startsWith("tdin1p") ||
                           address.startsWith("rdin1p");
    const bool isP2mr    = address.startsWith("din1r") ||
                           address.startsWith("tdin1r") ||
                           address.startsWith("rdin1r");

    if (!isTaproot && !isP2mr) {
      QMessageBox::StandardButton reply = QMessageBox::warning(this,
        "Address Not Eligible for Mining",
        QString("The selected address is neither Taproot nor P2MR:\n\n%1\n\n"
                "Mining requires a Taproot (din1p...) or P2MR (din1r...) address.\n\n"
                "Would you like to generate a new eligible address?")
                .arg(address),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);

      if (reply == QMessageBox::Yes) {
        onDeriveNewAddress();
        return;
      }
      return;
    }

    edtMiningAddress_->setText(address);
    QMessageBox::information(this, "Mining Address Set",
      "Taproot mining address set to:\n" + address);
  } else {
    QMessageBox::warning(this, "No Wallet Address",
      "No addresses found in your wallet.\n\n"
      "Please generate a new Taproot address first:\n"
      "1. Go to Receive tab\n"
      "2. Click 'New Transparent Address' to generate a Taproot address\n"
      "3. Return here and click 'Use Wallet' again");
  }
}

void MainWindow::onBrowseMinerBinary() {
  QString defaultPath = QCoreApplication::applicationDirPath();
  const QString mode = currentMiningMode();
  const bool poolV1  = (mode == "pool");
  const bool poolSv2 = (mode == "sv2_pool");
  const bool sv2UseGpu = poolSv2 &&
    cmbSv2Backend_ &&
    cmbSv2Backend_->currentData().toString() == QStringLiteral("metal");
  const QString binaryName = poolSv2
    ? sv2MinerBinaryNameForBackend(sv2UseGpu)
    : externalMinerBinaryName(poolV1);
  const QString settingsKey = poolSv2
    ? sv2MinerSettingsKeyForBackend(sv2UseGpu)
    : externalMinerSettingsKey(poolV1);
  const QString displayName = poolSv2
    ? sv2MinerDisplayNameForBackend(sv2UseGpu)
    : externalMinerDisplayName(poolV1);
  const bool poolMode = poolV1;  // legacy name retained in the code below

  // Try to find the relevant external miner in common locations first.
  QStringList searchPaths;
#ifdef Q_OS_MAC
  searchPaths = {
    defaultPath + "/../../../" + binaryName,
    defaultPath + "/../Resources/" + binaryName,
    defaultPath + "/" + binaryName
  };
#elif defined(Q_OS_WIN)
  searchPaths = {
    defaultPath + "/" + binaryName,
    defaultPath + "/../" + binaryName
  };
#else
  searchPaths = {
    defaultPath + "/" + binaryName,
    defaultPath + "/../" + binaryName
  };
#endif

  // Check if we have a saved path
  QString savedPath = QSettings().value(settingsKey).toString();
  if (!savedPath.isEmpty() &&
      QFile::exists(savedPath) &&
      (!poolSv2 || sv2PathMatchesBackend(savedPath, sv2UseGpu))) {
    defaultPath = QFileInfo(savedPath).absolutePath();
  } else {
    // Check common paths
    for (const QString& path : searchPaths) {
      if (QFile::exists(path)) {
        defaultPath = QFileInfo(path).absolutePath();
        edtMinerPath_->setText(QFileInfo(path).absoluteFilePath());
        break;
      }
    }
  }

  QString filter;
#ifdef Q_OS_WIN
  filter = QString("Executables (%1);;All Files (*)").arg(binaryName);
#else
  filter = QString("Executables (%1);;All Files (*)").arg(binaryName);
#endif

  QString path = QFileDialog::getOpenFileName(
    this,
    poolSv2
      ? QString("Select %1 Binary").arg(displayName)
      : poolMode ? "Select Dinero Stratum Worker Binary" : "Select Dinero External Miner Binary",
    defaultPath,
    filter
  );

  if (!path.isEmpty()) {
    if (poolSv2 && !sv2PathMatchesBackend(path, sv2UseGpu)) {
      QMessageBox::warning(this, "Wrong SV2 Miner",
        QString("The selected file does not match the current backend.\n\n"
                "Backend: %1\nExpected binary: %2\nSelected: %3")
          .arg(sv2UseGpu ? "GPU (Metal)" : "CPU",
               binaryName,
               QFileInfo(path).fileName()));
      return;
    }
    edtMinerPath_->setText(path);
    // Save to settings for next time
    QSettings().setValue(settingsKey, path);
    qDebug() << displayName << "path set to:" << path;

    QString helpText;
    QString probeError;
    const QString profile = probeExternalMinerProfile(path, &helpText, &probeError);
    if (profile != "unknown") {
      const QString profileText = (profile == "rpc") ? "RPC-style external miner detected" :
                                                    "Stratum-style external miner detected";
      if (txtMiningOutput_) {
        txtMiningOutput_->append(QString("[Miner Detection] %1").arg(profileText));
      }
      if (lblMiningStatus_ && !isMining_) {
        lblMiningStatus_->setText(miningStatusInactiveText());
        lblMiningStatus_->setToolTip(profileText);
        lblMiningStatus_->setStyleSheet(chromePillStyle());
      }
    } else if (!probeError.isEmpty() && txtMiningOutput_) {
      txtMiningOutput_->append(QString("[Miner Detection] Auto-detect uncertain: %1").arg(probeError));
    }
  }
}

void MainWindow::onToggleLocalStratumServer() {
  if (localStratumProcess_ && localStratumProcess_->state() == QProcess::Running) {
    stopLocalStratumServer();
    return;
  }

  if (!rpc_ || !rpc_->isConnected()) {
    QMessageBox::warning(this, "Daemon Required",
      "Start and connect the daemon before starting the local Stratum server.");
    return;
  }

  const QString payoutAddress = edtMiningAddress_ ? edtMiningAddress_->text().trimmed() : QString();
  const bool isTaproot = payoutAddress.startsWith("din1p") ||
                         payoutAddress.startsWith("tdin1p") ||
                         payoutAddress.startsWith("rdin1p");
  const bool isP2mr = payoutAddress.startsWith("din1r") ||
                      payoutAddress.startsWith("tdin1r") ||
                      payoutAddress.startsWith("rdin1r");
  if (!isTaproot && !isP2mr) {
    QMessageBox::warning(this, "Mining Address Required",
      "Set a Taproot or P2MR mining address before starting the local Stratum server.");
    return;
  }

  rpc_->loadCookie();
  const QString dataDir = rpc_->datadir();
  const QString cookiePath = QDir(dataDir).filePath(".cookie");
  if (!QFile::exists(cookiePath)) {
    QMessageBox::warning(this, "RPC Cookie Missing",
      "Could not find the daemon RPC cookie at:\n\n" + cookiePath);
    return;
  }

  const QString stratumPath = findLocalStratumServerBinary();
  if (stratumPath.isEmpty()) {
    QMessageBox::critical(this, "Stratum Server Not Found",
      "Could not find dinero-stratum.\n\n"
      "Build the Stratum server or set DINERO_STRATUM_PATH.");
    return;
  }

  if (!localStratumProcess_) {
    localStratumProcess_ = new QProcess(this);
    localStratumProcess_->setProcessChannelMode(QProcess::MergedChannels);

    connect(localStratumProcess_, &QProcess::readyReadStandardOutput, this, [this]() {
      if (!localStratumProcess_ || !txtMiningOutput_) {
        return;
      }
      const QString output = QString::fromUtf8(localStratumProcess_->readAllStandardOutput());
      const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
      for (const QString& line : lines) {
        appendMiningOutputLine(txtMiningOutput_, "[Local Stratum] " + line);
      }
    });

    connect(localStratumProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
      if (btnLocalStratum_) {
        btnLocalStratum_->setText("Start Local");
        btnLocalStratum_->setToolTip(
          "Start a localhost dinero-stratum server and use it as this pool endpoint.");
      }
      if (txtMiningOutput_ && !shuttingDown_) {
        txtMiningOutput_->append(QString("[Local Stratum] stopped (exit=%1)").arg(exitCode));
      }
      if (lblMiningStatus_ && !isMining_ && currentMiningMode() == "pool") {
        lblMiningStatus_->setText("Pool mode ready (Stratum stopped)");
        lblMiningStatus_->setStyleSheet(chromePillStyle());
      }
    });

    connect(localStratumProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
      if (txtMiningOutput_ && localStratumProcess_) {
        txtMiningOutput_->append("[Local Stratum] error: " + localStratumProcess_->errorString());
      }
    });
  }

  if (edtStratumEndpoint_) {
    edtStratumEndpoint_->setText(localStratumEndpoint());
    QSettings().setValue("mining/stratum_endpoint", localStratumEndpoint());
  }
  if (cmbMiningMode_ && currentMiningMode() != "pool") {
    const int poolIndex = cmbMiningMode_->findData("pool");
    if (poolIndex >= 0) {
      cmbMiningMode_->setCurrentIndex(poolIndex);
    }
  }

  const QStringList args = {
    "--rpchost=127.0.0.1",
    "--rpcport=20998",
    "--rpccookie=" + cookiePath,
    "--stratumhost=127.0.0.1",
    "--stratumport=" + QString::number(localStratumPort()),
    "--maxconnections=16",
    "--payout=" + payoutAddress + ":1"
  };

  if (txtMiningOutput_) {
    txtMiningOutput_->append("=== Starting local Stratum server ===");
    appendBinaryIdentityToMiningLog(txtMiningOutput_, "Local Stratum", stratumPath);
    txtMiningOutput_->append("Endpoint: " + localStratumEndpoint());
    txtMiningOutput_->append("Payout: " + payoutAddress);
    txtMiningOutput_->append("Cookie: " + cookiePath);
  }

  localStratumProcess_->start(stratumPath, args);
  if (!localStratumProcess_->waitForStarted(3000)) {
    QMessageBox::critical(this, "Stratum Server Failed",
      "Failed to start dinero-stratum:\n\n" + localStratumProcess_->errorString());
    return;
  }

  if (btnLocalStratum_) {
    btnLocalStratum_->setText("Stop Local");
    btnLocalStratum_->setToolTip("Stop the localhost Stratum server.");
  }
  if (lblMiningStatus_) {
    lblMiningStatus_->setText("Local Stratum ready");
    lblMiningStatus_->setStyleSheet(chromePillStyle());
  }
  updateStratumIdentityLabel();
}

void MainWindow::stopLocalStratumServer() {
  if (!localStratumProcess_) {
    return;
  }

  if (localStratumProcess_->state() == QProcess::Running) {
    if (txtMiningOutput_) {
      txtMiningOutput_->append("[Local Stratum] stopping...");
    }
    localStratumProcess_->terminate();
    if (!localStratumProcess_->waitForFinished(3000)) {
      localStratumProcess_->kill();
      localStratumProcess_->waitForFinished(1000);
    }
  }

  if (btnLocalStratum_) {
    btnLocalStratum_->setText("Start Local");
    btnLocalStratum_->setToolTip(
      "Start a localhost dinero-stratum server and use it as this pool endpoint.");
  }
  if (lblMiningStatus_ && !isMining_ && currentMiningMode() == "pool") {
    lblMiningStatus_->setText("Pool mode ready (Stratum stopped)");
    lblMiningStatus_->setStyleSheet(chromePillStyle());
  }
}

QString MainWindow::currentMiningMode() const {
  if (!cmbMiningMode_) {
    return "solo";
  }
  QString mode = cmbMiningMode_->currentData().toString();
  if (mode.isEmpty()) {
    mode = "solo";
  }
  return mode;
}

void MainWindow::onMiningModeChanged(int index) {
  Q_UNUSED(index);
  const QString mode = currentMiningMode();
  const bool poolV1  = (mode == "pool");
  const bool poolSv2 = (mode == "sv2_pool");
  const bool anyPool = poolV1 || poolSv2;

  if (lblMinerType_) {
    lblMinerType_->setVisible(!anyPool);
  }
  if (cmbMinerType_) {
    cmbMinerType_->setVisible(!anyPool);
    if (!anyPool) cmbMinerType_->setEnabled(true);
  }

  if (lblStratumEndpoint_) {
    lblStratumEndpoint_->setVisible(poolV1);
  }
  if (edtStratumEndpoint_) {
    edtStratumEndpoint_->setVisible(poolV1);
  }
  if (btnLocalStratum_) {
    btnLocalStratum_->setVisible(poolV1);
    const bool localRunning = localStratumProcess_ &&
      localStratumProcess_->state() == QProcess::Running;
    btnLocalStratum_->setText(localRunning ? "Stop Local" : "Start Local");
  }
  if (lblSv2Endpoint_)  lblSv2Endpoint_->setVisible(poolSv2);
  if (edtSv2Endpoint_)  edtSv2Endpoint_->setVisible(poolSv2);
  if (lblSv2Pubkey_)    lblSv2Pubkey_->setVisible(poolSv2);
  if (edtSv2Pubkey_)    edtSv2Pubkey_->setVisible(poolSv2);
  if (lblSv2Backend_)   lblSv2Backend_->setVisible(poolSv2);
  if (cmbSv2Backend_)   cmbSv2Backend_->setVisible(poolSv2);
  if (lblSv2RewardMode_) lblSv2RewardMode_->setVisible(poolSv2);
  if (cmbSv2RewardMode_) cmbSv2RewardMode_->setVisible(poolSv2);
  if (lblSv2Shares_)    lblSv2Shares_->setVisible(poolSv2);
  if (lblMiningUptimeCaption_) lblMiningUptimeCaption_->setVisible(true);
  if (lblMiningUptime_) lblMiningUptime_->setVisible(true);
  // The "Readiness: waiting for daemon mining state" row tracks local
  // solo-mining readiness. It's meaningless for SV2 pool mining where
  // readiness is pool-side and the ChannelOpen / SetNewPrevHash frames
  // in the output panel convey the equivalent state.
  if (lblMiningReadiness_)  lblMiningReadiness_->setVisible(!anyPool);
  if (lblStratumIdentity_) {
    lblStratumIdentity_->setVisible(poolV1);
  }

  if (!isMining_ && lblMiningStatus_) {
    if (poolV1) {
      QString endpoint = explicitStratumEndpoint();
      if (edtStratumEndpoint_) {
        const QString uiEndpoint = edtStratumEndpoint_->text().trimmed();
        if (!uiEndpoint.isEmpty()) endpoint = uiEndpoint;
      }
      const bool localRunning = localStratumProcess_ &&
        localStratumProcess_->state() == QProcess::Running;
      lblMiningStatus_->setText(localRunning
        ? "Local Stratum ready"
        : endpoint.isEmpty()
        ? "Pool mode ready (set endpoint)"
        : "Pool mode ready (Stratum V1)");
    } else if (poolSv2) {
      const QString endpoint =
        edtSv2Endpoint_ && !edtSv2Endpoint_->text().trimmed().isEmpty()
          ? edtSv2Endpoint_->text().trimmed()
          : sv2PoolEndpoint();
      lblMiningStatus_->setText(QString("SV2 pool ready (%1)").arg(endpoint));
    } else {
      lblMiningStatus_->setText(miningStatusInactiveText());
    }
    lblMiningStatus_->setStyleSheet(chromePillStyle());
  }

  onMinerTypeChanged(cmbMinerType_ ? cmbMinerType_->currentIndex() : 0);
  updateStratumIdentityLabel();
  qDebug() << "Mining mode changed to:" << mode;
}

void MainWindow::onMinerTypeChanged(int index) {
  Q_UNUSED(index);
  const QString mode = currentMiningMode();
  const bool poolV1  = (mode == "pool");
  const bool poolSv2 = (mode == "sv2_pool");
  const bool anyPool = poolV1 || poolSv2;
  QString minerType = cmbMinerType_ ? cmbMinerType_->currentData().toString() : "internal";
  const bool isExternal = anyPool || (minerType == "external") || (minerType == "stratum_worker");
  const bool isGPU = !anyPool && (minerType == "gpu" || minerType == "internal_gpu");

  if (lblMinerType_) lblMinerType_->setVisible(!anyPool);
  if (cmbMinerType_) cmbMinerType_->setVisible(!anyPool);
  if (lblMinerPath_) {
    lblMinerPath_->setText(poolSv2 ? "SV2 Miner:" : anyPool ? "Worker:" : "Miner:");
    lblMinerPath_->setVisible(isExternal);
  }
  if (edtMinerPath_) edtMinerPath_->setVisible(isExternal);
  if (btnBrowseMiner_) btnBrowseMiner_->setVisible(isExternal);
  if (lblStratumIdentity_) lblStratumIdentity_->setVisible(poolV1);
  if (edtMiningThreads_) edtMiningThreads_->setVisible(!isGPU);

  // Load saved path for the right binary based on mode.
  if (isExternal && edtMinerPath_) {
    const bool sv2UseGpu = poolSv2 &&
      cmbSv2Backend_ &&
      cmbSv2Backend_->currentData().toString() == QStringLiteral("metal");
    const QString key = poolSv2
      ? sv2MinerSettingsKeyForBackend(sv2UseGpu)
      : externalMinerSettingsKey(poolV1);
    const QString savedPath = QSettings().value(key).toString().trimmed();
    if (poolSv2) {
      const QString minerPath = discoverSv2MinerPath(sv2UseGpu, true, true);
      if (!minerPath.isEmpty()) {
        edtMinerPath_->setText(minerPath);
      } else {
        edtMinerPath_->clear();
      }
    } else if (!savedPath.isEmpty() && QFile::exists(savedPath)) {
      edtMinerPath_->setText(savedPath);
    } else {
      edtMinerPath_->clear();
    }
    edtMinerPath_->setPlaceholderText(
      poolSv2
        ? QString("Path to %1 binary...").arg(sv2MinerBinaryNameForBackend(sv2UseGpu))
      : poolV1 ? "Path to dinero-stratum-worker binary..."
               : "Path to external RPC miner binary...");
  }

  // The engine selector stays enabled while mining (see setMiningModeControlsLocked),
  // but a switch only applies on the next Start — say so instead of silently no-op'ing.
  if (isMining_ && !anyPool && txtMiningOutput_) {
    txtMiningOutput_->append(
      "Note: mining engine changed to \"" + (cmbMinerType_ ? cmbMinerType_->currentText() : minerType) +
      "\" — Stop and Start mining to apply it.");
  }

  qDebug() << "Miner type changed to:" << minerType << "(mode =" << mode << ")";
}

void MainWindow::setMiningModeControlsLocked(bool locked) {
  if (cmbMiningMode_) {
    cmbMiningMode_->setEnabled(!locked);
  }
  if (cmbMinerType_) {
    // Pool mode keeps miner type fixed to the dedicated Stratum worker.
    // In solo mode, keep the engine selector enabled even while mining so it
    // stays visibly a dropdown the user can open. Changing it mid-run only
    // updates the pending selection/visibility (onMinerTypeChanged); it does
    // not restart the running miner, so leaving it live is safe — the new
    // engine takes effect on the next Stop/Start.
    cmbMinerType_->setEnabled(currentMiningMode() != "pool");
  }
  if (edtMinerPath_) {
    edtMinerPath_->setEnabled(!locked);
  }
  if (btnBrowseMiner_) {
    btnBrowseMiner_->setEnabled(!locked);
  }
  if (edtStratumEndpoint_) {
    edtStratumEndpoint_->setEnabled(!locked);
  }
  if (edtSv2Endpoint_) {
    edtSv2Endpoint_->setEnabled(!locked);
  }
  if (edtSv2Pubkey_) {
    edtSv2Pubkey_->setEnabled(!locked);
  }
  if (cmbSv2Backend_) {
    cmbSv2Backend_->setEnabled(!locked);
  }
  if (cmbSv2RewardMode_) {
    cmbSv2RewardMode_->setEnabled(!locked);
  }
  if (btnLocalStratum_) {
    btnLocalStratum_->setEnabled(!locked ||
      (localStratumProcess_ && localStratumProcess_->state() == QProcess::Running));
  }
}

void MainWindow::applyMiningFocusDim(bool enabled) {
  auto ensureEffect = [](QWidget* widget, QGraphicsColorizeEffect*& effect) -> QGraphicsColorizeEffect* {
    if (!widget) {
      return nullptr;
    }
    if (!effect) {
      effect = qobject_cast<QGraphicsColorizeEffect*>(widget->graphicsEffect());
      if (!effect) {
        effect = new QGraphicsColorizeEffect(widget);
        widget->setGraphicsEffect(effect);
      }
    }
    return effect;
  };

  const auto applyStrength = [enabled](QGraphicsColorizeEffect* effect, double strength) {
    if (!effect) {
      return;
    }
    effect->setColor(QColor(0, 0, 0));
    effect->setStrength(enabled ? strength : 0.0);
  };

  bool anyTarget = false;
  if (auto* effect = ensureEffect(miningInfoGroup_, miningInfoDimEffect_)) {
    applyStrength(effect, 0.48);
    anyTarget = true;
  }
  if (auto* effect = ensureEffect(miningControlsGroup_, miningControlsDimEffect_)) {
    applyStrength(effect, 0.52);
    anyTarget = true;
  }
  if (auto* effect = ensureEffect(lblMiningOutputSection_, miningOutputLabelDimEffect_)) {
    applyStrength(effect, 0.45);
    anyTarget = true;
  }

  miningFocusDimApplied_ = enabled && anyTarget;
}

void MainWindow::updateMiningFocusDimState() {
  const bool miningTabActive =
    mainTabs_ && miningTabWidget_ && (mainTabs_->currentWidget() == miningTabWidget_);
  const bool shouldDim = isMining_ && miningTabActive;

  if (!shouldDim) {
    if (miningFocusDimTimer_ && miningFocusDimTimer_->isActive()) {
      miningFocusDimTimer_->stop();
    }
    if (miningFocusDimApplied_) {
      applyMiningFocusDim(false);
    }
    return;
  }

  if (miningFocusDimApplied_) {
    return;
  }

  if (miningFocusDimTimer_) {
    if (!miningFocusDimTimer_->isActive()) {
      miningFocusDimTimer_->start(3000);
    }
    return;
  }

  applyMiningFocusDim(true);
}

void MainWindow::setMiningOutputCinematicEnabled(bool enabled) {
  updateMiningFocusDimState();

  if (!txtMiningOutput_) {
    return;
  }

  QWidget* viewport = txtMiningOutput_->viewport();
  if (!viewport) {
    return;
  }

  const bool miningTabActive =
    mainTabs_ && miningTabWidget_ && mainTabs_->currentWidget() == miningTabWidget_;
  const bool shouldRun = dinero::qt::shouldRunHashEngine(enabled, miningTabActive, false);

  if (shouldRun && miningCinematicTimer_) {
    if (!miningCinematicTimer_->isActive()) {
      miningCinematicFrame_ = 0;
      miningCinematicLastLongCometFrame_ = -100000;
      miningCinematicLastUltraCometFrame_ = 0;
      miningCinematicSparks_.clear();
      miningCinematicTimer_->start();
    }
    updateMiningOutputCinematicFrame();
    return;
  }

  // Once a hidden cinematic has been stopped and restored to its idle
  // palette, mining-status polls must not repaint that hidden viewport.
  if (miningCinematicTimer_ && !miningCinematicTimer_->isActive()) {
    return;
  }

  if (miningCinematicTimer_ && miningCinematicTimer_->isActive()) {
    miningCinematicTimer_->stop();
  }
  if (mottoTickerLabel_) {
    mottoTickerLabel_->hide();
  }
  if (miningHashOverlay_) {
    miningHashOverlay_->hide();
    miningHashOverlay_->clear();
  }
  miningCinematicFrame_ = 0;
  miningCinematicLastLongCometFrame_ = -100000;
  miningCinematicLastUltraCometFrame_ = 0;
  miningCinematicSparks_.clear();

  QPalette palette = viewport->palette();
  palette.setColor(QPalette::Base, QColor(kMiningOutputIdleBackground));
#if defined(Q_OS_WIN)
  palette.setColor(QPalette::Window, QColor(kMiningOutputIdleBackground));
#endif
  viewport->setAutoFillBackground(true);
  viewport->setPalette(palette);
  viewport->update();
}

void MainWindow::updateMiningOutputCinematicFrame() {
  const bool miningTabActive =
    mainTabs_ && miningTabWidget_ && mainTabs_->currentWidget() == miningTabWidget_;
  if (!txtMiningOutput_ ||
      !dinero::qt::shouldRunHashEngine(isMining_, miningTabActive, false)) {
    return;
  }

  QWidget* viewport = txtMiningOutput_->viewport();
  if (!viewport) {
    return;
  }

  const QSize frameSize = viewport->size();
  if (frameSize.width() <= 0 || frameSize.height() <= 0) {
    return;
  }

  // Embedded solo mining exposes an inexpensive, genuine sample from its
  // active header. Build the living field from those candidates rather than
  // decorative random glyphs. Other miner modes retain the legacy cinematic
  // fallback below until their protocol supplies candidate samples.
  if (minerCtrl_ && minerCtrl_->running()) {
    quint32 nonce = 0;
    quint32 difficultyBits = 0;
    int height = 0;
    QString hash;
    QString headerFields;
    if (minerCtrl_->sampleCandidate(nonce, hash, headerFields, height, difficultyBits) &&
        !hash.isEmpty()) {
      if (miningHashSamples_.isEmpty() ||
          miningHashSamples_.constLast().nonce != nonce ||
          miningHashSamples_.constLast().hash != hash) {
        miningHashSamples_.append({nonce, hash, headerFields, false, 0});
      }
      if (lblMiningHeight_) lblMiningHeight_->setText(QString::number(height));
      if (lblMiningDifficulty_) {
        lblMiningDifficulty_->setText(dinero::qt::compactDifficultyText(difficultyBits));
      }
    }

    const qreal dpr = viewport->devicePixelRatioF();
    QPixmap frame(frameSize * dpr);
    frame.setDevicePixelRatio(dpr);
    frame.fill(Qt::transparent);
    QPainter painter(&frame);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QFont font = miningConsoleFont(10, QFont::Medium);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    const int rowHeight = qMax(12, metrics.height() + 2);
    // Reserve one unobscured line for the startup/status record at the top.
    const int rows = dinero::qt::hashSampleCapacity(frameSize.height() - 32, rowHeight);
    const int capacity = rows;
    if (miningHashSamples_.size() > capacity) {
      miningHashSamples_.remove(0, miningHashSamples_.size() - capacity);
    }

    const int count = miningHashSamples_.size();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (int visual = 0; visual < count; ++visual) {
      const int sampleIndex = count - 1 - visual;
      const MiningHashSample& sample = miningHashSamples_.at(sampleIndex);
      if (sample.blockFound && sample.highlightUntilMs > nowMs) {
        painter.setPen(QColor(255, 212, 59, 255));
      } else if (sample.blockFound) {
        painter.setPen(QColor(105, 219, 124, 185));
      } else {
        painter.setPen(QColor(151, 163, 174, 150));
      }
      const QString line = sample.headerFields;
      painter.drawText(8,
                       frameSize.height() - 6 - visual * rowHeight - metrics.descent(),
                       line);
    }
    if (miningHashOverlay_) {
      miningHashOverlay_->setGeometry(viewport->rect());
      miningHashOverlay_->setPixmap(frame);
      miningHashOverlay_->show();
      miningHashOverlay_->raise();
    }
    ++miningCinematicFrame_;
    return;
  }
  if (miningHashOverlay_) {
    miningHashOverlay_->hide();
  }
  const qreal dpr = viewport->devicePixelRatioF();
  QPixmap frame(frameSize * dpr);
  frame.setDevicePixelRatio(dpr);
  frame.fill(QColor(kMiningOutputMatrixBackground));

  QPainter painter(&frame);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setRenderHint(QPainter::TextAntialiasing, false);

  QFont hashFont = miningConsoleFont(10, QFont::Medium);
  painter.setFont(hashFont);

  const QFontMetrics hashMetrics(hashFont);
  const int rowHeight = qMax(10, hashMetrics.height());
  const int charWidth = qMax(1, hashMetrics.horizontalAdvance(QLatin1Char('0')));
  const int charsPerLine = qMax(96, (frameSize.width() / charWidth) + 14);
  const int visibleRows = qMax(1, (frameSize.height() / rowHeight) + 4);
  const double verticalSpeedPxPerFrame = 0.35; // Single-field slow upward drift.
  const double totalScroll = static_cast<double>(miningCinematicFrame_) * verticalSpeedPxPerFrame;
  const int pixelOffset = static_cast<int>(std::fmod(totalScroll, static_cast<double>(rowHeight)));
  const qint64 firstRow = static_cast<qint64>(std::floor(totalScroll / static_cast<double>(rowHeight)));

  const int flickerEpoch = miningCinematicFrame_ / 2; // Slow temporal jitter bucket.

  auto lineForRow = [charsPerLine, flickerEpoch](qint64 rowIndex) -> QString {
    static const char hexChars[] = "0123456789abcdef";
    quint64 state = 0xA24BAED4963EE407ULL ^
                    (static_cast<quint64>(rowIndex) * 0x9E3779B97F4A7C15ULL);
    QString line;
    line.reserve(charsPerLine + 8);

    for (int i = 0; i < charsPerLine; ++i) {
      if (i > 0 && (i % 65) == 64) {
        line.append(QLatin1Char('|'));
        continue;
      }
      state ^= (state >> 12);
      state ^= (state << 25);
      state ^= (state >> 27);
      const quint64 mixed = state * 0x2545F4914F6CDD1DULL;
      int nibble = static_cast<int>(mixed & 0x0F);

      // Rare per-glyph mutation to make the field feel alive while scrolling.
      quint64 jitter = static_cast<quint64>(rowIndex) * 0x94D049BB133111EBULL;
      jitter ^= static_cast<quint64>(i) * 0xBF58476D1CE4E5B9ULL;
      jitter ^= static_cast<quint64>(flickerEpoch) * 0x369DEA0F31A53F85ULL;
      jitter ^= (jitter >> 27);
      if ((jitter & 0x7FULL) < 3ULL) { // ~2.3% of glyphs mutate per epoch.
        nibble = static_cast<int>((jitter >> 17) & 0x0FULL);
      }

      line.append(QLatin1Char(hexChars[nibble]));
    }
    return line;
  };

  for (int row = -1; row < visibleRows; ++row) {
    const qint64 rowIndex = firstRow + row;
    const int y = row * rowHeight - pixelOffset + hashMetrics.ascent();
    const QString line = lineForRow(rowIndex);
    const quint64 rowTintSeed = static_cast<quint64>(rowIndex) * 0xBF58476D1CE4E5B9ULL;
    const int alpha = 74 + static_cast<int>((rowTintSeed >> 19) % 34ULL);
    painter.setPen(QColor(174, 184, 194, qBound(60, alpha, 112)));
    painter.drawText(0, y, line);
  }

  // Sparse highlight layer: random bright glyph sparks + comet streaks.
  auto spawnSpark = [&](int lifetimeMin, int lifetimeMax, int streakMin, int streakMax) {
    if (charsPerLine <= 0 || visibleRows <= 0) {
      return;
    }

    MiningCinematicSpark spark;
    spark.worldRow = firstRow + static_cast<qint64>(QRandomGenerator::global()->bounded(visibleRows + 2)) - 1;
    spark.col = QRandomGenerator::global()->bounded(charsPerLine);
    if (spark.col > 0 && (spark.col % 65) == 64) {
      --spark.col; // avoid separators most of the time
    }
    spark.bornFrame = miningCinematicFrame_;
    spark.lifetimeFrames = QRandomGenerator::global()->bounded(lifetimeMin, lifetimeMax);
    spark.streakLength = QRandomGenerator::global()->bounded(streakMin, streakMax);
    miningCinematicSparks_.append(spark);
  };

  auto spawnPrimaryPulse = [&]() {
    // Replace remaining single-glyph flare behavior with extra-long comets.
    if (QRandomGenerator::global()->bounded(1000) < 500) {
      spawnSpark(14, 28, 3, 7);
    } else {
      spawnSpark(64, 108, 100, 103); // ~100-char comet body
    }
  };

  if (QRandomGenerator::global()->bounded(1000) < 430) { // ~12 pulses/sec @28fps
    spawnPrimaryPulse();
  }
  if (QRandomGenerator::global()->bounded(1000) < 120) { // occasional extra pulse
    spawnPrimaryPulse();
  }
  if (QRandomGenerator::global()->bounded(1000) < 24) {  // ~3x short comet frequency
    spawnSpark(18, 34, 3, 9);
  }

  // Additional rare long comet, boosted to roughly ~3x prior rate.
  const int longCometCooldownFrames = 28 * 7; // ~7s cooldown
  const bool longCometReady =
    (miningCinematicFrame_ - miningCinematicLastLongCometFrame_) >= longCometCooldownFrames;
  if (longCometReady && QRandomGenerator::global()->bounded(10000) < 65) {
    spawnSpark(28, 56, 18, 24); // ~20-char feel, longer persistence
    miningCinematicLastLongCometFrame_ = miningCinematicFrame_;
  }

  // Extra-long comet (~40 chars) approximately every 60 seconds.
  const int ultraCometIntervalFrames = 28 * 60;
  const bool ultraCometDue =
    (miningCinematicFrame_ - miningCinematicLastUltraCometFrame_) >= ultraCometIntervalFrames;
  if (ultraCometDue) {
    spawnSpark(46, 78, 38, 43);
    miningCinematicLastUltraCometFrame_ = miningCinematicFrame_;
  }

  // Keep a bounded, active spark set.
  QVector<MiningCinematicSpark> activeSparks;
  activeSparks.reserve(miningCinematicSparks_.size());
  for (const MiningCinematicSpark& spark : std::as_const(miningCinematicSparks_)) {
    const int age = miningCinematicFrame_ - spark.bornFrame;
    if (age >= 0 && age < spark.lifetimeFrames) {
      activeSparks.append(spark);
    }
  }
  if (activeSparks.size() > 120) {
    activeSparks = activeSparks.mid(activeSparks.size() - 120);
  }
  miningCinematicSparks_ = activeSparks;

  for (const MiningCinematicSpark& spark : std::as_const(miningCinematicSparks_)) {
    const int age = miningCinematicFrame_ - spark.bornFrame;
    const double lifeT = qBound(0.0,
                                static_cast<double>(age) / qMax(1, spark.lifetimeFrames),
                                1.0);
    const double fade = 1.0 - lifeT;
    if (fade <= 0.0) {
      continue;
    }

    const int baseRow = static_cast<int>(spark.worldRow - firstRow);
    const int baseY = baseRow * rowHeight - pixelOffset + hashMetrics.ascent();
    if (baseY < -rowHeight || baseY > frameSize.height() + rowHeight) {
      continue;
    }

    const QString rowLine = lineForRow(spark.worldRow);
    const int safeCol = qBound(0, spark.col, qMax(0, rowLine.size() - 1));
    const QChar glyph = rowLine.isEmpty() ? QLatin1Char('a') : rowLine.at(safeCol);
    const int x = safeCol * charWidth;

    const int glowAlpha = qBound(0, static_cast<int>(150.0 * fade), 255);
    const int coreAlpha = qBound(0, static_cast<int>(255.0 * fade), 255);

    painter.setPen(QColor(197, 207, 216, glowAlpha));
    painter.drawText(x - 1, baseY, QString(glyph));
    painter.drawText(x + 1, baseY, QString(glyph));
    painter.drawText(x, baseY - 1, QString(glyph));
    painter.drawText(x, baseY + 1, QString(glyph));

    painter.setPen(QColor(241, 245, 249, coreAlpha));
    painter.drawText(x, baseY, QString(glyph));

    // Optional short downward tail for comet-like streaks.
    if (spark.streakLength > 1) {
      for (int t = 1; t < spark.streakLength; ++t) {
        const qint64 tailRowWorld = spark.worldRow + t;
        const int tailRow = static_cast<int>(tailRowWorld - firstRow);
        const int tailY = tailRow * rowHeight - pixelOffset + hashMetrics.ascent();
        if (tailY < -rowHeight || tailY > frameSize.height() + rowHeight) {
          continue;
        }

        const QString tailLine = lineForRow(tailRowWorld);
        const int tailCol = qBound(0, spark.col, qMax(0, tailLine.size() - 1));
        const QChar tailGlyph = tailLine.isEmpty() ? glyph : tailLine.at(tailCol);
        const double tailFade = fade * (1.0 - (static_cast<double>(t) / (spark.streakLength + 1.0)));
        const int tailAlpha = qBound(0, static_cast<int>(170.0 * tailFade), 255);

        painter.setPen(QColor(184, 194, 203, tailAlpha));
        painter.drawText(tailCol * charWidth, tailY, QString(tailGlyph));
      }
    }
  }

  // ── Motto ticker as floating overlay (not in background pixmap) ──
  {
    const QString motto = QStringLiteral("Dinero: Real Money\u2014For Free People");
    if (!mottoTickerLabel_) {
      mottoTickerLabel_ = new QLabel(viewport);
      mottoTickerLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
      mottoTickerLabel_->setStyleSheet(
        "background: transparent; color: #ffbd4a; font-size: 12pt; font-weight: 600;");
    }
    mottoTickerLabel_->setText(motto);
    mottoTickerLabel_->adjustSize();
    const int mottoWidth = mottoTickerLabel_->width();
    const double travel = static_cast<double>(frameSize.width() + mottoWidth + 24);
    const double tick = std::fmod(static_cast<double>(miningCinematicFrame_) * 2.55, travel);
    const int tickerX = static_cast<int>(tick) - mottoWidth;
    const int tickerY = frameSize.height() - mottoTickerLabel_->height() - 4;
    mottoTickerLabel_->move(tickerX, tickerY);
    mottoTickerLabel_->show();
    mottoTickerLabel_->raise();
  }

  QPalette palette = viewport->palette();
  palette.setBrush(QPalette::Base, QBrush(frame));
#if defined(Q_OS_WIN)
  palette.setBrush(QPalette::Window, QBrush(frame));
#endif
  viewport->setAutoFillBackground(true);
  viewport->setPalette(palette);
  viewport->update();

  ++miningCinematicFrame_;
}

void MainWindow::updateStratumIdentityLabel() {
  if (!lblStratumIdentity_) return;

  const bool poolMode = (currentMiningMode() == "pool");
  if (!poolMode) {
    lblStratumIdentity_->setText("");
    return;
  }

  QString user = "";
  if (edtMiningAddress_) {
    user = edtMiningAddress_->text().trimmed();
  }
  if (user.isEmpty()) {
    user = "<address>";
  }

  QString endpoint = explicitStratumEndpoint();
  if (edtStratumEndpoint_) {
    const QString uiEndpoint = edtStratumEndpoint_->text().trimmed();
    if (!uiEndpoint.isEmpty()) {
      endpoint = uiEndpoint;
    }
  }
  if (endpoint.isEmpty()) {
    endpoint = "<set endpoint>";
  }

  lblStratumIdentity_->setText(
    QString("Pool identity: user=%1, pass=x | endpoint=%2").arg(user, endpoint));
}

void MainWindow::onBrowseDaemonBinary() {
  QString defaultPath = QCoreApplication::applicationDirPath();

  // Try to find dinerod in common locations first
  QStringList searchPaths;
#ifdef Q_OS_MAC
  searchPaths = {
    defaultPath + "/../../../dinerod",
    defaultPath + "/../Resources/dinerod",
    defaultPath + "/dinerod"
  };
#elif defined(Q_OS_WIN)
  searchPaths = {
    defaultPath + "/dinerod.exe",
    defaultPath + "/../dinerod.exe"
  };
#else
  searchPaths = {
    defaultPath + "/dinerod",
    defaultPath + "/../dinerod"
  };
#endif

  // Check if we have a saved path
  QString savedPath = QSettings().value("daemon/custom_path").toString();
  if (savedPath.contains("/AppTranslocation/", Qt::CaseInsensitive) || !QFile::exists(savedPath)) {
    QSettings().remove("daemon/custom_path");
    savedPath.clear();
  }
  if (!savedPath.isEmpty() && QFile::exists(savedPath)) {
    defaultPath = QFileInfo(savedPath).absolutePath();
  } else {
    // Check common paths
    for (const QString& path : searchPaths) {
      if (QFile::exists(path)) {
        defaultPath = QFileInfo(path).absolutePath();
        edtDaemonPath_->setText(QFileInfo(path).absoluteFilePath());
        if (btnBrowseDaemon_) {
          btnBrowseDaemon_->setToolTip(QString("Daemon binary: %1").arg(QFileInfo(path).fileName()));
        }
        break;
      }
    }
  }

  QString filter;
#ifdef Q_OS_WIN
  filter = "Executables (dinerod.exe);;All Files (*)";
#else
  filter = "Executables (dinerod);;All Files (*)";
#endif

  QString path = QFileDialog::getOpenFileName(
    this,
    "Select Dinerod Daemon Binary",
    defaultPath,
    filter
  );

  if (!path.isEmpty()) {
    edtDaemonPath_->setText(path);
    if (btnBrowseDaemon_) {
      btnBrowseDaemon_->setToolTip(QString("Daemon binary: %1").arg(QFileInfo(path).fileName()));
    }
    // Save to settings for next time
    QSettings().setValue("daemon/custom_path", path);
    qDebug() << "Custom daemon path set to:" << path;
  }
}

void MainWindow::updateMiningStats() {
  // =========================================================================
  // EXTERNAL MINER STATS - Display process state, NOT invented data
  // =========================================================================
  // For external miners, the daemon doesn't know mining stats.
  // We can only display: (1) process running state, (2) uptime since start
  // Hashrate/blocks come from miner stdout and are labeled as "miner-reported"
  // =========================================================================

  if (!miningProcess_ || !lblMiningUptime_ || !lblMiningStatus_) {
    return;
  }

  // Check if external miner process is actually running
  bool processRunning = (miningProcess_->state() == QProcess::Running);

  if (!processRunning) {
    // Process stopped - update UI to reflect reality
    isMining_ = false;
    lblMiningStatus_->setText(miningStatusInactiveText());
    lblMiningStatus_->setStyleSheet(chromePillStyle());
    setMiningOutputCinematicEnabled(false);
    return;
  }

  updateMiningRuntimeLabel();

  // Query daemon for verifiable Stratum V1 external miner activity.
  // SV2 has its own stdout/JSON telemetry and can run without daemon-side
  // external miner stats, so avoid a meaningless RPC there.
  if (!activeMinerType_.startsWith("sv2_pool")) {
    rpc_->call("mining.getexternalstats", QJsonArray{});
  }
}

void MainWindow::updateMiningRuntimeLabel() {
  if (!lblMiningUptime_) {
    return;
  }

  if (mining_stats_.mining_started <= 0) {
    lblMiningUptime_->setText("-");
    lblMiningUptime_->setStyleSheet("QLabel { color: #868e96; }");
    return;
  }

  const qint64 elapsedMs =
      std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() - mining_stats_.mining_started);
  const int seconds = static_cast<int>(elapsedMs / 1000);
  const int hours = seconds / 3600;
  const int minutes = (seconds % 3600) / 60;
  const int secs = seconds % 60;

  QString runtime;
  if (hours > 0) {
    runtime = QString("%1h %2m %3s").arg(hours).arg(minutes).arg(secs);
  } else if (minutes > 0) {
    runtime = QString("%1m %2s").arg(minutes).arg(secs);
  } else {
    runtime = QString("%1s").arg(secs);
  }

  lblMiningUptime_->setText(runtime);
  lblMiningUptime_->setStyleSheet("QLabel { color: #d6dde6; font-weight: bold; }");
}

// Handle external miner stats response from daemon
void MainWindow::handleExternalMinerStats(const QJsonObject& stats) {
  if (!lblHashrate_ || !lblBlocksFound_) return;

  // Protocol event counters from daemon (verifiable, not invented)
  int templates = stats["templates_served"].toInt(0);
  int lastAgo = stats["last_template_ago"].toInt(-1);
  bool active = stats["external_miner_active"].toBool(false);

  // Block submission stats from MetricsRegistry (daemon-verified)
  int submitAttempts = stats["submit_attempts"].toInt(0);
  int acceptedBlocks = stats["accepted_blocks"].toInt(0);
  int rejectedBlocks = stats["rejected_blocks"].toInt(0);

  // Display verifiable protocol events (NOT invented hashrate)
  QString statusText;
  if (active) {
    statusText = QString("⛏️ External miner active | Templates: %1 | Last: %2s ago")
                   .arg(templates)
                   .arg(lastAgo);
    lblHashrate_->setStyleSheet("QLabel { color: #d6dde6; font-weight: bold; }");
  } else if (templates > 0) {
    statusText = QString("⏸️ External miner idle | Templates: %1 | Last: %2s ago")
                   .arg(templates)
                   .arg(lastAgo);
    lblHashrate_->setStyleSheet("QLabel { color: #b7c0ca; }");
  } else {
    statusText = "📋 No external miner detected (waiting for getblocktemplate)";
    lblHashrate_->setStyleSheet("QLabel { color: #868e96; font-style: italic; }");
  }

  lblHashrate_->setText(statusText);

  // Display daemon-verified block submission stats
  QString lastReason = stats["last_rejection_reason"].toString();

  if (submitAttempts > 0 || acceptedBlocks > 0 || rejectedBlocks > 0) {
    QString blocksText = QString("Submitted: %1 | Accepted: %2 | Rejected: %3")
                           .arg(submitAttempts)
                           .arg(acceptedBlocks)
                           .arg(rejectedBlocks);

    // Show last rejection reason if there were rejections
    if (rejectedBlocks > 0 && !lastReason.isEmpty()) {
      blocksText += QString(" (last: %1)").arg(lastReason);
    }

    lblBlocksFound_->setText(blocksText);

    // Color: green if accepted, red if rejections, neutral otherwise
    if (acceptedBlocks > 0 && rejectedBlocks == 0) {
      lblBlocksFound_->setStyleSheet("QLabel { color: #d6dde6; font-weight: bold; }");
    } else if (rejectedBlocks > 0) {
      lblBlocksFound_->setStyleSheet("QLabel { color: #b7c0ca; }");
    } else {
      lblBlocksFound_->setStyleSheet("");
    }
  } else {
    lblBlocksFound_->setText("No block submissions yet");
    lblBlocksFound_->setStyleSheet("QLabel { color: #868e96; font-style: italic; }");
  }

  // Build detailed tooltip with rejection reasons breakdown
  QString tooltip = "External miner stats from daemon protocol events:\n"
                    "• Templates served = getblocktemplate requests\n"
                    "• Active = template requested within 60 seconds\n"
                    "• Submit attempts = submitblock calls to daemon\n"
                    "• Accepted/Rejected = consensus validation result";

  // Add rejection reasons breakdown if available
  QJsonObject rejectionReasons = stats["rejection_reasons"].toObject();
  if (!rejectionReasons.isEmpty()) {
    tooltip += "\n\nRejection breakdown:";
    for (const QString& reason : rejectionReasons.keys()) {
      int count = rejectionReasons[reason].toInt();
      tooltip += QString("\n  • %1: %2").arg(reason).arg(count);
    }
  }

  lblHashrate_->setToolTip(tooltip);
}

// Phase Y: Poll integrated CPU miner status via RPC
void MainWindow::updateMiningStatsRPC() {
  // Call mining.getstatus RPC to get current stats
  rpc_->call("mining.getstatus", QJsonArray());
}

void MainWindow::parseMiningOutput(const QString& line) {
  // CRITICAL: Check if widgets still exist before accessing them
  if (!lblBlocksFound_ || !lblMiningStatus_ || !lblCurrentHash_ || !lblTotalHashes_ || !lblHashrate_) {
    return;
  }
  
  // =========================================================================
  // TRUTHFUL OUTPUT PARSING - GUI displays miner output, does NOT invent data
  // =========================================================================
  // The GUI NEVER decides if a block was accepted. Only the daemon knows.
  // We display miner output verbatim and query daemon for authoritative stats.
  // =========================================================================

  // Block candidate detection (informational only - NOT a confirmed acceptance)
  // The miner may report "block found" but consensus may reject it.
  // DO NOT increment blocks_found counter here - that's daemon authority.
  if (line.contains("Block found", Qt::CaseInsensitive) ||
      line.contains("New block mined", Qt::CaseInsensitive) ||
      line.contains("accepted", Qt::CaseInsensitive)) {

    // Show visual feedback but make clear this is unconfirmed
    lblMiningStatus_->setText(miningStatusActiveText() + " | Candidate solved - verifying with consensus...");
    lblMiningStatus_->setStyleSheet(chromePillStyle());

    // After 3 seconds, query daemon for actual chain state
    // The daemon is the ONLY authority on block acceptance
    QTimer::singleShot(3000, this, [this]() {
      rpc_->call("getblockchaininfo", QJsonArray{});
      // Result handled in onRpcResult - will update UI with real chain tip
      lblMiningStatus_->setText(miningStatusActiveText());
      lblMiningStatus_->setStyleSheet(chromePillStyle());
    });

    // Reload transaction history to check for new coinbase rewards
    QTimer::singleShot(5000, this, &MainWindow::loadTransactionHistory);
  }

  // Hashrate display from miner output (informational - miner-reported, not daemon)
  // NOTE: This is the miner's self-reported hashrate, displayed as-is
  // For authoritative stats, use mining.getstatus RPC (internal miner only)
  QRegularExpression hashRegex(R"((\d+\.?\d*)\s*(MH/s|H/s|KH/s|GH/s))");
  QRegularExpressionMatch match = hashRegex.match(line);
  if (match.hasMatch()) {
    QString hashStr = match.captured(1) + " " + match.captured(2);
    mining_stats_.current_hashrate = parseHashrateToHps(match.captured(1), match.captured(2));
    // Display miner-reported hashrate (clearly labeled as miner output)
    lblCurrentHash_->setText(hashStr);
    lblCurrentHash_->setToolTip("Hashrate reported by external miner (self-reported)");
    setOverviewLocalHashrate(mining_stats_.current_hashrate,
                             "Local hashrate reported by the active miner process");
  }

  // GPU miner session blocks-found (from stats line: "... | Blocks: 5")
  // Authoritative: GPU miner only increments after mining.submit returns accepted
  QRegularExpression blocksRegex(R"(Blocks:\s*(\d+))");
  QRegularExpressionMatch blocksMatch = blocksRegex.match(line);
  if (blocksMatch.hasMatch()) {
    int gpu_blocks = blocksMatch.captured(1).toInt();
    mining_stats_.blocks_found = gpu_blocks;
    if (lblBlocksFound_) {
      lblBlocksFound_->setText(QString::number(gpu_blocks));
    }
  }

  QRegularExpression totalHashesRegex(R"(Total:\s*(\d+)\s*MH)");
  QRegularExpressionMatch totalMatch = totalHashesRegex.match(line);
  if (totalMatch.hasMatch()) {
    bool ok = false;
    const qint64 totalMegaHashes = totalMatch.captured(1).toLongLong(&ok);
    if (ok) {
      mining_stats_.total_hashes = totalMegaHashes * 1000000LL;
      lblTotalHashes_->setText(QString("%1 hashes").arg(QLocale().toString(mining_stats_.total_hashes)));
    }
  }

  QRegularExpression gpuDeviceCountRegex(R"(\[GPU\]\s+Found\s+(\d+)\s+device)");
  QRegularExpressionMatch gpuDeviceCountMatch = gpuDeviceCountRegex.match(line);
  if (gpuDeviceCountMatch.hasMatch()) {
    overviewGpuTelemetrySeen_ = true;
    overviewGpuDeviceCount_ = gpuDeviceCountMatch.captured(1).toInt();
    updateOverviewHardwareTelemetry();
  }

  QRegularExpression gpuBackendRegex(R"(\[GPU\]\s+Selected backend:\s+([A-Za-z0-9_+-]+))");
  QRegularExpressionMatch gpuBackendMatch = gpuBackendRegex.match(line);
  if (gpuBackendMatch.hasMatch()) {
    overviewGpuTelemetrySeen_ = true;
    overviewGpuBackend_ = gpuBackendMatch.captured(1).trimmed();
    updateOverviewHardwareTelemetry();
  }

  QRegularExpression gpuReadyRegex(R"(\[GPU\]\s+Ready to mine on\s+(.+)\s+\(([^)]+)\))");
  QRegularExpressionMatch gpuReadyMatch = gpuReadyRegex.match(line);
  if (gpuReadyMatch.hasMatch()) {
    overviewGpuTelemetrySeen_ = true;
    overviewGpuDevice_ = gpuReadyMatch.captured(1).trimmed();
    overviewGpuBackend_ = gpuReadyMatch.captured(2).trimmed();
    updateOverviewHardwareTelemetry();
  }

  QRegularExpression gpuSummaryRegex(R"(GPU READY:\s+(.+)\s+\(([^)]+)\))");
  QRegularExpressionMatch gpuSummaryMatch = gpuSummaryRegex.match(line);
  if (gpuSummaryMatch.hasMatch()) {
    overviewGpuTelemetrySeen_ = true;
    overviewGpuDevice_ = gpuSummaryMatch.captured(1).trimmed();
    overviewGpuBackend_ = gpuSummaryMatch.captured(2).trimmed();
    updateOverviewHardwareTelemetry();
  }
}

void MainWindow::loadTransactionHistory() {
  QString txType = "all";
  if (cmbTxTypeFilter_) {
    txType = cmbTxTypeFilter_->currentData().toString();
    if (txType.isEmpty()) {
      txType = "all";
    }
  }

  if (txType == "private" || txType == "confsend" || txType == "confreceive" || txType == "contract") {
    txType = "all";
  }

  QJsonObject params{
    {"count", 200},
    {"offset", 0},
    {"type", txType}
  };

  qDebug() << "Loading transaction history with type filter:" << txType;
  rpc_->callNamed("wallet.listtransactions", params);
}

void MainWindow::updateTransactionTable(const QJsonArray& transactions) {
  tblTransactions_->setSortingEnabled(false); // Disable while updating
  tblTransactions_->setRowCount(0); // Clear existing rows
  
  qDebug() << "Updating transaction table with" << transactions.size() << "transactions";
  const QString activeFilter = cmbTxTypeFilter_ ? cmbTxTypeFilter_->currentData().toString() : QStringLiteral("all");
  
  for (const QJsonValue& txVal : transactions) {
    if (!txVal.isObject()) continue;
    
    QJsonObject tx = txVal.toObject();
    
    // Extract transaction data
    QString txid = tx["txid"].toString();
    QString type = tx["type"].toString();
    if (type.isEmpty()) {
      type = tx["category"].toString("unknown");
    }
    type = type.toLower();
    const bool amountHidden = tx["amount_hidden"].toBool(false);
    const QString displayAmount = tx["display_amount"].toString();
    double amount = tx["amount"].toDouble(0.0);
    int confirmations = tx["confirmations"].toInt(0);
    qint64 timestamp = tx["time"].toVariant().toLongLong();
    QString address = tx["address"].toString("-");
    
    // Format date/time
    QDateTime dateTime = QDateTime::fromSecsSinceEpoch(timestamp);
    QString date = dateTime.toString("yyyy-MM-dd");
    QString time = dateTime.toString("HH:mm:ss");
    
    // Determine type icon and style
    QString typeIcon;
    QString amountStr;
    QColor amountColor;
    QString uiTypeKey = type;

    // v7: only transparent CTV covenants are recognized as "contract" txs.
    const bool hasCovenant = tx.contains("covenant_script") || tx.contains("covenant");
    const bool isPublicCovenant = type == "sendpubliccovenant" ||
                                  (tx["visibility"].toString() == "public" && hasCovenant);

    if (isPublicCovenant) {
      typeIcon = amount < 0.0
        ? QString::fromUtf8("\xF0\x9F\x93\x9C Contract Created")
        : QString::fromUtf8("\xF0\x9F\x93\x9C Contract");
      amountStr = QString("%1%2").arg(amount < 0.0 ? "-" : "+").arg(qAbs(amount), 0, 'f', 8);
      amountColor = QColor("#4dabf7");  // Blue for public contracts
      uiTypeKey = "contract";
    } else if (type == "sent" || type == "send") {
      typeIcon = "📤 Send";
      amountStr = QString("-%1").arg(qAbs(amount), 0, 'f', 8);
      amountColor = QColor("#ff6b6b"); // Red for send
      uiTypeKey = "sent";
    } else if (type == "received" || type == "receive") {
      typeIcon = "📥 Receive";
      amountStr = QString("+%1").arg(qAbs(amount), 0, 'f', 8);
      amountColor = QColor("#51cf66"); // Green for receive
      uiTypeKey = "received";
    } else if (type == "mined" || type == "generate" || type == "immature" ||
               type == "mining" || type == "coinbase") {
      typeIcon = "⛏️ Mined";
      amountStr = QString("+%1").arg(qAbs(amount), 0, 'f', 8);
      amountColor = QColor("#339af0"); // Blue for mining
      uiTypeKey = "mined";
    } else {
      typeIcon = "❓ " + type;
      amountStr = amountHidden && !displayAmount.isEmpty() ? displayAmount : QString::number(amount, 'f', 8);
      amountColor = QColor("#868e96"); // Gray for unknown
    }

    if ((activeFilter == "sent" || activeFilter == "received" ||
         activeFilter == "mined" || activeFilter == "contract") && uiTypeKey != activeFilter) {
      continue;
    }
    
    // Confirmation status
    QString confirmStatus;
    if (confirmations == 0) {
      confirmStatus = "⏳ Pending";
    } else if (confirmations < 6) {
      confirmStatus = QString("🔄 %1/6").arg(confirmations);
    } else {
      confirmStatus = QString("✅ %1").arg(confirmations);
    }
    
    // Add row
    int row = tblTransactions_->rowCount();
    tblTransactions_->insertRow(row);
    
    // Date
    auto *dateItem = new QTableWidgetItem(date);
    dateItem->setData(Qt::UserRole, timestamp); // Store timestamp for sorting
    tblTransactions_->setItem(row, 0, dateItem);
    
    // Time
    tblTransactions_->setItem(row, 1, new QTableWidgetItem(time));
    
    // Type
    auto *typeItem = new QTableWidgetItem(typeIcon);
    tblTransactions_->setItem(row, 2, typeItem);
    
    // Amount
    auto *amountItem = new QTableWidgetItem(amountStr);
    amountItem->setData(Qt::UserRole, amount); // Store number for sorting
    amountItem->setForeground(QBrush(amountColor));
    QFont amountFont("monospace");
    amountFont.setBold(true);
    amountItem->setFont(amountFont);
    tblTransactions_->setItem(row, 3, amountItem);
    
    // Address
    tblTransactions_->setItem(row, 4, new QTableWidgetItem(address));
    
    // Confirmations
    tblTransactions_->setItem(row, 5, new QTableWidgetItem(confirmStatus));
    
    // TxID
    auto *txidItem = new QTableWidgetItem(txid.left(16) + "...");
    txidItem->setToolTip(txid); // Full TxID in tooltip
    txidItem->setData(Qt::UserRole, txid); // Store full txid
    tblTransactions_->setItem(row, 6, txidItem);
  }
  
  // Sort by date (newest first)
  tblTransactions_->sortByColumn(0, Qt::DescendingOrder);
  tblTransactions_->setSortingEnabled(true);
  
  qDebug() << "Transaction table updated with" << tblTransactions_->rowCount() << "rows";
}

bool MainWindow::isUtxoTabActive() const {
  return mainTabs_ && utxoTabWidget_ && mainTabs_->currentWidget() == utxoTabWidget_;
}

void MainWindow::requestUtxoRefresh(bool explicitRefresh) {
  if (!rpc_ || utxoRequestPending_ ||
      !dinero::qt::shouldPollUtxos(isUtxoTabActive(), explicitRefresh)) {
    return;
  }
  utxoRequestPending_ = true;
  rpc_->call("wallet.listunspent", QJsonArray());
}

void MainWindow::renderUtxoPage() {
  if (!tblUTXOs_ || !isUtxoTabActive()) return;
  updateUTXOTable(cachedUtxos_);
}

void MainWindow::updateUTXOTable(const QJsonArray& utxos) {
  if (!tblUTXOs_ || !isUtxoTabActive()) {
    return;
  }

  const auto page = dinero::qt::paginateUtxos(utxos, currentUtxoPage_);
  currentUtxoPage_ = page.page_index;
  auto* tblUTXOs = tblUTXOs_;
  if (!tblUTXOs) {
    qWarning() << "tblUTXOs not found!";
    return;
  }
  
  tblUTXOs->setSortingEnabled(false); // Disable while updating
  tblUTXOs->setRowCount(0); // Clear existing rows
  
  qDebug() << "Updating UTXO page" << page.page_index + 1 << "of"
           << page.page_count << "from" << page.total_rows << "UTXOs";
  
  double totalAmount = 0.0;
  
  for (const QJsonValue& utxoVal : page.rows) {
    if (!utxoVal.isObject()) continue;
    
    QJsonObject utxo = utxoVal.toObject();
    
    // Extract UTXO data
    QString txid = utxo["txid"].toString();
    int vout = utxo["vout"].toInt(0);
    double amount = utxo["amount"].toDouble(0.0);
    int confirmations = utxo["confirmations"].toInt(0);
    QString address = utxo["address"].toString("-");
    bool spendable = utxo["spendable"].toBool(true);
    bool isCoinbase = utxo["is_coinbase"].toBool(false);
    bool isMature = utxo["is_mature"].toBool(true);
    int maturityRemaining = utxo["maturity_remaining"].toInt(0);
    
    totalAmount += amount;
    
    // Add row
    int row = tblUTXOs->rowCount();
    tblUTXOs->insertRow(row);
    
    // TxID (shortened with tooltip)
    auto *txidItem = new QTableWidgetItem(txid.left(16) + "...");
    txidItem->setToolTip(txid); // Full TxID in tooltip
    txidItem->setData(Qt::UserRole, txid);
    tblUTXOs->setItem(row, 0, txidItem);
    
    // Vout
    auto *voutItem = new QTableWidgetItem(QString::number(vout));
    voutItem->setData(Qt::UserRole, vout);
    tblUTXOs->setItem(row, 1, voutItem);
    
    // Amount
    auto *amountItem = new QTableWidgetItem(QString::number(amount, 'f', 8) + " DIN");
    amountItem->setData(Qt::UserRole, amount);
    amountItem->setForeground(QBrush(QColor("#51cf66"))); // Green
    QFont amountFont("monospace");
    amountFont.setBold(true);
    amountItem->setFont(amountFont);
    tblUTXOs->setItem(row, 2, amountItem);
    
    // Confirmations
    QString confirmStatus;
    if (confirmations == 0) {
      confirmStatus = "⏳ 0";
    } else if (confirmations < 6) {
      confirmStatus = QString("🔄 %1").arg(confirmations);
    } else {
      confirmStatus = QString("✅ %1").arg(confirmations);
    }
    tblUTXOs->setItem(row, 3, new QTableWidgetItem(confirmStatus));

    // Maturity (column 4) - show progress bar for coinbase UTXOs
    QString maturityStatus;
    if (!isCoinbase) {
      // Regular transaction - always mature
      maturityStatus = "-";
    } else if (isMature) {
      // Coinbase with >= 100 confirmations
      maturityStatus = "✅ Mature";
    } else {
      // Coinbase with < 100 confirmations - show progress
      int progress = confirmations;
      int required = 100;
      double percent = (progress * 100.0) / required;

      // Create visual progress bar using blocks
      int barLength = 10;
      int filled = (progress * barLength) / required;
      QString progressBar;
      for (int i = 0; i < barLength; i++) {
        progressBar += (i < filled) ? "█" : "░";
      }

      maturityStatus = QString("⏳ %1/%2 [%3] %4%")
        .arg(progress)
        .arg(required)
        .arg(progressBar)
        .arg(static_cast<int>(percent));
    }

    auto *maturityItem = new QTableWidgetItem(maturityStatus);
    if (isCoinbase && !isMature) {
      maturityItem->setForeground(QBrush(QColor("#fab005"))); // Orange for immature
      maturityItem->setToolTip(QString("Coinbase requires 100 confirmations. %1 blocks remaining (~%2 minutes)")
        .arg(maturityRemaining)
        .arg(maturityRemaining * 3)); // Assuming 3 minute block time
    } else if (isCoinbase && isMature) {
      maturityItem->setForeground(QBrush(QColor("#51cf66"))); // Green for mature
      maturityItem->setToolTip("Coinbase output is fully mature and spendable");
    } else {
      maturityItem->setToolTip("Regular transaction (not coinbase)");
    }
    tblUTXOs->setItem(row, 4, maturityItem);

    // Address (column 5)
    auto *addrItem = new QTableWidgetItem(address.left(20) + "...");
    addrItem->setToolTip(address);
    tblUTXOs->setItem(row, 5, addrItem);

    // Spendable (column 6)
    QString spendableIcon = spendable ? "✅ Yes" : "🔒 No";
    auto *spendableItem = new QTableWidgetItem(spendableIcon);
    spendableItem->setForeground(QBrush(spendable ? QColor("#51cf66") : QColor("#868e96")));
    tblUTXOs->setItem(row, 6, spendableItem);
  }
  
  // Sort by amount (largest first)
  tblUTXOs->sortByColumn(2, Qt::DescendingOrder);
  tblUTXOs->setSortingEnabled(true); // Re-enable sorting
  
  // If no UTXOs, show helpful message
  if (page.total_rows == 0) {
    tblUTXOs->setRowCount(1);
    auto *item = new QTableWidgetItem("No UTXOs found. Start mining or receive DIN to see unspent outputs!");
    item->setForeground(QBrush(QColor("#868e96")));
    item->setTextAlignment(Qt::AlignCenter);
    tblUTXOs->setItem(0, 0, item);
    tblUTXOs->setSpan(0, 0, 1, 7);  // Updated for 7 columns
  } else {
    qDebug() << "Total UTXO amount:" << QString::number(totalAmount, 'f', 8) << "DIN";
  }

  // Track UTXO count and update Consolidate button visibility
  cachedUtxoCount_ = page.total_rows;
  if (lblUtxoPage_) {
    const int first = page.total_rows == 0 ? 0 : page.first_row + 1;
    const int last = page.first_row + page.rows.size();
    lblUtxoPage_->setText(QString("Showing %1–%2 of %3 · Page %4 of %5")
      .arg(first).arg(last).arg(page.total_rows)
      .arg(page.page_index + 1).arg(page.page_count));
  }
  if (btnPrevUtxoPage_) btnPrevUtxoPage_->setEnabled(page.page_index > 0);
  if (btnNextUtxoPage_) btnNextUtxoPage_->setEnabled(page.page_index + 1 < page.page_count);
  if (btnConsolidate_) {
    if (cachedUtxoCount_ > 50) {
      btnConsolidate_->setText(QString("\xF0\x9F\xA7\xB9 Consolidate (%1 UTXOs)").arg(cachedUtxoCount_));
      btnConsolidate_->setVisible(true);
    } else {
      btnConsolidate_->setVisible(false);
    }
  }
}

// ═══════════════════════════════════════════════════════════════════
// Phase 4: Contracts Management Tab
// ═══════════════════════════════════════════════════════════════════

void MainWindow::refreshContractsList() {
  // Reuse wallet.listtransactions — the result handler checks pendingContractsRefresh_
  pendingContractsRefresh_ = true;
  QJsonObject params{
    {"count", 200},
    {"offset", 0},
    {"type", "all"}
  };
  rpc_->callNamed("wallet.listtransactions", params);
}

void MainWindow::updateContractsTable(const QJsonValue& txList) {
  if (!tblContracts_) return;
  tblContracts_->setRowCount(0);

  int contractCount = 0;
  double totalLocked = 0.0;

  QJsonArray txs = txList.toArray();
  for (const auto& txVal : txs) {
    QJsonObject tx = txVal.toObject();

    // Detect contract transactions by version, covenant fields, or classification
    int version = tx.value("tx_version").toInt(0);
    bool hasCovenant = tx.contains("covenant_script") || tx.contains("covenant");
    QString classification = tx.value("classification").toString();
    bool isContract = (version == 4) || hasCovenant ||
                     classification.contains("covenant") || classification.contains("contract");

    if (!isContract) continue;

    contractCount++;
    int row = tblContracts_->rowCount();
    tblContracts_->insertRow(row);

    // Type column
    QString type = "Simple Lock";
    if (tx.contains("covenant_type")) type = tx.value("covenant_type").toString();
    tblContracts_->setItem(row, 0, new QTableWidgetItem(type));

    // Visibility column
    QString visibility = "Private";
    if (version == 4) visibility = "Private";
    else if (version == 3) visibility = "Confidential";
    else visibility = "Public";
    tblContracts_->setItem(row, 1, new QTableWidgetItem(visibility));

    // Amount column
    double amount = tx.value("amount").toDouble(0);
    QString amountStr;
    if (version >= 3) {
      amountStr = "confidential";
    } else {
      amountStr = QString::number(qAbs(amount), 'f', 8) + " DIN";
    }
    auto *amountItem = new QTableWidgetItem(amountStr);
    QFont amountFont("monospace");
    amountFont.setBold(true);
    amountItem->setFont(amountFont);
    tblContracts_->setItem(row, 2, amountItem);

    // Created column (block height or time)
    int height = tx.value("blockheight").toInt(tx.value("height").toInt(0));
    int confirmations = tx.value("confirmations").toInt(0);
    QString created;
    if (height > 0) {
      created = "Block " + QString::number(height);
    } else {
      created = "Unconfirmed";
    }
    tblContracts_->setItem(row, 3, new QTableWidgetItem(created));

    // Status column
    QString status;
    if (confirmations == 0) {
      status = "Pending";
    } else if (confirmations < 6) {
      status = "Confirming (" + QString::number(confirmations) + "/6)";
    } else {
      bool spent = tx.value("spent").toBool(false);
      if (spent) {
        status = "Spent";
      } else {
        status = "Active";
      }
    }
    auto *statusItem = new QTableWidgetItem(status);
    if (status == "Active") statusItem->setForeground(QColor("#2ecc71"));
    else if (status == "Spent") statusItem->setForeground(QColor("#888"));
    else if (status == "Pending") statusItem->setForeground(QColor("#f39c12"));
    tblContracts_->setItem(row, 4, statusItem);

    // Actions column — Withdraw button for active contracts
    if (status == "Active") {
      auto *btnWithdraw = new QPushButton("Withdraw");
      btnWithdraw->setStyleSheet(chromeButtonStyle());
      btnWithdraw->setToolTip("Spend from this contract (requires satisfying the covenant conditions)");
      QString txid = tx.value("txid").toString();
      int txVersion = version;
      double contractAmount = qAbs(amount);
      int contractVout = tx.value("vout").toInt(0);
      connect(btnWithdraw, &QPushButton::clicked, this, [this, txid, contractAmount, contractVout]() {
        // v7: only transparent CTV vaults; private (ring-covenant) vaults are removed.
        bool ok = false;
        QString dest = QInputDialog::getText(this, "Spend Contract",
          QString("Contract: %1 vout:%2\nAmount: %3 DIN\n\nEnter destination address:")
            .arg(txid.left(16) + "...").arg(contractVout).arg(contractAmount, 0, 'f', 8),
          QLineEdit::Normal, QString(), &ok);
        if (!ok || dest.trimmed().isEmpty()) return;

        QMessageBox confirmBox(this);
        confirmBox.setWindowTitle("Confirm Contract Spend");
        confirmBox.setText(QString(
          "<b>Spend Contract</b><br><br>"
          "<b>Contract UTXO:</b> %1:%2<br>"
          "<b>Amount:</b> %3 DIN<br>"
          "<b>Destination:</b> %4<br><br>"
          "The full contract balance (minus network fee) will be sent to the destination.")
          .arg(txid.left(24) + "...").arg(contractVout)
          .arg(contractAmount, 0, 'f', 8).arg(dest.left(32) + "..."));
        confirmBox.setIcon(QMessageBox::Question);
        auto *confirmBtn = confirmBox.addButton("Withdraw", QMessageBox::AcceptRole);
        confirmBox.addButton("Cancel", QMessageBox::RejectRole);
        confirmBox.setDefaultButton(confirmBtn);
        confirmBox.exec();
        if (confirmBox.clickedButton() != confirmBtn) return;

        QJsonObject params;
        params["txid"] = txid;
        params["vout"] = contractVout;
        params["destination"] = dest.trimmed();
        rpc_->callNamed("withdrawfromvault", params);
      });
      tblContracts_->setCellWidget(row, 5, btnWithdraw);

      if (amount != 0) totalLocked += qAbs(amount);
    } else {
      tblContracts_->setItem(row, 5, new QTableWidgetItem(QString::fromUtf8("\xe2\x80\x94")));
    }
  }

  // Update summary
  QString summary;
  if (contractCount == 0) {
    summary = "No active contracts found.\n\n"
              "Create your first contract using Send tab \xe2\x86\x92 Contracts.";
  } else {
    summary = QString::number(contractCount) + " contract(s) found. ";
    if (totalLocked > 0) {
      summary += "Total visible value: " + QString::number(totalLocked, 'f', 8) + " DIN. ";
    }
    summary += "Contract locks are transparent on-chain.";
  }
  if (lblContractsSummary_) lblContractsSummary_->setText(summary);

  // Resize columns to content
  tblContracts_->resizeColumnsToContents();
}

void MainWindow::onStartMining() {
  const QString mode = currentMiningMode();

  // Local solo miners need the daemon RPC connection. Remote pool miners do
  // not, and should keep working through daemon reconnects or wallet changes.
  if (miningModeNeedsDaemon(mode) && (!connectionMgr_ || !connectionMgr_->isConnected())) {
    QMessageBox::warning(this, "Daemon Not Connected",
      "Cannot start mining - daemon is not connected!\n\n"
      "Please start the daemon first:\n"
      "1. Click 'Start Daemon' button\n"
      "2. Wait for connection to establish\n"
      "3. Then try starting the miner again.");
    return;
  }

  QString addr = edtMiningAddress_->text().trimmed();
  if (addr.isEmpty()) {
    QMessageBox::warning(this, "No Mining Address",
      "Please set a mining address first!\n\n"
      "Steps to set up mining:\n"
      "1. Go to the Receive tab\n"
      "2. Unlock your wallet (if encrypted)\n"
      "3. Click 'New Transparent Address' to generate a Taproot address\n"
      "4. Return here and click 'Use Wallet'\n\n"
      "Mining rewards will be sent to your wallet's Taproot address.");
    return;
  }

  // v7 coinbase policy: Taproot (din1p...) or P2MR (din1r...) only.
  const bool isTaproot = addr.startsWith("din1p") ||
                         addr.startsWith("tdin1p") ||
                         addr.startsWith("rdin1p");
  const bool isP2mr    = addr.startsWith("din1r") ||
                         addr.startsWith("tdin1r") ||
                         addr.startsWith("rdin1r");

  if (!isTaproot && !isP2mr) {
    QString currentType = "unknown";
    if (addr.startsWith("din1q") || addr.startsWith("tdin1q") || addr.startsWith("rdin1q")) {
      currentType = "P2WPKH (SegWit v0)";
    } else if (addr.startsWith("D") || addr.startsWith("T") || addr.startsWith("R")) {
      currentType = "P2PKH (Legacy)";
    }

    QMessageBox::warning(this, "Address Not Eligible for Mining",
      QString("<h3>Mining Requires a Taproot or P2MR Address</h3>"
      "<p>v7 accepts Taproot (din1p...) or P2MR (din1r...) coinbase outputs.</p>"
      "<p><b>Your address:</b> %1</p>"
      "<p><b>Type detected:</b> %2</p>"
      "<p><b>Required format:</b> din1p... (Taproot) or din1r... (P2MR)</p>"
      "<hr>"
      "<p><b>How to get one:</b></p>"
      "<ol>"
      "<li>Go to Receive tab → pick Taproot or Quantum-Safe (P2MR) in the type combo</li>"
      "<li>Click 'New Address' to derive one</li>"
      "<li>Return here and click 'Use Wallet'</li>"
      "</ol>").arg(addr).arg(currentType));
    return;
  }

  // Route to appropriate miner based on mining mode and engine selection.
  if (mode == "pool") {
    startExternalMiner();
    return;
  }
  if (mode == "sv2_pool") {
    startSv2Miner();
    return;
  }

  QString minerType = cmbMinerType_ ? cmbMinerType_->currentData().toString() : "internal";
  if (minerType == "internal") {
    startInternalMiner(false);
  } else if (minerType == "internal_gpu") {
    startInternalMiner(true);
  } else if (minerType == "gpu") {
    startGPUMiner();
  } else {
    startExternalMiner();
  }
}

void MainWindow::startInternalMiner(bool useGpu) {
  QString addr = edtMiningAddress_->text().trimmed();

  // Get thread count from UI
  bool ok;
  int threads = edtMiningThreads_->text().toInt(&ok);
  if (!ok || threads < 1 || threads > 256) {
    threads = 4;  // Default to 4 threads if invalid
    edtMiningThreads_->setText("4");
  }

  // Build cookie path from datadir
  QString dataDirForMiner = rpc_->datadir();
  QString cookiePath;
  QDir dir(dataDirForMiner);
  if (dir.exists(".cookie")) {
    cookiePath = dir.filePath(".cookie");
  }

  if (txtMiningOutput_) {
    const QString engine = useGpu ? QStringLiteral("Embedded GPU solo miner")
                                  : QString("Embedded CPU solo miner · %1 threads").arg(threads);
    const QString auth = cookiePath.isEmpty() ? QStringLiteral("cookie auth unavailable")
                                              : QStringLiteral("cookie auth active");
    txtMiningOutput_->setPlainText(
      QString("%1 · payout %2 · %3").arg(engine, addr, auth));
  }

  mining_stats_.blocks_found = 0;
  mining_stats_.mining_started = QDateTime::currentMSecsSinceEpoch();
  mining_stats_.current_hashrate = 0.0;
  mining_stats_.total_hashes = 0;
  mining_stats_.hashrate_samples.clear();
  if (lblBlocksFound_) lblBlocksFound_->setText("0");
  if (lblCurrentHash_) lblCurrentHash_->setText("0.00 MH/s");
  updateMiningRuntimeLabel();

  // Use MinerController (in-process mining via dinero-solo-miner library)
  // MinerController::runningChanged signal handles UI state updates
  minerCtrl_->start("http://127.0.0.1:20998", cookiePath, addr, useGpu ? 0 : threads, useGpu);

  activeMinerType_ = useGpu ? "internal_gpu" : "internal";
  setMiningModeControlsLocked(true);
  updateOverviewHardwareTelemetry();
}

void MainWindow::startGPUMiner() {
  QString addr = edtMiningAddress_->text().trimmed();

  if (miningProcess_ && miningProcess_->state() == QProcess::Running) {
    QMessageBox::information(this, "Already Mining", "GPU mining is already running!");
    return;
  }

  // Search for the platform GPU miner binary.
  QString gpuMinerPath;
  {
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    auto addCandidate = [&candidates](const QString& path) {
      if (path.isEmpty()) return;
      const QString abs = QFileInfo(path).absoluteFilePath();
      if (QFile::exists(abs) && !candidates.contains(abs)) {
        candidates << abs;
      }
    };

    addCandidate(qEnvironmentVariable("DINERO_GPU_MINER_PATH"));
#if defined(Q_OS_MAC)
    addCandidate(QDir(appDir).absoluteFilePath("dinero-gpu-miner"));
    addCandidate(QDir(appDir).absoluteFilePath("../Resources/dinero-gpu-miner"));
    addCandidate(QDir(appDir).absoluteFilePath("../../../dinero-gpu-miner"));
    addCandidate(QDir(appDir).absoluteFilePath("../../../bin/dinero-gpu-miner"));
    addCandidate(QDir(appDir).absoluteFilePath("../../../../build/dinero-gpu-miner"));
    addCandidate(QStandardPaths::findExecutable("dinero-gpu-miner"));
#elif defined(Q_OS_WIN)
    addCandidate(qEnvironmentVariable("DINERO_CUDA_MINER_PATH"));
    addCandidate(QDir(appDir).absoluteFilePath("dinero-miner.exe"));
    addCandidate(QStandardPaths::findExecutable("dinero-miner.exe"));
    addCandidate(QDir(appDir).absoluteFilePath("dinero-gpu-miner.exe"));
    addCandidate(QStandardPaths::findExecutable("dinero-gpu-miner.exe"));
#else
    addCandidate(QDir(appDir).absoluteFilePath("dinero-gpu-miner"));
    addCandidate(QStandardPaths::findExecutable("dinero-gpu-miner"));
#endif
    if (!candidates.isEmpty()) gpuMinerPath = candidates.first();
  }

  if (gpuMinerPath.isEmpty()) {
    QMessageBox::critical(this, "GPU Miner Not Found",
#if defined(Q_OS_WIN)
      "Could not find CUDA-capable dinero-miner.exe.\n\n"
      "Build it with:\n"
      "  cmake -B build-msvc-cuda -DMINER_ENABLE_CUDA=ON\n"
      "  cmake --build build-msvc-cuda --config Release --target dinero-miner");
#else
      "Could not find dinero-gpu-miner binary.\n\n"
      "Download it from the Dinero releases page or build with:\n"
      "  cmake --build build --target dinero-gpu-miner");
#endif
    return;
  }

  // Reset mining stats
  mining_stats_.blocks_found = 0;
  mining_stats_.mining_started = QDateTime::currentMSecsSinceEpoch();
  mining_stats_.current_hashrate = 0.0;
  mining_stats_.total_hashes = 0;
  mining_stats_.hashrate_samples.clear();

  // Update UI
  lblBlocksFound_->setText("0");
  updateMiningRuntimeLabel();
  lblCurrentHash_->setText("0.00 MH/s");
  lblTotalHashes_->setText("0 hashes");
  lblHashrate_->setText("⛏️ GPU Initializing...");

  // Create process if needed (reuses same miningProcess_ as external miner)
  if (!miningProcess_) {
    miningProcess_ = new QProcess(this);
    miningProcess_->setProcessChannelMode(QProcess::MergedChannels);

    connect(miningProcess_, &QProcess::readyReadStandardOutput, this, [this]() {
      if (!miningProcess_ || !txtMiningOutput_ || !lblHashrate_) return;

      QString output = QString::fromUtf8(miningProcess_->readAllStandardOutput());
      QStringList lines = output.split('\n', Qt::SkipEmptyParts);
      for (const QString& line : lines) {
        parseMiningOutput(line);
        if (txtMiningOutput_) appendMiningOutputLine(txtMiningOutput_, line);
      }
      if (txtMiningOutput_) {
        QTextCursor cursor = txtMiningOutput_->textCursor();
        cursor.movePosition(QTextCursor::End);
        txtMiningOutput_->setTextCursor(cursor);
      }

      // Parse hashrate
      if (lblHashrate_) {
        QRegularExpression hashRateRegex(R"((\d+\.?\d*)\s*(K?M?G?)H/s)");
        QRegularExpressionMatch match = hashRateRegex.match(output);
        if (match.hasMatch()) {
          lblHashrate_->setText(QString("⛏️ %1 %2H/s (GPU)").arg(match.captured(1)).arg(match.captured(2)));
        }
      }
      if (output.contains("BLOCK ACCEPTED") || output.contains("BLOCK FOUND")) {
        mining_stats_.blocks_found++;
        if (lblBlocksFound_) {
          lblBlocksFound_->setText(QString::number(mining_stats_.blocks_found));
        }
        QTimer::singleShot(1000, this, [this]() { rpc_->getBalance(); });
      }
    });

    connect(miningProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
      if (!lblMiningStatus_ || !btnStartMining_ || !txtMiningOutput_) return;
      isMining_ = false;
      activeMinerType_ = "none";
      setMiningModeControlsLocked(false);
      setMiningOutputCinematicEnabled(false);
      btnStartMining_->setText("Start Mining");
      btnStartMining_->setStyleSheet(headerButtonStyle());
      btnStartMining_->setToolTip("Click to start mining");
      lblMiningStatus_->setText(miningStatusInactiveText());
      lblMiningStatus_->setStyleSheet(chromePillStyle());
      txtMiningOutput_->append("\n=== GPU Mining stopped ===\n");
      resetOverviewMiningTelemetry();
    });
  }

  // Build arguments
  QString dataDirForMiner = rpc_->datadir();
  QString cookiePath = QDir(dataDirForMiner).filePath(".cookie");

  QStringList args;
#if defined(Q_OS_WIN)
  QString stratumEndpoint = explicitStratumEndpoint();
  if (edtStratumEndpoint_) {
    const QString uiEndpoint = edtStratumEndpoint_->text().trimmed();
    if (!uiEndpoint.isEmpty()) {
      stratumEndpoint = uiEndpoint;
      QSettings().setValue("mining/stratum_endpoint", stratumEndpoint);
    }
  }

  if (stratumEndpoint.isEmpty()) {
    QMessageBox::warning(this, "Pool Endpoint Required",
      "CUDA GPU mining on Windows uses the Stratum miner.\n\n"
      "Set the Pool Endpoint in the Mining tab, for example:\n"
      "127.0.0.1:3333");
    return;
  }

  const QString threadCount = edtMiningThreads_->text().trimmed().isEmpty() ? "2" : edtMiningThreads_->text().trimmed();
  QString stratumUser;
  args = buildStratumMinerArgs(addr, stratumEndpoint, threadCount, &stratumUser);
  args.prepend("--gpu");
#else
  args << "--backend" << "metal"
       << "--address" << addr
       << "--cookie" << cookiePath;
#endif

  // Clear and launch
  txtMiningOutput_->clear();
  txtMiningOutput_->setProperty(kMiningBlockCardProperty, QVariant());
#if defined(Q_OS_WIN)
  txtMiningOutput_->append(QString("=== Starting GPU miner (CUDA) ==="));
#else
  txtMiningOutput_->append(QString("=== Starting GPU miner (Metal) ==="));
#endif
  appendBinaryIdentityToMiningLog(txtMiningOutput_, "GPU Miner", gpuMinerPath);
  txtMiningOutput_->append(QString("Address: %1").arg(addr));
#if defined(Q_OS_WIN)
  txtMiningOutput_->append(QString("Pool: %1").arg(stratumEndpoint));
  txtMiningOutput_->append(QString("Worker: %1\n").arg(stratumUser));
#else
  txtMiningOutput_->append(QString("Cookie: %1\n").arg(cookiePath));
#endif

  miningProcess_->start(gpuMinerPath, args);
  if (!miningProcess_->waitForStarted(5000)) {
    QMessageBox::critical(this, "GPU Miner Failed",
      QString("Failed to start GPU miner:\n%1").arg(miningProcess_->errorString()));
    return;
  }

  isMining_ = true;
  activeMinerType_ = "gpu";
  btnStartMining_->setText("Stop Mining");
  btnStartMining_->setStyleSheet(
    "QPushButton { background: #da3633; color: white; border: none; border-radius: 8px; "
    "padding: 6px 16px; font-weight: bold; } "
    "QPushButton:hover { background: #f85149; }");
#if defined(Q_OS_WIN)
  lblMiningStatus_->setText("CUDA Mining Active");
#else
  lblMiningStatus_->setText("GPU Mining Active");
#endif
  lblMiningStatus_->setStyleSheet(
    "QLabel { background: #1f6feb; color: white; border-radius: 10px; "
    "padding: 3px 10px; font-size: 11px; font-weight: bold; }");
  setMiningModeControlsLocked(true);
  setMiningOutputCinematicEnabled(true);

  if (miningStatsTimer_) {
    miningStatsTimer_->start(5000);
  }
  updateOverviewHardwareTelemetry();
}

void MainWindow::startExternalMiner() {
  QString addr = edtMiningAddress_->text().trimmed();
  const bool poolMode = (currentMiningMode() == "pool");
  const QString binaryName = externalMinerBinaryName(poolMode);
  const QString displayName = externalMinerDisplayName(poolMode);
  const QString settingsKey = externalMinerSettingsKey(poolMode);
  const QString envVar = externalMinerEnvVar(poolMode);

  // External miner process (reactivated from preserved code)
  if (miningProcess_ && miningProcess_->state() == QProcess::Running) {
    QMessageBox::information(this, "Already Mining", "Mining is already running!");
    return;
  }

  // Build miner path - use edtMinerPath_ if set, otherwise search
  QString minerPath = edtMinerPath_->text().trimmed();

  // If path not manually set, search for miner binary
  if (minerPath.isEmpty() || !QFile::exists(minerPath)) {
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidatePaths;
    auto addCandidate = [&candidatePaths](const QString& path) {
      if (path.isEmpty()) return;
      const QString abs = QFileInfo(path).absoluteFilePath();
      if (!QFile::exists(abs)) return;
      if (!candidatePaths.contains(abs)) {
        candidatePaths << abs;
      }
    };

    // Check environment variable override first.
    QString envMinerPath = qEnvironmentVariable(envVar.toUtf8().constData());
    addCandidate(envMinerPath);
    if (poolMode) {
      addCandidate(qEnvironmentVariable("DINERO_MINER_PATH"));
    }

#if defined(Q_OS_MAC)
    addCandidate(QDir(appDir).absoluteFilePath("../../../" + binaryName));
    addCandidate(QDir(appDir).absoluteFilePath("../../../bin/" + binaryName));
    addCandidate(QDir(appDir).absoluteFilePath("../Resources/" + binaryName));
    addCandidate(QDir(appDir).absoluteFilePath("../../../../build/bin/" + binaryName));
    addCandidate(QStandardPaths::findExecutable(binaryName));
#elif defined(Q_OS_WIN)
    addCandidate(QDir(appDir).absoluteFilePath(binaryName));
    addCandidate(QDir(appDir).absoluteFilePath("../build/" + binaryName));
    addCandidate(QDir(appDir).absoluteFilePath("../build-clean/" + binaryName));
    addCandidate(QStandardPaths::findExecutable(binaryName));
#else  // Linux
    addCandidate(QDir(appDir).absoluteFilePath(binaryName));
    addCandidate(QDir(appDir).absoluteFilePath("../build/" + binaryName));
    addCandidate(QDir(appDir).absoluteFilePath("../build-clean/" + binaryName));
    addCandidate(QStandardPaths::findExecutable(binaryName));
#endif

    // Prefer miner binaries that match the selected mode.
    const QString preferredProfile = poolMode ? "stratum" : "rpc";
    for (const QString& candidate : candidatePaths) {
      QString helpText;
      QString probeError;
      const QString profile = probeExternalMinerProfile(candidate, &helpText, &probeError);
      if (profile == preferredProfile) {
        minerPath = candidate;
        break;
      }
    }

    // Fallback: first available binary (may be stratum-only; launch policy handles that).
    if (minerPath.isEmpty() && !candidatePaths.isEmpty()) {
      minerPath = candidatePaths.first();
    }
    if (!minerPath.isEmpty()) {
      edtMinerPath_->setText(minerPath);
      QSettings().setValue(settingsKey, minerPath);
    }
  }

  if (!QFile::exists(minerPath)) {
    QString appDir = QCoreApplication::applicationDirPath();
    QString notFoundMessage =
      QString("Could not find %1.\n\n").arg(displayName) +
      QString("Please use the 'Browse...' button to locate the %1 binary.\n\n").arg(displayName) +
      QStringLiteral("Searched locations:\n");
#if defined(Q_OS_MAC)
    notFoundMessage += QStringLiteral("• ") + QDir(appDir).absoluteFilePath("../../../" + binaryName) + "\n";
    notFoundMessage += QStringLiteral("• ") + QDir(appDir).absoluteFilePath("../../../bin/" + binaryName) + "\n";
    notFoundMessage += QStringLiteral("• ") + QDir(appDir).absoluteFilePath("../Resources/" + binaryName) + "\n";
#else
    notFoundMessage += QStringLiteral("• ") + QDir(appDir).absoluteFilePath(binaryName) + "\n";
    notFoundMessage += QStringLiteral("• ") + QDir(appDir).absoluteFilePath("../build/" + binaryName) + "\n";
#endif
    notFoundMessage += QString("\nYou can also set %1 environment variable.\n\n").arg(envVar);
    notFoundMessage += QStringLiteral("To build the miner:\n");
    notFoundMessage += QString("cmake --build build --target %1").arg(displayName);
    QMessageBox::critical(this, "Miner Not Found", notFoundMessage);
    return;
  }
  
  // Reset mining stats
  mining_stats_.blocks_found = 0;
  mining_stats_.mining_started = QDateTime::currentMSecsSinceEpoch();
  mining_stats_.current_hashrate = 0.0;
  mining_stats_.total_hashes = 0;
  mining_stats_.hashrate_samples.clear();
  
  // Update UI
  lblBlocksFound_->setText("0");
  updateMiningRuntimeLabel();
  lblCurrentHash_->setText("0.00 MH/s");
  lblTotalHashes_->setText("0 hashes");
  lblHashrate_->setText("⛏️ Initializing...");
  
  // Create process if needed
  if (!miningProcess_) {
    miningProcess_ = new QProcess(this);
    miningProcess_->setProcessChannelMode(QProcess::MergedChannels);
    
    connect(miningProcess_, &QProcess::readyReadStandardOutput, this, [this]() {
      // CRITICAL: Check if widgets still exist (prevents crash during shutdown)
      if (!miningProcess_ || !txtMiningOutput_ || !lblHashrate_) {
        return;
      }
      
      QString output = QString::fromUtf8(miningProcess_->readAllStandardOutput());
      
      // Parse each line for stats
      QStringList lines = output.split('\n', Qt::SkipEmptyParts);
      for (const QString& line : lines) {
        parseMiningOutput(line);
        
        // Append to output window (check again in case widgets were deleted during parsing)
        if (txtMiningOutput_) {
          appendMiningOutputLine(txtMiningOutput_, line);
        }
      }
      
      // Auto-scroll to bottom
      if (txtMiningOutput_) {
        QTextCursor cursor = txtMiningOutput_->textCursor();
        cursor.movePosition(QTextCursor::End);
        txtMiningOutput_->setTextCursor(cursor);
      }
      
      // Parse hashrate from output (look for patterns like "1234.56 H/s" or "1.23 MH/s")
      if (lblHashrate_) {
        QRegularExpression hashRateRegex(R"((\d+\.?\d*)\s*(K?M?G?)H/s)");
        QRegularExpressionMatch match = hashRateRegex.match(output);
        if (match.hasMatch()) {
          QString rate = match.captured(1);
          QString unit = match.captured(2);
          lblHashrate_->setText(QString("⛏️ %1 %2H/s").arg(rate).arg(unit));
        }
      }
      
      // Parse accepted blocks
      if (output.contains("accepted") || output.contains("BLOCK FOUND")) {
        // Trigger balance refresh
        QTimer::singleShot(1000, this, [this]() { rpc_->getBalance(); });
      }
    });
    
    connect(miningProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int /*exitCode*/, QProcess::ExitStatus) {
      // Check if widgets still exist before updating (prevents crash during shutdown)
      if (!lblMiningStatus_ || !btnStartMining_ || !txtMiningOutput_) {
        return;
      }
      // v0.14.0.4: Reset mining state and toggle button
      isMining_ = false;
      activeMinerType_ = "none";
      setMiningModeControlsLocked(false);
      setMiningOutputCinematicEnabled(false);
      btnStartMining_->setText("Start Mining");
      btnStartMining_->setStyleSheet(headerButtonStyle());
      btnStartMining_->setToolTip("Click to start mining");

      lblMiningStatus_->setText(miningStatusInactiveText());
      lblMiningStatus_->setStyleSheet(chromePillStyle());
      resetOverviewMiningTelemetry();
      txtMiningOutput_->append("\n=== Mining stopped ===\n");
    });
  }
  
  // Clear previous output
  txtMiningOutput_->clear();
  txtMiningOutput_->setProperty(kMiningBlockCardProperty, QVariant());
  
  // Use the same datadir where RPC client successfully loaded the cookie
  // This ensures the miner can authenticate with the same daemon
  QString dataDirForMiner = rpc_->datadir();
  QString cookiePath = QDir(dataDirForMiner).filePath(".cookie");
  const QString threadCount = edtMiningThreads_->text().trimmed().isEmpty() ? "8" : edtMiningThreads_->text().trimmed();

  QString helpText;
  QString probeError;
  const QString detectedProfile = probeExternalMinerProfile(minerPath, &helpText, &probeError);
  QString stratumEndpoint = explicitStratumEndpoint();
  if (edtStratumEndpoint_) {
    const QString uiEndpoint = edtStratumEndpoint_->text().trimmed();
    if (!uiEndpoint.isEmpty()) {
      stratumEndpoint = uiEndpoint;
      QSettings().setValue("mining/stratum_endpoint", stratumEndpoint);
    }
  }

  QStringList profileOrder;
  if (poolMode) {
    profileOrder << "stratum";
  } else {
    profileOrder << "rpc";
  }

  if (poolMode && stratumEndpoint.isEmpty()) {
    QMessageBox::warning(this, "Pool Endpoint Required",
      "Pool mode requires a Stratum endpoint.\n\n"
      "Set it in the Mining tab (Pool Endpoint), for example:\n"
      "127.0.0.1:3333");
    setMiningModeControlsLocked(false);
    setMiningOutputCinematicEnabled(false);
    if (miningStatsTimer_) {
      miningStatsTimer_->stop();
    }
    if (lblMiningStatus_) {
      lblMiningStatus_->setText("Pool mode ready (set endpoint)");
      lblMiningStatus_->setStyleSheet(chromePillStyle());
    }
    return;
  }

  if (txtMiningOutput_) {
    txtMiningOutput_->append(QString("=== External miner selected: %1 ===").arg(minerPath));
    appendBinaryIdentityToMiningLog(txtMiningOutput_, "External Miner", minerPath);
    txtMiningOutput_->append(QString("[Mode] %1").arg(poolMode ? "Pool (Stratum)" : "Solo (RPC)"));
    if (detectedProfile != "unknown") {
      txtMiningOutput_->append(QString("[Auto-detect] Miner profile: %1").arg(detectedProfile));
    }
    if (!probeError.isEmpty()) {
      txtMiningOutput_->append(QString("[Auto-detect] Probe note: %1").arg(probeError));
    }
    if (!poolMode && detectedProfile == "stratum") {
      txtMiningOutput_->append(
        "[Policy] Solo mode uses RPC only. Switch to Pool mode for Stratum mining.");
    }
    if (poolMode) {
      txtMiningOutput_->append(QString("[Pool] Endpoint: %1").arg(stratumEndpoint));
    }
  }

  auto stopProcessIfNeeded = [this]() {
    if (!miningProcess_) return;
    if (miningProcess_->state() == QProcess::Running) {
      miningProcess_->terminate();
      if (!miningProcess_->waitForFinished(1000)) {
        miningProcess_->kill();
        miningProcess_->waitForFinished(500);
      }
    }
  };

  auto tryLaunchProfile = [&](const QString& profile,
                              bool includeInsecureAck,
                              QString* failureOutput,
                              QString* endpointUsed,
                              QString* userUsed) -> bool {
    QStringList args;
    if (profile == "rpc") {
      args = buildRpcMinerArgs(cookiePath, addr, threadCount, includeInsecureAck);
    } else {
      args = buildStratumMinerArgs(addr, stratumEndpoint, threadCount, userUsed);
      if (endpointUsed) {
        *endpointUsed = stratumEndpoint;
      }
    }

    miningProcess_->start(minerPath, args);
    if (!miningProcess_->waitForStarted(3000)) {
      if (failureOutput) {
        *failureOutput = QString("start failed: %1").arg(miningProcess_->errorString());
      }
      return false;
    }

    // If process exits immediately, treat as profile mismatch and try another profile.
    if (miningProcess_->waitForFinished(1400)) {
      QString output = QString::fromUtf8(miningProcess_->readAllStandardOutput());
      if (output.trimmed().isEmpty()) {
        output = QString::fromUtf8(miningProcess_->readAllStandardError());
      }
      if (output.trimmed().isEmpty()) {
        output = QString("exited early with code %1").arg(miningProcess_->exitCode());
      }
      if (failureOutput) {
        *failureOutput = compactProcessOutput(output);
      }
      return false;
    }

    return true;
  };

  bool started = false;
  QString selectedProfile = "unknown";
  QString selectedEndpoint;
  QString selectedUser;
  QStringList attemptDiagnostics;

  for (const QString& profile : profileOrder) {
    QString failureOutput;
    QString endpointUsed;
    QString userUsed;

    const bool requireAckHint = (profile == "rpc") && rpcMinerRequiresInsecureAck(helpText);
    if (tryLaunchProfile(profile, requireAckHint, &failureOutput, &endpointUsed, &userUsed)) {
      started = true;
      selectedProfile = profile;
      selectedEndpoint = endpointUsed;
      selectedUser = userUsed;
      break;
    }

    attemptDiagnostics << QString("%1 attempt failed: %2").arg(profile, failureOutput);
    stopProcessIfNeeded();

    // Retry RPC profile with explicit acknowledgment if miner demands it.
    if (profile == "rpc" && !requireAckHint &&
        failureOutput.contains("--i-know-this-is-insecure", Qt::CaseInsensitive)) {
      QString ackFailureOutput;
      if (tryLaunchProfile(profile, true, &ackFailureOutput, &endpointUsed, &userUsed)) {
        started = true;
        selectedProfile = profile;
        selectedEndpoint = endpointUsed;
        selectedUser = userUsed;
        break;
      }
      attemptDiagnostics << QString("%1 retry with insecure-ack failed: %2").arg(profile, ackFailureOutput);
      stopProcessIfNeeded();
    }
  }

  if (started) {
    QSettings().setValue(settingsKey, minerPath);
    activeMinerType_ = (selectedProfile == "stratum") ? "stratum_worker" : "external";
    setMiningModeControlsLocked(true);

    // v0.14.0.4: Set mining state and update toggle button
    isMining_ = true;
    btnStartMining_->setText("Stop Mining");
    btnStartMining_->setStyleSheet(headerButtonStyle());
    btnStartMining_->setToolTip("Click to stop mining");

    lblMiningStatus_->setText(poolMode ? "Pool Mining Active" : miningStatusActiveText());
    lblMiningStatus_->setStyleSheet(chromePillStyle());
    setMiningOutputCinematicEnabled(true);
    txtMiningOutput_->append("=== Mining started ===");
    txtMiningOutput_->append(QString("Mode: %1").arg(poolMode ? "Pool (Stratum)" : "Solo (RPC)"));
    txtMiningOutput_->append(QString("Profile: %1").arg(selectedProfile));
    txtMiningOutput_->append(QString("Address: %1").arg(addr));
    txtMiningOutput_->append(QString("Threads: %1").arg(threadCount));
    if (selectedProfile == "rpc") {
      txtMiningOutput_->append("RPC: http://127.0.0.1:20998/");
      txtMiningOutput_->append(QString("Cookie: %1").arg(cookiePath));
      txtMiningOutput_->append(QString("Datadir: %1").arg(dataDirForMiner));
    } else if (selectedProfile == "stratum") {
      txtMiningOutput_->append(QString("Stratum endpoint: %1").arg(selectedEndpoint));
      txtMiningOutput_->append(QString("Worker: %1").arg(selectedUser));
      txtMiningOutput_->append("Password: x (static)");
      txtMiningOutput_->append("Tip: override endpoint via DINERO_STRATUM_ENDPOINT");
    }
    txtMiningOutput_->append("===================\n");

    // Start stats timer after miner process is confirmed running
    if (miningStatsTimer_) {
      miningStatsTimer_->start(1000);
    }
    updateOverviewHardwareTelemetry();
  } else {
    activeMinerType_ = "none";
    isMining_ = false;
    setMiningModeControlsLocked(false);
    setMiningOutputCinematicEnabled(false);
    if (miningStatsTimer_) {
      miningStatsTimer_->stop();
    }
    lblMiningStatus_->setText(miningStatusInactiveText());
    lblMiningStatus_->setStyleSheet(chromePillStyle());
    resetOverviewMiningTelemetry();

    QString detail = attemptDiagnostics.join("\n");
    if (detail.isEmpty()) {
      detail = "No compatible launch profile succeeded.";
    }
    if (!poolMode && detectedProfile == "stratum") {
      detail += "\n\nThis miner appears Stratum-oriented; switch to Pool mode for Stratum mining.";
    }
    QMessageBox::critical(this, "Failed to Start",
      QString("Could not start external miner in %1 mode.\n\n")
        .arg(poolMode ? "Pool/Stratum" : "Solo/RPC") +
      "Details:\n" + detail);
  }
}

void MainWindow::onStopMining() {
  // Stop the miner mode that is actually active, not just currently selected in UI.
  QString minerType = activeMinerType_;
  if (minerType == "none" && cmbMinerType_) {
    minerType = cmbMinerType_->currentData().toString();
  }

  if (minerType == "internal" || minerType == "internal_gpu") {
    // Both internal lanes run inside this process via MinerController
    // (CPU SoloMiner or in-process CUDA backend). The Stop path must
    // call MinerController::stop() — terminating miningProcess_ is a
    // no-op for these because no QProcess was ever spawned.
    stopInternalMiner();
  } else if (minerType == "daemon") {
    rpc_->miningStop();
  } else {
    // External pool/SV2 miners and the Metal "gpu" lane use miningProcess_
    stopExternalMiner();
  }
}

void MainWindow::startSv2Miner() {
  const QString addr = edtMiningAddress_->text().trimmed();
  if (miningProcess_ && miningProcess_->state() == QProcess::Running) {
    QMessageBox::information(this, "Already Mining", "Mining is already running!");
    return;
  }

  const QString payoutScript = addressToScriptPubKeyHex(addr);
  if (payoutScript.isEmpty()) {
    QMessageBox::warning(this, "Address Not Decodable",
      "Could not convert mining address to a scriptPubKey.\n\n"
      "SV2 pool mining requires a Taproot (din1p…) or P2MR (din1r…) "
      "bech32m address. Generate one in the Receive tab, then click "
      "'Use Wallet'.");
    return;
  }

  const QString rewardMode = cmbSv2RewardMode_
    ? cmbSv2RewardMode_->currentData().toString()
    : QStringLiteral("shared");
  if (rewardMode == QStringLiteral("shared") &&
      (payoutScript.size() != 68 ||
       !payoutScript.startsWith(QStringLiteral("5120"), Qt::CaseInsensitive))) {
    QMessageBox::warning(this, "Taproot Address Required",
      "Pool Shared credits its PPLNS ledger to a Taproot (din1p...) address.\n\n"
      "Select a Taproot mining address, or choose Pool Solo to keep using "
      "this address.");
    return;
  }

  // Backend: CPU miner or GPU miner — different binaries.
  const QString backend = cmbSv2Backend_
    ? cmbSv2Backend_->currentData().toString()
    : QStringLiteral("cpu");
  const bool useGpu = (backend == "metal");
  const QString envVarName = sv2MinerEnvVarForBackend(useGpu);
  const QString humanName = sv2MinerDisplayNameForBackend(useGpu);
  const QString cargoBinName = sv2MinerCargoPackageForBackend(useGpu);
  QString minerPath = discoverSv2MinerPath(useGpu, true, true);

  if (minerPath.isEmpty() || !QFile::exists(minerPath)) {
    QMessageBox::critical(this, QString("%1 Not Found").arg(humanName),
      QString("Could not locate the %1 binary.\n\n"
              "Build it with:\n"
              "  cd ~/src/dinero-sv2 && cargo build --release -p %2\n\n"
              "Or set %3 to the binary path.")
        .arg(humanName).arg(cargoBinName).arg(envVarName));
    return;
  }

  // Thread count — reuse the shared mining-threads field.
  int threads = 0;
  if (edtMiningThreads_) {
    bool ok = false;
    threads = edtMiningThreads_->text().toInt(&ok);
    if (!ok || threads < 0) threads = 0;
  }

  // Prefer live UI values over settings, with defaults as last resort.
  QString endpoint = edtSv2Endpoint_ ? edtSv2Endpoint_->text().trimmed() : QString();
  if (endpoint.isEmpty()) endpoint = sv2PoolEndpoint();
  QString pubkey = edtSv2Pubkey_ ? edtSv2Pubkey_->text().trimmed() : QString();
  if (pubkey.isEmpty()) pubkey = sv2PoolServerPubkey();

  QStringList args;
  args << "--pool" << endpoint
       << "--server-pubkey" << pubkey
       << "--payout-script-hex" << payoutScript
       << "--reward-mode" << rewardMode
       << "--user-agent" << "dinero-qt"
       << "--json";
  if (useGpu) {
    // GPU batch-size: 1M nonces per dispatch (~2ms on M4 Max). Small
    // enough to respond to SetNewPrevHash within one dispatch.
    args << "--batch-size" << QString::number(1 << 20);
  } else {
    if (threads > 0) args << "--threads" << QString::number(threads);
  }

  if (miningProcess_) {
    miningProcess_->deleteLater();
    miningProcess_ = nullptr;
  }
  miningProcess_ = new QProcess(this);
  miningProcess_->setProcessChannelMode(QProcess::MergedChannels);

  connect(miningProcess_, &QProcess::readyReadStandardOutput, this, [this]() {
    if (!miningProcess_ || !txtMiningOutput_) return;
    const QString chunk = QString::fromUtf8(miningProcess_->readAllStandardOutput());
    const QStringList lines = chunk.split('\n', Qt::SkipEmptyParts);
    for (const QString& raw : lines) {
      const QByteArray bytes = raw.trimmed().toUtf8();
      if (bytes.isEmpty()) continue;
      QJsonParseError err{};
      const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
      if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        appendSv2EventLine(txtMiningOutput_, "raw", raw.trimmed(), "#b9c6cd", raw);
        continue;
      }
      const QJsonObject obj = doc.object();
      const QString event = obj.value("event").toString();
      if (event == "share_accepted") {
        // The miner now coalesces per-ack pool messages into 1-second
        // windows and reports `accepted_count` as the count *for that
        // window*. Older miner builds (per-event) still send 1 per
        // ack; we treat both identically by adding the count.
        const qint64 batchCount =
          obj.value("accepted_count").toVariant().toLongLong();
        mining_stats_.sv2_shares_accepted +=
          (batchCount > 0 ? batchCount : 1);
        if (lblSv2Shares_) {
          const QString window = mining_stats_.sv2_window_bps >= 0
            ? QString("  PPLNS=%1%").arg(
                mining_stats_.sv2_window_bps / 100.0, 0, 'f', 2)
            : QString();
          lblSv2Shares_->setText(QString("Shares: seq=%1  total=%2%3")
            .arg(obj.value("last_seq").toVariant().toLongLong())
            .arg(mining_stats_.sv2_shares_accepted)
            .arg(window));
        }
        // Output panel: log only on early shares (so the user sees
        // confirmation that mining started) and once every 1000 shares
        // thereafter as a heartbeat. At GPU rates we'd otherwise scroll
        // hundreds of lines a second.
        const qint64 total = mining_stats_.sv2_shares_accepted;
        if (total <= 3 || (total % 1000) == 0) {
          appendSv2EventLine(txtMiningOutput_, "share",
            QString("accepted seq=%1 total=%2")
              .arg(obj.value("last_seq").toVariant().toLongLong())
              .arg(total),
            "#95d66b",
            QString::number(total));
        }
      } else if (event == "window_status") {
        mining_stats_.sv2_window_bps =
          obj.value("window_bps").toVariant().toLongLong();
        mining_stats_.sv2_window_shares =
          obj.value("window_shares").toVariant().toLongLong();
        if (lblSv2Shares_) {
          lblSv2Shares_->setText(QString("PPLNS: %1%  accepted=%2  window=%3")
            .arg(mining_stats_.sv2_window_bps / 100.0, 0, 'f', 2)
            .arg(mining_stats_.sv2_shares_accepted)
            .arg(mining_stats_.sv2_window_shares));
        }
        appendSv2EventLine(txtMiningOutput_, "pplns",
          QString("your share=%1% window=%2")
            .arg(mining_stats_.sv2_window_bps / 100.0, 0, 'f', 2)
            .arg(mining_stats_.sv2_window_shares),
          "#95d66b",
          QString::number(mining_stats_.sv2_window_bps));
      } else if (event == "hashrate") {
        // Backend-agnostic hashrate event from dinero-sv2-miner and
        // dinero-sv2-gpu-miner. Feeds the top-of-panel MH/s widget.
        const double mhs = obj.value("mhs").toDouble();
        const double hps = mhs * 1e6;
        mining_stats_.sv2_hashrate_updates += 1;
        mining_stats_.current_hashrate = hps;
        if (lblCurrentHash_) {
          if (hps >= 1e9) {
            lblCurrentHash_->setText(QString::number(hps / 1e9, 'f', 2) + " GH/s");
          } else if (hps >= 1e6) {
            lblCurrentHash_->setText(QString::number(hps / 1e6, 'f', 2) + " MH/s");
          } else if (hps >= 1e3) {
            lblCurrentHash_->setText(QString::number(hps / 1e3, 'f', 2) + " KH/s");
          } else {
            lblCurrentHash_->setText(QString::number(hps, 'f', 2) + " H/s");
          }
        }
        setOverviewLocalHashrate(hps,
          QString("SV2 %1 miner hashrate")
            .arg(obj.value("backend").toString() == "metal" ? "GPU (Metal)" : "CPU"));
        if (mining_stats_.sv2_hashrate_updates == 1 ||
            (mining_stats_.sv2_hashrate_updates % 12) == 0) {
          const QString dispatch = obj.contains("dispatch_ms")
            ? QString(" dispatch=%1ms").arg(obj.value("dispatch_ms").toDouble(), 0, 'f', 2)
            : QString();
          appendSv2EventLine(txtMiningOutput_, "hash",
            QString("%1 MH/s%2 nonce=%3")
              .arg(mhs, 0, 'f', 2)
              .arg(dispatch)
              .arg(obj.value("nonce_start").toString(QStringLiteral("-"))),
            obj.value("backend").toString() == "metal" ? "#6ee7f2" : "#95d66b",
            QString::number(mhs, 'f', 2));
        }
      } else if (event == "share_rejected") {
        appendSv2EventLine(txtMiningOutput_, "reject",
          QString("seq=%1 reason=%2")
            .arg(obj.value("sequence_number").toVariant().toLongLong())
            .arg(obj.value("error").toString()),
          "#ff8b8b",
          obj.value("error").toString());
      } else if (event == "share_submitted") {
        if (obj.value("meets_block_target").toBool()) {
          mining_stats_.blocks_found += 1;
          if (lblBlocksFound_) lblBlocksFound_->setText(QString::number(mining_stats_.blocks_found));
          appendSv2EventLine(txtMiningOutput_, "block",
            QString("FOUND hash=%1 nonce=%2")
              .arg(obj.value("hash").toString())
              .arg(obj.value("nonce").toString()),
            "#f6c85f",
            obj.value("hash").toString());
        }
      } else if (event == "new_job") {
        appendSv2EventLine(txtMiningOutput_, "job",
          QString("%1 template=%2 target=%3")
            .arg(obj.contains("height")
              ? QString("height=%1").arg(obj.value("height").toVariant().toLongLong())
              : QStringLiteral("shared"))
            .arg(obj.value("template_id").toVariant().toLongLong())
            .arg(compactSv2Value(obj.value("share_target").toString(), 12, 8)),
          "#d8c27a",
          obj.value("block_target").toString());
      } else if (event == "connected") {
        const QString backendHint = obj.value("backend").toString();
        appendSv2EventLine(txtMiningOutput_, "link",
          backendHint.isEmpty()
            ? QString("connected %1").arg(obj.value("pool").toString())
            : QString("connected %1 backend=%2")
                .arg(obj.value("pool").toString())
                .arg(backendHint),
          "#78d6c6",
          obj.value("pool").toString());
      } else if (event == "gpu_ready") {
        appendSv2EventLine(txtMiningOutput_, "metal",
          QString("%1 threads/group=%2 batch=%3")
            .arg(obj.value("device").toString())
            .arg(obj.value("max_threads_per_group").toVariant().toLongLong())
            .arg(obj.value("batch_size").toVariant().toLongLong()),
          "#6ee7f2",
          obj.value("device").toString());
      } else if (event == "channel_open") {
        appendSv2EventLine(txtMiningOutput_, "chan",
          QString("open id=%1 target=%2")
            .arg(obj.value("channel_id").toVariant().toLongLong())
            .arg(compactSv2Value(obj.value("share_target").toString(), 16, 8)),
          "#8fb4ff",
          obj.value("share_target").toString());
      } else if (event == "session_end") {
        appendSv2EventLine(txtMiningOutput_, "end",
          QString("reason=%1 %2")
            .arg(obj.value("reason").toString())
            .arg(obj.value("error").toString()),
          "#ffb86b",
          obj.value("error").toString());
      } else if (event == "set_new_prev_hash") {
        appendSv2EventLine(txtMiningOutput_, "prev",
          QString("%1 nbits=%2 min_ntime=%3")
            .arg(compactSv2Value(obj.value("prev_hash").toString(), 18, 8))
            .arg(obj.value("nbits").toString())
            .arg(obj.value("min_ntime").toVariant().toLongLong()),
          "#9db4c0",
          obj.value("prev_hash").toString());
      } else if (event == "startup") {
        appendSv2EventLine(txtMiningOutput_, "boot",
          QString("%1 batch=%2 pinned=%3")
            .arg(obj.value("backend").toString())
            .arg(obj.value("batch_size").toVariant().toLongLong())
            .arg(obj.value("server_pubkey_pinned").toBool() ? "yes" : "no"),
          "#b7d7a8",
          raw);
      } else if (event == "reconnect_wait") {
        appendSv2EventLine(txtMiningOutput_, "retry",
          QString("waiting %1s").arg(obj.value("secs").toVariant().toLongLong()),
          "#ffb86b",
          raw);
      } else {
        appendSv2EventLine(txtMiningOutput_, event.isEmpty() ? "sv2" : event,
          QString::fromUtf8(bytes),
          "#b9c6cd",
          raw);
      }
    }
  });

  connect(miningProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
          this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
    const bool expectedStop = externalMinerStopRequested_ || shuttingDown_;
    externalMinerStopRequested_ = false;
    if (txtMiningOutput_ && !shuttingDown_) {
      if (expectedStop) {
        appendSv2EventLine(txtMiningOutput_, "stop",
          QStringLiteral("miner stopped normally"),
          "#95d66b",
          QString::number(exitCode));
      } else {
        appendSv2EventLine(txtMiningOutput_, "exit",
          exitStatus == QProcess::CrashExit
            ? QString("miner terminated unexpectedly (signal/code %1)").arg(exitCode)
            : QString("miner exited unexpectedly (code %1)").arg(exitCode),
          "#ff8b8b",
          QString::number(exitCode));
      }
    }
    isMining_ = false;
    activeMinerType_.clear();
    setMiningModeControlsLocked(false);
    setMiningOutputCinematicEnabled(false);
    if (miningStatsTimer_) {
      miningStatsTimer_->stop();
    }
    if (lblMiningStatus_) {
      lblMiningStatus_->setText(miningStatusInactiveText());
      lblMiningStatus_->setStyleSheet(chromePillStyle());
    }
    if (btnStartMining_) btnStartMining_->setText("Start Mining");
  });

  if (txtMiningOutput_) {
    txtMiningOutput_->clear();
    txtMiningOutput_->setProperty(kMiningBlockCardProperty, QVariant());
    const QString workHint = useGpu
      ? QString("%1 nonces/dispatch").arg(1 << 20)
      : (threads > 0 ? QString("%1 CPU threads").arg(threads) : QStringLiteral("auto CPU threads"));
    appendSv2SessionHeader(txtMiningOutput_,
      useGpu ? QStringLiteral("GPU / Metal") : QStringLiteral("CPU"),
      endpoint,
      pubkey.isEmpty() ? QStringLiteral("(none - TOFU)") : pubkey,
      addr,
      payoutScript,
      workHint);
    appendSv2EventLine(txtMiningOutput_, "mode",
      rewardMode == QStringLiteral("shared")
        ? QStringLiteral("Pool Shared (PPLNS)")
        : QStringLiteral("Pool Solo (miner-owned coinbase)"),
      rewardMode == QStringLiteral("shared") ? "#95d66b" : "#f6c85f",
      rewardMode);
    appendSv2EventLine(txtMiningOutput_, "bin",
      compactSv2Value(minerPath, 54, 24),
      "#7d8b94",
      minerPath);
  }

  externalMinerStopRequested_ = false;
  miningProcess_->start(minerPath, args);
  if (!miningProcess_->waitForStarted(5000)) {
    QMessageBox::critical(this, "SV2 Miner Failed to Start",
      QString("dinero-sv2-miner did not start:\n%1").arg(miningProcess_->errorString()));
    miningProcess_->deleteLater();
    miningProcess_ = nullptr;
    return;
  }

  isMining_ = true;
  activeMinerType_ = useGpu ? "sv2_pool_gpu" : "sv2_pool";
  mining_stats_.blocks_found = 0;
  mining_stats_.mining_started = QDateTime::currentMSecsSinceEpoch();
  mining_stats_.current_hashrate = 0.0;
  mining_stats_.sv2_shares_accepted = 0;
  mining_stats_.sv2_hashrate_updates = 0;
  mining_stats_.sv2_window_bps = -1;
  mining_stats_.sv2_window_shares = 0;
  if (lblCurrentHash_) lblCurrentHash_->setText("0.00");
  if (lblBlocksFound_) lblBlocksFound_->setText("0");
  updateMiningRuntimeLabel();
  if (lblSv2Shares_) lblSv2Shares_->setText("Shares: 0");
  setMiningModeControlsLocked(true);
  updateMiningStats();
  if (miningStatsTimer_) {
    miningStatsTimer_->start(1000);
  }
  if (btnStartMining_) btnStartMining_->setText("Stop Mining");
  if (lblMiningStatus_) {
    lblMiningStatus_->setText(rewardMode == QStringLiteral("shared")
      ? "Mining (SV2 Pool Shared)"
      : "Mining (SV2 Pool Solo)");
    lblMiningStatus_->setStyleSheet(chromePillStyle() + " color: #2cb84a;");
  }
  setMiningOutputCinematicEnabled(true);
}

// v0.14.0.4: Toggle button handler - Start or Stop mining based on current state
void MainWindow::onToggleMining() {
  if (isMining_) {
    // Currently mining -> stop
    onStopMining();
  } else {
    // Not mining -> start
    onStartMining();
  }
}

void MainWindow::stopInternalMiner() {
  // Use MinerController to stop (runningChanged signal handles UI updates)
  if (minerCtrl_) {
    minerCtrl_->stop();
  }

  if (txtMiningOutput_) {
    txtMiningOutput_->append("\n=== Mining stopped ===\n");
  }
}

void MainWindow::stopExternalMiner() {
  // External miner process stop (reactivated from preserved code)
  // CRITICAL: Check if widgets exist before accessing them
  if (!lblMiningStatus_ || !btnStartMining_ || !btnStopMining_) {
    // Still try to stop the process even if widgets are gone
    if (miningProcess_ && miningProcess_->state() == QProcess::Running) {
      externalMinerStopRequested_ = true;
      miningProcess_->terminate();
      if (!miningProcess_->waitForFinished(3000)) {
        miningProcess_->kill();
      }
    }
    if (miningStatsTimer_) {
      miningStatsTimer_->stop();
    }
    setMiningOutputCinematicEnabled(false);
    return;
  }

  if (miningProcess_ && miningProcess_->state() == QProcess::Running) {
    lblMiningStatus_->setText(miningStatusInactiveText() + " | Stopping miner...");
    lblMiningStatus_->setStyleSheet(chromePillStyle());

    externalMinerStopRequested_ = true;
    miningProcess_->terminate();
    if (!miningProcess_->waitForFinished(3000)) {
      // Force kill if it doesn't stop gracefully
      miningProcess_->kill();
      miningProcess_->waitForFinished(1000);
    }

    // Stop stats timer
    if (miningStatsTimer_) {
      miningStatsTimer_->stop();
    }

  }

  // v0.14.0.4: Reset mining state and toggle button
  isMining_ = false;
  activeMinerType_ = "none";
  setMiningModeControlsLocked(false);
  setMiningOutputCinematicEnabled(false);
  resetOverviewMiningTelemetry();
  btnStartMining_->setText("Start Mining");
  btnStartMining_->setStyleSheet(headerButtonStyle());
  btnStartMining_->setToolTip("Click to start mining");

  // Update status label
  lblMiningStatus_->setText(miningStatusInactiveText());
  lblMiningStatus_->setStyleSheet(chromePillStyle());
  if (lblHashrate_) {
    lblHashrate_->setText("0.00 MH/s");
  }
  if (lblMiningUptime_) {
    lblMiningUptime_->setText("-");
    lblMiningUptime_->setStyleSheet("QLabel { color: #868e96; }");
  }
  resetOverviewMiningTelemetry();
}

void MainWindow::onCreateWallet() {
  if (walletSwitchInFlight_) {
    QMessageBox::information(this, "Wallet Switch In Progress",
      "Wait for the current wallet switch to finish before creating or restoring another wallet.");
    return;
  }

  if (!activeReservationId_.isEmpty()) {
    QMessageBox::warning(this, "Send In Progress",
      "A send is in progress. Wait for it to finish before creating or restoring another wallet.");
    return;
  }

  if (walletRescanning_) {
    QMessageBox::warning(this, "Wallet Scan In Progress",
      "Wait for the current wallet scan to finish before creating or restoring another wallet.");
    return;
  }

  // 🛡️ Pass ConnectionManager to wizard for bulletproof wallet creation
  auto* wizard = new WalletWizard(connectionMgr_, rpc_, this);
  wizard->setAttribute(Qt::WA_DeleteOnClose, true);
  wizard->setWindowModality(Qt::ApplicationModal);
  wizard->setProperty("setupPreviousWalletName", currentWalletName_);
  wizard->setProperty("walletWasUnloadedForSetup", false);

  connect(wizard, &WalletWizard::walletCreated, this, &MainWindow::onWalletCreated);
  connect(wizard, &QWizard::finished, this, [this, wizard](int result) {
    const bool walletWasUnloaded = wizard->property("walletWasUnloadedForSetup").toBool();
    const QString previousWallet = wizard->property("setupPreviousWalletName").toString().trimmed();

    if (result == QDialog::Accepted || !walletWasUnloaded || previousWallet.isEmpty() || !currentWalletName_.isEmpty()) {
      return;
    }

    if (cmbWalletSelector_) {
      int idx = cmbWalletSelector_->findText(previousWallet);
      if (idx < 0) {
        cmbWalletSelector_->addItem(previousWallet);
        idx = cmbWalletSelector_->findText(previousWallet);
      }
      if (idx >= 0) {
        cmbWalletSelector_->setCurrentIndex(idx);
      }
    }

    if (cmbWalletSelector_ && cmbWalletSelector_->currentText().trimmed() == previousWallet) {
      onLoadSelectedWallet();
      return;
    }

    if (walletSwitchInFlight_) {
      return;
    }

    walletSwitchInFlight_ = true;
    pendingWalletOpenName_ = previousWallet;
    updateWalletSwitcherState();
    clearWalletScopedUiState();
    if (changeAddrMgr_) {
      changeAddrMgr_->setWalletIdentityKey(QString());
    }
    lblConnectionStatus_->setText("Reloading previous wallet");
    lblConnectionStatus_->setStyleSheet(headerPillStyle());
    lblConnectionStatus_->setToolTip(QString("Reloading wallet: %1").arg(previousWallet));
    rpc_->call("wallet.open", QJsonArray{previousWallet});
  });

  wizard->show();
  wizard->raise();
  wizard->activateWindow();
}

void MainWindow::startWalletRescan(const QString& statusText) {
  if (walletRescanning_) {
    QMessageBox::information(this, "Rescan in Progress",
      "Wallet blockchain scan is already running.");
    return;
  }

  clearSafeModeRescanRetry();
  walletRescanning_ = true;
  updateWalletUIState();

  if (lblSyncProgress_) {
    lblSyncProgress_->setText(statusText);
  }

  rpc_->call("wallet.rescanblockchain", QJsonArray());
}

void MainWindow::onRescanWallet() {
  if (currentWalletName_.isEmpty()) {
    QMessageBox::warning(this, "No Wallet Loaded",
      "Load or create your wallet first, then run rescan.");
    return;
  }

  const auto reply = QMessageBox::question(
    this,
    "Rescan Wallet",
    "Rescan blockchain for wallet balances and transaction history?\n\n"
    "Use this when balance/history appears missing.\n"
    "This can take time and lock some wallet actions until complete.",
    QMessageBox::Yes | QMessageBox::Cancel,
    QMessageBox::Cancel);
  if (reply != QMessageBox::Yes) {
    return;
  }

  startWalletRescan("🔄 Scanning blockchain for wallet funds...");
}

void MainWindow::onWalletCreated(const QString& walletName, const QString& fingerprint, bool restored) {
  const QString action = restored ? "restored" : "created";
  const QString walletDisplayName = walletName.isEmpty() ? QStringLiteral("active wallet") : walletName;
  QMessageBox::information(this, restored ? "Wallet Restored" : "Wallet Created",
    QString("Wallet '%1' has been %2 successfully.\n\n"
            "Fingerprint: %3\n\n"
            "You can now generate addresses and start mining.")
      .arg(walletDisplayName,
           action,
           fingerprint));

  if (!walletName.isEmpty()) {
    currentWalletName_ = walletName;
    lblWalletName_->setText(QString("Wallet: %1").arg(walletName));
    lblWalletName_->setStyleSheet(headerPillStyle());
    lblWalletName_->setToolTip(QString("Active wallet: %1").arg(walletName));

    if (cmbWalletSelector_) {
      const bool blocked = cmbWalletSelector_->blockSignals(true);
      if (cmbWalletSelector_->findText(walletName) < 0) {
        cmbWalletSelector_->addItem(walletName);
      }
      cmbWalletSelector_->setCurrentText(walletName);
      cmbWalletSelector_->blockSignals(blocked);
    }

    pendingWalletOpenName_.clear();
    bindWalletScopedState(walletName);
    clearWalletScopedUiState();
    refreshWalletMiningAddress();
    updateWalletSwitcherState();
  }

  // Phase 6: wallet identity = fingerprint:accountIndex (matches DineroDPI model)
  // onWalletCreated always creates account 0 (BIP86 default)
  if (changeAddrMgr_ && !fingerprint.isEmpty()) {
    changeAddrMgr_->setWalletIdentityKey(QString("%1:0").arg(fingerprint));
    changeAddrMgr_->reconcileStaleReservations();
  }

  // Always refresh wallet data first.
  rpc_->call("wallet.getinfo", QJsonArray());
  rpc_->getBalance();
  rpc_->call("wallet.listwallets", QJsonArray());
  rpc_->callNamed("wallet.listaddresses", QJsonObject{{"count", 200}});
  rpc_->call("wallet.listunspent", QJsonArray());
  rpc_->call("wallet.getviewkeyinfo", QJsonArray());
  loadTransactionHistory();

  // Primary addresses are now computed by the daemon on open/unlock
  // and returned via wallet.getinfo. No separate RPC call needed.

  // Critical UX fix: after restore, trigger a one-time rescan so historical
  // UTXOs are discovered without requiring manual CLI commands.
  if (restored) {
    startWalletRescan("🔄 Restored wallet: scanning blockchain for funds...");
  }
}

bool MainWindow::isRescanSafeModeError(const QString& errorText) const {
  return errorText.contains("safe mode", Qt::CaseInsensitive) ||
         errorText.contains("deep reorg", Qt::CaseInsensitive) ||
         errorText.contains("rescan blocked", Qt::CaseInsensitive) ||
         errorText.contains("reorg detected", Qt::CaseInsensitive);
}

bool MainWindow::isInputUtxoMissingError(const QString& errorText) const {
  return errorText.contains("input utxo not found", Qt::CaseInsensitive) ||
         errorText.contains("inputs missing", Qt::CaseInsensitive) ||
         errorText.contains("missingorspent", Qt::CaseInsensitive) ||
         errorText.contains("utxo not found", Qt::CaseInsensitive);
}

void MainWindow::handleSpendInputMissing(const QString& errorText) {
  if (lblSendStatus_) {
    lblSendStatus_->setText("❌ Wallet out of sync with chain: selected inputs are not in active UTXO set. Rescan wallet and retry.");
    lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; font-weight: 600; }");
  }

  if (txtSendResult_) {
    txtSendResult_->setHtml(QString(
      "<b>Broadcast failed: input not found in active UTXO set</b><br><br>"
      "<b>Likely cause:</b> wallet view is stale after reorg/reindex/restore.<br>"
      "<b>Fix:</b> run wallet rescan, wait for completion, then retry send.<br><br>"
      "<span style='font-family: monospace; font-size: 11px;'>%1</span>"
    ).arg(errorText.toHtmlEscaped()));
  }

  const auto reply = QMessageBox::question(
    this,
    "Wallet Out Of Sync",
    "This send failed because selected inputs are not present in the node's current UTXO set.\n\n"
    "Run wallet rescan now?",
    QMessageBox::Yes | QMessageBox::No,
    QMessageBox::Yes);
  if (reply == QMessageBox::Yes) {
    startWalletRescan("🔄 Rescanning wallet after input/UTXO mismatch...");
  }
}

void MainWindow::scheduleSafeModeRescanRetry(const QString& errorText) {
  constexpr int kMaxRetries = 12;

  if (safeModeRescanRetryAttempts_ >= kMaxRetries) {
    clearSafeModeRescanRetry();
    walletRescanning_ = false;
    updateWalletUIState();
    QMessageBox::warning(this, "Wallet Scan Deferred",
      QString("Wallet restore succeeded, but scan is still blocked by node safe mode:\n\n%1\n\n"
              "Try Sync Recovery:\n"
              "1. Settings -> Sync Settings -> Reindex Chainstate\n"
              "2. Retry wallet.rescanblockchain after sync recovers.")
        .arg(errorText));
    return;
  }

  if (safeModeRescanRetryScheduled_ && safeModeRescanRetryTimer_->isActive()) {
    return;
  }

  safeModeRescanRetryAttempts_++;
  safeModeRescanRetryScheduled_ = true;
  const int delaySeconds = qMin(30 + (safeModeRescanRetryAttempts_ - 1) * 15, 180);

  if (lblSyncProgress_) {
    lblSyncProgress_->setText(
      QString("⚠️ Node safe mode (deep reorg). Auto-retrying wallet scan in %1s...")
        .arg(delaySeconds));
  }
  if (txtAlerts_) {
    txtAlerts_->append(
      QString("⚠️ Wallet scan deferred by node safe mode. Retry %1/%2 in %3s.")
        .arg(safeModeRescanRetryAttempts_)
        .arg(kMaxRetries)
        .arg(delaySeconds));
  }

  safeModeRescanRetryTimer_->start(delaySeconds * 1000);
}

void MainWindow::clearSafeModeRescanRetry() {
  safeModeRescanRetryScheduled_ = false;
  safeModeRescanRetryAttempts_ = 0;
  if (safeModeRescanRetryTimer_ && safeModeRescanRetryTimer_->isActive()) {
    safeModeRescanRetryTimer_->stop();
  }
}

void MainWindow::onExportSeed() {
  if (currentWalletName_.isEmpty()) {
    QMessageBox::information(
      this,
      "Seed Backup",
      "No wallet is loaded.\n\n"
      "Create or restore a wallet first. The BIP39 seed phrase is shown during setup and should be written down offline.");
    return;
  }

  if (walletUnlocked_ == false) {
    QMessageBox::information(
      this,
      "Seed Backup",
      "Wallet seed export, when supported by the daemon, requires the wallet to be unlocked first.\n\n"
      "Unlock the wallet, then open Seed Backup / Mobile Restore again.");
    return;
  }

  auto exportSeedResultConn = std::make_shared<QMetaObject::Connection>();
  auto exportSeedErrorConn = std::make_shared<QMetaObject::Connection>();

  auto showBackupUnavailable = [this](const QString& detail, const QString& suggestion) {
    QDialog* dialog = new QDialog(this);
    dialog->setWindowTitle("Seed Backup / Mobile Restore");
    dialog->resize(620, 360);
    dialog->setStyleSheet(
      "QDialog { background: #1d2229; color: #dbe3ec; }"
      "QLabel { color: #dbe3ec; font-size: 13px; }"
      "QGroupBox { border: 1px solid #343b45; border-radius: 8px; margin-top: 10px; padding-top: 8px; background: #20262e; }"
      "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: #cbd5df; }");

    auto* layout = new QVBoxLayout(dialog);
    layout->setSpacing(10);

    auto* title = new QLabel("Seed phrase re-export is not available for this wallet.");
    title->setStyleSheet("QLabel { font-size: 16px; font-weight: 700; color: #eef3f8; }");
    title->setWordWrap(true);
    layout->addWidget(title);

    auto* body = new QLabel(
      "Dinero can restore from the BIP39 phrase you wrote down during setup, but this wallet storage cannot reconstruct those words later. "
      "The original phrase is converted into wallet seed material, and that conversion is one-way unless a future daemon explicitly stores an encrypted mnemonic backup.");
    body->setWordWrap(true);
    layout->addWidget(body);

    auto* restoreGroup = new QGroupBox("Restore Paths");
    auto* restoreLayout = new QVBoxLayout(restoreGroup);
    auto* restoreText = new QLabel(
      "Use the original paper seed backup for mobile restore.\n\n"
      "Taproot payments: m/86'/1448'/0'/0/i, din1p...\n"
      "Quantum-safe P2MR: m/88'/1448'/0'/0/i, din1r...");
    restoreText->setWordWrap(true);
    restoreLayout->addWidget(restoreText);
    layout->addWidget(restoreGroup);

    if (!detail.trimmed().isEmpty() || !suggestion.trimmed().isEmpty()) {
      auto* note = new QLabel;
      QString noteText;
      if (!detail.trimmed().isEmpty()) {
        noteText += "Backend note: " + detail.trimmed();
      }
      if (!suggestion.trimmed().isEmpty()) {
        if (!noteText.isEmpty()) noteText += "\n";
        noteText += "Suggestion: " + suggestion.trimmed();
      }
      note->setText(noteText);
      note->setWordWrap(true);
      note->setStyleSheet("QLabel { color: #9fabba; font-size: 11px; }");
      layout->addWidget(note);
    }

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    auto* btnClose = new QPushButton("OK");
    btnClose->setStyleSheet(chromeButtonStyle());
    connect(btnClose, &QPushButton::clicked, dialog, &QDialog::accept);
    buttonRow->addWidget(btnClose);
    layout->addLayout(buttonRow);

    dialog->exec();
    delete dialog;
  };

  auto showSeedPhrase = [this](const QString& seedPhrase, const QJsonObject& obj) {
    QMessageBox::StandardButton reply = QMessageBox::warning(
      this,
      "Critical Security Warning",
      "You are about to view your seed phrase.\n\n"
      "Anyone with this phrase can access your funds.\n\n"
      "Write it down on paper, store it offline, and never share it.",
      QMessageBox::Yes | QMessageBox::No,
      QMessageBox::No);

    if (reply != QMessageBox::Yes) {
      return;
    }

    QStringList words = seedPhrase.simplified().split(' ', Qt::SkipEmptyParts);

    QDialog* dialog = new QDialog(this);
    dialog->setWindowTitle("Seed Phrase");
    dialog->resize(640, 460);

    auto* layout = new QVBoxLayout(dialog);

    auto* warningLabel = new QLabel(
      "<b>Write this phrase down on paper.</b><br>"
      "Do not screenshot it, upload it, paste it into chat, or store it in cloud notes.");
    warningLabel->setWordWrap(true);
    warningLabel->setStyleSheet(
      "QLabel { background: #2c3036; color: #e6ecf2; padding: 10px; "
      "border: 1px solid #3d434d; border-radius: 6px; }");
    layout->addWidget(warningLabel);

    auto* phraseGroup = new QGroupBox("BIP39 Recovery Phrase");
    auto* phraseGrid = new QGridLayout(phraseGroup);
    phraseGrid->setHorizontalSpacing(16);
    phraseGrid->setVerticalSpacing(6);

    const int columns = 3;
    const int rows = (words.size() + columns - 1) / columns;
    for (int i = 0; i < words.size(); ++i) {
      const int col = i / rows;
      const int row = i % rows;
      auto* word = new QLabel(QString("%1. %2").arg(i + 1, 2, 10, QChar('0')).arg(words.at(i)));
      word->setTextInteractionFlags(Qt::TextSelectableByMouse);
      word->setStyleSheet(
        "QLabel { background: #171c23; color: #dfe7ef; padding: 7px 10px; "
        "border: 1px solid #303743; border-radius: 5px; font-family: monospace; }");
      phraseGrid->addWidget(word, row, col);
    }
    layout->addWidget(phraseGroup);

    auto* compat = new QLabel(
      "Restore path notes:\n"
      "• Taproot payments: m/86'/1448'/0'/0/i, din1p...\n"
      "• Quantum-safe P2MR: m/88'/1448'/0'/0/i, din1r...\n");
    compat->setWordWrap(true);
    layout->addWidget(compat);

    const QString fingerprint = obj.value("fingerprint").toString();
    if (!fingerprint.isEmpty()) {
      auto* fp = new QLabel("Master fingerprint: " + fingerprint);
      fp->setTextInteractionFlags(Qt::TextSelectableByMouse);
      layout->addWidget(fp);
    }

    auto* btnClose = new QPushButton("Close");
    btnClose->setStyleSheet(chromeButtonStyle());
    connect(btnClose, &QPushButton::clicked, dialog, &QDialog::accept);
    layout->addWidget(btnClose);

    dialog->exec();
    delete dialog;
  };

  *exportSeedResultConn = connect(rpc_, &RpcClient::rpcResult, this,
      [this, exportSeedResultConn, exportSeedErrorConn, showBackupUnavailable, showSeedPhrase](const QString& method, const QJsonValue& result) {
    if (method != "wallet.exportseed") {
      return;
    }

    QObject::disconnect(*exportSeedResultConn);
    QObject::disconnect(*exportSeedErrorConn);

    if (!result.isObject()) {
      showBackupUnavailable("Invalid response from wallet.exportseed.", QString());
      return;
    }

    const QJsonObject obj = result.toObject();
    const QString backendError = obj.value("error").toString();
    if (!backendError.isEmpty()) {
      showBackupUnavailable(backendError, obj.value("suggestion").toString());
      return;
    }

    QString seedPhrase = obj.value("mnemonic").toString().trimmed();
    if (seedPhrase.isEmpty()) {
      seedPhrase = obj.value("seed_phrase").toString().trimmed();
    }
    if (seedPhrase.isEmpty()) {
      seedPhrase = obj.value("seedPhrase").toString().trimmed();
    }

    if (seedPhrase.isEmpty()) {
      showBackupUnavailable("wallet.exportseed returned no mnemonic.", QString());
      return;
    }

    showSeedPhrase(seedPhrase, obj);
  });
  
  *exportSeedErrorConn = connect(rpc_, &RpcClient::rpcError, this,
      [this, exportSeedResultConn, exportSeedErrorConn](const QString& method, int code, const QString& message) {
    Q_UNUSED(code);
    if (method != "wallet.exportseed") {
      return;
    }

    QObject::disconnect(*exportSeedResultConn);
    QObject::disconnect(*exportSeedErrorConn);

    QMessageBox::critical(this, "Error",
      "Seed backup request failed:\n\n" + message + "\n\n"
      "If this daemon does not support encrypted mnemonic export, use the original paper seed backup from wallet setup.");
  });

  rpc_->call("wallet.exportseed", QJsonArray{});
}

static QString daemonDiagnosticLogPath(const QString& datadir) {
  const QStringList candidates = {
    QDir(datadir).filePath("debug.log"),
    QDir(datadir).filePath("dinero.log"),
    QDir(datadir).filePath("wallet.log"),
    QDir(datadir).filePath("p2p.log")};
  QString newest;
  for (const QString& path : candidates) {
    const QFileInfo info(path);
    if (!info.isFile()) continue;
    if (newest.isEmpty() || info.lastModified() > QFileInfo(newest).lastModified()) {
      newest = path;
    }
  }
  return newest;
}

// Last ~20 lines of the newest real daemon log for watchdog diagnostics.
static QString daemonDebugLogTail(const QString& datadir, int maxLines = 20) {
  const QString logPath = daemonDiagnosticLogPath(datadir);
  if (logPath.isEmpty()) {
    return QStringLiteral("(no daemon log found in %1)").arg(datadir);
  }
  QFile log(logPath);
  if (!log.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QStringLiteral("(could not read %1)").arg(logPath);
  }
  const qint64 kTailBytes = 64 * 1024;
  if (log.size() > kTailBytes) {
    log.seek(log.size() - kTailBytes);
  }
  const QStringList lines = QString::fromUtf8(log.readAll())
                                .split('\n', Qt::SkipEmptyParts);
  const int start = qMax(0, static_cast<int>(lines.size()) - maxLines);
  return QStringLiteral("Last lines of %1:\n%2")
      .arg(logPath, lines.mid(start).join('\n'));
}

void MainWindow::onStartupWatchdogTimeout() {
  if (shuttingDown_ || !connectionMgr_ || connectionMgr_->isConnected()) {
    return;
  }
  if (daemonStopRequested_) {
    return;  // User explicitly stopped the daemon — waiting is expected.
  }

  QString datadir = rpc_ ? rpc_->datadir() : QString();
  if (datadir.trimmed().isEmpty()) {
    datadir = defaultDineroDataDir();
  }
  const QString logPath = daemonDiagnosticLogPath(datadir);

  qWarning() << "Startup watchdog: no daemon RPC connection after 180s";

  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle("Still Waiting for Daemon");
  box.setText("Dinero has been waiting 180 seconds for the daemon (dinerod) "
              "and is still not connected — the daemon may have failed.");
  box.setInformativeText(
    "Common causes:\n"
    "  • Port 20998 is held by another process (lsof -i :20998)\n"
    "  • Another Dinero instance is using the same data directory\n\n"
    "You can keep waiting, open the daemon log, or quit.");
  box.setDetailedText(daemonDebugLogTail(datadir));
  QPushButton* showLogBtn = box.addButton("Show Log", QMessageBox::ActionRole);
  QPushButton* waitBtn = box.addButton("Keep Waiting", QMessageBox::AcceptRole);
  box.addButton("Quit", QMessageBox::DestructiveRole);
  box.setDefaultButton(waitBtn);
  box.exec();

  if (box.clickedButton() == showLogBtn) {
    if (!logPath.isEmpty()) {
      QDesktopServices::openUrl(QUrl::fromLocalFile(logPath));
    }
    // Re-arm: the user is investigating, keep the watchdog alive.
    QTimer::singleShot(180000, this, &MainWindow::onStartupWatchdogTimeout);
  } else if (box.clickedButton() == waitBtn) {
    QTimer::singleShot(180000, this, &MainWindow::onStartupWatchdogTimeout);
  } else {
    close();
  }
}

void MainWindow::maybeAutoStartDaemon() {
  if (autoStartDaemonAttempted_ || !connectionMgr_) {
    return;
  }

  const auto state = connectionMgr_->state();
  if (state == ConnectionManager::Connected) {
    return;
  }

  if (state == ConnectionManager::Connecting) {
    QTimer::singleShot(1500, this, &MainWindow::maybeAutoStartDaemon);
    return;
  }

  autoStartDaemonAttempted_ = true;
  if (!startDaemonWithOptions(false, false)) {
    qWarning() << "Silent auto-start of bundled dinerod failed";
  }
}

bool MainWindow::maybeShowP2PNetworkNotice() {
  QSettings settings;
  if (settings.contains(p2pPortMapAllowedKey())) {
    return settings.value(p2pPortMapAllowedKey(), true).toBool();
  }

  // rc8+: default-ON. The portmap subsystem runs on a worker thread with
  // a bounded discovery deadline (default 45s), so a hostile router can
  // no longer freeze startup. We still show a one-time non-blocking
  // notice and offer a Disable button for users who don't want it.
  settings.setValue(p2pAccessPromptAcceptedKey(), true);
  settings.setValue(p2pPortMapAllowedKey(), true);

  QMessageBox box(this);
  box.setIcon(QMessageBox::Information);
  box.setWindowTitle("Dinero P2P Networking");
  box.setText(QStringLiteral("Dinero is enabling automatic router port mapping."));
  box.setInformativeText(
      QStringLiteral("Dinero will ask your home router (UPnP / NAT-PMP) to open TCP %1 so "
                     "other Dinero nodes can connect inbound. Outbound sync works either way.\n\n"
                     "You can toggle this later via the Network menu.")
          .arg(kDineroMainnetP2PPort));
  box.setStandardButtons(QMessageBox::Ok);
  QPushButton* disableButton = box.addButton(QStringLiteral("Disable"), QMessageBox::RejectRole);
  box.setDefaultButton(QMessageBox::Ok);
  box.exec();

  if (box.clickedButton() == disableButton) {
    settings.setValue(p2pPortMapAllowedKey(), false);
    return false;
  }
  return true;
}

// Defined in main.cpp: sweep any orphaned dinerod/dinero-seeder squatting the
// RPC port (127.0.0.1:20998). Used by the daemon-restart retry below so a held
// port can't make every retry fail and surface the "Daemon Failed" dialog.
void killStaleDinerodByPort();

bool MainWindow::startDaemonWithOptions(bool showFeedback, bool openLogWindow) {
  if (daemonProcess_ && daemonProcess_->state() == QProcess::Running) {
    if (showFeedback) {
      QMessageBox::information(this, "Daemon Running", "Daemon is already running!");
    }
    return true;
  }

  // Suppress error dialogs during daemon startup to prevent nested dialog crash
  suppressErrorDialogs_ = true;

  // Find dinerod binary - check custom path first
  QString dinerodPath = edtDaemonPath_ ? edtDaemonPath_->text().trimmed() : QString();

  // If custom path is set, verify it exists
  if (!dinerodPath.isEmpty()) {
    if (QFile::exists(dinerodPath)) {
      qDebug() << "Using custom daemon path:" << dinerodPath;
    } else if (showFeedback) {
      QMessageBox::critical(this, "Invalid Daemon Path",
        QString("Custom daemon path not found:\n%1\n\n"
                "Please verify the path or use Browse button.").arg(dinerodPath));
      suppressErrorDialogs_ = false;
      return false;
    } else {
      qWarning() << "Saved daemon path missing, falling back to bundled search:" << dinerodPath;
      QSettings().remove("daemon/custom_path");
      if (edtDaemonPath_) {
        edtDaemonPath_->clear();
      }
      dinerodPath.clear();
    }
  }

  // If no custom path, search in standard locations
  if (dinerodPath.isEmpty()) {
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList searchPaths;

#ifdef Q_OS_MAC
    // macOS: Search in app bundle and common locations
    searchPaths = {
      // 1. Inside .app bundle: Dinero-qt.app/Contents/Resources/dinerod
      appDir + "/../Resources/dinerod",
      // 2. Inside .app bundle: Dinero-qt.app/Contents/MacOS/dinerod
      appDir + "/dinerod",
      // 3. Sibling to .app: DineroCoin/dinerod (distribution layout)
      appDir + "/../../../../dinerod",
      // 4. Parent directory bin folder: ../bin/dinerod (development)
      appDir + "/../../../bin/dinerod",
      // 5. System PATH
      QStandardPaths::findExecutable("dinerod")
    };
#elif defined(Q_OS_WIN)
    searchPaths = {
      appDir + "/dinerod.exe",
      appDir + "/../dinerod.exe",
      "C:/Program Files/DineroCoin/dinerod.exe",
      QStandardPaths::findExecutable("dinerod")
    };
#else
    searchPaths = {
      appDir + "/dinerod",
      appDir + "/../dinerod",
      "/usr/local/bin/dinerod",
      "/usr/bin/dinerod",
      QStandardPaths::findExecutable("dinerod")
    };
#endif

    for (const QString& path : searchPaths) {
      if (!path.isEmpty() && QFile::exists(path)) {
        dinerodPath = path;
        break;
      }
    }

    if (dinerodPath.isEmpty()) {
      if (showFeedback) {
        QMessageBox::critical(this, "Daemon Not Found",
          "Could not find dinerod binary.\n\n"
          "Please use the 'Browse...' button to locate dinerod.\n\n"
          "Searched in:\n" + searchPaths.join("\n"));
      } else {
        qWarning() << "Could not find dinerod binary. Searched in:" << searchPaths;
      }
      suppressErrorDialogs_ = false;
      return false;
    }
  }

  const QString datadir = rpc_->datadir();

  // #295: pre-spawn port check. If something already listens on the RPC
  // port, a fresh dinerod cannot bind and will exit — ask the user instead
  // of failing silently. The listener might be a usable existing daemon,
  // so offer to connect to it.
  {
    QTcpSocket probe;
    probe.connectToHost("127.0.0.1", 20998);
    const bool portInUse = probe.waitForConnected(500);
    probe.abort();
    if (portInUse && showFeedback) {
      QMessageBox box(this);
      box.setIcon(QMessageBox::Warning);
      box.setWindowTitle("Port Already in Use");
      box.setText("Port 20998 is already in use — another Dinero process "
                  "may be running.");
      box.setInformativeText(
        "If an existing Dinero daemon is running, the wallet can connect to "
        "it directly instead of starting a new one. If the port is held by a "
        "stale process, starting a new daemon will fail until it is cleared "
        "(lsof -i :20998).");
      QPushButton* connectBtn =
          box.addButton("Connect to Existing", QMessageBox::AcceptRole);
      QPushButton* startBtn =
          box.addButton("Start Anyway", QMessageBox::ActionRole);
      box.addButton("Cancel", QMessageBox::RejectRole);
      box.setDefaultButton(connectBtn);
      box.exec();
      if (box.clickedButton() == connectBtn) {
        suppressErrorDialogs_ = false;
        rpc_->loadCookie();
        connectionMgr_->connectToDaemon();
        return true;
      }
      if (box.clickedButton() != startBtn) {
        suppressErrorDialogs_ = false;
        return false;
      }
    }
  }

  const bool allowPortMapping = maybeShowP2PNetworkNotice();

  if (!daemonProcess_) {
    daemonProcess_ = new QProcess(this);
    // #295: fail loud if the daemon dies before RPC ever came up. Without
    // this the GUI sat in "Connecting..." forever when e.g. the RPC port
    // was squatted by an orphaned process. Connected once per QProcess
    // (the object is reused across restarts).
    connect(daemonProcess_,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
      if (shuttingDown_ || daemonStopRequested_) {
        return;
      }
      if (connectionMgr_ && connectionMgr_->isConnected()) {
        return;  // RPC was up; ConnectionManager surfaces the disconnect.
      }
      if (status == QProcess::NormalExit && exitCode == 0) {
        return;  // Clean external stop (e.g. dinero-cli stop).
      }
      // Auto-retry transient launch races before alarming the user. On a large
      // datadir a prior dinerod can still be flushing state / holding the datadir
      // lock or P2P port when we relaunch, so the freshly-started process exits
      // non-zero; a short delay lets the old one finish and the retry then
      // succeeds silently. Only a persistent failure (retries exhausted) surfaces
      // the "Daemon Failed" dialog. Counter resets on successful connect and on
      // a user-initiated start.
      static constexpr int kMaxDaemonLaunchRetries = 3;
      if (daemonLaunchRetries_ < kMaxDaemonLaunchRetries) {
        ++daemonLaunchRetries_;
        qWarning() << "Daemon exited (code" << exitCode << ") before RPC came up — retry"
                   << daemonLaunchRetries_ << "of" << kMaxDaemonLaunchRetries
                   << "in 2.5s (likely a transient datadir-lock / port race)";
        // Our daemon exited non-zero before RPC ever came up — almost always an
        // orphaned dinerod/dinero-seeder still squatting RPC port 20998 (#295).
        // We are NOT connected here, so the port-holder is blocking us (not a
        // healthy daemon to adopt); clear it so the retry below can actually bind.
        killStaleDinerodByPort();
        if (lblConnectionStatus_) {
          lblConnectionStatus_->setText("Starting daemon… (retry)");
          lblConnectionStatus_->setStyleSheet(headerPillStyle());
        }
        QTimer::singleShot(2500, this, [this]() {
          if (!shuttingDown_ && !daemonStopRequested_ &&
              (!connectionMgr_ || !connectionMgr_->isConnected())) {
            startDaemonWithOptions(false, false);
          }
        });
        return;
      }
      QString dir = rpc_ ? rpc_->datadir() : QString();
      if (dir.trimmed().isEmpty()) {
        dir = defaultDineroDataDir();
      }
      // The scary Critical "Daemon Failed" modal was removed: a daemon that
      // exits before RPC during startup is usually a transient race (port
      // handoff from a still-running daemon, datadir-lock release), and the
      // retry logic above plus the ConnectionManager keep trying and adopt the
      // daemon once it is healthy. Interrupting the user with a modal here made
      // them force-quit mid-init, which only made things worse. Surface the
      // state quietly; a GENUINE, persistent failure is still reported by the
      // (non-modal) startup watchdog after 180s.
      Q_UNUSED(dir);
      qWarning() << "Daemon exited before RPC came up, code:" << exitCode
                 << "(retries exhausted; ConnectionManager keeps trying, "
                    "watchdog will surface a persistent failure)";
      if (lblConnectionStatus_) {
        lblConnectionStatus_->setText("Starting daemon…");
        lblConnectionStatus_->setStyleSheet(headerPillStyle());
      }
      btnStartDaemon_->setVisible(true);
      btnStopDaemon_->setVisible(false);
    });
  }
  daemonStopRequested_ = false;

  QStringList args = {
    QString("--datadir=%1").arg(datadir),
    QString("--embedded-parent-pid=%1").arg(QCoreApplication::applicationPid())
  };
  args << "--listen" << "--p2pport" << QString::number(kDineroMainnetP2PPort)
       << "--rpc" << "--rpcport" << "20998";
  if (allowPortMapping) {
    args << "--portmap=auto";
  }
  args << "-addnode=173.249.200.59:20999";
  args << "-addnode=172.93.167.32:20999";
  args << "-addnode=92.118.190.62:20999";

  qDebug() << "Starting daemon:" << dinerodPath << args << "showFeedback=" << showFeedback
           << "openLogWindow=" << openLogWindow;

  daemonProcess_->start(dinerodPath, args);

  if (!daemonProcess_->waitForStarted(3000)) {
    suppressErrorDialogs_ = false;
    qWarning() << "Failed to start daemon:" << daemonProcess_->errorString();
    if (showFeedback) {
      QMessageBox::critical(this, "Start Failed",
        "Failed to start daemon:\n\n" + daemonProcess_->errorString());
    }
    return false;
  }

  btnStartDaemon_->setVisible(false);
  btnStopDaemon_->setVisible(true);
  lblConnectionStatus_->setText("Starting daemon...");
  lblConnectionStatus_->setStyleSheet(headerPillStyle());

  if (openLogWindow) {
    QString terminalCmd;
#ifdef Q_OS_MAC
    const QString debugLog = datadir + "/debug.log";
    terminalCmd = QString("osascript -e 'tell application \"Terminal\" to do script \"echo \\\"Dinero Daemon Output\\\"; echo \\\"===================\\\"; tail -f %1\"'").arg(debugLog);
#else
    const QString debugLog = datadir + "/debug.log";
    terminalCmd = QString("xterm -e 'tail -f %1' &").arg(debugLog);
#endif
    QProcess::startDetached("/bin/sh", QStringList() << "-c" << terminalCmd);
  }

  if (showFeedback) {
    const QString startedMessage = openLogWindow
      ? QStringLiteral("Dinerod daemon started successfully!\n\n"
                       "Terminal window opened showing daemon output.\n"
                       "Waiting for RPC to become available...")
      : QStringLiteral("Dinerod daemon started successfully!\n\n"
                       "Waiting for RPC to become available...");
    QMessageBox::information(this, "Daemon Started", startedMessage);
  }

  // Wait longer for daemon to create cookie and start RPC server
  QTimer::singleShot(5000, this, [this, datadir, showFeedback]() {
    const QString cookiePath = datadir + "/.cookie";
    int attempts = 0;
    while (attempts < 10 && !QFile::exists(cookiePath)) {
      QThread::msleep(500);
      attempts++;
    }

    if (QFile::exists(cookiePath)) {
      rpc_->loadCookie();
#ifdef DIN_EXPERIMENTAL_FEATURES
      ws_->loadCookie();
#endif
      qDebug() << "✅ Reloaded cookie after daemon start";
      connectionMgr_->connectToDaemon();
      btnStartDaemon_->setVisible(false);
      btnStopDaemon_->setVisible(true);
    } else {
      qWarning() << "⚠️ Cookie file not created yet after 5 seconds";
      if (lblConnectionStatus_) {
        lblConnectionStatus_->setText("Daemon started, waiting for RPC");
        lblConnectionStatus_->setStyleSheet(headerPillStyle());
      }
      if (showFeedback) {
        QMessageBox::warning(this, "Daemon Started",
          "Daemon started but RPC not ready yet.\n\n"
          "Wait a few more seconds and click 'Reconnect' if needed.");
      }
    }

    refresh();
#ifdef DIN_EXPERIMENTAL_FEATURES
    ws_->connectToServer();
#endif

    QTimer::singleShot(10000, this, [this]() {
      suppressErrorDialogs_ = false;
      qDebug() << "✅ Error dialogs re-enabled after daemon startup";
    });
  });

  return true;
}

void MainWindow::onStartDaemon() {
  daemonLaunchRetries_ = 0;  // fresh user-initiated start — full retry budget
  startDaemonWithOptions(true, true);
}

void MainWindow::onStopDaemon() {
  auto reply = QMessageBox::question(this, "Stop Daemon",
    "Are you sure you want to stop the daemon?\n\n"
    "This will:\n"
    "• Request the local dinerod for this wallet datadir to stop\n"
    "• Unlock database files\n"
    "• Disconnect GUI from blockchain\n\n"
    "Continue?",
    QMessageBox::Yes | QMessageBox::No);

  if (reply != QMessageBox::Yes) {
    return;
  }

  // #295: user-requested stop — keep the unexpected-exit dialog quiet.
  daemonStopRequested_ = true;

  stopLocalStratumServer();

  const QString datadir = rpc_->datadir();
  auto resolveCliPath = []() -> QString {
    const QString appDir = QCoreApplication::applicationDirPath();
    QString cliPath;
#ifdef Q_OS_MAC
    cliPath = QDir(appDir).absoluteFilePath("../Resources/dinero-cli");
    if (!QFile::exists(cliPath)) cliPath = QDir(appDir).absoluteFilePath("dinero-cli");
    if (!QFile::exists(cliPath)) cliPath = QDir(appDir).absoluteFilePath("../dinero-cli");
    if (!QFile::exists(cliPath)) cliPath = QDir(appDir).absoluteFilePath("../../dinero-cli");
    if (!QFile::exists(cliPath)) cliPath = QStandardPaths::findExecutable("dinero-cli");
#elif defined(Q_OS_WIN)
    cliPath = QDir(appDir).absoluteFilePath("dinero-cli.exe");
    if (!QFile::exists(cliPath)) cliPath = QDir(appDir).absoluteFilePath("../dinero-cli.exe");
    if (!QFile::exists(cliPath)) cliPath = QStandardPaths::findExecutable("dinero-cli.exe");
#else
    cliPath = QDir(appDir).absoluteFilePath("dinero-cli");
    if (!QFile::exists(cliPath)) cliPath = QDir(appDir).absoluteFilePath("../dinero-cli");
    if (!QFile::exists(cliPath)) cliPath = QStandardPaths::findExecutable("dinero-cli");
#endif
    return cliPath;
  };

  bool stopRequested = false;
  const QString cliPath = resolveCliPath();
  if (!cliPath.isEmpty() && QFile::exists(cliPath) && !datadir.isEmpty()) {
    QProcess stopProcess;
    stopProcess.start(cliPath, QStringList() << QString("--datadir=%1").arg(datadir) << "stop");
    if (stopProcess.waitForFinished(15000) &&
        stopProcess.exitStatus() == QProcess::NormalExit &&
        stopProcess.exitCode() == 0) {
      stopRequested = true;
    } else {
      qWarning() << "Scoped daemon stop via dinero-cli failed:" << stopProcess.errorString()
                 << stopProcess.readAllStandardError();
    }
  }

  if (!stopRequested) {
    // Best-effort fallback for older/bundled setups where dinero-cli may not be present.
    rpc_->call("daemon.stop", QJsonArray());
    QThread::msleep(1000);
  }

  // Disconnect ConnectionManager after we've sent the stop request.
  connectionMgr_->disconnectFromDaemon();

  // Stop GUI-managed wrapper process (if any)
  if (daemonProcess_ && daemonProcess_->state() == QProcess::Running) {
    daemonProcess_->terminate();
    if (!daemonProcess_->waitForFinished(10000)) {
      daemonProcess_->kill();
      daemonProcess_->waitForFinished(2000);
    }
  }

  // Update GUI state
  btnStartDaemon_->setVisible(true);
  btnStopDaemon_->setVisible(false);
  lblConnectionStatus_->setText("Daemon stopped");
  lblConnectionStatus_->setStyleSheet(headerPillStyle());

  QMessageBox::information(this, "Daemon Stopped", 
    "✅ All daemon processes stopped\n\n"
    "Database files unlocked.\n"
    "Click 'Start Daemon' when ready to restart.");
}

void MainWindow::detectExistingDaemon() {
  // Intelligent daemon state detection on startup
  // Try to connect via RPC to see if daemon is already running
  
  QNetworkAccessManager* nam = new QNetworkAccessManager(this);
  QNetworkRequest request(QUrl("http://127.0.0.1:20998/"));
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  
  // Read cookie for authentication
  const QString cookiePath = QDir(defaultDineroDataDir()).filePath(".cookie");
  QFile cookieFile(cookiePath);
  QString authHeader = "";
  if (cookieFile.open(QIODevice::ReadOnly)) {
    QString cookie = cookieFile.readAll().trimmed();
    authHeader = "Basic " + cookie.toUtf8().toBase64();
    cookieFile.close();
    request.setRawHeader("Authorization", authHeader.toUtf8());
  }
  
  // Send simple getblockcount request
  QJsonObject rpcRequest;
  rpcRequest["jsonrpc"] = "1.0";
  rpcRequest["id"] = "detect_daemon";
  rpcRequest["method"] = "getblockcount";
  rpcRequest["params"] = QJsonArray();
  
  QJsonDocument doc(rpcRequest);
  QByteArray data = doc.toJson();
  
  QNetworkReply* reply = nam->post(request, data);
  
  // Use QPointer to safely track reply lifetime (prevents crash if deleted)
  QPointer<QNetworkReply> replyPtr(reply);
  QPointer<QNetworkAccessManager> namPtr(nam);
  
  // Handle response
  connect(reply, &QNetworkReply::finished, this, [this, replyPtr, namPtr]() {
    // Check if reply still exists (might be deleted by timeout)
    if (!replyPtr) return;
    
    bool daemonRunning = false;
    
    if (replyPtr->error() == QNetworkReply::NoError) {
      // Daemon is running and responded
      daemonRunning = true;
    }
    
    // Update button states based on detection
    if (daemonRunning) {
      // Daemon is already running - show Stop button
      btnStartDaemon_->setVisible(false);
      btnStopDaemon_->setVisible(true);
      lblConnectionStatus_->setText("Daemon running");
      lblConnectionStatus_->setStyleSheet(headerPillStyle());
    } else {
      // Daemon is not running - show Start button
      btnStartDaemon_->setVisible(true);
      btnStopDaemon_->setVisible(false);
      lblConnectionStatus_->setText("Daemon not running");
      lblConnectionStatus_->setStyleSheet(headerPillStyle());
    }
    
    replyPtr->deleteLater();
    if (namPtr) namPtr->deleteLater();
  });
  
  // Set timeout for detection (use QPointer to avoid crash on deleted reply)
  QTimer::singleShot(2000, this, [replyPtr, namPtr]() {
    // Safely check if reply still exists before calling isRunning()
    if (replyPtr && replyPtr->isRunning()) {
      replyPtr->abort();
    }
  });
}

void MainWindow::onUnlockWallet() {
  bool ok;
  QString password = QInputDialog::getText(this, "Unlock Wallet",
    "Enter wallet password:", QLineEdit::Password, "", &ok);

  if (ok && !password.isEmpty()) {
    auto isNotEncryptedError = [](const QString& rawError) -> bool {
      QString error = rawError.trimmed();
      const QString failedPrefix = "failed to unlock wallet:";
      if (error.toLower().startsWith(failedPrefix)) {
        error = error.mid(failedPrefix.size()).trimmed();
      }
      return error.toLower().contains("not encrypted");
    };

    auto buildUnlockErrorMessage = [](const QString& rawError) -> QString {
      QString error = rawError.trimmed();
      const QString failedPrefix = "failed to unlock wallet:";
      if (error.toLower().startsWith(failedPrefix)) {
        error = error.mid(failedPrefix.size()).trimmed();
      }

      const QString lower = error.toLower();
      if (lower.contains("invalid passphrase") ||
          lower.contains("invalid password") ||
          lower.contains("wrong password") ||
          lower.contains("incorrect password")) {
        return "❌ Wrong password!\n\nThe password you entered is incorrect.\nPlease try again.";
      }
      if (lower.contains("not encrypted")) {
        return "ℹ️  Wallet is not encrypted.\n\nYou don't need a password to unlock an unencrypted wallet.\nConsider encrypting your wallet for security!";
      }
      if (lower.contains("no hd wallet")) {
        return "⚠️  No wallet found!\n\nPlease create a new wallet or restore from seed phrase first.";
      }
      return "❌ Failed to unlock wallet:\n\n" + error;
    };

    // Show unlocking spinner state
    btnWalletLock_->setEnabled(false);
    btnWalletLock_->setText("Unlocking...");
    btnWalletLock_->setStyleSheet(chromeButtonStyle());

    auto resultConn = std::make_shared<QMetaObject::Connection>();
    auto errorConn = std::make_shared<QMetaObject::Connection>();

    // Wait for wallet.unlock response only. Do not use SingleShotConnection:
    // unrelated RPC calls can arrive first and consume the callback.
    *resultConn = connect(rpc_, &RpcClient::rpcResult, this,
      [this, isNotEncryptedError, buildUnlockErrorMessage, resultConn, errorConn](const QString& method, const QJsonValue& result) {
        if (method != "wallet.unlock") return;
        QObject::disconnect(*resultConn);
        QObject::disconnect(*errorConn);

        // Re-enable button
        btnWalletLock_->setEnabled(true);

        // Check if result contains an error
        if (result.isObject() && result.toObject().contains("error")) {
          const QJsonValue rawErrorValue = result.toObject().value("error");
          QString error;
          if (rawErrorValue.isString()) {
            error = rawErrorValue.toString();
          } else if (rawErrorValue.isObject()) {
            error = rawErrorValue.toObject().value("message").toString("Unknown error");
          } else {
            error = "Unknown error";
          }
          if (isNotEncryptedError(error)) {
            walletUnlocked_ = true;
            unlockSecondsRemaining_ = 0;
            unlockCountdownTimer_->stop();
            btnWalletLock_->setText("Unlocked | Lock");
            btnWalletLock_->setStyleSheet(chromeButtonStyle());
            btnWalletLock_->setToolTip("Wallet is not encrypted. Encrypt wallet for stronger security.");
            updateWalletUIState();
            QMessageBox::information(this, "Wallet Unencrypted",
              "ℹ️ Wallet is not encrypted.\n\n"
              "No unlock is required.\n"
              "Use Encrypt Wallet to protect private keys.");
          } else {
            QMessageBox::critical(this, "Unlock Failed", buildUnlockErrorMessage(error));

            // Reset to locked state UI
            walletUnlocked_ = false;
            btnWalletLock_->setText("Locked | Unlock");
            btnWalletLock_->setStyleSheet(chromeButtonStyle());
          }

        } else {
          // SUCCESS: Wallet unlocked
          walletUnlocked_ = true;

          // Start countdown timer (15 minutes = 900 seconds)
          unlockSecondsRemaining_ = 900;
          unlockCountdownTimer_->start(1000);  // Tick every second

          // Update button to show unlocked state with countdown
          int mins = unlockSecondsRemaining_ / 60;
          int secs = unlockSecondsRemaining_ % 60;
          btnWalletLock_->setText(QString("Unlocked (%1:%2) | Lock")
            .arg(mins, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0')));
          btnWalletLock_->setStyleSheet(chromeButtonStyle());
          btnWalletLock_->setToolTip("Wallet unlocked. Click to lock and secure private keys.");

          // Update all wallet-dependent UI
          updateWalletUIState();

          QMessageBox::information(this, "Success",
            "✅ Wallet unlocked successfully!\n\n"
            "• Taproot signing is now enabled\n"
            "• Wallet will auto-lock in 15 minutes\n"
            "• You can now spend P2TR outputs and sign transactions");
        }
      });

    *errorConn = connect(rpc_, &RpcClient::rpcError, this,
      [this, isNotEncryptedError, buildUnlockErrorMessage, resultConn, errorConn](const QString& method, int code, const QString& message) {
        Q_UNUSED(code);
        if (method != "wallet.unlock") return;
        QObject::disconnect(*resultConn);
        QObject::disconnect(*errorConn);

        // Re-enable button
        btnWalletLock_->setEnabled(true);

        if (isNotEncryptedError(message)) {
          walletUnlocked_ = true;
          unlockSecondsRemaining_ = 0;
          unlockCountdownTimer_->stop();
          btnWalletLock_->setText("Unlocked | Lock");
          btnWalletLock_->setStyleSheet(chromeButtonStyle());
          btnWalletLock_->setToolTip("Wallet is not encrypted. Encrypt wallet for stronger security.");
          QMessageBox::information(this, "Wallet Unencrypted",
            "ℹ️ Wallet is not encrypted.\n\n"
            "No unlock is required.\n"
            "Use Encrypt Wallet to protect private keys.");
        } else {
          QMessageBox::critical(this, "Unlock Failed", buildUnlockErrorMessage(message));

          // Reset to locked state UI
          walletUnlocked_ = false;
          btnWalletLock_->setText("Locked | Unlock");
          btnWalletLock_->setStyleSheet(chromeButtonStyle());
          btnWalletLock_->setToolTip("Wallet locked. Click to unlock for Taproot signing.");
        }

        updateWalletUIState();
      });

    rpc_->walletUnlock(password, 900); // 15 minutes timeout
  }
}

void MainWindow::onWalletLockToggle() {
  // Single toggle button: calls unlock if locked, lock if unlocked
  if (walletUnlocked_) {
    onLockWallet();
  } else {
    onUnlockWallet();
  }
}

void MainWindow::onLockWallet() {
  rpc_->walletLock();

  // Stop countdown timer
  unlockCountdownTimer_->stop();
  unlockSecondsRemaining_ = 0;

  walletUnlocked_ = false;

  // Update button to show locked state
  btnWalletLock_->setText("Locked | Unlock");
  btnWalletLock_->setStyleSheet(chromeButtonStyle());
  btnWalletLock_->setToolTip("Wallet locked. Click to unlock for Taproot signing.");

  // Update all wallet-dependent UI
  updateWalletUIState();

  QTimer::singleShot(0, this, [this]() {
    QMessageBox::information(this, "Locked",
      "🔒 Wallet locked successfully!\n\n"
      "• Taproot signing is now disabled\n"
      "• Private keys are secured\n"
      "• Unlock to spend or sign transactions");
  });
}

void MainWindow::onUnlockCountdownTick() {
  if (!walletUnlocked_ || unlockSecondsRemaining_ <= 0) {
    unlockCountdownTimer_->stop();
    return;
  }

  unlockSecondsRemaining_--;

  // Update button with remaining time
  int mins = unlockSecondsRemaining_ / 60;
  int secs = unlockSecondsRemaining_ % 60;

  if (unlockSecondsRemaining_ > 0) {
    // Color changes as time runs out
    const QString style = chromeButtonStyle();

    btnWalletLock_->setText(QString("Unlocked (%1:%2) | Lock")
      .arg(mins, 2, 10, QChar('0'))
      .arg(secs, 2, 10, QChar('0')));
    btnWalletLock_->setStyleSheet(style);
  } else {
    // Timer expired - defer auto-lock out of the timer callback to avoid Qt reentrancy crash
    unlockCountdownTimer_->stop();
    QTimer::singleShot(0, this, &MainWindow::onLockWallet);
  }
}

void MainWindow::updateWalletSwitcherState() {
  if (!cmbWalletSelector_ || !btnLoadWallet_) {
    return;
  }

  if (singleWalletMode_) {
    cmbWalletSelector_->setVisible(false);
    btnLoadWallet_->setVisible(false);
    return;
  }

  if (walletSwitchInFlight_) {
    cmbWalletSelector_->setEnabled(false);
    btnLoadWallet_->setEnabled(false);
    btnLoadWallet_->setText("Switching...");
    btnLoadWallet_->setToolTip("Switching active wallet...");
    return;
  }

  const bool hasEntries = cmbWalletSelector_->count() > 0;
  const QString selectedWallet = cmbWalletSelector_->currentText().trimmed();
  const bool hasSelection = hasEntries && !selectedWallet.isEmpty();

  cmbWalletSelector_->setEnabled(hasEntries);
  btnLoadWallet_->setEnabled(hasSelection);

  if (!hasEntries) {
    btnLoadWallet_->setText("Open Wallet");
    btnLoadWallet_->setToolTip("No wallets found");
  } else if (!hasSelection) {
    btnLoadWallet_->setText("Open Wallet");
    btnLoadWallet_->setToolTip("Select a wallet to load");
  } else if (!currentWalletName_.isEmpty() && selectedWallet == currentWalletName_) {
    btnLoadWallet_->setText("Reload Wallet");
    btnLoadWallet_->setToolTip(QString("Reload wallet '%1'").arg(selectedWallet));
  } else if (currentWalletName_.isEmpty()) {
    btnLoadWallet_->setText("Open Wallet");
    btnLoadWallet_->setToolTip(QString("Open wallet '%1'").arg(selectedWallet));
  } else {
    btnLoadWallet_->setText("Switch Wallet");
    btnLoadWallet_->setToolTip(QString("Switch from '%1' to '%2'")
      .arg(currentWalletName_, selectedWallet));
  }
}

bool MainWindow::shouldIgnoreWalletScopedResult(const QString& method) const {
  if (!walletSwitchInFlight_) {
    return false;
  }

  static const QSet<QString> walletScopedMethods = {
    QStringLiteral("wallet.getbalance"),
    QStringLiteral("wallet.shieldedbalance"),
    // wallet.listaddresses removed from polling — too expensive with 24K+ addresses
    // Loaded on-demand when user opens receive tab instead
    QStringLiteral("wallet.listunspent"),
    QStringLiteral("wallet.listtransactions"),
    QStringLiteral("wallet.getviewkeyinfo"),
    QStringLiteral("wallet.getsyncstatus"),
    QStringLiteral("wallet.getreorginfo"),
    QStringLiteral("wallet.rescanblockchain"),
    QStringLiteral("wallet.getinfo"),
    QStringLiteral("getwalletinfo"),
    QStringLiteral("mining.getaddress"),
  };

  return walletScopedMethods.contains(method);
}

void MainWindow::bindWalletScopedState(const QString& walletName) {
  if (txTracker_) {
    txTracker_->setWalletScope(walletName);
  }

  if (bannerQueue_) {
    bannerQueue_->setWalletScope(walletName);
    if (!walletName.isEmpty()) {
      bannerQueue_->replayOnStartup();
    }
  }

  if (shieldedWidget_) {
    shieldedWidget_->setWalletScope(walletName);
  }

  if (vaultPanel_) {
    vaultPanel_->setWalletScope(walletName);
  }
}

void MainWindow::clearWalletScopedUiState() {
  if (tblAddresses_) {
    tblAddresses_->setRowCount(0);
  }
  if (tblTransactions_) {
    tblTransactions_->setRowCount(0);
  }
  if (tblUTXOs_) tblUTXOs_->setRowCount(0);
  cachedUtxos_ = {};
  currentUtxoPage_ = 0;
  utxoRequestPending_ = false;
  cachedUtxoCount_ = 0;
  if (btnConsolidate_) {
    btnConsolidate_->setVisible(false);
  }
  if (lblBalance_) {
    lblBalance_->setText("Loading...");
  }
  cachedTransparentBalance_ = 0.0;
  cachedP2mrBalance_ = 0.0;
  cachedShieldedBalance_ = 0.0;
  cachedPendingBalance_ = 0.0;
  cachedMiningBalance_ = 0.0;
  if (edtAddress_) {
    edtAddress_->clear();
  }
  if (edtMiningAddress_) {
    edtMiningAddress_->clear();
  }
  if (edtRecipient_) {
    edtRecipient_->clear();
  }
  if (edtAmount_) {
    edtAmount_->clear();
  }
  if (edtFee_) {
    edtFee_->clear();
  }
  if (txtSendResult_) {
    txtSendResult_->clear();
  }
  if (lblTotalWalletBalance_) {
    lblTotalWalletBalance_->setText("0.00000000 DIN");
  }
  updateWalletBalanceDisplay();
  if (lblSendStatus_) {
    lblSendStatus_->setText("ℹ️ Select or unlock a wallet to send transactions");
    lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
  }
  if (tblContracts_) {
    tblContracts_->setRowCount(0);
  }
  if (lblContractsSummary_) {
    lblContractsSummary_->setText("Loading...");
  }
  if (hardwareWalletWidget_) {
    hardwareWalletWidget_->clearWalletState();
  }
  if (dpiWidget_) {
    dpiWidget_->clearWalletState();
  }
  updateWalletAddressModeUi();
  updateReceiveModeUi();
  updateSendModeUi();
}

void MainWindow::refreshWalletMiningAddress() {
  if (edtMiningAddress_) {
    edtMiningAddress_->clear();
  }

  if (!currentWalletName_.isEmpty()) {
    rpc_->call("mining.getaddress", QJsonArray());
  }
}

void MainWindow::onLoadSelectedWallet() {
  if (!cmbWalletSelector_ || !btnLoadWallet_) {
    return;
  }

  if (walletSwitchInFlight_) {
    return;
  }

  // Block wallet switch while a send is in-flight (change reservation active)
  if (!activeReservationId_.isEmpty()) {
    QMessageBox::warning(this, "Wallet Switch Blocked",
        "A send is in progress. Wait for it to complete before switching wallets.");
    return;
  }

  const QString walletName = cmbWalletSelector_->currentText().trimmed();
  if (walletName.isEmpty()) {
    QMessageBox::warning(this, "Wallet Selection", "Select a wallet to load first.");
    return;
  }

  walletSwitchInFlight_ = true;
  pendingWalletOpenName_ = walletName;
  updateWalletSwitcherState();
  clearWalletScopedUiState();
  if (changeAddrMgr_) {
    changeAddrMgr_->setWalletIdentityKey(QString());
  }

  const bool switchingWallet = !currentWalletName_.isEmpty() && currentWalletName_ != walletName;
  lblConnectionStatus_->setText(switchingWallet ? "Switching wallet" : "Loading wallet");
  lblConnectionStatus_->setStyleSheet(headerPillStyle());
  lblConnectionStatus_->setToolTip(
    switchingWallet
      ? QString("Switching from '%1' to '%2'").arg(currentWalletName_, walletName)
      : QString("Loading wallet: %1").arg(walletName)
  );

  rpc_->call("wallet.open", QJsonArray{walletName});
}

void MainWindow::checkRescanStatus() {
  // Query wallet info to check if rescan is in progress and get wallet name
  rpc_->call("wallet.getinfo", QJsonArray{});

  auto walletInfoResultConn = std::make_shared<QMetaObject::Connection>();
  auto walletInfoErrorConn = std::make_shared<QMetaObject::Connection>();

  *walletInfoResultConn = connect(rpc_, &RpcClient::rpcResult, this,
      [this, walletInfoResultConn, walletInfoErrorConn](const QString& method, const QJsonValue& result) {
    if (method != "wallet.getinfo" || !result.isObject()) {
      return;
    }

    QObject::disconnect(*walletInfoResultConn);
    QObject::disconnect(*walletInfoErrorConn);

      auto obj = result.toObject();
      const bool unlocked = walletUnlockedFromRpc(obj);

      // Update wallet name and detect wallet switch
      QString walletName = obj.value("wallet_name").toString();
      bool walletSwitched = false;

      if (!walletName.isEmpty() && walletName != currentWalletName_) {
        // CRITICAL: Wallet changed - reset security state
        walletSwitched = true;
        currentWalletName_ = walletName;
        lblWalletName_->setText(QString("Wallet: %1").arg(walletName));
        lblWalletName_->setStyleSheet(headerPillStyle());
        lblWalletName_->setToolTip(QString("Active wallet: %1").arg(walletName));
      } else if (walletName.isEmpty() && !currentWalletName_.isEmpty()) {
        // Wallet unloaded
        walletSwitched = true;
        currentWalletName_.clear();
        lblWalletName_->setText("Wallet: none");
        lblWalletName_->setStyleSheet(headerPillStyle());
        lblWalletName_->setToolTip("No wallet loaded. Create or restore a wallet to get started.");
      }

      if (!pendingWalletOpenName_.isEmpty() && walletName == pendingWalletOpenName_) {
        walletSwitchInFlight_ = false;
        pendingWalletOpenName_.clear();
        lblConnectionStatus_->setText("Wallet loaded");
        lblConnectionStatus_->setStyleSheet(headerPillStyle());
        lblConnectionStatus_->setToolTip(QString("Active wallet: %1").arg(walletName));
      }

      updateWalletSwitcherState();

      // SECURITY FIX: Reset unlock state when wallet changes
      // New wallet must be explicitly unlocked - never inherit unlock from previous wallet
      if (walletSwitched) {
        bindWalletScopedState(currentWalletName_);
        walletUnlocked_ = unlocked;
        unlockSecondsRemaining_ = 0;
        unlockCountdownTimer_->stop();
        activeReservationId_.clear();

        // Sync lock button with daemon-reported state for the newly active wallet.
        if (walletUnlocked_) {
          btnWalletLock_->setText("Unlocked | Lock");
          btnWalletLock_->setStyleSheet(chromeButtonStyle());
          btnWalletLock_->setToolTip("Wallet unlocked. Click to lock and secure private keys.");
        } else {
          btnWalletLock_->setText("Locked | Unlock");
          btnWalletLock_->setStyleSheet(chromeButtonStyle());
          btnWalletLock_->setToolTip("Wallet locked. Click to unlock for Taproot signing.");
        }

        // Clear stale data from previous wallet
        clearWalletScopedUiState();
        refreshWalletMiningAddress();
        if (!currentWalletName_.isEmpty()) {
          rpc_->call("wallet.getviewkeyinfo", QJsonArray());
        } else if (changeAddrMgr_) {
          changeAddrMgr_->setWalletIdentityKey(QString());
        }

        // Poke wallet-scoped tabs that don't refresh from the
        // standard wallet.* RPC fanout. Without this they'd keep
        // displaying the previous wallet's data until their own
        // poll timers fire (6s for both Vault and Shielded). The
        // Contracts table is keyed off pendingContractsRefresh_,
        // so re-arm it explicitly here.
        if (vaultPanel_) {
          vaultPanel_->refresh();
        }
        if (shieldedWidget_) {
          shieldedWidget_->refresh();
        }
        if (tblContracts_) {
          refreshContractsList();
        }
      } else if (unlockSecondsRemaining_ <= 0 && walletUnlocked_ != unlocked) {
        // Keep UI aligned with daemon state when no local countdown session is active.
        walletUnlocked_ = unlocked;
        btnWalletLock_->setEnabled(true);  // ensure button isn't stuck in disabled "Unlocking..." state
        if (walletUnlocked_) {
          btnWalletLock_->setText("Unlocked | Lock");
          btnWalletLock_->setStyleSheet(chromeButtonStyle());
          btnWalletLock_->setToolTip("Wallet unlocked. Click to lock and secure private keys.");
        } else {
          btnWalletLock_->setText("Locked | Unlock");
          btnWalletLock_->setStyleSheet(chromeButtonStyle());
          btnWalletLock_->setToolTip("Wallet locked. Click to unlock for Taproot signing.");
        }
      }

      // Check for scanning status
      bool wasRescanning = walletRescanning_;
      walletRescanning_ = obj.value("scanning").toBool(false);

      // Update button state based on rescan
      if (walletRescanning_) {
        btnWalletLock_->setEnabled(false);
        btnWalletLock_->setToolTip("Wallet lock/unlock disabled during blockchain rescan");
      } else if (!wasRescanning || !walletRescanning_) {
        btnWalletLock_->setEnabled(true);
        // Restore appropriate tooltip
        if (walletUnlocked_) {
          btnWalletLock_->setToolTip("Lock wallet to secure private keys");
        } else {
          btnWalletLock_->setToolTip("Unlock wallet to enable Taproot signing and transactions");
        }
      }

      // Update all wallet-dependent UI
      updateWalletUIState();
  });

  *walletInfoErrorConn = connect(rpc_, &RpcClient::rpcError, this,
      [walletInfoResultConn, walletInfoErrorConn](const QString& method, int code, const QString& message) {
    Q_UNUSED(code);
    if (method != "wallet.getinfo") {
      return;
    }

    QObject::disconnect(*walletInfoResultConn);
    QObject::disconnect(*walletInfoErrorConn);
    qWarning() << "wallet.getinfo failed during rescan status check:" << message;
  });
}

void MainWindow::updateWalletUIState() {
  bool hasWallet = !currentWalletName_.isEmpty();
  bool canTransact = hasWallet && walletUnlocked_ && !walletRescanning_;
  const QString sendMode = currentSendMode();
  const bool standardSendMode = isSendModePublic(sendMode);
  updateWalletSwitcherState();

  if (btnRescanWallet_) {
    btnRescanWallet_->setEnabled(hasWallet && !walletRescanning_);
    if (!hasWallet) {
      btnRescanWallet_->setToolTip("Create or load the default wallet first");
    } else if (walletRescanning_) {
      btnRescanWallet_->setToolTip("Rescan already running");
    } else {
      btnRescanWallet_->setToolTip("Rescan blockchain for wallet funds/history");
    }
  }

  // Send tab controls
  if (btnSend_) {
    btnSend_->setEnabled(canTransact);
    if (!hasWallet) {
      btnSend_->setToolTip("Create or load a wallet first");
    } else if (walletRescanning_) {
      btnSend_->setToolTip("Wait for blockchain rescan to complete");
    } else if (!walletUnlocked_) {
      btnSend_->setToolTip("Unlock wallet to send transactions");
    } else {
      btnSend_->setToolTip("Send DIN to another address");
    }
  }
  if (btnHardwareWalletSend_) {
    btnHardwareWalletSend_->setEnabled(canTransact && standardSendMode);
    if (!hasWallet) {
      btnHardwareWalletSend_->setToolTip("Create or load a wallet first");
    } else if (walletRescanning_) {
      btnHardwareWalletSend_->setToolTip("Wait for blockchain rescan to complete");
    } else if (!walletUnlocked_) {
      btnHardwareWalletSend_->setToolTip("Unlock wallet to prepare a hardware-wallet send");
    } else if (!standardSendMode) {
      btnHardwareWalletSend_->setToolTip("Hardware-wallet signing is currently wired only for public Taproot transfers");
    } else {
      btnHardwareWalletSend_->setToolTip(hardwareWalletPsbtTooltip());
    }
  }

  // Send form inputs (visual feedback)
  if (edtRecipient_) {
    edtRecipient_->setEnabled(canTransact && sendMode != "shield");
    if (!canTransact) {
      edtRecipient_->setPlaceholderText(hasWallet ? "Unlock wallet to send..." : "Load wallet first...");
    }
  }
  if (edtAmount_) {
    edtAmount_->setEnabled(canTransact);
  }
  if (btnUseMax_) {
    btnUseMax_->setEnabled(canTransact);
  }

  if (btnNewAddress_) {
    btnNewAddress_->setEnabled(canTransact);
  }
  if (btnValidate_) {
    const QString wam = currentWalletAddressMode();
    btnValidate_->setEnabled(canTransact && (wam == "standard" || wam == "p2mr"));
  }
  if (btnCopy_) {
    btnCopy_->setEnabled(hasWallet && edtAddress_ && !edtAddress_->text().isEmpty());
  }

  // Receive tab - New Address button
  if (btnDeriveAddress_) {
    btnDeriveAddress_->setEnabled(canTransact);
    if (!hasWallet) {
      btnDeriveAddress_->setToolTip("Create or load a wallet first");
    } else if (!walletUnlocked_) {
      btnDeriveAddress_->setToolTip("Unlock wallet to generate new addresses");
    } else if (currentReceiveMode() == "p2mr") {
      btnDeriveAddress_->setToolTip("Generate a new quantum-safe P2MR receiving address (din1r...)");
    } else {
      btnDeriveAddress_->setToolTip("Generate a new Taproot receiving address (din1p...)");
    }
  }
  // Update Send status label with helpful hint.
  if (lblSendStatus_) {
    if (!canTransact) {
      if (!hasWallet) {
        lblSendStatus_->setText("ℹ️ Create or restore a wallet to send transactions");
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
      } else if (walletRescanning_) {
        lblSendStatus_->setText("🔄 Blockchain rescan in progress... Please wait.");
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
      } else if (!walletUnlocked_) {
        lblSendStatus_->setText("🔒 Wallet is locked. Unlock to send transactions.");
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
      }
    } else {
      const QString status = lblSendStatus_->text();
      const bool isStateHint =
          status.isEmpty() ||
          status.startsWith("ℹ️ Create or restore a wallet") ||
          status.startsWith("🔄 Blockchain rescan in progress") ||
          status.startsWith("🔒 Wallet is locked");
      if (isStateHint) {
        lblSendStatus_->setText("✅ Wallet unlocked. Ready to send transactions.");
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
      }
    }
  }

  updateWalletAddressModeUi();
  updateReceiveModeUi();
  updateSendModeUi();

  // =========================================================================
  // Mining tab - "Use Wallet" button requires unlocked wallet for address derivation
  // Note: Manual address entry and mining still work without wallet (pool mining)
  // =========================================================================
  if (btnUseWalletAddr_) {
    btnUseWalletAddr_->setEnabled(canTransact);
    if (!hasWallet) {
      btnUseWalletAddr_->setToolTip("Create or load a wallet first");
      btnUseWalletAddr_->setStyleSheet(headerButtonStyle());
    } else if (walletRescanning_) {
      btnUseWalletAddr_->setToolTip("Wait for blockchain rescan to complete");
      btnUseWalletAddr_->setStyleSheet(headerButtonStyle());
    } else if (!walletUnlocked_) {
      btnUseWalletAddr_->setToolTip("Unlock wallet to use your Taproot address");
      btnUseWalletAddr_->setStyleSheet(headerButtonStyle());
    } else {
      btnUseWalletAddr_->setToolTip("Fill mining address from your wallet");
      btnUseWalletAddr_->setStyleSheet(headerButtonStyle()); // Uniform mining control style
    }
  }

  // Mining status hint when wallet affects mining
  if (lblMiningStatus_ && !canTransact && edtMiningAddress_ && edtMiningAddress_->text().isEmpty()) {
    if (!hasWallet) {
      lblMiningStatus_->setText(miningStatusInactiveText() + " | No wallet - enter address manually or create wallet");
    } else if (!walletUnlocked_) {
      lblMiningStatus_->setText(miningStatusInactiveText() + " | Unlock wallet to use 'Use Wallet' button");
    }
  }
}

void MainWindow::onEncryptWallet() {
  // Confirm action
  QMessageBox::StandardButton reply = QMessageBox::warning(this, 
    "Encrypt Wallet",
    "⚠️  You are about to encrypt your wallet.\n\n"
    "• You will need a password to unlock and spend coins\n"
    "• Your wallet will be locked after encryption\n"
    "• SAVE YOUR 12-WORD SEED PHRASE - it's the only backup!\n\n"
    "Make sure you have written down your seed phrase.\n\n"
    "Do you want to continue?",
    QMessageBox::Yes | QMessageBox::No,
    QMessageBox::No);
  
  if (reply != QMessageBox::Yes) {
    return;
  }
  
  // Get password
  bool ok;
  QString password = QInputDialog::getText(this, "Encrypt Wallet",
    "Enter a strong password to encrypt your wallet:\n"
    "(You will need this to unlock and spend coins)",
    QLineEdit::Password, "", &ok);
  
  if (!ok || password.isEmpty()) {
    return;
  }
  
  // Confirm password
  QString password2 = QInputDialog::getText(this, "Confirm Password",
    "Re-enter password to confirm:",
    QLineEdit::Password, "", &ok);
  
  if (!ok || password != password2) {
    QMessageBox::critical(this, "Error", "Passwords do not match!");
    return;
  }
  
  // Check password strength
  if (password.length() < 8) {
    QMessageBox::warning(this, "Weak Password",
      "Password should be at least 8 characters for security.\n"
      "Consider using a longer, stronger password.");
  }
  
  // Call encryptwallet RPC
  QJsonArray params;
  params.append(password);
  rpc_->call("wallet.encrypt", params);
  
  // Wait for specific wallet.encrypt response.
  auto encryptResultConn = std::make_shared<QMetaObject::Connection>();
  auto encryptErrorConn = std::make_shared<QMetaObject::Connection>();

  *encryptResultConn = connect(rpc_, &RpcClient::rpcResult, this,
      [this, encryptResultConn, encryptErrorConn](const QString& method, const QJsonValue& result) {
    if (method != "wallet.encrypt") {
      return;
    }

    QObject::disconnect(*encryptResultConn);
    QObject::disconnect(*encryptErrorConn);

    if (result.isObject() && result.toObject().contains("error")) {
      QMessageBox::critical(this, "Error",
        "Failed to encrypt wallet:\n" + result.toObject()["error"].toString());
    } else {
      // Hide encrypt button after successful encryption
      btnEncryptWallet_->setVisible(false);

      // Stop countdown timer if running
      unlockCountdownTimer_->stop();
      unlockSecondsRemaining_ = 0;

      // Update wallet status
      walletUnlocked_ = false;

      // Update button to show locked state
      btnWalletLock_->setText("Encrypted | Unlock");
      btnWalletLock_->setStyleSheet(chromeButtonStyle());
      btnWalletLock_->setToolTip("Wallet encrypted and locked. Click to unlock for Taproot signing.");

      QMessageBox::information(this, "Success",
        "✅ Wallet encrypted successfully!\n\n"
        "• Your wallet is now locked and protected\n"
        "• Taproot signing requires password unlock\n"
        "• You will need your password to spend funds\n\n"
        "🚨 IMPORTANT: Make sure you have saved your 12-word seed phrase!");
    }
  });

  *encryptErrorConn = connect(rpc_, &RpcClient::rpcError, this,
      [this, encryptResultConn, encryptErrorConn](const QString& method, int code, const QString& message) {
    Q_UNUSED(code);
    if (method != "wallet.encrypt") {
      return;
    }

    QObject::disconnect(*encryptResultConn);
    QObject::disconnect(*encryptErrorConn);

    QMessageBox::critical(this, "Error", "Failed to encrypt wallet:\n" + message);
  });
}

void MainWindow::onDeriveNewAddress() {
  if (!walletUnlocked_) {
    QMessageBox::warning(this, "Wallet Locked",
      "Please unlock your wallet first to derive new addresses.");
    onUnlockWallet();
    return;
  }

  const QString receiveMode = currentReceiveMode();
  const bool p2mrMode = (receiveMode == "p2mr");

  rpc_->call("wallet.getnewaddress", QJsonArray{p2mrMode ? "p2mr" : "taproot"});

  // Wait for specific getnewaddress response and ignore unrelated RPC traffic.
  auto deriveResultConn = std::make_shared<QMetaObject::Connection>();
  auto deriveErrorConn = std::make_shared<QMetaObject::Connection>();

  *deriveResultConn = connect(rpc_, &RpcClient::rpcResult, this,
      [this, p2mrMode, deriveResultConn, deriveErrorConn](const QString& method, const QJsonValue& result) {
    if (method != "wallet.getnewaddress") {
      return;
    }

    QObject::disconnect(*deriveResultConn);
    QObject::disconnect(*deriveErrorConn);

    if (result.isObject()) {
      auto obj = result.toObject();

      if (obj.contains("error")) {
        QString error = obj.value("error").toString();
        QString errorMsg;
        if (error.contains("locked") || error.contains("Wallet is locked")) {
          errorMsg = "🔒 Wallet is locked!\n\nPlease unlock your wallet first to generate new addresses.";
          onUnlockWallet();
        } else if (error.contains("not encrypted")) {
          errorMsg = "ℹ️  Wallet is not encrypted.\n\nYour wallet will work but is not password-protected.\nConsider encrypting it for security!";
        } else if (error.contains("No HD wallet") || error.contains("No wallet loaded") || error.contains("No active wallet")) {
          errorMsg = "⚠️  No wallet found!\n\nPlease create a new wallet or restore from seed phrase first.";
        } else {
          errorMsg = "❌ Failed to generate address:\n\n" + error;
        }
        QMessageBox::critical(this, "Address Generation Failed", errorMsg);
        return;
      }

      const QString address = obj.value("address").toString();
      if (address.isEmpty()) {
        QMessageBox::critical(this, "Address Generation Failed",
          "❌ Address generation returned an empty address.\n\n"
          "Please verify wallet state and try again.");
        return;
      }

      // Refresh address list from daemon as single source of truth (index/path/balance).
      rpc_->callNamed("wallet.listaddresses", QJsonObject{{"count", 200}});

      // Also update mining address if it's empty
      if (edtMiningAddress_ && edtMiningAddress_->text().isEmpty()) {
        edtMiningAddress_->setText(address);
      }

      if (p2mrMode) {
        QMessageBox::information(this, "New Quantum-Safe Address",
          QString("New P2MR (ML-DSA-65) receive address generated:\n\n%1\n\n"
                  "This address is quantum-resistant. Funds sent here are "
                  "protected against future quantum computing threats.")
            .arg(address));
      } else {
        QMessageBox::information(this, "New Taproot Address",
          QString("New transparent Taproot address generated:\n\n%1").arg(address));
      }
    }
  });

  *deriveErrorConn = connect(rpc_, &RpcClient::rpcError, this,
      [this, deriveResultConn, deriveErrorConn](const QString& method, int code, const QString& message) {
    Q_UNUSED(code);
    if (method != "wallet.getnewaddress") {
      return;
    }
    QObject::disconnect(*deriveResultConn);
    QObject::disconnect(*deriveErrorConn);
    QMessageBox::critical(this, "Address Generation Failed",
      "❌ Failed to generate address:\n\n" + message);
  });
}

// === Send Tab Handlers ===

bool MainWindow::collectSendForm(QString& recipient,
                                 QString& amountText,
                                 double& amount,
                                 double& feeRate,
                                 bool requireUnlocked) {
  const QString mode = currentSendMode();
  recipient = edtRecipient_ ? edtRecipient_->text().trimmed() : QString();
  amountText = edtAmount_ ? edtAmount_->text().trimmed() : QString();

  if (mode != "shield" && mode != "unshield" && mode != "shield_covenant" &&
      !isSendModeContract(mode) && recipient.isEmpty()) {
    lblSendStatus_->setText("❌ Error: Recipient address is required");
    lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
    return false;
  }

  if (amountText.isEmpty() || amountText.toDouble() <= 0.0) {
    lblSendStatus_->setText("❌ Error: Amount must be greater than 0");
    lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
    return false;
  }

  amount = amountText.toDouble();

  if (isSendModePublic(mode) || mode == "unshield") {
    if (!recipient.isEmpty() && !isTransparentDineroAddress(recipient)) {
      lblSendStatus_->setText(QString::fromUtf8("\xE2\x9D\x8C Error: Invalid public Dinero address.\n"
        "Supported formats:\n"
        "\xE2\x80\xA2 Taproot (P2TR): din1p..., tdin1p..., rdin1p...\n"
        "\xE2\x80\xA2 Quantum-safe P2MR: din1r..., tdin1r..., rdin1r...\n"
        "\xE2\x80\xA2 SegWit (P2WPKH): din1q..., tdin1q..., rdin1q..."));
      lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
      return false;
    }
  } else if (isSendModeConfidential(mode)) {
    if (!recipient.isEmpty() && !isConfidentialDineroAddress(recipient)) {
      lblSendStatus_->setText(QString::fromUtf8("\xE2\x9D\x8C Error: Invalid private Dinero address.\n"
        "Supported formats:\n"
        "\xE2\x80\xA2 dina1..., tdina1..., rdina1...\n"
        "\xE2\x80\xA2 dinc1..., tdinc1..., rdinc1... (legacy)"));
      lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
      return false;
    }
  } else if (isSendModePrivate(mode)) {
    if (!recipient.isEmpty() && !isShieldedDineroAddress(recipient)) {
      lblSendStatus_->setText(QString::fromUtf8("\xE2\x9D\x8C Error: Invalid shielded Dinero address.\n"
        "Supported formats:\n"
        "\xE2\x80\xA2 dins1..., tdins1..., rdins1..."));
      lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
      return false;
    }
  } else if (mode == "shield_to") {
    if (recipient.isEmpty() || !isShieldedDineroAddress(recipient)) {
      lblSendStatus_->setText(QString::fromUtf8("\xE2\x9D\x8C Error: Send-to-shielded requires a valid shielded destination.\n"
        "Supported formats:\n"
        "\xE2\x80\xA2 dins1..., tdins1..., rdins1..."));
      lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
      return false;
    }
  } else if (mode == "shield" && !recipient.isEmpty()) {
    if (!isConfidentialDineroAddress(recipient)) {
      lblSendStatus_->setText("❌ Error: Shield destination must be a shielded dins1 address.\n"
        "Leave recipient blank to shield to your own private lane.");
      lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
      return false;
    }
  }

  if (requireUnlocked && !walletUnlocked_) {
    lblSendStatus_->setText("❌ Error: Wallet is locked. Please unlock it first.");
    lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
    QMessageBox::warning(this, "Wallet Locked",
      "Please unlock your wallet before sending transactions.");
    onUnlockWallet();
    return false;
  }

  // rc8: actually consume the fee priority preset (was cosmetic before).
  // Custom (-1)            -> read edtFee_ (manual una/vB value)
  // Low / Normal / High    -> use currentEstimatedFeeRate_ (cached from
  //                           wallet.estimatefee response that fired when
  //                           the preset changed). If the cache is empty
  //                           (e.g. estimate hasn't returned yet), fall
  //                           through to feeRate=0.0 which makes the RPC
  //                           use dinerod's internal default.
  feeRate = 0.0;
  if (cmbFeePreset_) {
    const int preset = cmbFeePreset_->currentData().toInt();
    if (preset == -1) {
      if (edtFee_ && !edtFee_->text().isEmpty()) {
        feeRate = edtFee_->text().toDouble();
      }
    } else if (currentEstimatedFeeRate_ > 0.0) {
      feeRate = currentEstimatedFeeRate_;
    }
  }

  return true;
}

void MainWindow::clearPendingHardwareWalletSend() {
  pendingHardwareWalletSend_ = PendingHardwareWalletSend{};
}

void MainWindow::startHardwareWalletSendFlow(const QString& recipient,
                                             const QString& amountText,
                                             double amount,
                                             double feeRate) {
  lblSendStatus_->setText("🔄 Creating PSBT for hardware wallet...");
  lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
  txtSendResult_->clear();

  pendingHardwareWalletSend_.active = true;
  pendingHardwareWalletSend_.recipient = recipient;
  pendingHardwareWalletSend_.amountText = amountText;
  pendingHardwareWalletSend_.amountUna = static_cast<qint64>(std::llround(amount * 1e8));
  pendingHardwareWalletSend_.selectedInputOutpoints.clear();
  pendingHardwareWalletSend_.selectedInputAmountsUna.clear();
  pendingHardwareWalletSend_.changeAddress.clear();
  pendingHardwareWalletSend_.changeAmountUna = 0;
  pendingHardwareWalletSend_.feePaidUna = 0;

  QJsonObject outputs;
  outputs[recipient] = amount;

  QJsonObject options;
  options["fee_rate"] = feeRate > 0.0 ? feeRate : 1.0;

  auto resultConn = std::make_shared<QMetaObject::Connection>();
  auto errorConn = std::make_shared<QMetaObject::Connection>();

  *resultConn = connect(rpc_, &RpcClient::rpcResult, this,
    [this, resultConn, errorConn](const QString& method, const QJsonValue& result) {
      if (method != "wallet.createfundedpsbt") return;
      QObject::disconnect(*resultConn);
      QObject::disconnect(*errorConn);

      const QJsonObject obj = result.toObject();
      if (obj.contains("error")) {
        const QString errorMessage =
          obj["error"].isString()
            ? obj["error"].toString()
            : obj["error"].toObject().value("message").toString("Failed to create funded PSBT");
        clearPendingHardwareWalletSend();
        lblSendStatus_->setText(QString("❌ Error: %1").arg(errorMessage));
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
        return;
      }

      const QString psbt = obj["psbt"].toString();

      if (psbt.isEmpty() || !hardwareWalletWidget_ || !mainTabs_) {
        clearPendingHardwareWalletSend();
        lblSendStatus_->setText("❌ Error: Failed to prepare hardware-wallet signing flow");
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
        return;
      }

      pendingHardwareWalletSend_.changeAddress = obj["change_address"].toString();
      pendingHardwareWalletSend_.changeAmountUna = obj["change_amount_una"].toVariant().toLongLong();
      pendingHardwareWalletSend_.feePaidUna = obj["fee_paid_una"].toVariant().toLongLong();

      const QJsonArray selectedInputs = obj["selected_inputs"].toArray();
      for (const auto& entry : selectedInputs) {
        const QJsonObject inputObj = entry.toObject();
        const QString outpoint = QString("%1:%2")
          .arg(inputObj["txid"].toString())
          .arg(inputObj["vout"].toInt());
        pendingHardwareWalletSend_.selectedInputOutpoints.append(outpoint);
        pendingHardwareWalletSend_.selectedInputAmountsUna[outpoint] =
          inputObj["amount_una"].toVariant().toLongLong();
        const QString path = inputObj["path"].toString();
        if (path.startsWith("m/88'") || path.startsWith("88'")) {
          clearPendingHardwareWalletSend();
          lblSendStatus_->setText("❌ Hardware-wallet PSBT cannot spend P2MR inputs yet.");
          lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
          if (txtSendResult_) {
            txtSendResult_->setHtml(
              "<b>Hardware Wallet PSBT Not Available</b><br><br>"
              "The coin selector chose a P2MR quantum-safe input. Current hardware-wallet PSBT signing is "
              "Taproot/BIP86 only; P2MR spends require the Dinero software wallet because they use ML-DSA-65, "
              "not a BIP86 hardware-wallet signature path.");
          }
          return;
        }
      }

      const bool directUsbSigner = hardwareWalletWidget_->hasConnectedDirectSigner();
      const QString fileStatus = directUsbSigner
        ? QStringLiteral("✅ Send flow loaded. Review and sign via connected USB")
        : QStringLiteral("✅ Send flow loaded. Export, scan, or sign with your hardware wallet");
      hardwareWalletWidget_->loadPsbtForSigning(psbt, fileStatus);
      mainTabs_->setCurrentWidget(hardwareWalletWidget_);

      lblSendStatus_->setText("✅ Hardware-wallet send prepared. Continue in the Hardware Wallet tab.");
      lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");

      if (txtSendResult_) {
        QStringList metadataLines;
        if (!pendingHardwareWalletSend_.selectedInputOutpoints.isEmpty()) {
          metadataLines << QString("Selected inputs: %1").arg(pendingHardwareWalletSend_.selectedInputOutpoints.size());
        }
        if (pendingHardwareWalletSend_.feePaidUna > 0) {
          metadataLines << QString("Estimated fee: %1 DIN")
            .arg(QString::number(pendingHardwareWalletSend_.feePaidUna / 1e8, 'f', 8));
        }
        if (pendingHardwareWalletSend_.changeAmountUna > 0 && !pendingHardwareWalletSend_.changeAddress.isEmpty()) {
          metadataLines << QString("Change: %1 DIN → %2")
            .arg(QString::number(pendingHardwareWalletSend_.changeAmountUna / 1e8, 'f', 8),
                 pendingHardwareWalletSend_.changeAddress);
        }

        const QString nextStep = directUsbSigner
          ? "A compatible USB signer is already connected. Review the PSBT in the Hardware Wallet tab and click Sign via Connected USB."
          : "The Hardware Wallet tab is preloaded with this PSBT. Continue there to export, scan, sign, finalize, and broadcast.";
        txtSendResult_->setHtml(QString(
          "<b>🔐 Hardware Wallet Send Ready</b><br><br>"
          "<b>Recipient:</b> %1<br>"
          "<b>Amount:</b> %2 DIN<br>%3<br><br>"
          "%4"
        ).arg(pendingHardwareWalletSend_.recipient,
              pendingHardwareWalletSend_.amountText,
              metadataLines.isEmpty() ? QString() : QString("<b>Tracking:</b> %1").arg(metadataLines.join(" • ")),
              nextStep));
      }
    });

  *errorConn = connect(rpc_, &RpcClient::rpcError, this,
    [this, resultConn, errorConn](const QString& method, int code, const QString& message) {
      Q_UNUSED(code);
      if (method != "wallet.createfundedpsbt") return;
      QObject::disconnect(*resultConn);
      QObject::disconnect(*errorConn);
      clearPendingHardwareWalletSend();
      lblSendStatus_->setText(QString("❌ Error: %1").arg(message));
      lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
    });

  rpc_->call("wallet.createfundedpsbt", QJsonArray{outputs, options});
}

void MainWindow::handleHardwareWalletBroadcast(const QString& txid, bool linkedSendFlow) {
  if (!linkedSendFlow || !pendingHardwareWalletSend_.active || txid.isEmpty()) {
    return;
  }

  if (txTracker_) {
    TrackedTransaction tracked;
    tracked.txid = txid;
    tracked.isIncoming = false;
    tracked.address = pendingHardwareWalletSend_.recipient;
    tracked.amountUna = pendingHardwareWalletSend_.amountUna;
    tracked.status = TxStatus::Pending;
    tracked.createdAt = QDateTime::currentDateTimeUtc();
    tracked.selectedInputOutpoints = pendingHardwareWalletSend_.selectedInputOutpoints;
    tracked.selectedInputAmountsUna = pendingHardwareWalletSend_.selectedInputAmountsUna;
    tracked.changeAddress = pendingHardwareWalletSend_.changeAddress;
    tracked.changeAmountUna = pendingHardwareWalletSend_.changeAmountUna;
    tracked.feePaidUna = pendingHardwareWalletSend_.feePaidUna;
    txTracker_->trackSend(tracked);
  }

  lblSendStatus_->setText("✅ Hardware-wallet transaction sent successfully!");
  lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; font-weight: 600; }");

  if (txtSendResult_) {
    txtSendResult_->setHtml(QString(
      "<b>✅ Transaction Broadcast Successfully</b><br><br>"
      "<b>Transaction ID:</b><br>"
      "<span style='font-family: monospace; font-size: 11px;'>%1</span><br><br>"
      "<b>Status:</b> Pending confirmation<br>"
      "<b>Recipient:</b> %2<br>"
      "<b>Amount:</b> %3 DIN<br><br>"
      "<i>The transaction was signed via your hardware wallet and broadcast to the network.</i>"
    ).arg(txid, pendingHardwareWalletSend_.recipient, pendingHardwareWalletSend_.amountText));
  }

  if (edtRecipient_) edtRecipient_->clear();
  if (edtAmount_) edtAmount_->clear();
  clearPendingHardwareWalletSend();
  refresh();
}

void MainWindow::onSendTransaction() {
  QString recipient;
  QString amountText;
  double amount = 0.0;
  double feeRate = 0.0;
  if (!collectSendForm(recipient, amountText, amount, feeRate, true)) {
    return;
  }
  
  // Show processing status
  lblSendStatus_->setText("🔄 Processing transaction...");
  lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
  txtSendResult_->clear();

  btnSend_->setEnabled(false);
  btnSend_->setText("Processing...");
  if (lblSendStatus_) {
    lblSendStatus_->setText("Sending transaction...");
    lblSendStatus_->setStyleSheet("QLabel { color: #9fb3c8; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
  }

  const QString mode = currentSendMode();

  // Phase 6: Reserve change address before send
  QString changeAddress;
  if (mode == "public_transfer" && changeAddrMgr_ && !changeAddrMgr_->walletIdentityKey().isEmpty()) {
    auto reservation = changeAddrMgr_->reserve();
    activeReservationId_ = reservation.reservationId;
    // Derive the actual address via RPC synchronously is not possible;
    // pass the change index to the backend which will derive it.
    // The backend response will contain the actual change_address used.
  }

  // v7: ring/CT modes removed.

  // Defence in depth for the shielded lockout (shieldedwidget.h). The mode
  // list above withholds these entries, so this is unreachable through the UI
  // — it catches a stale cmbSendMode_ value, a restored preference, or a future
  // code path that sets the mode directly. Refusing here means no shielded RPC
  // can be issued from the Send tab while the lockout stands.
  if (kShieldedUiLockedOut &&
      (mode == "private_transfer" || mode == "shield_to" ||
       mode == "unshield" || mode == "shield")) {
    lblSendStatus_->setText(QString::fromUtf8(
        "\xF0\x9F\x94\x92 Shielded transfers are temporarily unavailable.\n"
        "This feature is being finished and is held closed until its "
        "activation height is set."));
    lblSendStatus_->setStyleSheet(
        "QLabel { color: #d5d9e0; padding: 10px; background: #3a3f4a; "
        "border: 1px solid #4a5060; border-radius: 6px; }");
    return;
  }

  if (mode == "private_transfer") {
    const qint64 amountUna = static_cast<qint64>(std::llround(amount * 100000000.0));
    // rc8: derive fee_una from the priority-preset rate (feeRate is una/vB);
    // private/confidential txs are ~1000 vB. Floor at 1000 una so legacy
    // "minimum-fee" paths keep working when feeRate is 0 (no estimate yet).
    qint64 feeUna = privateModeFeeUna(feeRate);
    QJsonObject params{
      {"fee_una", feeUna},
      {"amount_una", amountUna},
      {"address", recipient},
    };
    rpc_->callNamed("wallet.transfer", params);
  } else if (mode == "shield_to") {
    // Transparent balance -> external shielded dins1 address via wallet.shield.
    // rc8: same fee derivation as private_transfer; shield_to is ~1000 vB.
    qint64 feeUna = privateModeFeeUna(feeRate);
    QJsonObject params{
      {"amount", amount},
      {"fee_una", feeUna},
      {"address", recipient},
    };
    rpc_->callNamed("wallet.shield", params);
  } else if (mode == "unshield") {
    // rc8: same derivation as private_transfer. Unshield tx size ~1000 vB.
    qint64 feeUna = privateModeFeeUna(feeRate);
    rpc_->call("wallet.unshield", QJsonArray{amount, static_cast<double>(feeUna)});
  } else if (mode == "public_contract") {
    // Public contract -- transparent send with covenant metadata (auditable vault)
    QString templateKey = cmbContractTemplate_ ? cmbContractTemplate_->currentData().toString() : "vault";
    QString templateLabel;
    QString covenantScriptHex;
    QString covenantDescription;

    if (templateKey == "vault") {
      templateLabel = "Simple Lock";
      covenantScriptHex = "auto";
      covenantDescription = "Simple lock \xe2\x80\x94 funds locked to specific spending template";
    } else if (templateKey == "conditional") {
      templateLabel = "Lock with Recovery Key";
      QString recoveryKey = edtRecoveryPubkey_ ? edtRecoveryPubkey_->text().trimmed() : "";
      if (recoveryKey.length() != 64) {
        lblSendStatus_->setText(QString::fromUtf8(
          "\xE2\x9D\x8C Error: Recovery pubkey must be 64 hex characters."));
        btnSend_->setEnabled(true); updateSendModeUi(); return;
      }
      covenantScriptHex = "6320" + QString(64, '0') + "b36720" + recoveryKey.toLower() + "ac68";
      covenantDescription = "Conditional: CTV OR recovery key " + recoveryKey.left(8) + "...";
    } else if (templateKey == "timelock") {
      templateLabel = "Timelock";
      covenantScriptHex = "auto";
      covenantDescription = "Timelock \xe2\x80\x94 using vault script (timelock wiring pending)";
    } else if (templateKey == "payroll") {
      templateLabel = "Payroll";
      int recipientCount = 0;
      double totalAmount = 0;
      if (tblPayrollRecipients_) {
        for (int r = 0; r < tblPayrollRecipients_->rowCount(); ++r) {
          auto *addrItem = tblPayrollRecipients_->item(r, 0);
          auto *amtItem = tblPayrollRecipients_->item(r, 1);
          if (addrItem && amtItem && !addrItem->text().trimmed().isEmpty() && amtItem->text().toDouble() > 0) {
            recipientCount++;
            totalAmount += amtItem->text().toDouble();
          }
        }
      }
      if (recipientCount == 0) {
        lblSendStatus_->setText(QString::fromUtf8("\xE2\x9D\x8C Error: Add at least one payroll recipient with address and amount."));
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
        btnSend_->setEnabled(true);
        updateSendModeUi();
        return;
      }
      covenantScriptHex = "auto";
      covenantDescription = "Payroll batch payment: " + QString::number(recipientCount) +
        " recipients, total " + QString::number(totalAmount, 'f', 8) + " DIN";
    } else if (templateKey == "custom") {
      templateLabel = "Custom Script (Advanced)";
      covenantScriptHex = edtCustomScript_ ? edtCustomScript_->text().trimmed() : QString();
      if (covenantScriptHex.isEmpty()) {
        lblSendStatus_->setText(QString::fromUtf8("\xE2\x9D\x8C Error: Custom script hex is required."));
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
        btnSend_->setEnabled(true);
        updateSendModeUi();
        return;
      }
      QRegularExpression hexRe("^[0-9a-fA-F]+$");
      if (!hexRe.match(covenantScriptHex).hasMatch()) {
        lblSendStatus_->setText(QString::fromUtf8("\xE2\x9D\x8C Error: Custom script must be valid hex."));
        lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
        btnSend_->setEnabled(true);
        updateSendModeUi();
        return;
      }
      covenantDescription = "Custom Tapscript";
    } else {
      templateLabel = templateKey;
      covenantScriptHex = "20" + QString(64, '0') + "b3";
      covenantDescription = "Unknown template \xe2\x80\x94 using vault fallback";
    }

    // === Review dialog ===
    // rc8: compute a real fee estimate for the dialog based on the priority
    // preset's fee rate, instead of the hardcoded "~0.0001 DIN" string.
    // Public covenant tx size approx ~250 vB.
    QString covenantFeeText;
    if (feeRate > 0.0) {
      const double feeDin = estimatedFeeDin(feeRate, kPublicSendEstimateVbytes);
      covenantFeeText = QString("~%1 DIN @ %2 una/vB")
                            .arg(QString::number(feeDin, 'f', 8))
                            .arg(QString::number(feeRate, 'f', 2));
    } else {
      covenantFeeText = QStringLiteral("daemon default (no estimate yet)");
    }

    QMessageBox reviewBox(this);
    reviewBox.setWindowTitle("Review Public Contract");
    reviewBox.setIcon(QMessageBox::Information);
    reviewBox.setText(QString("<b>Review Public Contract</b>"));
    reviewBox.setInformativeText(
      QString(
        "<table style='border-spacing: 6px;'>"
        "<tr><td><b>Type:</b></td><td>Public Contract (auditable)</td></tr>"
        "<tr><td><b>Template:</b></td><td>%1</td></tr>"
        "<tr><td><b>Amount:</b></td><td>%2 DIN (visible on-chain)</td></tr>"
        "<tr><td><b>Destination:</b></td><td>%3</td></tr>"
        "<tr><td><b>Covenant:</b></td><td>%4</td></tr>"
        "<tr><td><b>Tx version:</b></td><td>v2 (standard SegWit)</td></tr>"
        "<tr><td><b>Estimated fee:</b></td><td>%5</td></tr>"
        "</table><br>"
        "<b>\xe2\x9a\xa0\xef\xb8\x8f This transaction is irreversible once broadcast.</b><br>"
        "The amount and destination are visible on-chain. The covenant "
        "rules are recorded for auditing and wallet tracking."
      ).arg(templateLabel,
            QString::number(amount, 'f', 8),
            recipient.isEmpty() ? "(self)" : recipient,
            covenantDescription,
            covenantFeeText));

    QPushButton *broadcastBtn = reviewBox.addButton("Broadcast", QMessageBox::AcceptRole);
    reviewBox.addButton("Cancel", QMessageBox::RejectRole);
    reviewBox.setDefaultButton(broadcastBtn);
    reviewBox.exec();

    if (reviewBox.clickedButton() != broadcastBtn) {
      btnSend_->setEnabled(true);
      updateSendModeUi();
      lblSendStatus_->setText(QString::fromUtf8("\xE2\x9D\x8C Contract broadcast cancelled."));
      lblSendStatus_->setStyleSheet("QLabel { color: #d6dde6; padding: 10px; background: #2c3036; border: 1px solid #3d434d; border-radius: 6px; }");
      return;
    }

    // Call sendpubliccovenant RPC
    QJsonObject params;
    params["amount"] = amount;
    params["destination"] = recipient;
    params["covenant_script"] = covenantScriptHex;
    // rc8: pass through the priority-preset fee rate when present. The RPC
    // ignores fee_rate if it's omitted, so 0.0 falls back to daemon default.
    if (feeRate > 0.0) {
      params["fee_rate"] = feeRate;
    }
    rpc_->callNamed("sendpubliccovenant", params);
  } else {
    // public_transfer (default) — Use named-param sendtoaddress with optional change address
    rpc_->sendToAddressNamed(recipient, amount, feeRate, changeAddress);
  }

  // Note: Response is handled in onRpcResult via the main result handler
}

void MainWindow::onCreatePSBT() {
  if (currentSendMode() != "public_transfer") {
    QMessageBox::information(this, "Hardware Wallet",
      "Hardware-wallet signing is currently wired only for public Taproot transfers.");
    return;
  }
  QString recipient;
  QString amountText;
  double amount = 0.0;
  double feeRate = 0.0;
  if (!collectSendForm(recipient, amountText, amount, feeRate, true)) {
    return;
  }

  if (recipient.startsWith("din1r") || recipient.startsWith("tdin1r") || recipient.startsWith("rdin1r")) {
    QMessageBox::information(this, "Hardware Wallet PSBT",
      "P2MR is a Dinero quantum-safe address type. Current hardware-wallet PSBT signing is "
      "Taproot/BIP86 only, so use the normal Send button for P2MR transfers.");
    return;
  }

  startHardwareWalletSendFlow(recipient, amountText, amount, feeRate);
}

void MainWindow::onConsolidateUTXOs() {
  // Check wallet unlock state
  if (!walletUnlocked_) {
    QMessageBox::StandardButton reply = QMessageBox::warning(this,
      "Wallet Locked",
      "Your wallet must be unlocked to consolidate UTXOs.\n\n"
      "Would you like to unlock your wallet now?",
      QMessageBox::Yes | QMessageBox::No,
      QMessageBox::Yes);

    if (reply == QMessageBox::Yes) {
      onUnlockWallet();
    }
    return;
  }

  // How many inputs a single consolidation transaction sweeps (matches the
  // RPC's max_inputs below). One run = ONE transaction over ONE address family
  // (Taproot OR Quantum-Safe) producing ONE output — NOT a full wallet sweep.
  int utxoCount = cachedUtxoCount_;
  int maxInputs = 200;

  // Confirmation dialog — describe what one run actually does. The real number
  // of coins and the exact fee are shown on the preview/confirm step that
  // follows (the daemon dry-runs first), so we do NOT invent estimates here.
  QMessageBox msgBox(this);
  msgBox.setWindowTitle("Consolidate UTXOs");
  msgBox.setIcon(QMessageBox::Question);
  msgBox.setText(QString("<b>Consolidate small coins</b>"));
  msgBox.setInformativeText(
    QString("Your wallet holds %1 coins. This combines up to %2 of your "
            "smallest coins from a single address type (Taproot or "
            "Quantum-Safe) into one new coin, reducing fees and improving "
            "performance.\n\n"
            "The next step previews the exact number of coins and the fee "
            "before anything is sent. To fully flatten a large wallet you may "
            "need to run this more than once, and once per address type.\n\n"
            "Continue?")
      .arg(utxoCount)
      .arg(maxInputs));

  QPushButton *consolidateBtn = msgBox.addButton("Preview\xE2\x80\xA6", QMessageBox::AcceptRole);
  msgBox.addButton("Cancel", QMessageBox::RejectRole);
  msgBox.setDefaultButton(consolidateBtn);

  msgBox.exec();

  if (msgBox.clickedButton() != consolidateBtn) {
    return;
  }

  // Disable button to prevent double-click
  if (btnConsolidate_) {
    btnConsolidate_->setEnabled(false);
    btnConsolidate_->setText("Consolidating...");
  }

  // Preview first: dry-run plan drives the confirmation dialog, then broadcast.
  QJsonObject params;
  params["address_type"] = "auto";
  params["max_inputs"] = maxInputs;
  params["dry_run"] = true;
  params["broadcast"] = false;
  pendingConsolidateParams_ = params;
  rpc_->callNamed("wallet.consolidate", params);
}

void MainWindow::onListUTXOs() {
  rpc_->listUnspent();
}

void MainWindow::onUseMaxAmount() {
  // Get current balance and set it as amount (minus estimated fee)
  QString balanceStr = lblBalance_->text();

  // Extract numeric value from "X.XXXXXXXX DIN" format
  QRegularExpression re("([0-9]+\\.[0-9]+)");
  QRegularExpressionMatch match = re.match(balanceStr);

  if (match.hasMatch()) {
    double balance = match.captured(1).toDouble();
    double estimatedFee = 0.00001; // 0.00001 DIN conservative default fee

    if (!edtFee_->text().isEmpty()) {
      estimatedFee = estimatedFeeDin(edtFee_->text().toDouble(), kPublicSendEstimateVbytes);
    } else if (currentEstimatedFeeRate_ > 0.0) {
      estimatedFee = estimatedFeeDin(currentEstimatedFeeRate_, kPublicSendEstimateVbytes);
    }

    double maxAmount = balance - estimatedFee;
    if (maxAmount < 0) maxAmount = 0;

    edtAmount_->setText(QString::number(maxAmount, 'f', 8));
  }
}

// === Fee Estimation (Phase 35) ===

void MainWindow::onFeePresetChanged(int index) {
  if (!cmbFeePreset_) return;

  int confTarget = cmbFeePreset_->currentData().toInt();

  if (confTarget == -1) {
    // Custom fee - show the input field
    edtFee_->setVisible(true);
    lblEstimatedFee_->setText("Enter custom fee in una/vB");
  } else {
    // Preset fee - hide input and fetch estimate
    edtFee_->setVisible(false);
    edtFee_->clear();
    updateFeeEstimate();
  }
}

void MainWindow::updateFeeEstimate() {
  if (!cmbFeePreset_ || !rpc_) return;

  int confTarget = cmbFeePreset_->currentData().toInt();
  if (confTarget <= 0) return;  // Don't estimate for custom

  // Update label to show we're fetching
  lblEstimatedFee_->setText("Estimating...");

  // Map preset to mode
  QString mode = (confTarget >= 12) ? "ECONOMICAL" : "CONSERVATIVE";

  // Request fee estimate from daemon
  rpc_->estimateSmartFee(confTarget, mode);

  // Note: Response handled in onRpcResult
}

// === Network Verification ===

void MainWindow::verifyProductionNetwork() {
  // Verify we're connected to production mainnet
  expectedGenesisHash_ = QString::fromLatin1(dinero::solo::kMainnetGenesisHash);

  // Request block 0 (genesis)
  QJsonArray params;
  params.append(0); // block height 0
  rpc_->call("blockchain.getblockhash", params);

  // Response will be handled in onRpcResult
  // Will compare returned hash with expectedGenesisHash_
}

// === Peer Management ===

void MainWindow::onDisconnectPeer() {
  if (!tblPeers_) return;

  int row = tblPeers_->currentRow();
  if (row < 0) {
    QMessageBox::warning(this, "No Peer Selected", "Please select a peer to disconnect.");
    return;
  }

  auto* peerItem = tblPeers_->item(row, 1);
  QString addr = peerItem ? peerItem->data(Qt::UserRole).toString() : QString();
  if (addr.isEmpty() && peerItem) addr = peerItem->text();
  const QString label = peerItem ? peerItem->text() : addr;

  auto reply = QMessageBox::question(this, "Disconnect Peer",
    QString("Disconnect from %1?").arg(label),
    QMessageBox::Yes | QMessageBox::No);

  if (reply == QMessageBox::Yes) {
    // Use disconnectnode RPC
    QJsonArray params;
    params.append(addr);
    rpc_->call("disconnectnode", params);

    // Refresh peer table after disconnect
    QTimer::singleShot(1000, this, [this]() {
      rpc_->call("getpeerinfo", QJsonArray());
      rpc_->call("getnetworkinfo", QJsonArray());
    });
  }
}

void MainWindow::onBanPeer() {
  if (!tblPeers_) return;

  int row = tblPeers_->currentRow();
  if (row < 0) {
    QMessageBox::warning(this, "No Peer Selected", "Please select a peer to manage.");
    return;
  }

  auto* peerItem = tblPeers_->item(row, 1);
  QString addr = peerItem ? peerItem->data(Qt::UserRole).toString() : QString();
  if (addr.isEmpty() && peerItem) addr = peerItem->text();
  const QString label = peerItem ? peerItem->text() : addr;
  const QString ip = peerHostFromEndpoint(addr);
  if (ip.isEmpty()) {
    QMessageBox::warning(this, "Peer Address Missing", "The selected peer does not have a usable address.");
    return;
  }

  QMenu menu(this);
  QAction* block24h = menu.addAction("Block for 24 hours");
  block24h->setData(86400);
  QAction* block7d = menu.addAction("Block for 7 days");
  block7d->setData(604800);
  QAction* blockLong = menu.addAction("Block permanently");
  blockLong->setData(315360000); // 10 years; effectively permanent for a desktop ban.
  menu.addSeparator();
  QAction* unblock = menu.addAction("Unblock this address");
  unblock->setData(QStringLiteral("remove"));

  QAction* chosen = menu.exec(btnBanPeer_
      ? btnBanPeer_->mapToGlobal(QPoint(0, btnBanPeer_->height()))
      : QCursor::pos());
  if (!chosen) {
    return;
  }

  const bool removing = chosen->data().toString() == QStringLiteral("remove");

  if (!removing && isDefaultBootstrapPeerHost(ip)) {
    auto reply = QMessageBox::question(this, "Block Bootstrap Peer",
      QString("%1 is one of Dinero's default bootstrap peers.\n\nBlocking it is allowed, but it can reduce your node's ability to find peers automatically. Continue?")
        .arg(label),
      QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
      return;
    }
  }

  if (!removing) {
    auto reply = QMessageBox::question(this, "Block Peer",
      QString("%1\n\n%2 will disconnect this peer and prevent reconnection.")
        .arg(label, chosen->text()),
      QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
      return;
    }
  }

  QJsonArray params;
  params.append(ip);
  if (removing) {
    params.append("remove");
  } else {
    params.append("add");
    params.append(chosen->data().toInt());
    params.append(false);
  }
  rpc_->call("setban", params);

  QTimer::singleShot(1000, this, [this]() {
    rpc_->call("getpeerinfo", QJsonArray());
    rpc_->call("getnetworkinfo", QJsonArray());
  });
}

void MainWindow::onReconnectAllPeers() {
  if (!tblPeers_) return;

  // Collect all peer addresses from the table
  QStringList peerAddrs;
  for (int i = 0; i < tblPeers_->rowCount(); i++) {
    auto* peerItem = tblPeers_->item(i, 1);
    if (!peerItem) continue;
    QString addr = peerItem->data(Qt::UserRole).toString();
    if (addr.isEmpty()) addr = peerItem->text();
    peerAddrs.append(addr);
  }

  if (peerAddrs.isEmpty()) {
    QMessageBox::information(this, "No Peers", "No peers connected to reconnect.");
    return;
  }

  // Disconnect all peers
  for (const QString& addr : peerAddrs) {
    QJsonArray params;
    params.append(addr);
    rpc_->call("disconnectnode", params);
  }

  // Re-add all peers after a short delay
  QTimer::singleShot(3000, this, [this, peerAddrs]() {
    for (const QString& addr : peerAddrs) {
      QJsonArray params;
      params.append(addr);
      params.append("add");
      rpc_->call("addnode", params);
    }

    // Refresh peer table after reconnect
    QTimer::singleShot(3000, this, [this]() {
      rpc_->call("getpeerinfo", QJsonArray());
      rpc_->call("getnetworkinfo", QJsonArray());
    });
  });
}

void MainWindow::onCopyNetworkDiagnostics() {
  QApplication::clipboard()->setText(networkDiagnosticsText());
  if (lblPeerSummary_) {
    const QString previous = lblPeerSummary_->text();
    lblPeerSummary_->setText("Diagnostics copied to clipboard");
    QTimer::singleShot(1800, this, [this, previous]() {
      if (lblPeerSummary_) {
        lblPeerSummary_->setText(previous);
      }
    });
  }
}

// === Block Template Viewer ===

void MainWindow::onRefreshTemplate() {
  if (!edtMiningAddress_) return;

  QString addr = edtMiningAddress_->text().trimmed();
  if (addr.isEmpty()) {
    QMessageBox::warning(this, "Mining Address Required",
      "To request a block template, enter a Taproot mining address first.\n\n"
      "Tip: Wallet → Receive → New Transparent Address generates a Taproot (din1p/tdin1p/rdin1p) address by default.");
    return;
  }

  // v0.14: getblocktemplate REQUIRES an address param; calling with empty params fails.
  QJsonArray params;
  QJsonObject req;
  req["address"] = addr;
  params.append(req);
  rpc_->call("mining.getblocktemplate", params);
}

// === QR Code Generator ===

void MainWindow::onGenerateQR() {
  if (!lblQRCode_ || !edtQRAddress_) return;

  QString address = edtQRAddress_->text().trimmed();

  if (address.isEmpty()) {
    QMessageBox::warning(this, "Empty Address", "Please enter an address to generate QR code.");
    return;
  }

  // Validate address format
  if (!address.startsWith("din1q") && !address.startsWith("tdin1q") && !address.startsWith("rdin1q")) {
    QMessageBox::warning(this, "Invalid Address",
      "Invalid Dinero address format.\n\nAddress must start with din1q (mainnet), tdin1q (testnet), or rdin1q (regtest).");
    return;
  }

  // Generate QR code using our custom generator
  QPixmap qrCode = dinero::QRCodeGenerator::generate(address, 300, 4);

  if (qrCode.isNull()) {
    QMessageBox::critical(this, "QR Generation Failed", "Failed to generate QR code.");
    return;
  }

  // Display QR code
  lblQRCode_->setPixmap(qrCode.scaled(lblQRCode_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

  // Add address text overlay for verification
  QPixmap withText(qrCode.size() + QSize(0, 40));
  withText.fill(Qt::white);

  QPainter painter(&withText);
  painter.drawPixmap(0, 0, qrCode);

  // Draw address below QR code
  QFont font = painter.font();
  font.setPointSize(8);
  font.setFamily("Courier");
  painter.setFont(font);
  painter.setPen(Qt::black);

  QRect textRect(0, qrCode.height(), withText.width(), 40);
  painter.drawText(textRect, Qt::AlignCenter, address);

  lblQRCode_->setPixmap(withText.scaled(lblQRCode_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

  // Show success message
  QMessageBox::information(this, "QR Code Generated",
    QString("QR code generated successfully!\n\nAddress: %1\n\nScan this code with your mobile wallet to receive payments.").arg(address));
}

// === Address Book CSV Import/Export ===

void MainWindow::onImportAddresses() {
  QString filename = QFileDialog::getOpenFileName(this,
    "Import Watch-Only Addresses (CSV)",
    QDir::homePath(),
    "CSV Files (*.csv);;All Files (*)");

  if (filename.isEmpty()) return;

  QFile file(filename);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QMessageBox::critical(this, "Error", "Failed to open file for reading.");
    return;
  }

  QTextStream in(&file);
  QStringList validAddresses;
  QStringList labels;
  int lineNum = 0;

  // Parse CSV file
  while (!in.atEnd()) {
    QString line = in.readLine().trimmed();
    lineNum++;

    // Skip empty lines and header
    if (line.isEmpty() || line.startsWith("Label") || line.startsWith("#")) {
      continue;
    }

    QStringList fields = line.split(',');

    if (fields.size() >= 2) {
      QString label = fields[0].trimmed();
      QString address = fields[1].trimmed();

      // Validate address format
      if (address.startsWith("din1q") || address.startsWith("tdin1q") || address.startsWith("rdin1q")) {
        // Additional validation: check address length (Bech32 addresses are typically 42-62 chars)
        if (address.length() >= 42 && address.length() <= 62) {
          validAddresses.append(address);
          labels.append(label);
        }
      }
    }
  }

  file.close();

  if (validAddresses.isEmpty()) {
    QMessageBox::warning(this, "No Valid Addresses",
      "No valid Dinero addresses found in CSV file.\n\nExpected format:\nLabel,Address");
    return;
  }

  // Confirm import
  auto reply = QMessageBox::question(this, "Confirm Import",
    QString("Found %1 valid addresses.\n\nImport as watch-only addresses?\n\n"
            "Watch-only addresses allow you to monitor balances and transactions "
            "without the ability to spend funds.")
      .arg(validAddresses.size()),
    QMessageBox::Yes | QMessageBox::No);

  if (reply != QMessageBox::Yes) {
    return;
  }

  // Import addresses using importaddress RPC call
  int imported = 0;
  int failed = 0;

  for (int i = 0; i < validAddresses.size(); ++i) {
    QString address = validAddresses[i];
    QString label = labels[i];

    // Use importaddress RPC (wallet must be unlocked if encrypted)
    // Format: importaddress "address" "label" rescan=false
    QJsonArray params;
    params.append(address);
    params.append(label.isEmpty() ? QString("Imported_%1").arg(i) : label);
    params.append(false); // Don't rescan (too slow for bulk import)

    // Call RPC asynchronously
    // Note: We can't easily track success/failure here without modifying RpcClient
    // For now, assume success and let user check wallet afterward
    rpc_->call("wallet.importwallet", params);
    imported++;

    // Add small delay to avoid overwhelming RPC
    QThread::msleep(10);
  }

  // Show completion message
  QMessageBox::information(this, "Import Complete",
    QString("Imported %1 watch-only addresses.\n\n"
            "Note: Balances will update after blockchain rescan.\n"
            "Use 'rescanblockchain' RPC command if needed.")
      .arg(imported));

  // Refresh address table
  QTimer::singleShot(500, this, [this]() {
    // Request listreceivedbyaddress to update table
    rpc_->call("wallet.listreceivedbyaddress", QJsonArray() << 0 << true);
  });
}

void MainWindow::onExportAddresses() {
  QString filename = QFileDialog::getSaveFileName(this,
    "Export Addresses (CSV)",
    QDir::homePath() + "/dinero_addresses.csv",
    "CSV Files (*.csv);;All Files (*)");

  if (filename.isEmpty()) return;

  QFile file(filename);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    QMessageBox::critical(this, "Error", "Failed to open file for writing.");
    return;
  }

  QTextStream out(&file);

  // Write header
  out << "Index,Address,Label,Balance,Derivation Path\n";

  // Export from address table
  if (tblAddresses_) {
    for (int row = 0; row < tblAddresses_->rowCount(); ++row) {
      QString index = tblAddresses_->item(row, 0) ? tblAddresses_->item(row, 0)->text() : "";
      QString address = tblAddresses_->item(row, 1) ? tblAddresses_->item(row, 1)->text() : "";
      QString label = tblAddresses_->item(row, 2) ? tblAddresses_->item(row, 2)->text() : "";
      QString balance = tblAddresses_->item(row, 3) ? tblAddresses_->item(row, 3)->text() : "0";
      QString path = tblAddresses_->item(row, 4) ? tblAddresses_->item(row, 4)->text() : "";

      // Clean up "(no label)" placeholder for export
      if (label == "(no label)") label = "";

      // Escape commas in label if any
      if (label.contains(',') || label.contains('"')) {
        label = "\"" + label.replace("\"", "\"\"") + "\"";
      }

      out << QString("%1,%2,%3,%4,%5\n").arg(index).arg(address).arg(label).arg(balance).arg(path);
    }
  }

  file.close();

  QMessageBox::information(this, "Export Complete",
    QString("Exported addresses to:\n%1").arg(filename));
}

// === Inline Label Editing ===

void MainWindow::onAddressLabelDoubleClicked(int row, int column) {
  // Only allow editing on the Label column (column 2)
  if (column != 2) return;

  QTableWidgetItem* item = tblAddresses_->item(row, column);
  if (!item) return;

  // If it's showing placeholder "(no label)", clear it for editing
  if (item->text() == "(no label)") {
    item->setText("");
  }

  // Store original value for cancel detection
  item->setData(Qt::UserRole, item->text());

  // Enable editing for this cell
  tblAddresses_->setEditTriggers(QAbstractItemView::DoubleClicked);
  tblAddresses_->editItem(item);
}

void MainWindow::onAddressLabelChanged(QTableWidgetItem* item) {
  // Only process Label column (column 2)
  if (!item || item->column() != 2) return;

  // Prevent re-entrancy during programmatic updates
  if (labelEditInProgress_) return;

  int row = item->row();
  QTableWidgetItem* addrItem = tblAddresses_->item(row, 1);
  if (!addrItem) return;

  QString address = addrItem->text();
  QString newLabel = item->text().trimmed();
  QString originalLabel = item->data(Qt::UserRole).toString();

  // If unchanged, just restore display
  if (newLabel == originalLabel) {
    // Disable editing triggers again
    tblAddresses_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    return;
  }

  labelEditInProgress_ = true;

  // Save label via RPC
  QJsonObject params;
  params["address"] = address;
  params["label"] = newLabel;

  connectionMgr_->call("wallet.setlabel", params,
    [this, item, newLabel](QJsonObject result) {
      qDebug() << "MainWindow: Label saved successfully for address";

      // Update display style
      if (newLabel.isEmpty()) {
        item->setText("(no label)");
        item->setForeground(QBrush(QColor("#868e96")));
        item->setToolTip("Double-click to add a label");
      } else {
        item->setForeground(QBrush(QColor("#c6ced8")));
        item->setToolTip("");
      }

      // Disable editing triggers
      tblAddresses_->setEditTriggers(QAbstractItemView::NoEditTriggers);
      labelEditInProgress_ = false;
    },
    [this, item, originalLabel](QString error) {
      qWarning() << "MainWindow: Failed to save label:" << error;

      // Restore original value on failure
      item->setText(originalLabel.isEmpty() ? "(no label)" : originalLabel);
      item->setForeground(originalLabel.isEmpty() ? QBrush(QColor("#868e96")) : QBrush(QColor("#c6ced8")));

      // Disable editing triggers
      tblAddresses_->setEditTriggers(QAbstractItemView::NoEditTriggers);
      labelEditInProgress_ = false;

      QMessageBox::warning(this, "Label Error",
        QString("Failed to save label: %1").arg(error));
    }
  );
}

// === WebSocket Event Handlers ===

void MainWindow::onWsConnected() {
  // WebSocket status label removed (WebSockets disabled in production)
  // if (lblWsStatus_) {
  //   lblWsStatus_->setText("🟢 WebSocket: Connected");
  //   lblWsStatus_->setStyleSheet("QLabel { color: #51cf66; font-weight: bold; }");
  // }
}

void MainWindow::onWsDisconnected() {
  // WebSocket status label removed (WebSockets disabled in production)
  // if (lblWsStatus_) {
  //   lblWsStatus_->setText("🔴 WebSocket: Disconnected");
  //   lblWsStatus_->setStyleSheet("QLabel { color: #ff6b6b; font-weight: bold; }");
  // }
}

void MainWindow::onWsError(const QString& error) {
  // WebSocket status label removed (WebSockets disabled in production)
  // if (lblWsStatus_) {
  //   lblWsStatus_->setText(QString("⚠️ WebSocket Error: %1").arg(error));
  //   lblWsStatus_->setStyleSheet("QLabel { color: #fab005; font-weight: bold; }");
  // }
  Q_UNUSED(error);
}

void MainWindow::discoverAndConnectWebSocket() {
  qDebug() << "MainWindow: Discovering WebSocket port via RPC...";

  // Query getserverinfo to discover WebSocket port
  rpc_->call("server.getinfo");
}

void MainWindow::onWsNewBlock(const QJsonObject& blockData) {
  // Add new block to live events table
  if (!tblLiveEvents_) return;

  int row = tblLiveEvents_->rowCount();
  tblLiveEvents_->insertRow(0); // Insert at top

  QString height = QString::number(blockData["height"].toInt());
  QString hash = blockData["hash"].toString().left(16) + "...";
  QString time = QDateTime::currentDateTime().toString("HH:mm:ss");

  tblLiveEvents_->setItem(0, 0, new QTableWidgetItem(time));
  tblLiveEvents_->setItem(0, 1, new QTableWidgetItem("🟦 BLOCK"));
  tblLiveEvents_->setItem(0, 2, new QTableWidgetItem(QString("Height: %1  Hash: %2").arg(height).arg(hash)));

  // Limit table to 100 rows
  while (tblLiveEvents_->rowCount() > 100) {
    tblLiveEvents_->removeRow(tblLiveEvents_->rowCount() - 1);
  }

  // Trigger refresh of overview stats
  refresh();
}

void MainWindow::onWsNewTransaction(const QJsonObject& txData) {
  // Add new transaction to live events table
  if (!tblLiveEvents_) return;

  tblLiveEvents_->insertRow(0); // Insert at top

  QString txid = txData["txid"].toString().left(16) + "...";
  QString time = QDateTime::currentDateTime().toString("HH:mm:ss");

  tblLiveEvents_->setItem(0, 0, new QTableWidgetItem(time));
  tblLiveEvents_->setItem(0, 1, new QTableWidgetItem("💸 TX"));
  tblLiveEvents_->setItem(0, 2, new QTableWidgetItem(QString("TXID: %1").arg(txid)));

  // Limit table to 100 rows
  while (tblLiveEvents_->rowCount() > 100) {
    tblLiveEvents_->removeRow(tblLiveEvents_->rowCount() - 1);
  }
}

void MainWindow::onWsMiningInfo(const QJsonObject& miningData) {
  // Update mining stats from WebSocket
  updateMiningStats(miningData);
}

void MainWindow::onWsNetworkInfo(const QJsonObject& networkData) {
  updateNetworkInfo(networkData);
  // Update network status
  if (lblConnections_) {
    int connections = networkData["connections"].toInt();
    lblConnections_->setText(QString("Connections: %1").arg(connections));
    cachedPeerCount_ = connections;
    refreshAiStatusStrip();
  }
}

void MainWindow::onWsMempoolUpdate(const QJsonObject& mempoolData) {
  // Update mempool stats
  if (lblMempool_) {
    int txCount = mempoolData["size"].toInt();
    int bytes = mempoolData["bytes"].toInt(0);
    lblMempool_->setText(QString("Mempool: %1 txs, %2 bytes").arg(txCount).arg(bytes));
  }
}

void MainWindow::onWsSyncProgress(const QJsonObject& syncData) {
  // Update sync progress
  if (lblSyncProgress_) {
    double progress = syncData["progress"].toDouble() * 100.0;
    lblSyncProgress_->setText(QString("%1%").arg(progress, 0, 'f', 2));
  }
}

// === Node Status Update (for status pill) ===

void MainWindow::updateNodeStatus(const QJsonObject& blockchainInfo, const QJsonObject& networkInfo, const QJsonObject& mempoolInfo) {
  if (!lblNodeChain_ || !lblNodeHeight_ || !lblNodePeers_ || !lblNodeMempool_ || !lblNodeSyncStatus_) {
    return;
  }

  // Chain
  QString chain = blockchainInfo["chain"].toString("unknown");
  lblNodeChain_->setText(chain);

  // Height
  int height = blockchainInfo["blocks"].toInt();
  lblNodeHeight_->setText(QString::number(height));

  // Peers
  int connections = networkInfo["connections"].toInt();
  lblNodePeers_->setText(QString::number(connections));

  // Mempool
  int mempoolSize = mempoolInfo["size"].toInt();
  lblNodeMempool_->setText(QString::number(mempoolSize));

  // Sync status
  int headers = blockchainInfo["headers"].toInt();
  bool synced = (height >= headers - 1);

  if (synced) {
    lblNodeSyncStatus_->setText("✅ Synced");
    lblNodeSyncStatus_->setStyleSheet("QLabel { color: #d6dde6; font-weight: 600; }");
  } else {
    double progress = (height * 100.0) / headers;
    lblNodeSyncStatus_->setText(QString("🔄 Syncing %1%").arg(progress, 0, 'f', 1));
    lblNodeSyncStatus_->setStyleSheet("QLabel { color: #b9c2cc; font-weight: 600; }");
  }
}

void MainWindow::updateNetworkInfo(const QJsonObject& networkInfo) {
  cachedNetworkInfo_ = networkInfo;

  const int connections = networkInfo["connections"].toInt(0);
  const int inbound = networkInfo["connections_in"].toInt(0);
  const int outbound = networkInfo["connections_out"].toInt(0);
  const bool networkActive = networkInfo["networkactive"].toBool(false);
  const bool listening = networkInfo["listen"].toBool(false);
  const int listenPort = networkInfo["listen_port"].toInt(kDineroMainnetP2PPort);
  const bool inboundObserved = networkInfo["inbound_observed"].toBool(inbound > 0);
  const bool directReachable = networkInfo["direct_reachable"].toBool(inboundObserved);
  const bool relayFallbackEligible = networkInfo["relay_fallback_eligible"].toBool(false);
  const QJsonObject relay = networkInfo["relay"].toObject();
  const QString relayMode = relay["mode"].toString(networkInfo["relay_mode"].toString("auto"));
  const bool localRelay = relay["local"].toBool(networkInfo["localrelay"].toBool(false));
  const bool miningRelayActive =
      relay["mining_active"].toBool(networkInfo["mining_relay_active"].toBool(false));

  cachedPeerCount_ = connections;
  if (lblConnections_) {
    lblConnections_->setText(QString("Connections: %1").arg(connections));
  }
  if (lblPeersCount_) {
    lblPeersCount_->setText(QString("%1 peers").arg(connections));
  }
  if (lblPeersStatus_) {
    if (!networkActive) {
      lblPeersStatus_->setText("P2P disabled");
      lblPeersStatus_->setStyleSheet("QLabel { font-size: 11px; color: #a9b2bc; }");
    } else if (connections == 0) {
      lblPeersStatus_->setText("No peers");
      lblPeersStatus_->setStyleSheet("QLabel { font-size: 11px; color: #a9b2bc; }");
    } else if (inboundObserved) {
      lblPeersStatus_->setText(QString("%1 outbound, %2 inbound").arg(outbound).arg(inbound));
      lblPeersStatus_->setStyleSheet("QLabel { font-size: 11px; color: #d0d7df; }");
    } else {
      lblPeersStatus_->setText(QString("%1 outbound, inbound not seen").arg(outbound));
      lblPeersStatus_->setStyleSheet("QLabel { font-size: 11px; color: #b8c0ca; }");
    }
  }

  if (lblPeerSummary_) {
    lblPeerSummary_->setText(QString("Connections: %1 (%2 outbound, %3 inbound)")
        .arg(connections).arg(outbound).arg(inbound));
  }
  if (lblPeerReachability_) {
    lblPeerReachability_->setText(QString("Listening: %1 on TCP %2%3")
        .arg(listening ? "yes" : "no")
        .arg(listenPort)
        .arg(inboundObserved ? " · inbound seen" : " · inbound not seen"));
  }

  const QJsonObject portMap = networkInfo["port_mapping"].toObject();
  const bool requested = portMap["requested"].toBool(false);
  const bool active = portMap["active"].toBool(false);
  const QString protocol = portMap["protocol"].toString();
  const QString mode = portMap["mode"].toString("disabled");
  const QString message = portMap["message"].toString();
  const QJsonObject onionTransport = networkInfo["onion_transport"].toObject();
  const bool onionConfigured = onionTransport["configured"].toBool(false);
  const bool onionEnabled = onionTransport["enabled"].toBool(false);
  const bool onionReachable = onionTransport["reachable"].toBool(false);
  const QString onionProxy = onionTransport["proxy"].toString();
  const QString onionNote = onionTransport["note"].toString();
  QString reachabilityAdvice;
  QString reachabilityAdviceColor = "#aeb8c2";
  if (!networkActive) {
    reachabilityAdvice = "Reachability: P2P is disabled.";
  } else if (!listening) {
    reachabilityAdvice = "Reachability: outbound peer connections only.";
  } else if (inboundObserved) {
    reachabilityAdvice = "Reachability: inbound peers are connecting normally.";
    reachabilityAdviceColor = "#9fd4a8";
  } else if (active) {
    reachabilityAdvice = QString("Reachability: router mapping is active%1.")
        .arg(protocol.isEmpty() ? QString() : QString(" via %1").arg(protocol));
    reachabilityAdviceColor = "#9fd4a8";
  } else if (onionReachable) {
    reachabilityAdvice = "Reachability: Tor fallback is ready for onion peers; clearnet may still be outbound-only.";
    reachabilityAdviceColor = "#c6d7ff";
  } else if (onionConfigured) {
    reachabilityAdvice = "Reachability: Tor fallback is configured but not reachable yet.";
    reachabilityAdviceColor = "#d8c08a";
  } else if (relayFallbackEligible) {
    reachabilityAdvice = localRelay
        ? "Reachability: direct inbound is blocked; relay fallback is eligible and this miner can help relay the network."
        : "Reachability: direct inbound is blocked; relay fallback is eligible.";
    reachabilityAdviceColor = "#d8c08a";
  } else if (requested) {
    reachabilityAdvice = "Reachability: outbound works; router mapping did not open inbound yet.";
    reachabilityAdviceColor = "#d8c08a";
  } else {
    reachabilityAdvice = "Reachability: outbound works; inbound may need router mapping or Tor fallback.";
  }

  if (lblPeerPortMapping_) {
    QString text;
    if (!requested) {
      text = "Port mapping: not requested";
    } else if (active) {
      text = QString("Port mapping: %1 active").arg(protocol.isEmpty() ? mode : protocol);
    } else {
      text = QString("Port mapping: %1").arg(message.isEmpty() ? "unavailable" : message);
    }
    if (onionEnabled) {
      text += QString(" · Tor: %1").arg(onionReachable ? "ready" : "not reachable");
      if (!onionProxy.isEmpty()) {
        text += QString(" (%1)").arg(onionProxy);
      }
    } else if (onionConfigured) {
      text += QString(" · Tor: %1").arg(onionNote.isEmpty() ? "not available" : onionNote);
    }
    lblPeerPortMapping_->setText(text);
  }
  if (lblPeerRelay_) {
    QString role;
    if (relayMode == "0" || relayMode == "off" || relayMode == "false" || relayMode == "no") {
      role = "off";
    } else if (localRelay) {
      role = miningRelayActive ? "helping while mining" : "helping";
    } else if (relayMode == "auto") {
      role = "auto, idle until mining";
    } else {
      role = relayMode;
    }

    QString fallback = directReachable
        ? "direct reachable"
        : (relayFallbackEligible ? "fallback eligible" : "fallback idle");
    lblPeerRelay_->setText(QString("Relay: %1 · %2").arg(role, fallback));
  }
  if (lblPeerReachabilityAdvice_) {
    lblPeerReachabilityAdvice_->setText(reachabilityAdvice);
    lblPeerReachabilityAdvice_->setStyleSheet(
        QString("QLabel { color: %1; font-size: 12px; }").arg(reachabilityAdviceColor));
  }

  const QJsonArray localAddresses = networkInfo["localaddresses"].toArray();
  QStringList advertised;
  for (const QJsonValue& value : localAddresses) {
    const QJsonObject obj = value.toObject();
    const QString address = obj["address"].toString();
    const int port = obj["port"].toInt(0);
    if (!address.isEmpty() && port > 0) {
      advertised << QString("%1:%2").arg(address).arg(port);
    }
  }
  if (lblPeerAdvertised_) {
    lblPeerAdvertised_->setText(advertised.isEmpty()
        ? "Advertised: none yet"
        : QString("Advertised: %1").arg(advertised.join(", ")));
  }

  refreshAiStatusStrip();
}

QString MainWindow::networkDiagnosticsText() const {
  QJsonObject out;
  out["generated_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);
  out["rpc_endpoint"] = rpc_ ? rpc_->currentServer() : QString();
  out["network_info"] = cachedNetworkInfo_;

  QJsonArray peers;
  if (tblPeers_) {
    for (int row = 0; row < tblPeers_->rowCount(); ++row) {
      QJsonObject peer;
      auto* locationItem = tblPeers_->item(row, 1);
      peer["location"] = locationItem ? locationItem->text() : "";
      peer["endpoint"] = locationItem ? locationItem->data(Qt::UserRole).toString() : "";
      peer["type"] = tblPeers_->item(row, 2) ? tblPeers_->item(row, 2)->text() : "";
      peer["client"] = tblPeers_->item(row, 3) ? tblPeers_->item(row, 3)->text() : "";
      peer["height"] = tblPeers_->item(row, 4) ? tblPeers_->item(row, 4)->text() : "";
      peer["direction"] = tblPeers_->item(row, 5) ? tblPeers_->item(row, 5)->text() : "";
      peers.append(peer);
    }
  }
  out["peers"] = peers;

  return QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented));
}

// === Peer Table Update ===

void MainWindow::updatePeerTable(const QJsonArray& peers) {
  if (!tblPeers_) return;

  tblPeers_->setRowCount(0); // Clear existing rows

  for (const QJsonValue& peerVal : peers) {
    QJsonObject peer = peerVal.toObject();

    int row = tblPeers_->rowCount();
    tblPeers_->insertRow(row);

    // Week 7: Backend doesn't provide id field, use row index instead
    int id = row;
    QString addr = peer["addr"].toString();
    QString rawClient = peer["subver"].toString();
    int startHeight = peer["startingheight"].toInt(-1);
    int syncedBlocks = peer["synced_blocks"].toInt(-1);
    int syncedHeaders = peer["synced_headers"].toInt(-1);
    int bestKnown = peer["best_known_height"].toInt(-1);
    bool inbound = peer["inbound"].toBool();

    // Best available height: prefer best_known > synced_blocks > synced_headers > startingheight
    int bestHeight = startHeight;
    if (syncedHeaders > bestHeight) bestHeight = syncedHeaders;
    if (syncedBlocks > bestHeight) bestHeight = syncedBlocks;
    if (bestKnown > bestHeight) bestHeight = bestKnown;

    tblPeers_->setItem(row, 0, new QTableWidgetItem(QString::number(id)));
    const QString location = peerLocationLabel(addr, row);
    auto* locationItem = new QTableWidgetItem(location);
    locationItem->setData(Qt::UserRole, addr);
    locationItem->setToolTip(peerLocationTooltip(addr, location));
    tblPeers_->setItem(row, 1, locationItem);
    tblPeers_->setItem(row, 2, new QTableWidgetItem(inbound ? "In" : "Out"));
    auto* clientItem = new QTableWidgetItem(peerClientLabel(rawClient));
    clientItem->setToolTip(peerClientTooltip(rawClient));
    tblPeers_->setItem(row, 3, clientItem);
    auto* heightItem = new QTableWidgetItem(peerHeightDisplayText(bestHeight, cachedHeight_));
    heightItem->setToolTip(peerHeightBreakdownTooltip(startHeight,
                                                      syncedHeaders,
                                                      syncedBlocks,
                                                      bestKnown,
                                                      cachedHeight_));
    if (cachedHeight_ > 0 && bestHeight >= 0 && bestHeight + 2 < cachedHeight_) {
      heightItem->setForeground(QColor("#d9b36a"));
    }
    tblPeers_->setItem(row, 4, heightItem);
    tblPeers_->setItem(row, 5, new QTableWidgetItem(inbound ? "Inbound" : "Outbound"));
  }
}

// === Block Template Update ===

void MainWindow::updateBlockTemplate(const QJsonObject& blockTemplate) {
  if (!txtBlockTemplate_) return;

  // Display formatted JSON
  QJsonDocument doc(blockTemplate);
  txtBlockTemplate_->setPlainText(doc.toJson(QJsonDocument::Indented));

  // Update template stats labels
  if (lblTemplateHeight_) {
    int height = blockTemplate["height"].toInt();
    lblTemplateHeight_->setText(QString::number(height));
  }

  if (lblTemplateTxCount_) {
    QJsonArray transactions = blockTemplate["transactions"].toArray();
    lblTemplateTxCount_->setText(QString::number(transactions.size()));
  }

  if (lblTemplateFees_) {
    // totalfees is in una, convert to DIN
    double feesUnas = blockTemplate["totalfees"].toDouble();
    double feesDIN = feesUnas / 100000000.0;  // 1 DIN = 100,000,000 unas
    lblTemplateFees_->setText(QString::number(feesDIN, 'f', 8) + " DIN");
  }

  if (lblTemplateDifficulty_) {
    // Use bits field (compact difficulty format like "1d31ffce")
    QString bits = blockTemplate["bits"].toString();
    if (!bits.isEmpty()) {
      lblTemplateDifficulty_->setText("0x" + bits);
    } else {
      lblTemplateDifficulty_->setText("-");
    }
  }
}

// ═══════════════════════════════════════════════════════════════════
// 📊 MONITORING DASHBOARD IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════

void MainWindow::updateMonitoringDashboard() {
  // Only update monitoring dashboard if daemon is connected
  if (!connectionMgr_ || connectionMgr_->state() != ConnectionManager::Connected) {
    // Daemon is not connected - skip monitoring updates to avoid "Connection refused" errors
    return;
  }
  
  // Update CPU / thermal / power telemetry
  rpc_->call("node.getcpustats", QJsonArray());

  // Update hashrate sources
  rpc_->call("blockchain.getmininginfo", QJsonArray());
  rpc_->call("mining.info", QJsonArray());
  
  // Update Peers (Overview compact table)
  rpc_->call("getpeerinfo", QJsonArray());  // Will update via onRpcResult
  
  // Update Mempool stats
  rpc_->call("mempool.getinfo", QJsonArray());  // Will update via onRpcResult
}

void MainWindow::onExportMetrics() {
  // Build metrics snapshot
  QJsonObject metrics;
  
  // System metrics
  QJsonObject system;
  system["cpu_usage"] = cpuProgressBar_ ? cpuProgressBar_->value() : 0;
  system["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
  metrics["system"] = system;
  
  // Mining metrics
  QJsonObject mining;
  mining["local_hashrate"] = lblLocalHashrate_ ? lblLocalHashrate_->text() : "0 H/s";
  mining["network_hashrate"] = lblNetworkHashrate_ ? lblNetworkHashrate_->text() : "0 H/s";
  metrics["mining"] = mining;
  
  // Mempool metrics
  QJsonObject mempool;
  mempool["size"] = lblMempoolSize_ ? lblMempoolSize_->text() : "0 txs";
  mempool["bytes"] = lblMempoolBytes_ ? lblMempoolBytes_->text() : "0 bytes";
  metrics["mempool"] = mempool;
  
  // Network metrics
  QJsonObject network;
  network["peers_count"] = lblPeersCount_ ? lblPeersCount_->text() : "0 peers";
  network["status"] = lblPeersStatus_ ? lblPeersStatus_->text() : "Disconnected";
  network["p2p_status"] = cachedNetworkInfo_;
  
  // Peers list
  QJsonArray peers;
  if (tblPeersOverview_) {
    for (int i = 0; i < tblPeersOverview_->rowCount(); i++) {
      QJsonObject peer;
      auto* locationItem = tblPeersOverview_->item(i, 0);
      peer["location"] = locationItem ? locationItem->text() : "";
      peer["endpoint"] = locationItem ? locationItem->data(Qt::UserRole).toString() : "";
      peer["activity"] = tblPeersOverview_->item(i, 1) ? tblPeersOverview_->item(i, 1)->text() : "";
      peer["uptime"] = tblPeersOverview_->item(i, 2) ? tblPeersOverview_->item(i, 2)->text() : "";
      peer["height"] = tblPeersOverview_->item(i, 3) ? tblPeersOverview_->item(i, 3)->text() : "";
      peer["client"] = tblPeersOverview_->item(i, 4) ? tblPeersOverview_->item(i, 4)->text() : "";
      peers.append(peer);
    }
  }
  network["peers"] = peers;
  metrics["network"] = network;
  
  // Blockchain metrics
  QJsonObject blockchain;
  blockchain["height"] = lblHeight_ ? lblHeight_->text() : "-";
  blockchain["connections"] = lblConnections_ ? lblConnections_->text() : "-";
  blockchain["sync_progress"] = lblSyncProgress_ ? lblSyncProgress_->text() : "-";
  metrics["blockchain"] = blockchain;
  
  // Alerts
  QJsonArray alerts;
  if (txtAlerts_) {
    QString alertsText = txtAlerts_->toPlainText();
    QStringList alertLines = alertsText.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : alertLines) {
      alerts.append(line);
    }
  }
  metrics["alerts"] = alerts;
  
  // Save dialog
  QString defaultFileName = QString("dinero-metrics-%1").arg(
    QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")
  );
  
  QString filter = "JSON Files (*.json);;CSV Files (*.csv);;All Files (*)";
  QString selectedFilter;
  QString fileName = QFileDialog::getSaveFileName(
    this,
    "Export Metrics",
    defaultFileName,
    filter,
    &selectedFilter
  );
  
  if (fileName.isEmpty()) return;
  
  QFile file(fileName);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    QMessageBox::warning(this, "Export Failed", "Could not open file for writing");
    return;
  }
  
  // Export based on file extension
  if (fileName.endsWith(".csv") || selectedFilter.contains("CSV")) {
    // CSV export
    QTextStream out(&file);
    out << "Metric,Value\n";
    out << "CPU Usage," << system["cpu_usage"].toInt() << "%\n";
    out << "Local Hashrate," << mining["local_hashrate"].toString() << "\n";
    out << "Network Hashrate," << mining["network_hashrate"].toString() << "\n";
    out << "Mempool Size," << mempool["size"].toString() << "\n";
    out << "Mempool Bytes," << mempool["bytes"].toString() << "\n";
    out << "Peers Count," << network["peers_count"].toString() << "\n";
    out << "Network Status," << network["status"].toString() << "\n";
    out << "Block Height," << blockchain["height"].toString() << "\n";
    out << "Connections," << blockchain["connections"].toString() << "\n";
    out << "Sync Progress," << blockchain["sync_progress"].toString() << "\n";
    out << "Timestamp," << system["timestamp"].toString() << "\n";
    
    // Add peers
    out << "\nPeers\n";
    out << "Location,Endpoint,Activity,Uptime,Seen Height,Client\n";
    for (const QJsonValue& peerVal : peers) {
      QJsonObject peer = peerVal.toObject();
      out << peer["location"].toString() << ","
          << peer["endpoint"].toString() << ","
          << peer["activity"].toString() << ","
          << peer["uptime"].toString() << ","
          << peer["height"].toString() << ","
          << peer["client"].toString() << "\n";
    }
    
    // Add alerts
    if (!alerts.isEmpty()) {
      out << "\nAlerts\n";
      for (const QJsonValue& alert : alerts) {
        out << alert.toString() << "\n";
      }
    }
  } else {
    // JSON export (default)
    QJsonDocument doc(metrics);
    file.write(doc.toJson(QJsonDocument::Indented));
  }
  
  file.close();
  
  // Add alert
  QString alertMsg = QString("[%1] ✅ Metrics exported to %2").arg(
    QTime::currentTime().toString("HH:mm:ss"),
    QFileInfo(fileName).fileName()
  );
  if (txtAlerts_) {
    txtAlerts_->append(alertMsg);
  }
  
  QMessageBox::information(this, "Export Successful", 
    QString("Metrics exported to:\n%1").arg(fileName));
}

// ═══════════════════════════════════════════════════════════════════
// END MONITORING DASHBOARD IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════
// 🛡️ CONNECTION MANAGER HANDLERS (Bulletproof Connection Management)
// ═══════════════════════════════════════════════════════════════════
// AI Assistant
// ═══════════════════════════════════════════════════════════════════

void MainWindow::onToggleAiPanel() {
  if (cmdKPanel_) {
    cmdKPanel_->togglePanel();
  }
}

// ═══════════════════════════════════════════════════════════════════

void MainWindow::onDaemonConnected() {
  qDebug() << "MainWindow: Daemon connected ✅";
  daemonLaunchRetries_ = 0;  // connected — reset the launch-retry budget
  autoLoadDefaultAttempted_ = false;
  walletReorgInfoSupported_ = true;

  // Enable all tabs and controls
  setEnabled(true);

  // Update status bar
  lblConnectionStatus_->setText("Connected");
  lblConnectionStatus_->setStyleSheet(headerPillStyle());

  // Always show Stop button when connected
  btnStartDaemon_->setVisible(false);
  btnStopDaemon_->setVisible(true);

  // CRITICAL FIX: Enable mining buttons when daemon connects
  if (btnStartMining_) {
    btnStartMining_->setEnabled(true);
    btnStartMining_->setToolTip("Start mining to this address");
    if (!isMining_) {
      btnStartMining_->setText("Start Mining");
      btnStartMining_->setStyleSheet(headerButtonStyle());
    }
  }
  if (lblMiningStatus_) {
    lblMiningStatus_->setText(miningStatusInactiveText());
    lblMiningStatus_->setStyleSheet(chromePillStyle());
  }
  resetMiningReadinessDisplay("Readiness: waiting for daemon mining state",
                              "Daemon connected. Waiting for fresh mining.info.");
  if (!isMining_) {
    activeMinerType_ = "none";
    setMiningModeControlsLocked(false);
    setMiningOutputCinematicEnabled(false);
  }

  // Clear any previous error messages at bottom
  lblErrorMessage_->setVisible(false);

  // Trigger initial data refresh
  QTimer::singleShot(1000, this, &MainWindow::refresh);
}

void MainWindow::onDaemonDisconnected() {
  if (shuttingDown_) {
    return;
  }

  const bool poolMinerActive =
    isPoolProcessMinerType(activeMinerType_) &&
    miningProcess_ &&
    miningProcess_->state() != QProcess::NotRunning;

  qDebug() << "MainWindow: Daemon disconnected ⚪";
  walletSwitchInFlight_ = false;
  pendingWalletOpenName_.clear();
  currentWalletName_.clear();
  autoLoadDefaultAttempted_ = false;
  activeReservationId_.clear();
  bindWalletScopedState(QString());
  if (changeAddrMgr_) {
    changeAddrMgr_->setWalletIdentityKey(QString());
  }
  if (lblWalletName_) {
    lblWalletName_->setText("Wallet: none");
    lblWalletName_->setStyleSheet(headerPillStyle());
    lblWalletName_->setToolTip("No wallet loaded. Create or restore a wallet to get started.");
  }
  clearWalletScopedUiState();
  updateWalletUIState();

  // Update status bar
  lblConnectionStatus_->setText("Disconnected");
  lblConnectionStatus_->setStyleSheet(headerPillStyle());

  // Always show Start button when disconnected
  btnStartDaemon_->setVisible(true);
  btnStopDaemon_->setVisible(false);

  // Daemon-backed mining is unavailable while disconnected. Remote pool
  // miners keep running because their work comes from the pool connection.
  if (minerCtrl_ && minerCtrl_->running()) {
    qDebug() << "Stopping embedded miner due to daemon disconnect";
    minerCtrl_->stop();
  }
  if (btnStartMining_) {
    const bool poolModeSelected = !miningModeNeedsDaemon(currentMiningMode());
    btnStartMining_->setEnabled(poolMinerActive || poolModeSelected);
    btnStartMining_->setToolTip(poolMinerActive
      ? "Click to stop pool mining"
      : poolModeSelected
        ? "Start pool mining"
        : "Start daemon first to enable mining");
    btnStartMining_->setText(poolMinerActive ? "Stop Mining" : "Start Mining");
    btnStartMining_->setStyleSheet(headerButtonStyle());
  }
  if (btnStopMining_) {
    btnStopMining_->setEnabled(poolMinerActive);
  }
  if (lblMiningStatus_) {
    lblMiningStatus_->setText(poolMinerActive ? "Mining (Pool)" : miningStatusInactiveText());
    lblMiningStatus_->setStyleSheet(chromePillStyle());
  }
  resetMiningReadinessDisplay("Readiness: daemon unavailable",
                              "Daemon disconnected. Mining readiness cannot be evaluated.");

  if (!poolMinerActive) {
    isMining_ = false;
    activeMinerType_ = "none";
    setMiningModeControlsLocked(false);
    setMiningOutputCinematicEnabled(false);
  } else {
    isMining_ = true;
    setMiningModeControlsLocked(true);
  }

  // Stop mining stats timer since we can't poll anymore
  if (!poolMinerActive && miningStatsTimer_ && miningStatsTimer_->isActive()) {
    miningStatsTimer_->stop();
  }

  // Kill daemon-backed external miners only. Pool workers do not depend on the
  // local daemon and should not be SIGTERM'd during a reconnect.
  if (miningProcess_ && miningProcess_->state() != QProcess::NotRunning) {
    if (poolMinerActive) {
      qDebug() << "Daemon disconnected; leaving pool miner running";
      if (txtMiningOutput_) {
        appendMiningOutputLine(txtMiningOutput_,
          "[Pool miner] local daemon disconnected; pool miner left running");
      }
    } else {
      qDebug() << "Terminating daemon-backed miner due to daemon disconnect";
      miningProcess_->terminate();
      if (!miningProcess_->waitForFinished(3000)) {
        qWarning() << "External miner did not terminate gracefully after daemon disconnect, force-killing";
        miningProcess_->kill();
        miningProcess_->waitForFinished(1000);
      }
    }
  }
}

void MainWindow::updateConnectionStatus(ConnectionManager::ConnectionState state, ConnectionManager::ConnectionState oldState) {
  Q_UNUSED(oldState);

  auto poolMinerActive = [this]() {
    return isPoolProcessMinerType(activeMinerType_) &&
           miningProcess_ &&
           miningProcess_->state() != QProcess::NotRunning;
  };

  auto selectedModeNeedsDaemon = [this]() {
    return miningModeNeedsDaemon(currentMiningMode());
  };

  // Helper lambda to disable daemon-backed mining when not connected. Remote
  // pool miners are intentionally left alone; they do not consume daemon RPC.
  auto disableMining = [this]() {
    const bool poolActive =
      isPoolProcessMinerType(activeMinerType_) &&
      miningProcess_ &&
      miningProcess_->state() != QProcess::NotRunning;
    const bool poolModeSelected = !miningModeNeedsDaemon(currentMiningMode());

    if (btnStartMining_) {
      btnStartMining_->setEnabled(poolActive || poolModeSelected);
      btnStartMining_->setToolTip(poolActive
        ? "Click to stop pool mining"
        : poolModeSelected
          ? "Start pool mining"
          : "Start daemon first to enable mining");
      btnStartMining_->setText(poolActive ? "Stop Mining" : "Start Mining");
      btnStartMining_->setStyleSheet(headerButtonStyle());
    }
    if (btnStopMining_) btnStopMining_->setEnabled(poolActive);
    if (lblMiningStatus_) {
      lblMiningStatus_->setText(poolActive ? "Mining (Pool)" : miningStatusInactiveText());
      lblMiningStatus_->setStyleSheet(chromePillStyle());
    }
    resetMiningReadinessDisplay("Readiness: daemon unavailable",
                                "Connection state is not connected. Mining readiness cannot be evaluated.");
    // Stop mining timer
    if (!poolActive && miningStatsTimer_ && miningStatsTimer_->isActive()) {
      miningStatsTimer_->stop();
    }
    if (minerCtrl_ && minerCtrl_->running()) {
      qDebug() << "Stopping embedded miner due to connection state transition";
      minerCtrl_->stop();
    }
    // Terminate daemon-backed external miners only.
    if (miningProcess_ && miningProcess_->state() != QProcess::NotRunning) {
      if (poolActive) {
        qDebug() << "Connection state changed; leaving pool miner running";
      } else {
        miningProcess_->terminate();
        if (!miningProcess_->waitForFinished(3000)) {
          qWarning() << "External miner did not terminate gracefully on disconnect state transition, force-killing";
          miningProcess_->kill();
          miningProcess_->waitForFinished(1000);
        }
      }
    }
    if (!poolActive) {
      isMining_ = false;
      activeMinerType_ = "none";
      setMiningModeControlsLocked(false);
      setMiningOutputCinematicEnabled(false);
      resetOverviewMiningTelemetry();
    } else {
      isMining_ = true;
      setMiningModeControlsLocked(true);
    }
  };

  // Helper lambda to enable mining when connected
  auto enableMining = [this]() {
    const bool poolActive =
      isPoolProcessMinerType(activeMinerType_) &&
      miningProcess_ &&
      miningProcess_->state() != QProcess::NotRunning;
    if (btnStartMining_) {
      btnStartMining_->setEnabled(true);
      btnStartMining_->setText(poolActive ? "Stop Mining" : "Start Mining");
      btnStartMining_->setToolTip(poolActive ? "Click to stop pool mining" : "Start mining to this address");
    }
    if (lblMiningStatus_) {
      lblMiningStatus_->setText(poolActive
        ? (activeMinerType_.startsWith("sv2_pool") ? "Mining (SV2 Pool)" : "Pool Mining Active")
        : miningStatusInactiveText());
      lblMiningStatus_->setStyleSheet(chromePillStyle());
    }
    if (!poolActive) {
      resetMiningReadinessDisplay("Readiness: waiting for daemon mining state",
                                  "Daemon connected. Waiting for fresh mining.info.");
    }
  };

  switch (state) {
    case ConnectionManager::Disconnected:
      lblConnectionStatus_->setText("Disconnected");
      lblConnectionStatus_->setStyleSheet(headerPillStyle());
      // Always show Start button when disconnected
      btnStartDaemon_->setVisible(true);
      btnStopDaemon_->setVisible(false);
      // CRITICAL: Disable mining when disconnected
      disableMining();
      break;

    case ConnectionManager::Connecting:
      lblConnectionStatus_->setText("Connecting...");
      lblConnectionStatus_->setStyleSheet(headerPillStyle());
      // Show Stop button while connecting (will be connected soon)
      btnStartDaemon_->setVisible(false);
      btnStopDaemon_->setVisible(true);
      // Mining disabled during connection attempt
      if (selectedModeNeedsDaemon() || poolMinerActive()) {
        disableMining();
      }
      break;

    case ConnectionManager::Connected:
      lblConnectionStatus_->setText("Connected");
      lblConnectionStatus_->setStyleSheet(headerPillStyle());
      // Always show Stop button when connected (regardless of who started daemon)
      btnStartDaemon_->setVisible(false);
      btnStopDaemon_->setVisible(true);
      // CRITICAL: Enable mining when connected
      enableMining();
      break;

    case ConnectionManager::Reconnecting:
      // Status is updated by reconnecting signal with attempt counter
      lblConnectionStatus_->setStyleSheet(headerPillStyle());
      // Show Stop button during reconnect (daemon is running)
      btnStartDaemon_->setVisible(false);
      btnStopDaemon_->setVisible(true);
      // Mining disabled during reconnect
      if (selectedModeNeedsDaemon() || poolMinerActive()) {
        disableMining();
      }
      break;

    case ConnectionManager::Failed:
      lblConnectionStatus_->setText("Connection failed");
      lblConnectionStatus_->setStyleSheet(headerPillStyle());
      // Always show Start button on failure
      btnStartDaemon_->setVisible(true);
      btnStopDaemon_->setVisible(false);
      // CRITICAL: Disable mining on failure
      disableMining();
      break;
  }

  qDebug() << "MainWindow: Connection state changed to:" << connectionMgr_->stateString();
}

void MainWindow::onConnectionStatusMessage(QString message, QString level) {
  qDebug() << "MainWindow: Connection status [" << level << "]:" << message;
  
  // Show in status bar for important messages
  if (level == "error") {
    lblErrorMessage_->setText("❌ " + message);
    lblErrorMessage_->setStyleSheet("QLabel { color: #d6dde6; background: #2c3036; border: 1px solid #3d434d; padding: 6px; border-radius: 6px; }");
    lblErrorMessage_->setVisible(true);
  } else if (level == "warning") {
    lblErrorMessage_->setText("⚠️ " + message);
    lblErrorMessage_->setStyleSheet("QLabel { color: #d6dde6; background: #2c3036; border: 1px solid #3d434d; padding: 6px; border-radius: 6px; }");
    lblErrorMessage_->setVisible(true);
  }
}

void MainWindow::onBlockchainSyncUpdate(int blocks, int headers) {
  // Update block height display
  if (lblHeight_) {
    lblHeight_->setText(QString("Height: %1").arg(blocks));
  }
  
  // Always show headers; when equal, that means fully synced.
  if (lblHeaders_ && headers >= 0) {
    lblHeaders_->setText(QString("Headers: %1").arg(headers));
  }

  if (lblSyncProgress_ && headers > 0) {
    if (headers > blocks) {
      double progress = (blocks * 100.0) / headers;
      lblSyncProgress_->setText(QString("⏬ Syncing: %1% (%2 / %3)")
        .arg(progress, 0, 'f', 1)
        .arg(blocks)
        .arg(headers));
      lblSyncProgress_->setStyleSheet("QLabel { color: #d0d7df; font-weight: 600; background: #262b32; border: 1px solid #373d46; border-radius: 6px; padding: 5px; }");
    } else {
      lblSyncProgress_->setText("✅ Fully synced!");
      lblSyncProgress_->setStyleSheet("QLabel { color: #e1e6ec; font-weight: 600; background: #2b3037; border: 1px solid #3a4048; border-radius: 6px; padding: 5px; }");
    }
  }
  
  qDebug() << "MainWindow: Blockchain sync update - blocks:" << blocks << "headers:" << headers;
}

// ═══════════════════════════════════════════════════════════════════
// END CONNECTION MANAGER HANDLERS
// ═══════════════════════════════════════════════════════════════════
