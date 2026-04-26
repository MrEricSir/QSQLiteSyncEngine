// MIT License
//
// Copyright (c) 2026 Eric Gregory <mrericsir@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <QObject>
#include <QTimer>
#include <QAtomicInt>
#include <QSqlDatabase>
#include <QSqlDriver>
#include <memory>

#include "syncengine/SyncableDatabase.h"
#include "syncengine/SharedFolderTransport.h"
#include "syncengine/ChangesetManager.h"
#include "syncengine/ConflictResolver.h"

namespace syncengine {

/*!
    \module QSQLiteSyncEngine
    \title QSQLiteSyncEngine C++ Classes
    \brief Multi-writer SQLite sync engine for Qt applications.
*/

/*!
    \class SyncEngine
    \inmodule QSQLiteSyncEngine
    \brief Orchestrates multi-writer SQLite sync over a shared folder.

    SyncEngine is the main entry point for the sync library. It manages a local
    SQLite database, captures changes using the SQLite Session Extension, writes
    binary changesets to a shared folder, and applies incoming changesets from
    other clients.

    \section1 Recommended Usage (QSQLITE_SYNC driver)

    The simplest way to use the engine is with the QSQLITE_SYNC driver plugin.
    All writes made through QSqlQuery are automatically captured and synced --
    no special API calls are needed beyond start(), sync(), and setSchemaVersion().

    \code
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE_SYNC");
    db.setDatabaseName("myapp.db");
    db.open();

    SyncEngine engine(db, "/shared/folder", "client-id");
    engine.setSchemaVersion(1);
    engine.start();

    // Use QSqlQuery as normal -- writes are auto-captured
    QSqlQuery q(db);
    q.exec("INSERT INTO items (name) VALUES ('hello')");

    // Pull remote changes
    engine.sync();
    \endcode

    \section1 Fallback Usage (without QSQLITE_SYNC driver)

    If the driver plugin is not available, use the path-based constructor and
    wrap writes in beginWrite() / endWrite():

    \code
    SyncEngine engine("/path/to/local.db", "/shared/folder", "client-id");
    engine.start();
    engine.beginWrite();
    engine.database()->exec("INSERT INTO items (name) VALUES ('hello')");
    engine.endWrite();
    \endcode

    \section1 Schema Versioning

    Each changeset is stamped with a schema version. Clients reject changesets
    from a newer version and emit syncError() with an actionable message. Call
    setSchemaVersion() to set the version before or after start().

    \section1 Conflict Resolution

    Conflicts are resolved using last-write-wins (LWW) based on hybrid logical
    clock timestamps. When two clients modify the same row, the change with the
    higher HLC wins. Equal HLCs are broken by client ID for deterministic
    convergence.

    \sa SyncableDatabase, SharedFolderTransport
*/
class SyncEngine : public QObject {
    Q_OBJECT
    Q_ENUMS(SyncError)
public:
    /*!
        \enum SyncEngine::SyncError
        Error types emitted with the syncErrorOccurred() signal.

        \value NoError             No error.
        \value DatabaseError       Failed to open or access the local database.
        \value TransportError      Failed to read or write changeset files on the shared folder.
        \value VersionMismatch     A remote changeset requires a newer schema version than this client supports.
        \value SchemaMismatch      A remote changeset was skipped because the local table schema is incompatible (column count, missing table, or primary key mismatch).
        \value ChangesetError      A remote changeset could not be applied (general apply failure).
    */
    enum SyncError {
        NoError,
        DatabaseError,
        TransportError,
        VersionMismatch,
        SchemaMismatch,
        ChangesetError
    };

    /*!
        Constructs a SyncEngine from a QSqlDatabase opened with the
        \c QSQLITE_SYNC driver. This is the recommended constructor.

        All writes made through QSqlQuery on \a database are automatically
        captured as changesets and synced to \a sharedFolderPath. No manual
        beginWrite() / endWrite() calls are needed.

        If \a database uses the plain \c QSQLITE driver instead, the engine
        falls back to a parallel connection and auto-capture is disabled.
        In that case, use beginWrite() / endWrite() to track changes.
    */
    explicit SyncEngine(const QSqlDatabase &database,
                        const QString &sharedFolderPath,
                        const QString &clientId,
                        QObject *parent = nullptr);

    /*!
        \internal
        Constructs a SyncEngine with its own database connection at \a dbPath.
        Used when no QSqlDatabase is available. Requires beginWrite() /
        endWrite() to track changes.
    */
    explicit SyncEngine(const QString &dbPath,
                        const QString &sharedFolderPath,
                        const QString &clientId,
                        QObject *parent = nullptr);

    ~SyncEngine();

    /*!
        Opens the database and begins syncing. If \a syncIntervalMs is greater
        than zero, a periodic sync timer is started. Pass 0 to disable
        automatic syncing. Returns true on success.
    */
    bool start(int syncIntervalMs = 1000);

    /*!
        Stops the sync timer and closes the database.
    */
    void stop();

    /*!
        Manually triggers a sync cycle: pulls remote changesets from the shared
        folder and applies them. Returns the number of changesets applied.
    */
    int sync();

    /*!
        Sets the schema version for this client. Produced changesets are stamped
        with this version. Incoming changesets from a newer version are rejected
        with a syncError(). The application is responsible for managing the
        version number.
    */
    void setSchemaVersion(int version);

    /*!
        Returns the current schema version.
    */
    int schemaVersion() const { return m_schemaVersion; }

    /*!
        Returns true if the engine is currently running.
    */
    bool isRunning() const { return m_running.loadRelaxed() != 0; }

    /*!
        \internal
        Returns a pointer to the underlying SyncableDatabase. For advanced use
        or when the QSQLITE_SYNC driver is not available. Prefer QSqlQuery on
        the QSQLITE_SYNC QSqlDatabase instead.
    */
    SyncableDatabase *database() { return m_db.get(); }

    /*!
        \internal
        Begins a tracked write session. Not needed when using the QSQLITE_SYNC
        driver (auto-capture handles this). Only needed with the path-based
        constructor or plain QSQLITE.
    */
    bool beginWrite();

    /*!
        \internal
        Ends the tracked write session and writes the changeset.
        \sa beginWrite()
    */
    QString endWrite();

    /*!
        \internal
        Returns the SharedFolderTransport for testing (e.g., simulated latency).
    */
    SharedFolderTransport *transport() { return m_transport.get(); }

signals:
    /*!
        Emitted when a remote changeset has been successfully applied.
    */
    void changesetApplied(const QString &filename);

    /*!
        Emitted when a conflict was resolved during changeset application.
    */
    void conflictResolved(const QString &tableName, int conflictType);

    /*!
        Emitted when a sync error occurs. The \a error type identifies the
        category of problem, and \a message contains a human-readable
        description. Use \a error for programmatic handling (e.g., prompting
        the user to upgrade) and \a message for display or logging.
    */
    void syncErrorOccurred(SyncEngine::SyncError error, const QString &message);

    /*!
        Emitted after a sync cycle completes. \a appliedCount is the number
        of changesets that were successfully applied.
    */
    void syncCompleted(int appliedCount);

private:
    void recordRowHlcs(const QByteArray &changeset, uint64_t hlc);
    void installCommitHook();
    static int commitHookCallback(void *ctx);

    Q_INVOKABLE void onTransactionCommitted();

    std::unique_ptr<SyncableDatabase> m_db;
    std::unique_ptr<SharedFolderTransport> m_transport;
    std::unique_ptr<ChangesetManager> m_changesetMgr;
    QTimer m_syncTimer;
    QAtomicInt m_running{0};
    int m_schemaVersion = 0;
    bool m_autoCapture = false;
    QAtomicInt m_inManualWrite{0};
};

} // namespace syncengine
