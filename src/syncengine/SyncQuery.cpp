#include "syncengine/SyncQuery.h"
#include "syncengine/SyncableDatabase.h"

#include <QDebug>

namespace syncengine {

SyncQuery::SyncQuery(SyncableDatabase *database)
    : db(database)
{
}

SyncQuery::~SyncQuery()
{
    finalize();
}

SyncQuery::SyncQuery(SyncQuery &&other) noexcept
    : db(other.db)
    , stmt(other.stmt)
    , lastDbError(std::move(other.lastDbError))
{
    other.stmt = nullptr;
}

SyncQuery &SyncQuery::operator=(SyncQuery &&other) noexcept
{
    if (this != &other) {
        finalize();
        db = other.db;
        stmt = other.stmt;
        lastDbError = std::move(other.lastDbError);
        other.stmt = nullptr;
    }
    return *this;
}

bool SyncQuery::prepare(const QString &sql)
{
    finalize();
    lastDbError.clear();

    QByteArray utf8 = sql.toUtf8();
    int rc = sqlite3_prepare_v2(db->handle(), utf8.constData(), utf8.size(), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        lastDbError = QString::fromUtf8(sqlite3_errmsg(db->handle()));
        stmt = nullptr;
        return false;
    }
    return true;
}

void SyncQuery::bindValue(int index, const QVariant &value)
{
    if (!stmt)
        return;

    // sqlite3 uses 1-based parameter indices
    int idx = index + 1;

    if (value.isNull()) {
        sqlite3_bind_null(stmt, idx);
    } else {
        switch (value.typeId()) {
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::Bool:
            sqlite3_bind_int(stmt, idx, value.toInt());
            break;
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
            sqlite3_bind_int64(stmt, idx, value.toLongLong());
            break;
        case QMetaType::Double:
        case QMetaType::Float:
            sqlite3_bind_double(stmt, idx, value.toDouble());
            break;
        case QMetaType::QByteArray: {
            QByteArray ba = value.toByteArray();
            sqlite3_bind_blob(stmt, idx, ba.constData(), ba.size(), SQLITE_TRANSIENT);
            break;
        }
        default: {
            // Treat everything else as text
            QByteArray utf8 = value.toString().toUtf8();
            sqlite3_bind_text(stmt, idx, utf8.constData(), utf8.size(), SQLITE_TRANSIENT);
            break;
        }
        }
    }
}

bool SyncQuery::exec()
{
    if (!stmt) {
        lastDbError = QStringLiteral("No prepared statement");
        return false;
    }

    lastDbError.clear();
    int rc = sqlite3_step(stmt);

    if (rc == SQLITE_DONE) {
        // Statement completed (INSERT/UPDATE/DELETE with no results)
        // Reset so next() won't be called, but statement stays valid for numRowsAffected
        return true;
    } else if (rc == SQLITE_ROW) {
        // SELECT returned a row -- reset so next() starts from the beginning
        sqlite3_reset(stmt);
        return true;
    } else {
        lastDbError = QString::fromUtf8(sqlite3_errmsg(db->handle()));
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
    if (!stmt)
        return false;

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        return true;

    if (rc != SQLITE_DONE)
        lastDbError = QString::fromUtf8(sqlite3_errmsg(db->handle()));

    return false;
}

QVariant SyncQuery::value(int index) const
{
    if (!stmt)
        return {};

    int type = sqlite3_column_type(stmt, index);
    switch (type) {
    case SQLITE_INTEGER:
        return QVariant(static_cast<qlonglong>(sqlite3_column_int64(stmt, index)));
    case SQLITE_FLOAT:
        return QVariant(sqlite3_column_double(stmt, index));
    case SQLITE_BLOB: {
        const void *data = sqlite3_column_blob(stmt, index);
        int size = sqlite3_column_bytes(stmt, index);
        return QVariant(QByteArray(static_cast<const char *>(data), size));
    }
    case SQLITE_NULL:
        return QVariant();
    case SQLITE_TEXT:
    default:
        return QVariant(QString::fromUtf8(
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, index)),
            sqlite3_column_bytes(stmt, index)));
    }
}

int SyncQuery::columnCount() const
{
    return stmt ? sqlite3_column_count(stmt) : 0;
}

QString SyncQuery::columnName(int index) const
{
    if (!stmt)
        return {};
    const char *name = sqlite3_column_name(stmt, index);
    return name ? QString::fromUtf8(name) : QString();
}

int SyncQuery::numRowsAffected() const
{
    return db ? sqlite3_changes(db->handle()) : 0;
}

qint64 SyncQuery::lastInsertId() const
{
    return db ? sqlite3_last_insert_rowid(db->handle()) : 0;
}

QString SyncQuery::lastError() const
{
    return lastDbError;
}

void SyncQuery::finalize()
{
    if (stmt) {
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }
}

} // namespace syncengine
