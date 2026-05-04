#include "syncengine/SyncableDatabase.h"

#include <QDebug>

namespace syncengine {

SyncableDatabase::SyncableDatabase(const QString &dbPath, const QString &clientId)
    : dbPath(dbPath)
    , clientIdentifier(clientId)
    , ownsHandle(true)
{
}

SyncableDatabase::SyncableDatabase(sqlite3 *existingHandle, const QString &clientId)
    : clientIdentifier(clientId)
    , db(existingHandle)
    , ownsHandle(false)
{
}

SyncableDatabase::~SyncableDatabase()
{
    close();
}

bool SyncableDatabase::open()
{
    if (db && !ownsHandle) {
        // Borrowed handle -- already open, just init metadata
        return initSyncMetadata();
    }

    int rc = sqlite3_open(dbPath.toUtf8().constData(), &db);
    if (rc != SQLITE_OK) {
        qWarning() << "Failed to open database:" << sqlite3_errmsg(db);
        return false;
    }

    // Enable WAL mode for better concurrent access
    exec(QStringLiteral("PRAGMA journal_mode=WAL"));

    return initSyncMetadata();
}

void SyncableDatabase::close()
{
    if (session) {
        sqlite3session_delete(session);
        session = nullptr;
    }
    if (db && ownsHandle) {
        sqlite3_close(db);
        db = nullptr;
    }
}

bool SyncableDatabase::beginSession()
{
    if (!db) {
        return false;
    }

    if (session) {
        // Already have an active session
        sqlite3session_delete(session);
        session = nullptr;
    }

    int rc = sqlite3session_create(db, "main", &session);
    if (rc != SQLITE_OK) {
        qWarning() << "Failed to create session:" << sqlite3_errmsg(db);
        return false;
    }

    // Track all tables
    rc = sqlite3session_attach(session, nullptr);
    if (rc != SQLITE_OK) {
        qWarning() << "Failed to attach session:" << sqlite3_errmsg(db);
        sqlite3session_delete(session);
        session = nullptr;
        return false;
    }

    return true;
}

QByteArray SyncableDatabase::endSession()
{
    if (!session) {
        return {};
    }

    int nChangeset = 0;
    void *pChangeset = nullptr;

    int rc = sqlite3session_changeset(session, &nChangeset, &pChangeset);
    sqlite3session_delete(session);
    session = nullptr;

    if (rc != SQLITE_OK) {
        qWarning() << "Failed to get changeset:" << sqlite3_errmsg(db);
        sqlite3_free(pChangeset);
        return {};
    }

    QByteArray result(static_cast<const char *>(pChangeset), nChangeset);
    sqlite3_free(pChangeset);
    return result;
}

struct ConflictContext {
    SyncableDatabase::ConflictHandler handler;
};

static int conflictCallback(void *pCtx, int eConflict, sqlite3_changeset_iter *pIter)
{
    // REPLACE is only valid for DATA and CONFLICT types.
    // For CONSTRAINT/FOREIGN_KEY, we must use OMIT or ABORT.
    if (eConflict == SQLITE_CHANGESET_CONSTRAINT
        || eConflict == SQLITE_CHANGESET_FOREIGN_KEY) {
        return SQLITE_CHANGESET_OMIT;
    }

    auto *ctx = static_cast<ConflictContext *>(pCtx);
    if (!ctx || !ctx->handler) {
        if (eConflict == SQLITE_CHANGESET_DATA
            || eConflict == SQLITE_CHANGESET_CONFLICT) {
            return SQLITE_CHANGESET_REPLACE;
        }
        return SQLITE_CHANGESET_OMIT;
    }

    const char *tableName = nullptr;
    int nCols = 0;
    int op = 0;
    int bIndirect = 0;
    sqlite3changeset_op(pIter, &tableName, &nCols, &op, &bIndirect);

    auto action = ctx->handler(eConflict,
                               tableName ? QString::fromUtf8(tableName) : QString(),
                               pIter);
    switch (action) {
    case SyncableDatabase::ConflictAction::Replace:
        // Only valid for DATA and CONFLICT
        if (eConflict == SQLITE_CHANGESET_DATA
            || eConflict == SQLITE_CHANGESET_CONFLICT) {
            return SQLITE_CHANGESET_REPLACE;
        }
        return SQLITE_CHANGESET_OMIT;
    case SyncableDatabase::ConflictAction::Skip:
        return SQLITE_CHANGESET_OMIT;
    case SyncableDatabase::ConflictAction::Abort:
        return SQLITE_CHANGESET_ABORT;
    }
    return SQLITE_CHANGESET_OMIT;
}

bool SyncableDatabase::applyChangeset(const QByteArray &changeset, ConflictHandler handler,
                                      QList<SchemaLogCapture::Warning> *outWarnings)
{
    if (!db || changeset.isEmpty()) {
        return false;
    }

    ConflictContext ctx{handler};
    SchemaLogCapture::Guard logGuard;

    int rc = sqlite3changeset_apply(
        db,
        changeset.size(),
        const_cast<void *>(static_cast<const void *>(changeset.data())),
        nullptr,       // filter callback
        conflictCallback,
        &ctx);

    if (rc != SQLITE_OK) {
        qWarning() << "Failed to apply changeset:" << sqlite3_errmsg(db);
        return false;
    }

    const QList<SchemaLogCapture::Warning> captured = logGuard.warnings();
    if (!captured.isEmpty()) {
        if (outWarnings)
            *outWarnings = captured;
        qWarning() << "Schema mismatch -- changeset partially skipped:";
        for (const auto &w : captured)
            qWarning() << "  " << w.message;
        return false;
    }

    return true;
}

QByteArray SyncableDatabase::generateSnapshot(const QStringList &tables)
{
    if (!db || tables.isEmpty()) {
        return {};
    }

    // Attach an empty in-memory database to diff against.
    if (sqlite3_exec(db, "ATTACH DATABASE ':memory:' AS _snapshot_empty",
                     nullptr, nullptr, nullptr) != SQLITE_OK) {
        qWarning() << "generateSnapshot: failed to attach empty database";
        return {};
    }

    // Separate session so we don't disturb the auto-capture session.
    sqlite3_session *snapSession = nullptr;
    if (sqlite3session_create(db, "main", &snapSession) != SQLITE_OK) {
        sqlite3_exec(db, "DETACH DATABASE _snapshot_empty", nullptr, nullptr, nullptr);
        return {};
    }
    sqlite3session_attach(snapSession, nullptr);

    for (const QString &table : tables) {
        sqlite3_stmt *stmt = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT sql FROM sqlite_master WHERE type='table' AND name=?",
            -1, &stmt, nullptr);
        QByteArray nameUtf8 = table.toUtf8();
        sqlite3_bind_text(stmt, 1, nameUtf8.constData(), nameUtf8.size(), SQLITE_STATIC);

        QString createSql;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            createSql = QString::fromUtf8(
                reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)));
        }
        sqlite3_finalize(stmt);

        if (createSql.isEmpty()) {
            continue;
        }

        // Mirror the table structure in the empty db.
        int parenPos = createSql.indexOf('(');
        if (parenPos < 0) {
            continue;
        }
        QString createInEmpty = QString("CREATE TABLE _snapshot_empty.\"%1\" %2")
            .arg(table, createSql.mid(parenPos));
        sqlite3_exec(db, createInEmpty.toUtf8().constData(), nullptr, nullptr, nullptr);

        // Diff main vs empty -- produces INSERT records for every row.
        char *errMsg = nullptr;
        sqlite3session_diff(snapSession, "_snapshot_empty", nameUtf8.constData(), &errMsg);
        if (errMsg) {
            qWarning() << "generateSnapshot: diff error for" << table << ":" << errMsg;
            sqlite3_free(errMsg);
        }
    }

    // Extract the changeset.
    int nChangeset = 0;
    void *pChangeset = nullptr;
    int rc = sqlite3session_changeset(snapSession, &nChangeset, &pChangeset);

    sqlite3session_delete(snapSession);
    sqlite3_exec(db, "DETACH DATABASE _snapshot_empty", nullptr, nullptr, nullptr);

    if (rc != SQLITE_OK) {
        sqlite3_free(pChangeset);
        return {};
    }

    QByteArray result(static_cast<const char *>(pChangeset), nChangeset);
    sqlite3_free(pChangeset);
    return result;
}

bool SyncableDatabase::exec(const QString &sql)
{
    if (!db) {
        return false;
    }

    char *errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.toUtf8().constData(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        qWarning() << "SQL error:" << errMsg;
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

QList<QStringList> SyncableDatabase::query(const QString &sql)
{
    QList<QStringList> results;
    if (!db) {
        return results;
    }

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        qWarning() << "Query prepare error:" << sqlite3_errmsg(db);
        return results;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QStringList row;
        int cols = sqlite3_column_count(stmt);
        for (int i = 0; i < cols; ++i) {
            const char *text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));
            row.append(text ? QString::fromUtf8(text) : QString());
        }
        results.append(row);
    }

    sqlite3_finalize(stmt);
    return results;
}

bool SyncableDatabase::initSyncMetadata()
{
    bool ok = true;
    ok = ok && exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS _sync_applied ("
        "  changeset_id TEXT PRIMARY KEY,"
        "  client_id TEXT NOT NULL,"
        "  hlc INTEGER NOT NULL,"
        "  applied_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ")"));

    ok = ok && exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS _sync_row_hlc ("
        "  table_name TEXT NOT NULL,"
        "  rowid INTEGER NOT NULL,"
        "  hlc INTEGER NOT NULL,"
        "  client_id TEXT NOT NULL,"
        "  PRIMARY KEY (table_name, rowid)"
        ")"));

    ok = ok && exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS _sync_client ("
        "  client_id TEXT PRIMARY KEY,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ")"));

    // Insert our client ID if not already present
    if (ok) {
        sqlite3_stmt *stmt = nullptr;
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO _sync_client (client_id) VALUES (?)", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, clientIdentifier.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }

    return ok;
}

bool SyncableDatabase::markChangesetApplied(const QString &changesetId, uint64_t hlc)
{
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO _sync_applied (changeset_id, client_id, hlc) VALUES (?, ?, ?)",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    QByteArray csId = changesetId.toUtf8();
    QByteArray cId = clientIdentifier.toUtf8();
    sqlite3_bind_text(stmt, 1, csId.constData(), csId.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, cId.constData(), cId.size(), SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(hlc));

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool SyncableDatabase::isChangesetApplied(const QString &changesetId)
{
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "SELECT 1 FROM _sync_applied WHERE changeset_id = ?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    QByteArray csId = changesetId.toUtf8();
    sqlite3_bind_text(stmt, 1, csId.constData(), csId.size(), SQLITE_STATIC);

    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

bool SyncableDatabase::updateRowHlc(const QString &tableName, int64_t rowid,
                                     uint64_t hlc, const QString &clientId)
{
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO _sync_row_hlc (table_name, rowid, hlc, client_id) "
        "VALUES (?, ?, ?, ?)", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    QByteArray tn = tableName.toUtf8();
    QByteArray cId = clientId.toUtf8();
    sqlite3_bind_text(stmt, 1, tn.constData(), tn.size(), SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, rowid);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(hlc));
    sqlite3_bind_text(stmt, 4, cId.constData(), cId.size(), SQLITE_STATIC);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

uint64_t SyncableDatabase::getRowHlc(const QString &tableName, int64_t rowid)
{
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "SELECT hlc FROM _sync_row_hlc WHERE table_name = ? AND rowid = ?",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    QByteArray tn = tableName.toUtf8();
    sqlite3_bind_text(stmt, 1, tn.constData(), tn.size(), SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, rowid);

    uint64_t result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));

    sqlite3_finalize(stmt);
    return result;
}

} // namespace syncengine
