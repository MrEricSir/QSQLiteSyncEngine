// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
//
// Modified for QSQLiteSyncEngine: registers as QSQLITE_SYNC and links against
// a sqlite3 build with SQLITE_ENABLE_SESSION / SQLITE_ENABLE_PREUPDATE_HOOK.

#include <qsqldriverplugin.h>
#include <qstringlist.h>
#include "qsql_sqlite_p.h"

QT_BEGIN_NAMESPACE

using namespace Qt::StringLiterals;

class QSQLiteSyncDriverPlugin : public QSqlDriverPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QSqlDriverFactoryInterface" FILE "qsqlite_sync.json")

public:
    QSQLiteSyncDriverPlugin();
    QSqlDriver* create(const QString &) override;
};

QSQLiteSyncDriverPlugin::QSQLiteSyncDriverPlugin()
    : QSqlDriverPlugin()
{
}

QSqlDriver* QSQLiteSyncDriverPlugin::create(const QString &name)
{
    if (name == "QSQLITE_SYNC"_L1) {
        QSQLiteDriver* driver = new QSQLiteDriver();
        return driver;
    }
    return nullptr;
}

QT_END_NAMESPACE

#include "smain.moc"
