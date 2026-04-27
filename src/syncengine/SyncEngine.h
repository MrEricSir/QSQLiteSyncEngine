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
    \brief SQLite sync engine for Qt applications.

    Syncs multiple database instances over a shared folder (Google Drive,
    Dropbox, local network drive), etc. for integration in your application.
*/

/*!
    \namespace syncengine
    \inmodule QSQLiteSyncEngine
    \brief Contains all classes for the QSQLiteSyncEngine library.
*/

/*!
    \class syncengine::SyncEngine
    \inmodule QSQLiteSyncEngine
    \brief Orchestrates SQLite sync over a shared folder.

    SyncEngine is the main entry point for the sync library. Clients write to
    their own local SQLite database. When changes are made, the engine captures
    a binary changeset using the SQLite Session Extension, annotates it with a
    hybrid logical clock (HLC) timestamp and the client's schema version, and
    writes it to a shared folder. Other clients watch the folder and apply
    incoming changesets with automatic conflict resolution.

    \section1 Recommended Usage (QSQLITE_SYNC driver)

    The library ships a QSQLITE_SYNC driver plugin which works almost exactly
    like Qt's built-in QSQLITE driver. The difference? SQLite Session Extension
    support.

    Once set up, use QSqlDatabase and QSqlQuery as normal and the database
    changes will push any local changes to the shared folder automatically.

    To pull and apply changes from remote clients, users can either configure
    it to happen automatically via the start() method, or call the sync()
    method manually.

    \code
    // Open with the QSQLITE_SYNC driver.
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE_SYNC");
    db.setDatabaseName("myapp.db");
    db.open();

    // Create the sync engine with a shared folder and client ID.
    SyncEngine engine(db, "/shared/folder", "client-id");
    engine.setSchemaVersion(1);
    engine.start();

    // Use QSqlQuery as you normally would for database operations.
    // Writes are pushed to the shared folder automatically.
    QSqlQuery q(db);
    q.exec("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, name TEXT)");
    q.exec("INSERT INTO items (name) VALUES ('hello')");
    q.prepare("INSERT INTO items (name) VALUES (?)");
    q.bindValue(0, "world");
    q.exec();

    // Call sync() to pull and apply changes from other clients.
    engine.sync();
    \endcode

    Transactions, QSqlTableModel, and all other Qt SQL classes work as expected.

    \section1 Fallback Usage (without QSQLITE_SYNC driver)

    If the driver plugin is not available (e.g., Qt source tree not installed),
    the engine falls back to a parallel connection and requires beginWrite() /
    endWrite() calls to track changes:

    \code
    SyncEngine engine("/path/to/local.db", "/shared/folder", "client-id");
    engine.start();
    engine.beginWrite();
    engine.database()->exec("INSERT INTO items (name) VALUES ('hello')");
    engine.endWrite();
    \endcode

    \section1 Pushing and Pulling

    Sync has two directions -- pushing local changes out and pulling remote
    changes in -- and they work differently:

    \b{Pushing (automatic).} When users write to the database via QSqlQuery,
    the engine's commit hook captures the changes and writes a changeset file
    to the shared folder immediately.

    \b{Pulling (manual or timed).} Remote changesets sitting in the shared
    folder are NOT applied automatically in the background. Users control when
    they're applied by calling sync(), either manually or on a timer:

    \code
    // Option 1: auto-pull on a timer.
    // start() accepts an interval in milliseconds (default: 1000ms).
    engine.start(5000);  // pull every 5 seconds

    // Option 2: pull manually whenever you want.
    engine.start(0);     // disable the timer
    engine.sync();       // pull now
    \endcode

    Users should generally prefer explicit pulling. This is because applying
    remote changes modifies the local database, which could affect in-progress
    queries or UI state. By controlling the timing, users can pull at safe
    points, for example before or after a UI update.

    Listen for the syncCompleted signal to be notified of potential changes:

    \code
    connect(&engine, &SyncEngine::syncCompleted, [&](int count) {
        if (count > 0)
            model->select();  // refresh QSqlTableModel
    });
    \endcode

    \section1 Schema Versioning

    The engine embeds a schema version in each changeset filename. Clients
    reject changesets from a newer schema version and emit syncErrorOccurred()
    with VersionMismatch and an actionable upgrade message. Rejected changesets
    remain in the shared folder and are automatically retried after the client
    upgrades.

    \code
    engine.setSchemaVersion(2);  // Ideally set before start()
    \endcode

    \section1 Conflict Resolution

    When two clients modify the same row, conflicts are resolved using hybrid
    logical clock (HLC) timestamps: the change with the higher HLC wins. Equal
    HLCs are broken by client ID for deterministic convergence.

    In practice, this means the last client to write after syncing will win.
    Syncing advances a client's HLC to be at least as high as all received
    changesets, so any subsequent local write is guaranteed to have a higher
    HLC than anything previously synced. The net effect is that the most
    recent write from a client that is up-to-date takes priority.
*/
class SyncEngine : public QObject {
    Q_OBJECT
public:
    /*!
        \enum syncengine::SyncEngine::SyncError
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
    Q_ENUM(SyncError)

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
    bool applyOneChangeset(const ChangesetInfo &info, int &applied);
    void emitApplyError(const ChangesetInfo &info);
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
