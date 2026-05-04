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

#include <QString>
#include <QByteArray>
#include <functional>
#include <memory>

#include "sqlite3.h"
#include "syncengine/HybridLogicalClock.h"
#include "syncengine/SchemaLogCapture.h"

namespace syncengine {

/*!
    \class syncengine::SyncableDatabase
    \inmodule QSQLiteSyncEngine
    \internal
    \brief SQLite database wrapper with session extension support for change capture.

    SyncableDatabase is an internal class used by SyncEngine. When using the
    recommended QSQLITE_SYNC driver, prefer QSqlQuery on the QSqlDatabase
    directly. This class is only exposed for advanced use cases and fallback
    when the driver plugin is not available.

    \sa SyncEngine
*/
class SyncableDatabase {
public:
    /*!
        \enum syncengine::SyncableDatabase::ConflictAction
        Actions that a conflict handler can return.

        \value Replace Overwrite the local row with the incoming change.
        \value Skip    Keep the local row and discard the incoming change.
        \value Abort   Abort the entire changeset application.
    */
    enum class ConflictAction {
        Replace,
        Skip,
        Abort
    };

    using ConflictHandler = std::function<ConflictAction(
        int conflictType,
        const QString &tableName,
        sqlite3_changeset_iter *iter)>;

    explicit SyncableDatabase(const QString &dbPath, const QString &clientId);

    /*!
        Constructs a SyncableDatabase that borrows an existing sqlite3 handle
        (e.g. from QSqlDriver::handle()). The caller retains ownership -- the
        handle will NOT be closed on destruction.
    */
    explicit SyncableDatabase(sqlite3 *existingHandle, const QString &clientId);

    ~SyncableDatabase();

    SyncableDatabase(const SyncableDatabase &) = delete;
    SyncableDatabase &operator=(const SyncableDatabase &) = delete;

    /*! Opens the database. Returns true on success. Only valid for path-based construction. */
    bool open();

    /*! Closes the database and releases resources. No-op if using a borrowed handle. */
    void close();

    /*! Begins tracking changes. Call before write operations. */
    bool beginSession();

    /*! Ends tracking and extracts the changeset. Returns an empty QByteArray if no changes were made. */
    QByteArray endSession();

    /*!
        Applies a binary \a changeset to this database. Returns true on success.
        If \a warnings is non-null, it is populated with any schema warnings
        produced during application (e.g. tables skipped due to schema mismatch).
    */
    bool applyChangeset(const QByteArray &changeset, ConflictHandler handler = nullptr,
                        QList<SchemaLogCapture::Warning> *warnings = nullptr);

    /*!
        Generates a changeset containing INSERT records for all rows in the
        specified \a tables. Uses sqlite3session_diff against an empty
        in-memory database so every existing row appears as an INSERT.
        Returns an empty QByteArray on failure or if the tables are empty.
    */
    QByteArray generateSnapshot(const QStringList &tables);

    /*! Executes a SQL statement. Returns true on success. */
    bool exec(const QString &sql);

    /*! Executes a SQL query and returns results as a list of string lists. */
    QList<QStringList> query(const QString &sql);

    /*! Returns the raw sqlite3 handle for advanced usage. */
    sqlite3 *handle() const { return db; }

    /*! Returns the client identifier. */
    QString clientId() const { return clientIdentifier; }

    /*! \internal Returns a mutable reference to the HybridLogicalClock. */
    HybridLogicalClock &clock() { return hlcClock; }

    /*! Returns a const reference to the HybridLogicalClock. */
    const HybridLogicalClock &clock() const { return hlcClock; }

    /*! \internal Initializes internal sync metadata tables. */
    bool initSyncMetadata();

    /*! \internal Records that \a changesetId has been applied. */
    bool markChangesetApplied(const QString &changesetId, uint64_t hlc);

    /*! Returns true if the changeset \a changesetId has already been applied. */
    bool isChangesetApplied(const QString &changesetId);

    /*! \internal Updates the HLC for a given row. */
    bool updateRowHlc(const QString &tableName, int64_t rowid, uint64_t hlc, const QString &clientId);

    /*! \internal Returns the HLC for a given row. */
    uint64_t getRowHlc(const QString &tableName, int64_t rowid);

private:
    QString dbPath;
    QString clientIdentifier;
    sqlite3 *db = nullptr;
    sqlite3_session *session = nullptr;
    HybridLogicalClock hlcClock;
    bool ownsHandle = true;
};

} // namespace syncengine
