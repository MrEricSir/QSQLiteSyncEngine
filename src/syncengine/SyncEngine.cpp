#include "syncengine/SyncEngine.h"
#include "syncengine/SharedFolderTransport.h"

#include <QDebug>

namespace syncengine {

SyncEngine::SyncEngine(const QString &dbPath,
                       const QString &sharedFolderPath,
                       const QString &clientId,
                       QObject *parent)
    : QObject(parent)
    , db(std::make_unique<SyncableDatabase>(dbPath, clientId))
    , sharedTransport(std::make_unique<SharedFolderTransport>(sharedFolderPath, this))
    , changesetMgr(std::make_unique<ChangesetManager>(db.get(), sharedTransport.get(), this))
{
    connect(&syncTimer, &QTimer::timeout, this, [this]() {
        sync();
    });
}

SyncEngine::SyncEngine(const QString &dbPath,
                       std::unique_ptr<ITransport> transport,
                       const QString &clientId,
                       QObject *parent)
    : QObject(parent)
    , db(std::make_unique<SyncableDatabase>(dbPath, clientId))
    , sharedTransport(std::move(transport))
    , changesetMgr(std::make_unique<ChangesetManager>(db.get(), sharedTransport.get(), this))
{
    connect(&syncTimer, &QTimer::timeout, this, [this]() {
        sync();
    });
}

SyncEngine::SyncEngine(const QSqlDatabase &database,
                       const QString &sharedFolderPath,
                       const QString &clientId,
                       QObject *parent)
    : QObject(parent)
    , sharedTransport(std::make_unique<SharedFolderTransport>(sharedFolderPath, this))
    , autoCapture(true)
{
    // QSQLITE_SYNC uses our sqlite3 (with session extension) -- safe to share handle.
    // Plain QSQLITE uses Qt's bundled sqlite3 -- must open a parallel connection.
    if (database.driverName() == QStringLiteral("QSQLITE_SYNC")) {
        QVariant handle = database.driver()->handle();
        auto *sqliteHandle = *static_cast<sqlite3 **>(handle.data());
        db = std::make_unique<SyncableDatabase>(sqliteHandle, clientId);
    } else {
        db = std::make_unique<SyncableDatabase>(database.databaseName(), clientId);
        autoCapture = false;
    }
    changesetMgr = std::make_unique<ChangesetManager>(db.get(), sharedTransport.get(), this);
    connect(&syncTimer, &QTimer::timeout, this, [this]() {
        sync();
    });
}

SyncEngine::~SyncEngine()
{
    stop();
}

bool SyncEngine::start(int syncIntervalMs)
{
    if (running.loadRelaxed()) {
        return true;
    }

    SchemaLogCapture::install();

    if (!db->open()) {
        emit syncErrorOccurred(DatabaseError, QStringLiteral("Failed to open database"));
        return false;
    }

    running.storeRelaxed(1);

    if (autoCapture) {
        installCommitHook();
    }

    // On first start with a new shared folder, snapshot existing data so
    // other clients can pick it up.
    snapshotIfNeeded();

    // Do an initial sync
    sync();

    // Start periodic sync
    if (syncIntervalMs > 0) {
        syncTimer.start(syncIntervalMs);
    }

    return true;
}

void SyncEngine::stop()
{
    syncTimer.stop();

    if (autoCapture && db->handle()) {
        sqlite3_commit_hook(db->handle(), nullptr, nullptr);
    }

    running.storeRelaxed(0);
    db->close();
}

void SyncEngine::setSchemaVersion(int version)
{
    currentSchemaVersion = version;
    if (changesetMgr) {
        changesetMgr->setSchemaVersion(version);
    }
}

bool SyncEngine::beginWrite()
{
    inManualWrite.storeRelaxed(1);
    return db->beginSession();
}

QString SyncEngine::endWrite()
{
    inManualWrite.storeRelaxed(0);

    QByteArray changeset = db->endSession();
    if (changeset.isEmpty()) {
        if (autoCapture) {
            db->beginSession();
        }
        return {};
    }

    uint64_t hlc = db->clock().now();

    // Record row HLCs for locally modified rows so conflict resolution works
    recordRowHlcs(changeset, hlc);

    QString filename = changesetMgr->writeChangeset(changeset, hlc);

    if (filename.isEmpty()) {
        emit syncErrorOccurred(TransportError, QStringLiteral("Failed to write changeset"));
    }

    // Restart auto-capture session
    if (autoCapture) {
        db->beginSession();
    }

    return filename;
}

void SyncEngine::installCommitHook()
{
    if (!db->handle()) {
        return;
    }

    sqlite3_commit_hook(db->handle(), commitHookCallback, this);

    // Start the initial session so changes are captured from now on
    db->beginSession();
}

int SyncEngine::commitHookCallback(void *ctx)
{
    auto *engine = static_cast<SyncEngine *>(ctx);
    // Cannot do SQL inside the commit hook -- queue for the next event loop tick
    QMetaObject::invokeMethod(engine, "onTransactionCommitted", Qt::QueuedConnection);
    return 0; // allow the commit to proceed
}

void SyncEngine::onTransactionCommitted()
{
    if (!running.loadRelaxed() || inManualWrite.loadRelaxed()) {
        return;
    }

    QByteArray changeset = db->endSession();
    if (!changeset.isEmpty()) {
        uint64_t hlc = db->clock().now();
        recordRowHlcs(changeset, hlc);
        changesetMgr->writeChangeset(changeset, hlc);
    }

    // Restart session to capture next set of changes
    db->beginSession();
}

void SyncEngine::recordRowHlcs(const QByteArray &changeset, uint64_t hlc)
{
    sqlite3_changeset_iter *iter = nullptr;
    int rc = sqlite3changeset_start(&iter, changeset.size(),
                                     const_cast<void *>(static_cast<const void *>(changeset.data())));
    if (rc != SQLITE_OK) {
        if (iter) {
            sqlite3changeset_finalize(iter);
        }
        return;
    }

    while (sqlite3changeset_next(iter) == SQLITE_ROW) {
        const char *tableName = nullptr;
        int nCols = 0;
        int op = 0;
        int bIndirect = 0;
        sqlite3changeset_op(iter, &tableName, &nCols, &op, &bIndirect);

        if (!tableName) {
            continue;
        }

        QString table = QString::fromUtf8(tableName);
        if (table.startsWith(QStringLiteral("_sync_"))) {
            continue;
        }

        // Get PK value (column 0) to record the row HLC
        sqlite3_value *pkVal = nullptr;
        if (op == SQLITE_INSERT) {
            sqlite3changeset_new(iter, 0, &pkVal);
        } else {
            // UPDATE or DELETE -- old values contain the PK
            sqlite3changeset_old(iter, 0, &pkVal);
        }

        if (pkVal) {
            int64_t rowid = sqlite3_value_int64(pkVal);
            db->updateRowHlc(table, rowid, hlc, db->clientId());
        }
    }
    sqlite3changeset_finalize(iter);
}

QStringList SyncEngine::discoverUserTables()
{
    QStringList tables;
    if (!db->handle()) {
        return tables;
    }

    // Collect virtual table names so we can skip their shadow tables below.
    QStringList virtualTableNames;
    sqlite3_stmt *vtStmt = nullptr;
    sqlite3_prepare_v2(db->handle(),
        "SELECT name FROM sqlite_master WHERE sql LIKE 'CREATE VIRTUAL TABLE%'",
        -1, &vtStmt, nullptr);
    while (sqlite3_step(vtStmt) == SQLITE_ROW) {
        virtualTableNames.append(QString::fromUtf8(
            reinterpret_cast<const char *>(sqlite3_column_text(vtStmt, 0))));
    }
    sqlite3_finalize(vtStmt);

    // Real user tables, excluding sync metadata and SQLite internals.
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(db->handle(),
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND sql LIKE 'CREATE TABLE%' "
        "AND name NOT LIKE '_sync_%' "
        "AND name NOT LIKE 'sqlite_%'",
        -1, &stmt, nullptr);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QString name = QString::fromUtf8(
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)));

        // Skip shadow tables (e.g., MyFTS_content, MyFTS_data).
        bool isShadow = false;
        for (const QString &vt : virtualTableNames) {
            if (name.startsWith(vt + QStringLiteral("_"))) {
                isShadow = true;
                break;
            }
        }
        if (!isShadow) {
            tables.append(name);
        }
    }
    sqlite3_finalize(stmt);

    return tables;
}

void SyncEngine::snapshotIfNeeded()
{
    // Check if this client has ever written a changeset to the shared folder.
    const QStringList allFiles = sharedTransport->listChangesets();
    const QString myId = db->clientId();
    for (const QString &file : allFiles) {
        ChangesetInfo info = ChangesetManager::parseFilename(file);
        if (info.clientId == myId) {
            return; // Already have changesets - not a first start.
        }
    }

    // First start with this shared folder. Snapshot all user tables.
    QStringList tables = discoverUserTables();
    if (tables.isEmpty()) {
        return;
    }

    QByteArray changeset = db->generateSnapshot(tables);
    if (changeset.isEmpty()) {
        return;
    }

    uint64_t hlc = db->clock().now();
    recordRowHlcs(changeset, hlc);
    QString filename = changesetMgr->writeChangeset(changeset, hlc);
    if (filename.isEmpty()) {
        emit syncErrorOccurred(TransportError,
            QStringLiteral("Failed to write initial snapshot"));
    }
}

bool SyncEngine::applyOneChangeset(const ChangesetInfo &info, int &applied,
                                   QList<SchemaLogCapture::Warning> &outWarnings)
{
    // Reject changesets from a newer schema version
    if (info.schemaVersion > currentSchemaVersion) {
        emit syncErrorOccurred(VersionMismatch, QStringLiteral(
            "Changeset %1 requires schema version %2 but this client is on "
            "version %3. Please upgrade to the latest version of the application.")
            .arg(info.filename)
            .arg(info.schemaVersion)
            .arg(currentSchemaVersion));
        return true; // not retryable -- don't defer
    }

    QByteArray data = sharedTransport->readChangeset(info.filename);
    if (data.isEmpty()) {
        qWarning() << "Empty changeset file:" << info.filename;
        return true; // not retryable
    }

    db->clock().receive(info.hlc);

    ConflictResolver resolver(db.get(), info.hlc, info.clientId);

    QList<SchemaLogCapture::Warning> warnings;
    if (db->applyChangeset(data, resolver.handler(), &warnings)) {
        db->markChangesetApplied(info.filename, info.hlc);
        emit changesetApplied(info.filename);
        ++applied;
        return true; // success
    }

    // Schema mismatch is retryable (a later changeset may create the table)
    if (!warnings.isEmpty()) {
        outWarnings = std::move(warnings);
        return false; // defer for retry
    }

    // Other errors are not retryable
    emit syncErrorOccurred(ChangesetError,
        QStringLiteral("Failed to apply changeset: %1").arg(info.filename));
    return true; // not retryable
}

void SyncEngine::emitApplyError(const ChangesetInfo &info,
                                const QList<SchemaLogCapture::Warning> &warnings)
{
    if (!warnings.isEmpty()) {
        for (const auto &w : warnings) {
            emit syncErrorOccurred(SchemaMismatch,
                QStringLiteral("Schema mismatch applying %1: %2")
                    .arg(info.filename, w.message));
        }
    } else {
        emit syncErrorOccurred(ChangesetError,
            QStringLiteral("Failed to apply changeset: %1").arg(info.filename));
    }
}

int SyncEngine::sync()
{
    struct DeferredItem {
        ChangesetInfo info;
        QList<SchemaLogCapture::Warning> warnings;
    };

    QList<ChangesetInfo> pending = changesetMgr->pendingChangesets();

    int applied = 0;
    QList<DeferredItem> deferred;

    // First pass: apply all pending changesets. Changesets that fail due to
    // schema mismatch (e.g., table doesn't exist yet) are deferred for a
    // retry, because a later changeset in this batch may create the table.
    for (const ChangesetInfo &info : pending) {
        QList<SchemaLogCapture::Warning> warnings;
        if (!applyOneChangeset(info, applied, warnings))
            deferred.append({info, std::move(warnings)});
    }

    // Retry pass: if any changesets were deferred and at least one changeset
    // succeeded (possibly creating the missing table), try the deferred ones
    // again. Only emit errors for changesets that fail on the retry.
    if (!deferred.isEmpty() && applied > 0) {
        QList<DeferredItem> stillFailing;
        for (auto &item : deferred) {
            QList<SchemaLogCapture::Warning> warnings;
            if (!applyOneChangeset(item.info, applied, warnings))
                stillFailing.append({item.info, std::move(warnings)});
        }
        deferred = std::move(stillFailing);
    }

    // Emit errors for changesets that failed on all attempts
    for (const auto &item : deferred)
        emitApplyError(item.info, item.warnings);

    if (applied > 0 || !pending.isEmpty())
        emit syncCompleted(applied);

    return applied;
}

} // namespace syncengine
