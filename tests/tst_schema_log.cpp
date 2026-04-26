#include <QTest>
#include <QTemporaryDir>

#include "syncengine/SyncableDatabase.h"
#include "sqlite3.h"

using namespace syncengine;

/// Proof-of-concept: can we detect schema mismatches via sqlite3_config(SQLITE_CONFIG_LOG)?
class TestSchemaLog : public QObject {
    Q_OBJECT

    static QStringList s_logMessages;
    static int s_lastErrorCode;

    static void sqliteLogCallback(void * /*userData*/, int errorCode, const char *message)
    {
        s_lastErrorCode = errorCode;
        s_logMessages.append(QString::fromUtf8(message));
    }

private slots:
    void initTestCase()
    {
        // Install the global log callback BEFORE any database is opened.
        // SQLITE_CONFIG_LOG must be called before sqlite3_initialize().
        sqlite3_shutdown();
        int rc = sqlite3_config(SQLITE_CONFIG_LOG, sqliteLogCallback, nullptr);
        QCOMPARE(rc, SQLITE_OK);
        sqlite3_initialize();
    }

    void init()
    {
        s_logMessages.clear();
        s_lastErrorCode = 0;
    }

    void testLogFiredOnColumnCountMismatch()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        // Source: 3 columns
        SyncableDatabase dbA(tmpDir.filePath("a.db"), "clientA");
        QVERIFY(dbA.open());
        dbA.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, extra TEXT)");

        // Target: 2 columns
        SyncableDatabase dbB(tmpDir.filePath("b.db"), "clientB");
        QVERIFY(dbB.open());
        dbB.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        // Capture a changeset from A (3 columns)
        QVERIFY(dbA.beginSession());
        dbA.exec("INSERT INTO items (id, name, extra) VALUES (1, 'test', 'val')");
        QByteArray changeset = dbA.endSession();
        QVERIFY(!changeset.isEmpty());

        // Apply to B (2 columns) -- should trigger SQLITE_SCHEMA log
        s_logMessages.clear();
        dbB.applyChangeset(changeset);

        // Check that the log callback was invoked with a schema message
        qDebug() << "Log messages:" << s_logMessages;
        qDebug() << "Last error code:" << s_lastErrorCode;

        bool foundSchemaWarning = false;
        for (const QString &msg : s_logMessages) {
            if (msg.contains("sqlite3changeset_apply")
                && msg.contains("items")) {
                foundSchemaWarning = true;
                break;
            }
        }
        QVERIFY2(foundSchemaWarning,
                 "Expected sqlite3_log to fire with SQLITE_SCHEMA for column mismatch");
    }

    void testLogFiredOnMissingTable()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncableDatabase dbA(tmpDir.filePath("a.db"), "clientA");
        QVERIFY(dbA.open());
        dbA.exec("CREATE TABLE newtable (id INTEGER PRIMARY KEY, val TEXT)");

        SyncableDatabase dbB(tmpDir.filePath("b.db"), "clientB");
        QVERIFY(dbB.open());
        // B does NOT have "newtable"

        QVERIFY(dbA.beginSession());
        dbA.exec("INSERT INTO newtable (id, val) VALUES (1, 'x')");
        QByteArray changeset = dbA.endSession();

        s_logMessages.clear();
        dbB.applyChangeset(changeset);

        qDebug() << "Log messages:" << s_logMessages;

        bool foundMissingTable = false;
        for (const QString &msg : s_logMessages) {
            if (msg.contains("no such table")) {
                foundMissingTable = true;
                break;
            }
        }
        QVERIFY2(foundMissingTable,
                 "Expected sqlite3_log to fire with 'no such table' for missing table");
    }

    void cleanupTestCase()
    {
        // Remove the log callback
        sqlite3_shutdown();
        sqlite3_config(SQLITE_CONFIG_LOG, nullptr, nullptr);
        sqlite3_initialize();
    }
};

QStringList TestSchemaLog::s_logMessages;
int TestSchemaLog::s_lastErrorCode = 0;

QTEST_MAIN(TestSchemaLog)

#include "tst_schema_log.moc"
