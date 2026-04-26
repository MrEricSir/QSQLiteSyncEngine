#include "syncengine/SharedFolderTransport.h"

#include <QDateTime>
#include <QFile>
#include <QDebug>

namespace syncengine {

SharedFolderTransport::SharedFolderTransport(const QString &folderPath, QObject *parent)
    : QObject(parent)
    , m_folderPath(folderPath)
{
    QDir dir(folderPath);
    if (!dir.exists())
        dir.mkpath(QStringLiteral("."));

    m_watcher.addPath(folderPath);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &SharedFolderTransport::changesetsAvailable);
}

bool SharedFolderTransport::writeChangeset(const QString &filename, const QByteArray &data)
{
    if (m_latencyMs > 0) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_delayed.append({filename, data, now + m_latencyMs});
        return true;
    }

    QString filePath = m_folderPath + QStringLiteral("/") + filename;
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
    QString filePath = m_folderPath + QStringLiteral("/") + filename;
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

    QDir dir(m_folderPath);
    QStringList filters;
    filters << QStringLiteral("*.changeset");
    QStringList files = dir.entryList(filters, QDir::Files, QDir::Name);
    return files;
}

void SharedFolderTransport::setSimulatedLatencyMs(int ms)
{
    m_latencyMs = ms;
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
            QString filePath = m_folderPath + QStringLiteral("/") + df.filename;
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
