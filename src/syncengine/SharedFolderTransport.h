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

#include <QDir>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QFileSystemWatcher>
#include <QObject>

namespace syncengine {

/*!
    \class syncengine::SharedFolderTransport
    \inmodule QSQLiteSyncEngine
    \internal
    \brief File I/O abstraction for changeset files on a shared folder.

    SharedFolderTransport reads and writes changeset files to a shared folder
    that may be backed by Google Drive, Dropbox, or a local network drive.
    It uses QFileSystemWatcher to emit changesetsAvailable() when new files
    appear.

    For testing, setSimulatedLatencyMs() can be used to delay the visibility
    of written files, simulating a slow network drive.

    \sa SyncEngine, ChangesetManager
*/
class SharedFolderTransport : public QObject {
    Q_OBJECT
public:
    explicit SharedFolderTransport(const QString &folderPath, QObject *parent = nullptr);

    /*! Writes a changeset \a data to the shared folder as \a filename. */
    bool writeChangeset(const QString &filename, const QByteArray &data);

    /*! Reads and returns the contents of the changeset \a filename. */
    QByteArray readChangeset(const QString &filename);

    /*! Returns all changeset filenames sorted by name (HLC order). */
    QStringList listChangesets();

    /*!
        Sets the simulated write delay in milliseconds. When non-zero, written
        files are held in memory and only flushed to disk after \a ms
        milliseconds, simulating network propagation delay.
    */
    void setSimulatedLatencyMs(int ms);

    /*! Returns the current simulated latency in milliseconds. */
    int simulatedLatencyMs() const { return latencyMs; }

    /*! Flushes any delayed files whose delay has elapsed. Called automatically by listChangesets(). */
    void flushDelayed();

    /*! Returns the shared folder path. */
    QString folderPath() const { return folderPathString; }

signals:
    /*! Emitted when new changeset files appear in the folder. */
    void changesetsAvailable();

private:
    QString folderPathString;
    int latencyMs = 0;
    QFileSystemWatcher watcher;

    struct DelayedFile {
        QString filename;
        QByteArray data;
        qint64 visibleAfterMs;
    };
    QList<DelayedFile> m_delayed;
};

} // namespace syncengine
