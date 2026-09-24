// In-place restart of dinero-qt, safe against the collisions the GUI already
// guards against elsewhere.
//
// WHY THIS IS NOT "startDetached() then quit()".
// dinero-qt owns two exclusive resources for a datadir:
//   * a QLockFile at <datadir>/.dinero-qt.lock, held for the whole process
//     lifetime and released only when the process exits, and
//   * an embedded dinerod bound to fixed ports (RPC 20998 / P2P 20999), which
//     is stopped by gracefulShutdownDaemon() AFTER app.exec() returns.
// Launching the replacement before those are released reproduces exactly the
// failure main.cpp already documents: the new GUI sees a not-yet-free port,
// races the old daemon, and dies with "port in use", or bounces off the single
// instance lock and shows "Dinero is already running".
//
// The order here avoids that:
//   1. RequestRestart() records the intent and asks Qt to quit.
//   2. app.exec() returns; the EXISTING graceful shutdown stops the daemon and
//      waits for it, so the ports are free.
//   3. main() relaunches, passing kAwaitingRestartFlag, as its last act.
//   4. The replacement sees that flag and WAITS for the lock instead of
//      refusing to start, because the old process is still exiting.
// Daemon port reuse is then handled by the bounded spawn retry that main.cpp
// already performs on a quick relaunch; this file does not duplicate it.

#pragma once

#include <QString>
#include <QStringList>

namespace dinero::qt::apprestart {

// Passed to the replacement process so it tolerates the brief window in which
// the outgoing process still holds the single-instance lock.
QString AwaitingRestartFlag();

// How long a replacement waits for the outgoing instance to release the lock
// before giving up and showing the normal "already running" message.
int LockWaitMilliseconds();

// Build the replacement's argument list from the outgoing process's arguments.
// Keeps everything meaningful (notably -datadir=), drops the program name, and
// guarantees the flag appears EXACTLY once however often this restarts.
// Pure and total so it can be tested without launching anything.
QStringList RelaunchArguments(const QStringList& original_arguments);

// True when this process was started by a restart and should wait for the lock.
bool StartedAwaitingRestart(const QStringList& arguments);

// Record that the user asked for a restart, then ask Qt to quit. Safe to call
// from a widget slot: it returns immediately and the work happens as the event
// loop unwinds.
void RequestRestart();

// Read by main() after app.exec() returns.
bool RestartRequested();

}  // namespace dinero::qt::apprestart
