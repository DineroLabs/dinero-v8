#include "apprestart.h"

#include <QCoreApplication>

namespace dinero::qt::apprestart {
namespace {

bool g_restart_requested = false;

}  // namespace

QString AwaitingRestartFlag() {
  return QStringLiteral("--awaiting-restart");
}

int LockWaitMilliseconds() {
  // Generous on purpose. The outgoing instance must first stop its embedded
  // daemon, which sends `dinero-cli stop` and waits for the daemon to flush
  // wallet and chain state before escalating. On a busy node that is seconds,
  // not milliseconds, and giving up early would show the user a spurious
  // "Dinero is already running" instead of completing their restart.
  return 30000;
}

QStringList RelaunchArguments(const QStringList& original_arguments) {
  QStringList relaunch;
  // Skip element 0: it is the program path, supplied separately to the process
  // starter. Everything else is preserved so the replacement opens the SAME
  // datadir; dropping -datadir= would silently restart onto a different wallet.
  for (int index = 1; index < original_arguments.size(); ++index) {
    const QString argument = original_arguments.at(index);
    // Never accumulate the flag across repeated restarts.
    if (argument == AwaitingRestartFlag()) {
      continue;
    }
    relaunch.append(argument);
  }
  relaunch.append(AwaitingRestartFlag());
  return relaunch;
}

bool StartedAwaitingRestart(const QStringList& arguments) {
  return arguments.contains(AwaitingRestartFlag());
}

void RequestRestart() {
  g_restart_requested = true;
  // Quit through the event loop rather than exiting here, so app.exec()
  // returns normally and main()'s graceful daemon shutdown runs. Calling
  // exit() or _exit() at this point would leave dinerod orphaned with the
  // ports still bound, which is the collision this whole file exists to avoid.
  QCoreApplication::quit();
}

bool RestartRequested() { return g_restart_requested; }

}  // namespace dinero::qt::apprestart
