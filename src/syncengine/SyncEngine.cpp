#include "syncengine/SyncEngine.h"

#include <QDebug>

namespace syncengine {

SyncEngine::SyncEngine(const QString &dbPath,
                       const QString &sharedFolderPath,
                       const QString &clientId,
                       QObject *parent)
    : QObject(parent)
    , m_db(std::make_unique<SyncableDatabase>(dbPath, clientId))
    , m_transport(std::make_unique<SharedFolderTransport>(sharedFolderPath, this))
    , m_changesetMgr(std::make_unique<ChangesetManager>(m_db.get(), m_transport.get(), this))
{
    connect(&m_syncTimer, &QTimer::timeout, this, [this]() {
        sync();
    });
}

SyncEngine::SyncEngine(const QSqlDatabase &database,
                       const QString &sharedFolderPath,
                       const QString &clientId,
                       QObject *parent)
    : QObject(parent)
    , m_transport(std::make_unique<SharedFolderTransport>(sharedFolderPath, this))
    , m_autoCapture(true)
{
    // QSQLITE_SYNC uses our sqlite3 (with session extension) -- safe to share handle.
    // Plain QSQLITE uses Qt's bundled sqlite3 -- must open a parallel connection.
    if (database.driverName() == QStringLiteral("QSQLITE_SYNC")) {
        QVariant handle = database.driver()->handle();
        auto *sqliteHandle = *static_cast<sqlite3 **>(handle.data());
        m_db = std::make_unique<SyncableDatabase>(sqliteHandle, clientId);
    } else {
        m_db = std::make_unique<SyncableDatabase>(database.databaseName(), clientId);
        m_autoCapture = false;
    }
    m_changesetMgr = std::make_unique<ChangesetManager>(m_db.get(), m_transport.get(), this);
    connect(&m_syncTimer, &QTimer::timeout, this, [this]() {
        sync();
    });
}

SyncEngine::~SyncEngine()
{
    stop();
}

bool SyncEngine::start(int syncIntervalMs)
{
    if (m_running.loadRelaxed())
        return true;

    SchemaLogCapture::install();

    if (!m_db->open()) {
        emit syncErrorOccurred(DatabaseError, QStringLiteral("Failed to open database"));
        return false;
    }

    m_running.storeRelaxed(1);

    if (m_autoCapture)
        installCommitHook();

    // Do an initial sync
    sync();

    // Start periodic sync
    if (syncIntervalMs > 0)
        m_syncTimer.start(syncIntervalMs);

    return true;
}

void SyncEngine::stop()
{
    m_syncTimer.stop();

    if (m_autoCapture && m_db->handle())
        sqlite3_commit_hook(m_db->handle(), nullptr, nullptr);

    m_running.storeRelaxed(0);
    m_db->close();
}

void SyncEngine::setSchemaVersion(int version)
{
    m_schemaVersion = version;
    if (m_changesetMgr)
        m_changesetMgr->setSchemaVersion(version);
}

bool SyncEngine::beginWrite()
{
    m_inManualWrite.storeRelaxed(1);
    return m_db->beginSession();
}

QString SyncEngine::endWrite()
{
    m_inManualWrite.storeRelaxed(0);

    QByteArray changeset = m_db->endSession();
    if (changeset.isEmpty()) {
        if (m_autoCapture)
            m_db->beginSession();
        return {};
    }

    uint64_t hlc = m_db->clock().now();

    // Record row HLCs for locally modified rows so conflict resolution works
    recordRowHlcs(changeset, hlc);

    QString filename = m_changesetMgr->writeChangeset(changeset, hlc);

    if (filename.isEmpty()) {
        emit syncErrorOccurred(TransportError, QStringLiteral("Failed to write changeset"));
    }

    // Restart auto-capture session
    if (m_autoCapture)
        m_db->beginSession();

    return filename;
}

void SyncEngine::installCommitHook()
{
    if (!m_db->handle())
        return;

    sqlite3_commit_hook(m_db->handle(), commitHookCallback, this);

    // Start the initial session so changes are captured from now on
    m_db->beginSession();
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
    if (!m_running.loadRelaxed() || m_inManualWrite.loadRelaxed())
        return;

    QByteArray changeset = m_db->endSession();
    if (!changeset.isEmpty()) {
        uint64_t hlc = m_db->clock().now();
        recordRowHlcs(changeset, hlc);
        m_changesetMgr->writeChangeset(changeset, hlc);
    }

    // Restart session to capture next set of changes
    m_db->beginSession();
}

void SyncEngine::recordRowHlcs(const QByteArray &changeset, uint64_t hlc)
{
    sqlite3_changeset_iter *iter = nullptr;
    int rc = sqlite3changeset_start(&iter, changeset.size(),
                                     const_cast<void *>(static_cast<const void *>(changeset.data())));
    if (rc != SQLITE_OK) {
        if (iter) sqlite3changeset_finalize(iter);
        return;
    }

    while (sqlite3changeset_next(iter) == SQLITE_ROW) {
        const char *tableName = nullptr;
        int nCols = 0;
        int op = 0;
        int bIndirect = 0;
        sqlite3changeset_op(iter, &tableName, &nCols, &op, &bIndirect);

        if (!tableName)
            continue;

        QString table = QString::fromUtf8(tableName);
        if (table.startsWith(QStringLiteral("_sync_")))
            continue;

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
            m_db->updateRowHlc(table, rowid, hlc, m_db->clientId());
        }
    }
    sqlite3changeset_finalize(iter);
}

int SyncEngine::sync()
{
    QList<ChangesetInfo> pending = m_changesetMgr->pendingChangesets();

    int applied = 0;
    for (const ChangesetInfo &info : pending) {
        // Reject changesets from a newer schema version
        if (info.schemaVersion > m_schemaVersion) {
            emit syncErrorOccurred(VersionMismatch, QStringLiteral(
                "Changeset %1 requires schema version %2 but this client is on "
                "version %3. Please upgrade to the latest version of the application.")
                .arg(info.filename)
                .arg(info.schemaVersion)
                .arg(m_schemaVersion));
            continue;
        }

        QByteArray data = m_transport->readChangeset(info.filename);
        if (data.isEmpty()) {
            qWarning() << "Empty changeset file:" << info.filename;
            continue;
        }

        // Update our clock with the remote HLC
        m_db->clock().receive(info.hlc);

        // Create conflict resolver for this changeset
        ConflictResolver resolver(m_db.get(), info.hlc, info.clientId);

        if (m_db->applyChangeset(data, resolver.handler())) {
            m_db->markChangesetApplied(info.filename, info.hlc);
            emit changesetApplied(info.filename);
            ++applied;
        } else {
            const auto &warnings = m_db->lastSchemaWarnings();
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
    }

    if (applied > 0 || !pending.isEmpty())
        emit syncCompleted(applied);

    return applied;
}

} // namespace syncengine
