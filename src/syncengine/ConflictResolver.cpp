#include "syncengine/ConflictResolver.h"

#include <QDebug>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSync, "syncengine")

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
            // Both sides modified/inserted the same row.
            // Use LWW: compare incoming changeset HLC vs the HLC stored
            // for the local version of this row.
            //
            // We extract the primary key from the conflict iterator to look
            // up the per-row HLC in _sync_row_hlc.

            // Get PK value (column 0) to find the row
            sqlite3_value *pkVal = nullptr;
            int rc = sqlite3changeset_conflict(iter, 0, &pkVal);
            int64_t rowid = 0;
            if (rc == SQLITE_OK && pkVal) {
                rowid = sqlite3_value_int64(pkVal);
            }

            uint64_t localHlc = m_db->getRowHlc(tableName, rowid);

            qCDebug(lcSync) << (conflictType == SQLITE_CHANGESET_DATA ? "DATA" : "CONFLICT")
                     << "conflict on" << tableName
                     << "row" << rowid
                     << "- incoming HLC:" << m_incomingHlc
                     << "local HLC:" << localHlc;

            if (m_incomingHlc > localHlc) {
                // Incoming is newer -- replace and update row HLC
                m_db->updateRowHlc(tableName, rowid, m_incomingHlc, m_incomingClientId);
                return SyncableDatabase::ConflictAction::Replace;
            } else if (m_incomingHlc == localHlc) {
                // Tiebreak by client ID for deterministic convergence
                if (m_incomingClientId > m_db->clientId()) {
                    m_db->updateRowHlc(tableName, rowid, m_incomingHlc, m_incomingClientId);
                    return SyncableDatabase::ConflictAction::Replace;
                }
                return SyncableDatabase::ConflictAction::Skip;
            } else {
                // Local is newer -- skip incoming
                return SyncableDatabase::ConflictAction::Skip;
            }
        }

        case SQLITE_CHANGESET_NOTFOUND:
            qCDebug(lcSync) << "NOTFOUND conflict on" << tableName;
            return SyncableDatabase::ConflictAction::Skip;

        case SQLITE_CHANGESET_CONSTRAINT:
            qCDebug(lcSync) << "CONSTRAINT conflict on" << tableName;
            return SyncableDatabase::ConflictAction::Skip;

        case SQLITE_CHANGESET_FOREIGN_KEY:
            qCDebug(lcSync) << "FOREIGN_KEY conflict on" << tableName;
            return SyncableDatabase::ConflictAction::Skip;

        default:
            return SyncableDatabase::ConflictAction::Skip;
        }
    };
}

} // namespace syncengine
