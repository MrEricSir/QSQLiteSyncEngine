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
#include <QList>
#include <mutex>

#include "sqlite3.h"

namespace syncengine {

/*!
    \class SchemaLogCapture
    \inmodule QSQLiteSyncEngine
    \internal
    \brief Captures SQLITE_SCHEMA warnings during changeset application.

    SchemaLogCapture installs a process-global sqlite3 log callback that
    captures SQLITE_SCHEMA warnings emitted by sqlite3changeset_apply() when
    tables are skipped due to schema incompatibility. A thread_local Guard
    scopes the capture to the calling thread.

    \sa SyncableDatabase
*/
class SchemaLogCapture {
public:
    /*!
        \struct SchemaLogCapture::Warning
        \brief A captured SQLITE_SCHEMA warning.
    */
    struct Warning {
        int errorCode;      ///< The SQLite error code (typically SQLITE_SCHEMA).
        QString message;    ///< The diagnostic message from sqlite3_log().
    };

    /*!
        Installs the global sqlite3 log callback. Idempotent and safe to call
        multiple times. Must be called before any sqlite3 database is opened.
    */
    static void install();

    /*!
        \class SchemaLogCapture::Guard
        \brief RAII guard that captures SQLITE_SCHEMA log messages on the current thread.
    */
    class Guard {
    public:
        Guard();
        ~Guard();

        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;

        /*! Returns true if any schema warnings were captured. */
        bool hasWarnings() const { return !m_warnings.isEmpty(); }

        /*! Returns the list of captured warnings. */
        const QList<Warning> &warnings() const { return m_warnings; }

    private:
        friend class SchemaLogCapture;
        QList<Warning> m_warnings;
    };

private:
    static void logCallback(void *userData, int errorCode, const char *message);
    static thread_local Guard *s_activeGuard;
};

} // namespace syncengine
