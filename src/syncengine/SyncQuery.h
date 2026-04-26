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
#include <QVariant>

#include "sqlite3.h"

namespace syncengine {

class SyncableDatabase;

/*!
    \class syncengine::SyncQuery
    \inmodule QSQLiteSyncEngine
    \internal
    \brief Prepared query interface for the sync engine's internal connection.

    SyncQuery is a fallback for when the QSQLITE_SYNC driver plugin is not
    available. When using QSQLITE_SYNC, prefer QSqlQuery on the QSqlDatabase
    directly -- it provides the same functionality with full Qt SQL integration.

    \sa SyncEngine
*/
class SyncQuery {
public:
    /*! Constructs a SyncQuery on the given \a database. */
    explicit SyncQuery(SyncableDatabase *database);

    ~SyncQuery();

    SyncQuery(const SyncQuery &) = delete;
    SyncQuery &operator=(const SyncQuery &) = delete;

    SyncQuery(SyncQuery &&other) noexcept;
    SyncQuery &operator=(SyncQuery &&other) noexcept;

    /*!
        Prepares the SQL statement \a sql for execution. Returns true on success.
        Use bindValue() to set parameter values before calling exec().
    */
    bool prepare(const QString &sql);

    /*!
        Binds the parameter at \a index to \a value. Parameters use 0-based
        indexing, matching '?' placeholders in the prepared statement.
    */
    void bindValue(int index, const QVariant &value);

    /*!
        Executes a previously prepared statement. Returns true on success.
    */
    bool exec();

    /*!
        Prepares and executes \a sql in one call. Returns true on success.
    */
    bool exec(const QString &sql);

    /*!
        Advances to the next row of results. Returns true if a row is available.
    */
    bool next();

    /*!
        Returns the value of column \a index in the current row.
    */
    QVariant value(int index) const;

    /*!
        Returns the number of columns in the result set.
    */
    int columnCount() const;

    /*!
        Returns the column name at \a index.
    */
    QString columnName(int index) const;

    /*!
        Returns the number of rows affected by the last INSERT, UPDATE, or DELETE.
    */
    int numRowsAffected() const;

    /*!
        Returns the rowid of the last inserted row.
    */
    qint64 lastInsertId() const;

    /*!
        Returns the last error message, or an empty string if no error.
    */
    QString lastError() const;

private:
    void finalize();

    SyncableDatabase *m_db;
    sqlite3_stmt *m_stmt = nullptr;
    QString m_lastError;
};

} // namespace syncengine
