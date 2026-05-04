#include "syncengine/SharedFolderTransport.h"

#include <QFile>
#include <QDebug>

namespace syncengine {

SharedFolderTransport::SharedFolderTransport(const QString &folderPath, QObject *parent)
    : QObject(parent)
    , folderPathString(folderPath)
{
    QDir dir(folderPath);
    if (!dir.exists())
        dir.mkpath(QStringLiteral("."));

    watcher.addPath(folderPath);
    connect(&watcher, &QFileSystemWatcher::directoryChanged,
            this, &SharedFolderTransport::changesetsAvailable);
}

bool SharedFolderTransport::writeChangeset(const QString &filename, const QByteArray &data)
{
    QString filePath = folderPathString + QStringLiteral("/") + filename;
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to write changeset:" << filePath << file.errorString();
        return false;
    }
    file.write(data);
    file.close();
    return true;
}

QByteArray SharedFolderTransport::readChangeset(const QString &filename)
{
    QString filePath = folderPathString + QStringLiteral("/") + filename;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to read changeset:" << filePath;
        return {};
    }
    return file.readAll();
}

QStringList SharedFolderTransport::listChangesets()
{
    QDir dir(folderPathString);
    QStringList filters;
    filters << QStringLiteral("*.changeset");
    QStringList files = dir.entryList(filters, QDir::Files, QDir::Name);
    return files;
}

} // namespace syncengine
