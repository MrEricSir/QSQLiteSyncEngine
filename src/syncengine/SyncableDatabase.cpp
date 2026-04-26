#include "syncengine/SyncableDatabase.h"

#include <QDebug>

namespace syncengine {

SyncableDatabase::SyncableDatabase(const QString &dbPath, const QString &clientId)
    : m_dbPath(dbPath)
    , m_clientId(clientId)
    , m_ownsHandle(true)
{
}

SyncableDatabase::SyncableDatabase(sqlite3 *existingHandle, const QString &clientId)
    : m_clientId(clientId)
    , m_db(existingHandle)
    , m_ownsHandle(false)
{
}

SyncableDatabase::~SyncableDatabase()
{
    close();
}

bool SyncableDatabase::open()
{
    if (m_db && !m_ownsHandle) {
        // Borrowed handle -- already open, just init metadata
        return initSyncMetadata();
    }

    int rc = sqlite3_open(m_dbPath.toUtf8().constData(), &m_db);
    if (rc != SQLITE_OK) {
        qWarning() << "Failed to open database:" << sqlite3_errmsg(m_db);
        return false;
    }

    // Enable WAL mode for better concurrent access
    exec(QStringLiteral("PRAGMA journal_mode=WAL"));

    return initSyncMetadata();
}

void SyncableDatabase::close()
{
    if (m_session) {
        sqlite3session_delete(m_session);
        m_session = nullptr;
    }
    if (m_db && m_ownsHandle) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool SyncableDatabase::beginSession()
{
    if (!m_db)
        return false;

    if (m_session) {
        // Already have an active session
        sqlite3session_delete(m_session);
        m_session = nullptr;
    }

    int rc = sqlite3session_create(m_db, "main", &m_session);
    if (rc != SQLITE_OK) {
        qWarning() << "Failed to create session:" << sqlite3_errmsg(m_db);
        return false;
    }

    // Track all tables
    rc = sqlite3session_attach(m_session, nullptr);
    if (rc != SQLITE_OK) {
        qWarning() << "Failed to attach session:" << sqlite3_errmsg(m_db);
        sqlite3session_delete(m_session);
        m_session = nullptr;
        return false;
    }

    return true;
}

QByteArray SyncableDatabase::endSession()
{
    if (!m_session)
        return {};

    int nChangeset = 0;
    void *pChangeset = nullptr;

    int rc = sqlite3session_changeset(m_session, &nChangeset, &pChangeset);
    sqlite3session_delete(m_session);
    m_session = nullptr;

    if (rc != SQLITE_OK) {
        qWarning() << "Failed to get changeset:" << sqlite3_errmsg(m_db);
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

bool SyncableDatabase::applyChangeset(const QByteArray &changeset, ConflictHandler handler)
{
    if (!m_db || changeset.isEmpty())
        return false;

    m_schemaWarnings.clear();
    ConflictContext ctx{handler};
    SchemaLogCapture::Guard logGuard;

    int rc = sqlite3changeset_apply(
        m_db,
        changeset.size(),
        const_cast<void *>(static_cast<const void *>(changeset.data())),
        nullptr,       // filter callback
        conflictCallback,
        &ctx);

    m_schemaWarnings = logGuard.warnings();

    if (rc != SQLITE_OK) {
        qWarning() << "Failed to apply changeset:" << sqlite3_errmsg(m_db);
        return false;
    }

    if (!m_schemaWarnings.isEmpty()) {
        qWarning() << "Schema mismatch -- changeset partially skipped:";
        for (const auto &w : m_schemaWarnings)
            qWarning() << "  " << w.message;
        return false;
    }

    return true;
}

bool SyncableDatabase::exec(const QString &sql)
{
    if (!m_db)
        return false;

    char *errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql.toUtf8().constData(), nullptr, nullptr, &errMsg);
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
    if (!m_db)
        return results;

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql.toUtf8().constData(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        qWarning() << "Query prepare error:" << sqlite3_errmsg(m_db);
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
        sqlite3_prepare_v2(m_db,
            "INSERT OR IGNORE INTO _sync_client (client_id) VALUES (?)", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, m_clientId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }

    return ok;
}

bool SyncableDatabase::markChangesetApplied(const QString &changesetId, uint64_t hlc)
{
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "INSERT OR IGNORE INTO _sync_applied (changeset_id, client_id, hlc) VALUES (?, ?, ?)",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    QByteArray csId = changesetId.toUtf8();
    QByteArray cId = m_clientId.toUtf8();
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
    int rc = sqlite3_prepare_v2(m_db,
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
    int rc = sqlite3_prepare_v2(m_db,
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
    int rc = sqlite3_prepare_v2(m_db,
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
