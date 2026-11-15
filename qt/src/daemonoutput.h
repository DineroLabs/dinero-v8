#pragma once

#include <QString>

class QProcess;

namespace dinero::qt {

QString daemonOutputLogPath(const QString& datadir);
bool configureDaemonOutput(QProcess* process, const QString& datadir, QString* error = nullptr);

}  // namespace dinero::qt
