#include "syncengine/ConflictResolver.h"

#include <QDebug>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSync, "syncengine", QtWarningMsg)

namespace syncengine {

ConflictResolver::ConflictResolver(SyncableDatabase *db, uint64_t incomingHlc,
                                   const QString &incomingClientId)
    : m_db(db)
    , m_incomingHlc(incomingHlc)
    , m_incomingClientId(incomingClientId)
{
}

SyncableDatabase::ConflictHandler ConflictResolver::handler()
{
    return [this](int conflictType, const QString &tableName,
                  sqlite3_changeset_iter *iter) -> SyncableDatabase::ConflictAction {
        // Skip conflicts on internal sync tables
        if (tableName.startsWith(QStringLiteral("_sync_")))
            return SyncableDatabase::ConflictAction::Skip;

        switch (conflictType) {
        case SQLITE_CHANGESET_DATA:
        case SQLITE_CHANGESET_CONFLICT: {
            sqlite3_value *pkVal = nullptr;
            int rc = sqlite3changeset_conflict(iter, 0, &pkVal);
            int64_t rowid = 0;
            if (rc == SQLITE_OK && pkVal) {
                rowid = sqlite3_value_int64(pkVal);
            }

            uint64_t localHlc = m_db->getRowHlc(tableName, rowid);

            if (m_incomingHlc > localHlc) {
                m_db->updateRowHlc(tableName, rowid, m_incomingHlc, m_incomingClientId);
                return SyncableDatabase::ConflictAction::Replace;
            } else if (m_incomingHlc == localHlc) {
                if (m_incomingClientId > m_db->clientId()) {
                    m_db->updateRowHlc(tableName, rowid, m_incomingHlc, m_incomingClientId);
                    return SyncableDatabase::ConflictAction::Replace;
                }
                return SyncableDatabase::ConflictAction::Skip;
            } else {
                return SyncableDatabase::ConflictAction::Skip;
            }
        }

        case SQLITE_CHANGESET_NOTFOUND:
            return SyncableDatabase::ConflictAction::Skip;

        case SQLITE_CHANGESET_CONSTRAINT:
            qCWarning(lcSync) << "CONSTRAINT conflict on" << tableName;
            return SyncableDatabase::ConflictAction::Skip;

        case SQLITE_CHANGESET_FOREIGN_KEY:
            qCWarning(lcSync) << "FOREIGN_KEY conflict on" << tableName;
            return SyncableDatabase::ConflictAction::Skip;

        default:
            return SyncableDatabase::ConflictAction::Skip;
        }
    };
}

} // namespace syncengine
