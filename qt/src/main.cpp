#include <QApplication>
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QProcess>
#include <QSettings>
#include <QTcpSocket>
#include <QThread>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <iostream>
#include <QMessageBox>
#include <QPushButton>
#include <QCheckBox>
#include <QTimer>
#include <QMutex>
#include <QPointer>
#ifndef Q_OS_WIN
#include <signal.h>
#include <sys/types.h>
#endif
#ifdef Q_OS_WIN
// windows.h pollutes the global namespace with #defines for ERROR, DELETE,
// IN, OUT, etc. Several of those collide with enum values in our Qt UI
// headers (DebugConsole::LogLevel::ERROR is the most common offender).
// WIN32_LEAN_AND_MEAN trims most of the pollution; we explicitly undef
// the few macros that still come through and clash here.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  ifdef ERROR
#    undef ERROR
#  endif
#  ifdef DELETE
#    undef DELETE
#  endif
#  ifdef IN
#    undef IN
#  endif
#  ifdef OUT
#    undef OUT
#  endif
#endif
#include "mainwindow.h"
#include "build_identity.h"
#include "debugconsole.h"

#include "minercontroller.h"
#include <solo_miner/build_identity.h>

#ifdef HAVE_QT_QUICK
#include <QQmlApplicationEngine>
#include <QQmlContext>
#endif

// Global pointer to debug console for message handler
// Using QPointer to safely handle object deletion
static QPointer<dinero::DebugConsole> g_debugConsole;
static QMutex g_logMutex;

namespace {

constexpr int kDineroMainnetP2PPort = 20999;

QString p2pPortMapAllowedKey() {
    return QStringLiteral("network/p2p_portmap_auto_allowed_v1");
}

bool p2pPortMappingAllowed() {
    return QSettings().value(p2pPortMapAllowedKey(), true).toBool();
}

void appendDaemonNetworkArgs(QStringList& args) {
    args << "--listen" << "--p2pport" << QString::number(kDineroMainnetP2PPort)
         << "--rpc" << "--rpcport" << "20998";
    if (p2pPortMappingAllowed()) {
        args << "--portmap=auto";
    }
}

QStringList currentBootstrapAddnodes() {
    return {
        "-addnode=173.249.200.59:20999", // SJ
        "-addnode=172.93.167.32:20999",  // NA
        "-addnode=92.118.190.62:20999",  // EU1
    };
}

// Bundled AssumeUTXO snapshot shipped in the app's Resources. On a FRESH datadir
// the embedded daemon is pointed at it so a new wallet fast-syncs to the snapshot
// height instead of syncing from genesis — the slow path that maximizes exposure
// to the block-download catch-up. The daemon verifies the file's SHA256 against
// its compiled-in trust anchor (consensus/assume_utxo.cpp) before trusting it.
constexpr char kBundledSnapshotFile[] = "utxo-snapshot-52287.dat";

void appendAssumeUtxoSnapshotArgIfFresh(QStringList& args, const QString& datadir) {
    if (datadir.isEmpty()) {
        return;
    }
    // "Fresh" = no chain data written yet (block flatfiles + chaindb absent).
    // On an existing datadir we must NOT pass the snapshot (the node is past it).
    const bool fresh = !QDir(datadir + "/blocks").exists() &&
                       !QDir(datadir + "/blockchain").exists();
    if (!fresh) {
        return;
    }
    // macOS bundle layout: applicationDirPath() == <app>/Contents/MacOS, so the
    // snapshot lives one level up under Resources. Resolves to nonexistent in dev
    // builds (no bundled .dat) → we simply sync normally there.
    const QString snapshotPath =
        QFileInfo(QCoreApplication::applicationDirPath() + "/../Resources/" +
                  QString::fromLatin1(kBundledSnapshotFile))
            .absoluteFilePath();
    if (!QFile::exists(snapshotPath)) {
        qWarning() << "Fresh datadir but bundled AssumeUTXO snapshot not found at"
                   << snapshotPath << "— daemon will sync from genesis";
        return;
    }
    qInfo() << "Fresh datadir: fast-syncing from bundled AssumeUTXO snapshot"
            << snapshotPath;
    args << QString("--assumeutxo_snapshot=%1").arg(snapshotPath);
}

} // namespace

// Temporary message handler - suppresses output until Debug Console is ready
static void suppressingMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
  // Suppress all output in production builds
  // In debug builds, still print critical errors to stderr
#ifndef QT_NO_DEBUG
  if (type == QtCriticalMsg || type == QtFatalMsg) {
    fprintf(stderr, "[FATAL] %s\n", msg.toUtf8().constData());
  }
#endif
}

// Custom Qt message handler - redirects all qDebug/qWarning/qCritical to Debug Console
static void customMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
  QMutexLocker locker(&g_logMutex);

  // Map Qt message types to DebugConsole log levels
  dinero::DebugConsole::LogLevel level;
  switch (type) {
    case QtDebugMsg:
      level = dinero::DebugConsole::LogLevel::DEBUG;
      break;
    case QtInfoMsg:
      level = dinero::DebugConsole::LogLevel::INFO;
      break;
    case QtWarningMsg:
      level = dinero::DebugConsole::LogLevel::WARNING;
      break;
    case QtCriticalMsg:
    case QtFatalMsg:
      level = dinero::DebugConsole::LogLevel::ERROR;
      break;
    default:
      level = dinero::DebugConsole::LogLevel::INFO;
  }

  // Send to debug console if available
  // Use queued connection to ensure GUI is fully initialized
  // QPointer will automatically become null if the object is destroyed
  if (!g_debugConsole.isNull()) {
    // Capture by value to avoid dangling pointer issues
    QPointer<dinero::DebugConsole> consolePtr = g_debugConsole;
    QMetaObject::invokeMethod(g_debugConsole.data(), [consolePtr, level, msg]() {
      // Check if console still exists when lambda executes
      if (!consolePtr.isNull()) {
        consolePtr->logGuiEvent(level, msg);
      }
    }, Qt::QueuedConnection);
  }

  // Also print to stderr in debug builds (helpful for development)
#ifndef QT_NO_DEBUG
  fprintf(stderr, "%s\n", msg.toUtf8().constData());
#endif
}

static QString defaultDataDir() {
#if defined(Q_OS_WIN)
    const QString appData = QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA"));
    const QString dir = appData.isEmpty()
        ? QDir::home().filePath("Dinero")
        : QDir(appData).filePath("Dinero");
#elif defined(Q_OS_MAC)
    const QString legacyDir = QDir::home().filePath(".dinero");
    const QString dir = QDir::home().filePath("Library/Application Support/Dinero");

    if (!QDir(dir).exists() && QDir(legacyDir).exists()) {
        QDir().mkpath(QFileInfo(dir).absolutePath());
        if (QDir().rename(legacyDir, dir)) {
            qInfo() << "Migrated macOS Dinero data directory from" << legacyDir << "to" << dir;
        } else {
            qWarning() << "Could not migrate macOS Dinero data directory from" << legacyDir
                       << "to" << dir << "- continuing with legacy directory";
            return legacyDir;
        }
    }
#else
    const QString dir = QDir::home().filePath(".dinero");
#endif
    QDir().mkpath(dir);
    return dir;
}

static QString defaultMinerPath() {
    const QString appDir = QCoreApplication::applicationDirPath();
#if defined(Q_OS_MAC)
    return QDir(appDir).absoluteFilePath("../Resources/dinero-miner");
#elif defined(Q_OS_WIN)
    return QDir(appDir).absoluteFilePath("dinero-miner.exe");
#else
    return QDir(appDir).absoluteFilePath("dinero-miner");
#endif
}

static QString defaultDaemonPath() {
    const QString appDir = QCoreApplication::applicationDirPath();
    QDir dir(appDir);

#if defined(Q_OS_MAC)
    // Check multiple possible locations in priority order
    QString homeDir = QDir::homePath();
    QStringList possiblePaths = {
        dir.absoluteFilePath("../Resources/dinerod"), // macOS app bundle Resources
        dir.absoluteFilePath("dinerod"),           // Same directory (packaged binaries)
        dir.absoluteFilePath("../dinerod"),        // Parent directory (build directory)
        dir.absoluteFilePath("../../../dinerod"),  // .app bundle structure
        homeDir + "/Desktop/MAC_DINERO/bin/dinerod",  // Desktop deployment folder
        "/usr/local/bin/dinerod",                  // System installation
    };

    for (const QString& path : possiblePaths) {
        QFileInfo info(path);
        if (info.exists() && info.isExecutable()) {
            qDebug() << "Found daemon at:" << info.absoluteFilePath();
            return info.absoluteFilePath();
        }
    }

    // If not found, return most likely path (will fail with clear error)
    qWarning() << "Daemon binary not found in any expected location";
    qWarning() << "Searched paths:" << possiblePaths;
    return dir.absoluteFilePath("dinerod");

#elif defined(Q_OS_WIN)
    // Windows: check same directory first, then parent
    QStringList possiblePaths = {
        dir.absoluteFilePath("dinerod.exe"),
        dir.absoluteFilePath("../dinerod.exe"),
    };
    for (const QString& path : possiblePaths) {
        if (QFileInfo(path).exists()) {
            return path;
        }
    }
    return dir.absoluteFilePath("dinerod.exe");

#else
    // Linux/BSD: check same directory first, then parent, then system
    QStringList possiblePaths = {
        dir.absoluteFilePath("dinerod"),
        dir.absoluteFilePath("../dinerod"),
        "/usr/local/bin/dinerod",
        "/usr/bin/dinerod",
    };
    for (const QString& path : possiblePaths) {
        QFileInfo info(path);
        if (info.exists() && info.isExecutable()) {
            return info.absoluteFilePath();
        }
    }
    return dir.absoluteFilePath("dinerod");
#endif
}

// Path to dinero-cli, used by the graceful-shutdown helper to send a
// `dinero-cli stop` RPC before falling back to terminate()/kill(). Mirrors
// defaultDaemonPath()'s search order; dinero-cli ships in the same directory
// as dinerod on every platform.
static QString defaultCliPath() {
    const QString appDir = QCoreApplication::applicationDirPath();
    QDir dir(appDir);

#if defined(Q_OS_MAC)
    QString homeDir = QDir::homePath();
    QStringList possiblePaths = {
        dir.absoluteFilePath("../Resources/dinero-cli"),
        dir.absoluteFilePath("dinero-cli"),
        dir.absoluteFilePath("../dinero-cli"),
        dir.absoluteFilePath("../../../dinero-cli"),
        homeDir + "/Desktop/MAC_DINERO/bin/dinero-cli",
        "/usr/local/bin/dinero-cli",
    };
    for (const QString& path : possiblePaths) {
        QFileInfo info(path);
        if (info.exists() && info.isExecutable()) {
            return info.absoluteFilePath();
        }
    }
    return dir.absoluteFilePath("dinero-cli");

#elif defined(Q_OS_WIN)
    QStringList possiblePaths = {
        dir.absoluteFilePath("dinero-cli.exe"),
        dir.absoluteFilePath("../dinero-cli.exe"),
    };
    for (const QString& path : possiblePaths) {
        if (QFileInfo(path).exists()) {
            return path;
        }
    }
    return dir.absoluteFilePath("dinero-cli.exe");

#else
    QStringList possiblePaths = {
        dir.absoluteFilePath("dinero-cli"),
        dir.absoluteFilePath("../dinero-cli"),
        "/usr/local/bin/dinero-cli",
        "/usr/bin/dinero-cli",
    };
    for (const QString& path : possiblePaths) {
        QFileInfo info(path);
        if (info.exists() && info.isExecutable()) {
            return info.absoluteFilePath();
        }
    }
    return dir.absoluteFilePath("dinero-cli");
#endif
}

// === Daemon orphan-process lifecycle (rc6 backlog item, fixed in v2.2.6-rc2) ===
//
// Problem: when dinero-qt exits or crashes, the dinerod child it spawned
// can survive — leaking a process, holding port 20998, and locking the
// datadir. Visible to users as zombie dinerod processes after a Qt crash;
// visible in the smoke harness as a leftover daemon between test modes
// blocking the next dinerod from binding RPC.
//
// Fix has two parts:
//
//   1. gracefulShutdownDaemon(): a clean shutdown sequence that runs on
//      normal Qt exit (when app.exec() returns). Sends `dinero-cli stop`
//      first so dinerod flushes wallet / chain / mempool state to disk,
//      then falls back to QProcess::terminate() and finally kill() if
//      the daemon doesn't acknowledge.
//
//   2. assignDaemonToWinJobObject(): on Windows only, the dinerod child
//      is assigned to a Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.
//      When dinero-qt's last handle to the job closes — including on
//      hard crashes that bypass app.exec() — the Windows kernel kills
//      every process in the job. Bulletproof on Windows.
//
// POSIX (Linux/macOS) gets the same crash cleanup through dinerod's
// --embedded-parent-pid mode. Qt passes its own PID when it launches the
// bundled daemon; dinerod arms PR_SET_PDEATHSIG on Linux and kqueue parent
// monitoring on macOS/BSD. Normal operator daemons do not pass that flag.

#ifdef Q_OS_WIN
// Single per-process Job Object that owns every dinerod we spawn. Created
// on first call to assignDaemonToWinJobObject(); never explicitly closed
// — the OS releases the handle on dinero-qt exit, which is exactly the
// trigger for KILL_ON_JOB_CLOSE.
static HANDLE g_dinerodJob = NULL;
#endif

static void assignDaemonToWinJobObject(QProcess* daemonProc) {
#ifdef Q_OS_WIN
    if (!daemonProc) return;

    if (g_dinerodJob == NULL) {
        g_dinerodJob = CreateJobObjectW(NULL, NULL);
        if (g_dinerodJob == NULL) {
            qWarning() << "CreateJobObject failed (error" << GetLastError()
                       << ") — daemon orphan cleanup on crash will not work";
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(g_dinerodJob,
                                     JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))) {
            qWarning() << "SetInformationJobObject failed (error" << GetLastError()
                       << ") — daemon orphan cleanup on crash may not work";
        }
    }

    DWORD pid = static_cast<DWORD>(daemonProc->processId());
    if (pid == 0) {
        qWarning() << "Daemon QProcess has no PID; skipping Job Object assignment";
        return;
    }
    HANDLE childHandle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                     FALSE, pid);
    if (childHandle == NULL) {
        qWarning() << "OpenProcess(" << pid << ") failed (error" << GetLastError()
                   << ") — daemon will not be tied to parent lifetime";
        return;
    }
    if (!AssignProcessToJobObject(g_dinerodJob, childHandle)) {
        DWORD err = GetLastError();
        // ERROR_ACCESS_DENIED here usually means the child already belongs
        // to a non-nestable job (rare on modern Windows; some Defender
        // configs do this). Log and continue — the graceful-shutdown path
        // still applies on normal exit.
        qWarning() << "AssignProcessToJobObject failed (error" << err
                   << ") — daemon will rely on graceful shutdown only";
    }
    CloseHandle(childHandle);
#else
    Q_UNUSED(daemonProc);
#endif
}

// Set when the app is quitting on purpose (aboutToQuit). The unexpected-
// daemon-exit handler checks this so a daemon we terminate during shutdown
// doesn't trigger a "daemon exited unexpectedly" dialog.
static bool g_quittingCleanly = false;

// Last N lines of dinerod's debug.log for fail-loud error dialogs.
// dinerod writes its real diagnostics there (its stdout/stderr is
// forwarded to the parent terminal, which doesn't exist for a
// double-clicked .app bundle).
static QString daemonLogTail(const QString& datadir, int maxLines = 20) {
    QFile log(QDir(datadir).filePath("debug.log"));
    if (!log.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QStringLiteral("(no debug.log found in %1)").arg(datadir);
    }
    // debug.log can be large; read only the trailing 64 KiB.
    const qint64 kTailBytes = 64 * 1024;
    if (log.size() > kTailBytes) {
        log.seek(log.size() - kTailBytes);
    }
    const QStringList lines = QString::fromUtf8(log.readAll())
                                  .split('\n', Qt::SkipEmptyParts);
    const int start = qMax(0, static_cast<int>(lines.size()) - maxLines);
    return lines.mid(start).join('\n');
}

static void gracefulShutdownDaemon(QProcess* daemonProc) {
    if (!daemonProc) return;
    if (daemonProc->state() != QProcess::Running) {
        qDebug() << "Daemon already stopped; nothing to do";
        return;
    }

    qDebug() << "Shutting down daemon (RPC stop -> terminate -> kill)...";

    // Step 1: graceful RPC stop. dinerod handles this by flushing wallet
    // and chain state to disk and exiting cleanly within ~1-3 seconds.
    const QString cliPath = defaultCliPath();
    if (QFile::exists(cliPath)) {
        QProcess stopProc;
        stopProc.start(cliPath, QStringList() << "stop");
        stopProc.waitForFinished(3000);
        // dinero-cli returns immediately after the daemon acknowledges
        // the stop request; the daemon itself takes a moment to actually
        // exit. Wait for the QProcess to report finished too.
        if (daemonProc->waitForFinished(5000)) {
            qDebug() << "Daemon stopped gracefully via RPC";
            return;
        }
        qDebug() << "RPC stop didn't take effect within 5s; escalating";
    } else {
        qDebug() << "dinero-cli not found at" << cliPath << "; skipping RPC stop";
    }

    // Step 2: ask the OS to terminate (SIGTERM on POSIX,
    // CtrlBreak/CloseWindow on Windows GUI; for our console-less daemon
    // this typically just calls TerminateProcess after a short grace).
    daemonProc->terminate();
    if (daemonProc->waitForFinished(5000)) {
        qDebug() << "Daemon terminated";
        return;
    }

    // Step 3: hard kill. Last resort; daemon may have left state in an
    // inconsistent disk state, but on next start dinerod will doctor-check
    // and recover.
    qWarning() << "Daemon didn't respond to terminate() within 5s; calling kill()";
    daemonProc->kill();
    daemonProc->waitForFinished(3000);
}

// Exit code for genesis mismatch (must match dinerod main.cpp)
static constexpr int GENESIS_MISMATCH_EXIT_CODE = 10;

// Check if daemon is running by testing connection to RPC port.
// TCP-bind only — does NOT prove the daemon is responding to requests.
// Used as a fast pre-check; pair with isDaemonHealthy() for liveness.
static bool isDaemonRunning(int timeoutMs = 100) {
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", 20998);
    return socket.waitForConnected(timeoutMs);
}

// Check if daemon is actually serving HTTP. Sends a no-auth POST and
// requires *some* HTTP response within timeout — even a 401 proves the
// HTTP server is alive. A bound port that hangs (stuck dinerod in
// chainstate recovery, REORG ABORT loop, or mid-startup) returns false.
static bool isDaemonHealthy(int timeoutMs = 3000) {
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", 20998);
    if (!socket.waitForConnected(timeoutMs)) {
        return false;
    }
    // Minimal JSON-RPC ping. No cookie auth needed — we just want to
    // see ANY HTTP status line back.
    const QByteArray body =
        "{\"jsonrpc\":\"1.0\",\"id\":\"qt-ping\",\"method\":\"getblockcount\",\"params\":[]}";
    QByteArray req = "POST / HTTP/1.1\r\n"
                     "Host: 127.0.0.1:20998\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                     "Connection: close\r\n\r\n" + body;
    socket.write(req);
    if (!socket.waitForBytesWritten(timeoutMs)) {
        return false;
    }
    if (!socket.waitForReadyRead(timeoutMs)) {
        return false;
    }
    QByteArray response = socket.readAll();
    return response.startsWith("HTTP/");
}

// Find PIDs of any dinerod processes whose --datadir matches ours.
// Used to clean up orphans from prior force-quit Qt sessions before
// spawning a fresh daemon. Returns empty list if none found or pgrep
// is unavailable.
static QList<qint64> findStaleDinerodPids(const QString& datadir) {
    QList<qint64> pids;
#ifndef Q_OS_WIN
    QProcess pgrep;
    pgrep.start("pgrep", QStringList() << "-f" << "/dinerod ");
    if (!pgrep.waitForFinished(2000)) {
        pgrep.kill();
        return pids;
    }
    const QStringList lines = QString::fromUtf8(pgrep.readAllStandardOutput())
                                  .split('\n', Qt::SkipEmptyParts);
    const QString datadirNorm = QDir::cleanPath(datadir);
    for (const QString& pidStr : lines) {
        bool ok = false;
        qint64 pid = pidStr.trimmed().toLongLong(&ok);
        if (!ok || pid <= 0) continue;
        // Read this PID's full command line to check --datadir match.
        QProcess ps;
        ps.start("ps", QStringList() << "-p" << QString::number(pid) << "-o" << "command=");
        if (!ps.waitForFinished(1000)) {
            ps.kill();
            continue;
        }
        const QString cmd = QString::fromUtf8(ps.readAllStandardOutput());
        // Match either `--datadir <path>` or `--datadir=<path>`.
        // Skip non-dinerod processes whose path happens to contain "/dinerod ".
        if (!cmd.contains("/dinerod ") && !cmd.endsWith("/dinerod")) continue;
        if (cmd.contains("--datadir=" + datadirNorm) ||
            cmd.contains("--datadir " + datadirNorm) ||
            cmd.contains("-datadir=" + datadirNorm) ||
            cmd.contains("-datadir " + datadirNorm)) {
            pids.append(pid);
        }
    }
#else
    Q_UNUSED(datadir);
#endif
    return pids;
}

// Cleanly terminate any stale dinerod processes for our datadir.
// Sends SIGTERM, waits up to 10s, then SIGKILLs survivors. Safe to
// call when no orphans exist (no-op).
static void killStaleDinerodForDatadir(const QString& datadir) {
#ifndef Q_OS_WIN
    QList<qint64> pids = findStaleDinerodPids(datadir);
    if (pids.isEmpty()) return;
    qWarning() << "Found stale dinerod PIDs for our datadir, terminating:" << pids;
    for (qint64 pid : pids) {
        ::kill(static_cast<pid_t>(pid), SIGTERM);
    }
    // Grace period for graceful shutdown (chainstate flush).
    for (int i = 0; i < 50; ++i) {  // 10s total
        QThread::msleep(200);
        QList<qint64> remaining;
        for (qint64 pid : pids) {
            if (::kill(static_cast<pid_t>(pid), 0) == 0) remaining.append(pid);
        }
        if (remaining.isEmpty()) {
            qDebug() << "All stale dinerod PIDs exited cleanly";
            return;
        }
        pids = remaining;
    }
    // Anyone still alive is wedged. Hard-kill is the right call here:
    // they were already orphan/stuck (no parent UI, RPC unresponsive).
    qWarning() << "Stale dinerod PIDs did not exit on SIGTERM, sending SIGKILL:" << pids;
    for (qint64 pid : pids) {
        ::kill(static_cast<pid_t>(pid), SIGKILL);
    }
    QThread::msleep(500);
#else
    Q_UNUSED(datadir);
#endif
}

// Sweep orphaned dinero-seeder processes (issue #295: an rc37 seeder
// survived 3 days holding 127.0.0.1:20998, so the fresh dinerod could
// never bind RPC and the GUI waited forever). The seeder is normally a
// child of dinerod; one whose parent is gone (PPID == 1) is by
// definition stale, so killing it can't break a live opt-in seeder.
static void killStaleOrphanSeeders() {
#ifndef Q_OS_WIN
    QProcess pgrep;
    pgrep.start("pgrep", QStringList() << "-f" << "dinero-seeder");
    if (!pgrep.waitForFinished(2000)) {
        pgrep.kill();
        return;
    }
    const QStringList lines = QString::fromUtf8(pgrep.readAllStandardOutput())
                                  .split('\n', Qt::SkipEmptyParts);
    QList<qint64> orphans;
    for (const QString& pidStr : lines) {
        bool ok = false;
        qint64 pid = pidStr.trimmed().toLongLong(&ok);
        if (!ok || pid <= 0) continue;
        QProcess ps;
        ps.start("ps", QStringList() << "-p" << QString::number(pid) << "-o" << "ppid=");
        if (!ps.waitForFinished(1000)) {
            ps.kill();
            continue;
        }
        const qint64 ppid = QString::fromUtf8(ps.readAllStandardOutput())
                                .trimmed().toLongLong();
        if (ppid == 1) orphans.append(pid);
    }
    if (orphans.isEmpty()) return;
    qWarning() << "Found orphaned dinero-seeder PIDs, terminating:" << orphans;
    for (qint64 pid : orphans) {
        ::kill(static_cast<pid_t>(pid), SIGTERM);
    }
    for (int i = 0; i < 15; ++i) {  // 3s grace
        QThread::msleep(200);
        QList<qint64> remaining;
        for (qint64 pid : orphans) {
            if (::kill(static_cast<pid_t>(pid), 0) == 0) remaining.append(pid);
        }
        if (remaining.isEmpty()) return;
        orphans = remaining;
    }
    qWarning() << "Orphaned dinero-seeder PIDs did not exit on SIGTERM, sending SIGKILL:" << orphans;
    for (qint64 pid : orphans) {
        ::kill(static_cast<pid_t>(pid), SIGKILL);
    }
    QThread::msleep(200);
#endif
}

// Start the daemon in the background (returns QProcess* for output capture)
static QProcess* startDaemon(const QString& datadir, dinero::DebugConsole* debugConsole) {
    QString daemonPath = defaultDaemonPath();

    // Check if daemon binary exists
    if (!QFile::exists(daemonPath)) {
        qWarning() << "Daemon binary not found at:" << daemonPath;
        return nullptr;
    }

    qDebug() << "Starting daemon:" << daemonPath;
    qDebug() << "Data directory:" << datadir;

    // Create daemon process. Parented to the application instance so the
    // QProcess object itself can never outlive the app (#295); actual child
    // teardown happens in the aboutToQuit handler / post-exec shutdown.
    QProcess* daemonProc = new QProcess(QCoreApplication::instance());

    // Build arguments WITH SEED NODES
    QStringList args;
    if (!datadir.isEmpty()) {
        args << "--datadir" << datadir;
    }
    args << QString("--embedded-parent-pid=%1").arg(QCoreApplication::applicationPid());

    // Enable inbound P2P + RPC so Qt wallet can connect and the node can
    // request UPnP/NAT-PMP mapping from the same early launcher used at app boot.
    appendDaemonNetworkArgs(args);

    // CRITICAL: Add current fleet bootstrap nodes so daemon can connect to network.
    args << currentBootstrapAddnodes();

    // Fresh wallet: fast-sync from the bundled AssumeUTXO snapshot rather than
    // syncing from genesis (removes the long, catch-up-stall-prone first run).
    appendAssumeUtxoSnapshotArgIfFresh(args, datadir);

    // Track C: Liquidity Vault. Daemon defaults are vault=1 and
    // (as of v2.1.29) shadow=0 — credits open for real once a
    // deposit reaches k_credit confirmations. Operators wire the
    // auto-observer by passing `-vault.address=<din1p…>` and
    // optional `-vault.account=<id>` on the daemon command line.

    // Handle output based on whether Debug Console exists
    if (debugConsole) {
        // Debug Console exists - capture output and send to it
        QObject::connect(daemonProc, &QProcess::readyReadStandardOutput, [daemonProc, debugConsole]() {
            QString output = QString::fromUtf8(daemonProc->readAllStandardOutput());
            // Send each line to Debug Console
            for (const QString& line : output.split('\n', Qt::SkipEmptyParts)) {
                debugConsole->logDaemonOutput(line);
            }
        });

        QObject::connect(daemonProc, &QProcess::readyReadStandardError, [daemonProc, debugConsole]() {
            QString output = QString::fromUtf8(daemonProc->readAllStandardError());
            // Send each line to Debug Console
            for (const QString& line : output.split('\n', Qt::SkipEmptyParts)) {
                debugConsole->logDaemonOutput(line);
            }
        });
    } else {
        // No Debug Console yet - forward output to parent Terminal
        // This ensures "dinero-qt" launched from Terminal shows daemon output
        daemonProc->setProcessChannelMode(QProcess::ForwardedChannels);
    }

    // Start the daemon
    daemonProc->start(daemonPath, args);

    if (!daemonProc->waitForStarted(5000)) {
        qWarning() << "Failed to start daemon:" << daemonProc->errorString();
        delete daemonProc;
        return nullptr;
    }

    qDebug() << "Daemon started successfully with PID:" << daemonProc->processId();

    // Tie this daemon's lifetime to dinero-qt's process handle on Windows
    // so a crash of the GUI takes the daemon down with it. No-op on POSIX
    // (see Phase 2 in the lifecycle comment above).
    assignDaemonToWinJobObject(daemonProc);

    // Wait for daemon to be ready (up to 10 seconds)
    for (int i = 0; i < 100; i++) {
        QThread::msleep(100);

        // Check if daemon exited early (genesis mismatch or other error)
        if (daemonProc->state() == QProcess::NotRunning) {
            qWarning() << "Daemon exited early with code:" << daemonProc->exitCode();
            // Don't delete — caller inspects the exit code and fails loud
            // (#295: silently dropping this left the GUI waiting forever).
            return daemonProc;
        }

        if (isDaemonRunning(100)) {
            qDebug() << "Daemon is ready!";
            return daemonProc;
        }
    }

    qWarning() << "Daemon started but didn't become ready within 10 seconds";
    // Don't delete process - it's still running
    return daemonProc;
}

// Ensure daemon is running, start if needed. Returns QProcess* if we started it.
static QProcess* ensureDaemonRunning(const QString& datadir, dinero::DebugConsole* debugConsole = nullptr) {
    qDebug() << "═══════════════════════════════════════════════════════";
    qDebug() << "Checking daemon status...";
    qDebug() << "Data directory:" << datadir;

    // Two-stage check: TCP-bind first (fast no-op when port is free),
    // then HTTP ping to confirm the daemon is actually serving requests.
    // Port-bound-but-stuck daemons (chainstate recovery, REORG ABORT)
    // pass the TCP check but fail the HTTP one — without this, the UI
    // would silently attach to a wedged daemon and look broken.
    if (isDaemonRunning(2000) && isDaemonHealthy(3000)) {
        qDebug() << "✅ Daemon is already running and responsive";
        qDebug() << "═══════════════════════════════════════════════════════";
        return nullptr;  // We didn't start it, so we don't own it
    }

    // Sweep any orphan dinerod from a prior force-quit Qt session that
    // shares our datadir. Without this, the spawn below races with the
    // orphan over port 20998 and the LOCK file. Also sweep orphaned
    // dinero-seeder processes that can squat the RPC port (#295).
    killStaleDinerodForDatadir(datadir);
    killStaleOrphanSeeders();

    qDebug() << "Daemon not detected (or unresponsive), attempting to start fresh...";

    // Bounded spawn retry (#295 follow-up). On a quick relaunch the previous
    // session's daemon may still be releasing port 20998 / the datadir LOCK
    // for a second or two, so a fresh dinerod exits 1 ("port in use") on the
    // first attempt and then succeeds. Without a retry the caller fired a
    // scary "daemon exited before the wallet could connect" dialog on every
    // such start, even though the GUI connected fine moments later. Retry a
    // few times — only fall through (and let the caller fail loud) on a
    // genuinely persistent failure.
    constexpr int kMaxSpawnAttempts = 3;
    QProcess* daemonProc = nullptr;
    for (int attempt = 1; attempt <= kMaxSpawnAttempts; ++attempt) {
        daemonProc = startDaemon(datadir, debugConsole);

        if (daemonProc && daemonProc->state() != QProcess::NotRunning) {
            qDebug() << "✅ Successfully started daemon";
            qDebug() << "═══════════════════════════════════════════════════════";
            return daemonProc;
        }

        // Genesis mismatch is a real, non-transient condition the caller
        // handles with its own wipe-and-restart flow — never retry it.
        if (daemonProc && daemonProc->exitCode() == GENESIS_MISMATCH_EXIT_CODE) {
            return daemonProc;
        }

        if (attempt < kMaxSpawnAttempts) {
            const int code = daemonProc ? daemonProc->exitCode() : -1;
            qWarning() << "Daemon spawn attempt" << attempt << "of" << kMaxSpawnAttempts
                       << "failed (exit code" << code << "); likely a prior daemon "
                          "still releasing the port — retrying after a short wait";
            if (daemonProc) { delete daemonProc; daemonProc = nullptr; }

            // The previous daemon may have just finished settling and become
            // healthy — adopt it rather than spawning again.
            if (isDaemonRunning(500) && isDaemonHealthy(2000)) {
                qDebug() << "✅ A healthy daemon became available; adopting it";
                return nullptr;  // not ours
            }

            // Clear any stale instance and give the port + LOCK time to free.
            killStaleDinerodForDatadir(datadir);
            killStaleOrphanSeeders();
            QThread::msleep(1500);
        }
    }

    if (!daemonProc) {
        qWarning() << "═══════════════════════════════════════════════════════";
        qWarning() << "❌ ERROR: Failed to auto-start daemon after"
                   << kMaxSpawnAttempts << "attempts";
        qWarning() << "";
        qWarning() << "Possible causes:";
        qWarning() << "  1. Another daemon instance is already running with this datadir";
        qWarning() << "  2. Daemon binary not found or not executable";
        qWarning() << "  3. Port 20998 is already in use by another process";
        qWarning() << "";
        qWarning() << "You can try:";
        qWarning() << "  - Close any other Dinero applications";
        qWarning() << "  - Check if port 20998 is available: lsof -i :20998";
        qWarning() << "  - Start dinerod manually: ./dinerod --datadir" << datadir;
        qWarning() << "═══════════════════════════════════════════════════════";
    }
    return daemonProc;  // null or last dead process → caller fails loud (genuine failure)
}

// macOS: running from a mounted DMG is common and makes orphaned child
// processes stickier (#295). Offer the conventional "Move to Applications"
// drag-install hint once; "Don't show again" persists via QSettings.
#ifdef Q_OS_MAC
static void maybeOfferMoveToApplications() {
    if (!QCoreApplication::applicationDirPath().startsWith(QStringLiteral("/Volumes/"))) {
        return;
    }
    const QString suppressKey = QStringLiteral("ui/dmg_install_prompt_suppressed_v1");
    QSettings settings;
    if (settings.value(suppressKey, false).toBool()) {
        return;
    }
    QMessageBox box;
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle("Running from Disk Image");
    box.setText("Dinero is running directly from the disk image.");
    box.setInformativeText(
        "For best results, drag Dinero.app into your Applications folder "
        "and launch it from there. Running from the mounted disk image can "
        "leave background processes behind after quitting.");
    auto* dontShowAgain = new QCheckBox("Don't show this again", &box);
    box.setCheckBox(dontShowAgain);
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
    if (dontShowAgain->isChecked()) {
        settings.setValue(suppressKey, true);
    }
}
#endif

// Pre-spawn port check (#295): if something accepts TCP on the daemon RPC
// port but does not answer HTTP, it is a squatter (e.g. an orphaned
// dinero-seeder) — a fresh dinerod will fail to bind and exit. Ask the
// user instead of silently entering an endless "waiting" state.
// Returns false if the user chose to quit.
static bool confirmRpcPortSquatter() {
    if (!isDaemonRunning(500)) {
        return true;  // Port free — normal startup.
    }
    if (isDaemonHealthy(3000)) {
        return true;  // Usable daemon — ensureDaemonRunning() connects to it.
    }
    QMessageBox box;
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle("Port Already in Use");
    box.setText("Port 20998 is already in use — another Dinero process may be running.");
    box.setInformativeText(
        "Something is listening on the daemon RPC port (127.0.0.1:20998) but "
        "is not responding like a Dinero daemon. This is usually a leftover "
        "Dinero process from a previous session.\n\n"
        "Dinero will try to clean up stale processes and continue, but if the "
        "port stays occupied the daemon cannot start. You can also quit and "
        "check what holds the port (lsof -i :20998).");
    QPushButton* continueBtn = box.addButton("Continue", QMessageBox::AcceptRole);
    box.addButton("Quit", QMessageBox::RejectRole);
    box.setDefaultButton(continueBtn);
    box.exec();
    return box.clickedButton() == continueBtn;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromUtf8(argv[i]);
    if (arg == "--version") {
      const auto solo_identity = dinero::solo::GetBuildIdentity();
      std::cout << dinero::qt::build::FormatIdentity();
      std::cout << "embedded_solo_miner_commit: " << solo_identity.full_sha << "\n";
      std::cout << "embedded_solo_miner_version: " << solo_identity.version << "\n";
      return 0;
    }
  }

  QApplication app(argc, argv);
  app.setApplicationName("Dinero");
  app.setOrganizationName("Dinero");

  // Install suppressing message handler early to prevent console output
  // This will be replaced with the full Debug Console handler later
  qInstallMessageHandler(suppressingMessageHandler);

  // Production: Always use standard system data directory
  // This prevents conflicts with development data directories
  QString datadir = defaultDataDir();

  // Allow override via command-line argument
  QStringList args = app.arguments();
  for (int i = 1; i < args.size(); ++i) {
    if (args[i].startsWith("-datadir=")) {
      datadir = args[i].mid(9); // Extract value after "-datadir="
      break;
    }
  }

#ifdef Q_OS_MAC
  // One-time drag-install hint when launched from a mounted DMG (#295).
  maybeOfferMoveToApplications();
#endif

  // Fail loud if a non-daemon process is squatting the RPC port (#295).
  if (!confirmRpcPortSquatter()) {
    return 0;
  }

  // Auto-launch daemon if not already running
  QProcess* daemonProcess = ensureDaemonRunning(datadir);

  // Check if daemon exited with genesis mismatch
  if (daemonProcess &&
      daemonProcess->state() == QProcess::NotRunning &&
      daemonProcess->exitCode() == GENESIS_MISMATCH_EXIT_CODE) {

    QMessageBox msgBox;
    msgBox.setWindowTitle("Incompatible Chain Data");
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setText(
      "Your chain data is from an older or incompatible version of Dinero.\n\n"
      "This can happen after a chain reset or major upgrade. "
      "Your wallet will be backed up automatically before wiping.\n\n"
      "Wipe chain data and restart with a fresh sync?"
    );
    QPushButton* wipeBtn = msgBox.addButton("Wipe and Restart", QMessageBox::AcceptRole);
    msgBox.addButton("Quit", QMessageBox::RejectRole);
    msgBox.exec();

    if (msgBox.clickedButton() == wipeBtn) {
      // Re-launch daemon with --wipe-stale-chain
      delete daemonProcess;
      daemonProcess = nullptr;

      QString daemonPath = defaultDaemonPath();
      QStringList wipeArgs;
      if (!datadir.isEmpty()) {
        wipeArgs << "--datadir" << datadir;
      }
      wipeArgs << "--wipe-stale-chain";
      appendDaemonNetworkArgs(wipeArgs);
      wipeArgs << currentBootstrapAddnodes();

      daemonProcess = new QProcess(QCoreApplication::instance());
      daemonProcess->start(daemonPath, wipeArgs);

      if (!daemonProcess->waitForStarted(5000)) {
        QMessageBox::critical(nullptr, "Error",
          "Failed to restart daemon after wipe.\n" + daemonProcess->errorString());
        delete daemonProcess;
        return 1;
      }

      // Wait for daemon to be ready after wipe
      bool ready = false;
      for (int i = 0; i < 300; i++) {  // 30 seconds — wipe + init takes longer
        QThread::msleep(100);
        if (daemonProcess->state() == QProcess::NotRunning) {
          QMessageBox::critical(nullptr, "Error",
            "Daemon exited unexpectedly after chain wipe (exit code: "
            + QString::number(daemonProcess->exitCode()) + ").");
          delete daemonProcess;
          return 1;
        }
        if (isDaemonRunning(100)) {
          ready = true;
          break;
        }
      }

      if (!ready) {
        qWarning() << "Daemon wipe+restart didn't become ready within 30 seconds, continuing anyway...";
      }
    } else {
      // User chose Quit
      delete daemonProcess;
      return 0;
    }
  }

  // Fail loud on any other early daemon exit (#295): previously this was
  // a qWarning the user never saw, leaving the GUI in an endless
  // "waiting" state when e.g. the RPC port could not be bound.
  if (daemonProcess && daemonProcess->state() == QProcess::NotRunning) {
    const int exitCode = daemonProcess->exitCode();
    QMessageBox box;
    box.setIcon(QMessageBox::Critical);
    box.setWindowTitle("Daemon Failed to Start");
    box.setText(QString("The Dinero daemon (dinerod) exited during startup "
                        "with exit code %1.").arg(exitCode));
    box.setInformativeText(
      "Common causes:\n"
      "  • Port 20998 is already in use by another process\n"
      "  • Another Dinero instance is using the same data directory\n\n"
      "See the daemon log below for details (Show Details).");
    box.setDetailedText(QString("Last lines of %1/debug.log:\n\n%2")
                            .arg(datadir, daemonLogTail(datadir)));
    QPushButton* continueBtn =
        box.addButton("Continue Anyway", QMessageBox::AcceptRole);
    box.addButton("Quit", QMessageBox::RejectRole);
    box.setDefaultButton(continueBtn);
    box.exec();

    const bool keepGoing = box.clickedButton() == continueBtn;
    delete daemonProcess;  // already dead — nothing to shut down
    daemonProcess = nullptr;
    if (!keepGoing) {
      return 1;
    }
  }

  // Child lifecycle (#295): make sure every quit path (menu Quit, Cmd+Q,
  // window close, app-level quit) tears the daemon down. aboutToQuit fires
  // for all of them; the post-exec gracefulShutdownDaemon() call below is
  // kept as an idempotent backstop (it no-ops once the daemon is stopped).
  QPointer<QProcess> daemonProcessGuard(daemonProcess);
  QObject::connect(&app, &QCoreApplication::aboutToQuit, [daemonProcessGuard]() {
    g_quittingCleanly = true;
    if (daemonProcessGuard) {
      gracefulShutdownDaemon(daemonProcessGuard.data());
    }
  });

  // Fail loud if our daemon dies AFTER startup but before/while the GUI is
  // using it (#295). Suppressed during intentional shutdown.
  if (daemonProcess) {
    QObject::connect(daemonProcess,
                     qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     [datadir](int exitCode, QProcess::ExitStatus status) {
      if (g_quittingCleanly) {
        return;
      }
      if (status == QProcess::NormalExit && exitCode == 0) {
        // Clean stop (e.g. the GUI's Stop Daemon button sent `stop` via
        // RPC). The connection UI already reflects this; no error dialog.
        qDebug() << "Daemon exited cleanly (user-requested stop)";
        return;
      }
      qWarning() << "Daemon exited unexpectedly with code:" << exitCode;
      QMessageBox box;
      box.setIcon(QMessageBox::Critical);
      box.setWindowTitle("Daemon Stopped Unexpectedly");
      box.setText(QString("The Dinero daemon (dinerod) exited unexpectedly "
                          "with exit code %1.").arg(exitCode));
      box.setInformativeText(
        "The wallet is no longer connected to the network. You can restart "
        "the daemon from the toolbar (Start Daemon) or quit and relaunch.\n\n"
        "See the daemon log below for details (Show Details).");
      box.setDetailedText(QString("Last lines of %1/debug.log:\n\n%2")
                              .arg(datadir, daemonLogTail(datadir)));
      box.exec();
    });
  }

#ifdef HAVE_QT_QUICK
  // Register MinerController with QML (if Qt Quick is available)
  qmlRegisterType<MinerController>("Dinero", 1, 0, "MinerController");
#endif

  MainWindow w;
  w.setDatadir(datadir);

  // Show window first
  w.show();

  // Debug console disabled temporarily - causes crashes
  // TODO: Fix DebugConsole initialization in MainWindow constructor
  // auto* consolePtr = w.getDebugConsole();
  // if (consolePtr != nullptr) {
  //   g_debugConsole = consolePtr;
  //   qInstallMessageHandler(customMessageHandler);
  // }

  int ret = app.exec();

  // Graceful daemon shutdown if we started it. Sends `dinero-cli stop`
  // first (flushes wallet/chain to disk), then escalates to terminate()
  // and kill() with timeouts. On Windows the Job Object created in
  // startDaemon() also kills the daemon if dinero-qt itself crashed
  // before reaching this point.
  if (daemonProcess) {
    gracefulShutdownDaemon(daemonProcess);
    delete daemonProcess;
  }

  return ret;
}
