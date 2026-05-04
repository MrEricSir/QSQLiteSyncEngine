#pragma once

#include "syncengine/ITransport.h"
#include "syncengine/SharedFolderTransport.h"

#include <QDateTime>
#include <QList>
#include <memory>

///
/// Test-only ITransport wrapper that delays writeChangeset() calls by a
/// configurable number of milliseconds, simulating slow network-drive
/// propagation. Call flushDelayed() to release held files to disk.
///
class DelayedTransport : public syncengine::ITransport {
public:
    explicit DelayedTransport(const QString &folderPath)
        : m_inner(std::make_unique<syncengine::SharedFolderTransport>(folderPath, nullptr))
    {}

    bool writeChangeset(const QString &filename, const QByteArray &data) override
    {
        if (m_latencyMs > 0) {
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            m_delayed.append({filename, data, now + m_latencyMs});
            return true;
        }
        return m_inner->writeChangeset(filename, data);
    }

    QByteArray readChangeset(const QString &filename) override
    {
        return m_inner->readChangeset(filename);
    }

    QStringList listChangesets() override
    {
        return m_inner->listChangesets();
    }

    void setLatencyMs(int ms) { m_latencyMs = ms; }

    void flushDelayed()
    {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        QList<DelayedFile> stillPending;
        for (const auto &df : m_delayed) {
            if (now >= df.visibleAfterMs)
                m_inner->writeChangeset(df.filename, df.data);
            else
                stillPending.append(df);
        }
        m_delayed = stillPending;
    }

private:
    struct DelayedFile {
        QString filename;
        QByteArray data;
        qint64 visibleAfterMs;
    };

    std::unique_ptr<syncengine::SharedFolderTransport> m_inner;
    int m_latencyMs = 0;
    QList<DelayedFile> m_delayed;
};
