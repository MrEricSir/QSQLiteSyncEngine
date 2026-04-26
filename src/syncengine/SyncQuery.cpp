#include "syncengine/SyncQuery.h"
#include "syncengine/SyncableDatabase.h"

#include <QDebug>

namespace syncengine {

SyncQuery::SyncQuery(SyncableDatabase *database)
    : m_db(database)
{
}

SyncQuery::~SyncQuery()
{
    finalize();
}

SyncQuery::SyncQuery(SyncQuery &&other) noexcept
    : m_db(other.m_db)
    , m_stmt(other.m_stmt)
    , m_lastError(std::move(other.m_lastError))
{
    other.m_stmt = nullptr;
}

SyncQuery &SyncQuery::operator=(SyncQuery &&other) noexcept
{
    if (this != &other) {
        finalize();
        m_db = other.m_db;
        m_stmt = other.m_stmt;
        m_lastError = std::move(other.m_lastError);
        other.m_stmt = nullptr;
    }
    return *this;
}

bool SyncQuery::prepare(const QString &sql)
{
    finalize();
    m_lastError.clear();

    QByteArray utf8 = sql.toUtf8();
    int rc = sqlite3_prepare_v2(m_db->handle(), utf8.constData(), utf8.size(), &m_stmt, nullptr);
    if (rc != SQLITE_OK) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db->handle()));
        m_stmt = nullptr;
        return false;
    }
    return true;
}

void SyncQuery::bindValue(int index, const QVariant &value)
{
    if (!m_stmt)
        return;

    // sqlite3 uses 1-based parameter indices
    int idx = index + 1;

    if (value.isNull()) {
        sqlite3_bind_null(m_stmt, idx);
    } else {
        switch (value.typeId()) {
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::Bool:
            sqlite3_bind_int(m_stmt, idx, value.toInt());
            break;
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
            sqlite3_bind_int64(m_stmt, idx, value.toLongLong());
            break;
        case QMetaType::Double:
        case QMetaType::Float:
            sqlite3_bind_double(m_stmt, idx, value.toDouble());
            break;
        case QMetaType::QByteArray: {
            QByteArray ba = value.toByteArray();
            sqlite3_bind_blob(m_stmt, idx, ba.constData(), ba.size(), SQLITE_TRANSIENT);
            break;
        }
        default: {
            // Treat everything else as text
            QByteArray utf8 = value.toString().toUtf8();
            sqlite3_bind_text(m_stmt, idx, utf8.constData(), utf8.size(), SQLITE_TRANSIENT);
            break;
        }
        }
    }
}

bool SyncQuery::exec()
{
    if (!m_stmt) {
        m_lastError = QStringLiteral("No prepared statement");
        return false;
    }

    m_lastError.clear();
    int rc = sqlite3_step(m_stmt);

    if (rc == SQLITE_DONE) {
        // Statement completed (INSERT/UPDATE/DELETE with no results)
        // Reset so next() won't be called, but statement stays valid for numRowsAffected
        return true;
    } else if (rc == SQLITE_ROW) {
        // SELECT returned a row -- reset so next() starts from the beginning
        sqlite3_reset(m_stmt);
        return true;
    } else {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db->handle()));
        return false;
    }
}

bool SyncQuery::exec(const QString &sql)
{
    if (!prepare(sql))
        return false;
    return exec();
}

bool SyncQuery::next()
{
    if (!m_stmt)
        return false;

    int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW)
        return true;

    if (rc != SQLITE_DONE)
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db->handle()));

    return false;
}

QVariant SyncQuery::value(int index) const
{
    if (!m_stmt)
        return {};

    int type = sqlite3_column_type(m_stmt, index);
    switch (type) {
    case SQLITE_INTEGER:
        return QVariant(static_cast<qlonglong>(sqlite3_column_int64(m_stmt, index)));
    case SQLITE_FLOAT:
        return QVariant(sqlite3_column_double(m_stmt, index));
    case SQLITE_BLOB: {
        const void *data = sqlite3_column_blob(m_stmt, index);
        int size = sqlite3_column_bytes(m_stmt, index);
        return QVariant(QByteArray(static_cast<const char *>(data), size));
    }
    case SQLITE_NULL:
        return QVariant();
    case SQLITE_TEXT:
    default:
        return QVariant(QString::fromUtf8(
            reinterpret_cast<const char *>(sqlite3_column_text(m_stmt, index)),
            sqlite3_column_bytes(m_stmt, index)));
    }
}

int SyncQuery::columnCount() const
{
    return m_stmt ? sqlite3_column_count(m_stmt) : 0;
}

QString SyncQuery::columnName(int index) const
{
    if (!m_stmt)
        return {};
    const char *name = sqlite3_column_name(m_stmt, index);
    return name ? QString::fromUtf8(name) : QString();
}

int SyncQuery::numRowsAffected() const
{
    return m_db ? sqlite3_changes(m_db->handle()) : 0;
}

qint64 SyncQuery::lastInsertId() const
{
    return m_db ? sqlite3_last_insert_rowid(m_db->handle()) : 0;
}

QString SyncQuery::lastError() const
{
    return m_lastError;
}

void SyncQuery::finalize()
{
    if (m_stmt) {
        sqlite3_finalize(m_stmt);
        m_stmt = nullptr;
    }
}

} // namespace syncengine
