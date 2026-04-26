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
#include <QString>
#include <QByteArray>

#include "syncengine/SyncableDatabase.h"
#include "syncengine/SharedFolderTransport.h"

namespace syncengine {

/*!
    \struct ChangesetInfo
    \inmodule QSQLiteSyncEngine
    \internal
    \brief Metadata parsed from a changeset filename.

    \list
    \li \c filename -- the full filename including extension
    \li \c clientId -- the client that produced this changeset
    \li \c hlc -- hybrid logical clock timestamp
    \li \c sequence -- per-client sequence number
    \li \c schemaVersion -- database schema version (0 = legacy/unversioned)
    \endlist
*/
struct ChangesetInfo {
    QString filename;
    QString clientId;
    uint64_t hlc = 0;
    uint64_t sequence = 0;
    int schemaVersion = 0;
};

/*!
    \class ChangesetManager
    \inmodule QSQLiteSyncEngine
    \internal
    \brief Manages the lifecycle of changeset files on the shared folder.

    ChangesetManager writes local changesets to the shared folder with
    filenames that encode the client ID, HLC timestamp, sequence number,
    and schema version. It also reads and sorts pending changesets from
    other clients.

    \sa SyncEngine, SharedFolderTransport
*/
class ChangesetManager : public QObject {
    Q_OBJECT
public:
    explicit ChangesetManager(SyncableDatabase *db,
                              SharedFolderTransport *transport,
                              QObject *parent = nullptr);

    /*!
        Writes a \a changeset blob to the shared folder stamped with \a hlc.
        Returns the filename used, or an empty string on failure.
    */
    QString writeChangeset(const QByteArray &changeset, uint64_t hlc);

    /*!
        Returns all changeset files that have not been applied to the local
        database, sorted by HLC order.
    */
    QList<ChangesetInfo> pendingChangesets();

    /*! Parses a changeset \a filename into a ChangesetInfo struct. */
    static ChangesetInfo parseFilename(const QString &filename);

    /*!
        Builds a changeset filename from its components.
    */
    static QString buildFilename(const QString &clientId, uint64_t hlc, uint64_t sequence,
                                 int schemaVersion = 0);

    /*! Sets the \a version to embed in produced changesets. */
    void setSchemaVersion(int version) { m_schemaVersion = version; }

    /*! Returns the current schema version. */
    int schemaVersion() const { return m_schemaVersion; }

private:
    SyncableDatabase *m_db;
    SharedFolderTransport *m_transport;
    uint64_t m_sequence = 0;
    int m_schemaVersion = 0;
};

} // namespace syncengine
