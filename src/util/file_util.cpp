#include "util/file_util.h"
#include <QDir>
#include <QStandardPaths>
#include <QDebug>

QString dineroDataDir(const QString& network, const QString& subdir, const QString& filename) {
    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (baseDir.isEmpty()) {
        baseDir = QDir::homePath() + "/.dinero";
    }
    
    QDir dir(baseDir);
    if (!network.isEmpty()) {
        dir.cd(network);
    }
    if (!subdir.isEmpty()) {
        dir.cd(subdir);
    }
    
    if (!filename.isEmpty()) {
        return dir.absoluteFilePath(filename);
    }
    
    return dir.absolutePath();
}

QString dineroNodeInfoPath(const QString& network) {
    return dineroDataDir(network, "", "node.info");
}
