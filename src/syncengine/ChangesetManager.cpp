#include "syncengine/ChangesetManager.h"

#include <QDebug>
#include <algorithm>

namespace syncengine {

ChangesetManager::ChangesetManager(SyncableDatabase *db,
                                   ITransport *transport,
                                   QObject *parent)
    : QObject(parent)
    , db(db)
    , transport(transport)
{
}

QString ChangesetManager::writeChangeset(const QByteArray &changeset, uint64_t hlc)
{
    if (changeset.isEmpty())
        return {};

    QString filename = buildFilename(db->clientId(), hlc, sequence++, schemaVer);

    if (!transport->writeChangeset(filename, changeset)) {
        qWarning() << "Failed to write changeset:" << filename;
        return {};
    }

    // Mark as applied locally (we produced it, no need to replay our own changes)
    db->markChangesetApplied(filename, hlc);

    return filename;
}

QList<ChangesetInfo> ChangesetManager::pendingChangesets()
{
    QStringList allFiles = transport->listChangesets();
    QList<ChangesetInfo> pending;

    for (const QString &file : allFiles) {
        if (db->isChangesetApplied(file))
            continue;

        ChangesetInfo info = parseFilename(file);
        if (info.filename.isEmpty())
            continue;

        pending.append(info);
    }

    // Sort by HLC, then by sequence within same client
    std::sort(pending.begin(), pending.end(),
              [](const ChangesetInfo &a, const ChangesetInfo &b) {
                  if (a.hlc != b.hlc)
                      return a.hlc < b.hlc;
                  if (a.clientId != b.clientId)
                      return a.clientId < b.clientId;
                  return a.sequence < b.sequence;
              });

    return pending;
}

ChangesetInfo ChangesetManager::parseFilename(const QString &filename)
{
    // Format: {clientId}_{hlc}_{sequence}_v{version}.changeset
    // Legacy:  {clientId}_{hlc}_{sequence}.changeset  (no version = 0)
    QString base = filename;
    if (base.endsWith(QStringLiteral(".changeset")))
        base.chop(10); // remove ".changeset"

    QStringList parts = base.split(QChar('_'));
    if (parts.size() < 3)
        return {};

    ChangesetInfo info;
    info.filename = filename;

    // Check if the last part is a version tag (starts with 'v')
    QString lastPart = parts.last();
    if (lastPart.startsWith(QChar('v'))) {
        info.schemaVersion = lastPart.mid(1).toInt();
        parts.removeLast();
    } else {
        info.schemaVersion = 0; // legacy format
    }

    if (parts.size() < 3)
        return {}; // invalid format -- return empty info

    // Take last two parts as sequence and hlc, rest is client ID
    info.sequence = parts.takeLast().toULongLong();
    info.hlc = parts.takeLast().toULongLong();
    info.clientId = parts.join(QChar('_'));

    return info;
}

QString ChangesetManager::buildFilename(const QString &clientId, uint64_t hlc,
                                        uint64_t sequence, int schemaVersion)
{
    return QStringLiteral("%1_%2_%3_v%4.changeset")
        .arg(clientId)
        .arg(hlc, 20, 10, QChar('0'))  // Zero-pad for lexicographic sorting
        .arg(sequence, 10, 10, QChar('0'))
        .arg(schemaVersion);
}

} // namespace syncengine
