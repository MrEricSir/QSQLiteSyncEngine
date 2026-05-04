#include "syncengine/SharedFolderTransport.h"

#include <QDateTime>
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
    if (latencyMs > 0) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_delayed.append({filename, data, now + latencyMs});
        return true;
    }

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
    flushDelayed();

    QDir dir(folderPathString);
    QStringList filters;
    filters << QStringLiteral("*.changeset");
    QStringList files = dir.entryList(filters, QDir::Files, QDir::Name);
    return files;
}

void SharedFolderTransport::setSimulatedLatencyMs(int ms)
{
    latencyMs = ms;
}

void SharedFolderTransport::flushDelayed()
{
    if (m_delayed.isEmpty())
        return;

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<DelayedFile> stillDelayed;

    for (const auto &df : m_delayed) {
        if (now >= df.visibleAfterMs) {
            // Write to disk now
            QString filePath = folderPathString + QStringLiteral("/") + df.filename;
            QFile file(filePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(df.data);
                file.close();
            }
        } else {
            stillDelayed.append(df);
        }
    }
    m_delayed = stillDelayed;
}

} // namespace syncengine
