#include <QTest>
#include <QTemporaryDir>

#include "syncengine/SyncEngine.h"
#include "syncengine/SyncQuery.h"

using namespace syncengine;

class TestSyncQuery : public QObject {
    Q_OBJECT
private slots:

    void testPrepareAndExec()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "c1");
        QVERIFY(engine.start(0));
        engine.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        engine.beginWrite();

        SyncQuery q(engine.database());
        QVERIFY(q.prepare("INSERT INTO t (id, val) VALUES (?, ?)"));
        q.bindValue(0, 1);
        q.bindValue(1, QStringLiteral("hello"));
        QVERIFY(q.exec());

        engine.endWrite();

        // Verify the row exists
        SyncQuery r(engine.database());
        QVERIFY(r.exec("SELECT val FROM t WHERE id = 1"));
        QVERIFY(r.next());
        QCOMPARE(r.value(0).toString(), QStringLiteral("hello"));
    }

    void testBindVariousTypes()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "c1");
        QVERIFY(engine.start(0));
        engine.database()->exec(
            "CREATE TABLE t (id INTEGER PRIMARY KEY, i INT, d REAL, t TEXT, b BLOB)");

        engine.beginWrite();

        SyncQuery q(engine.database());
        q.prepare("INSERT INTO t (id, i, d, t, b) VALUES (?, ?, ?, ?, ?)");
        q.bindValue(0, 1);
        q.bindValue(1, 42);
        q.bindValue(2, 3.14);
        q.bindValue(3, QStringLiteral("text"));
        q.bindValue(4, QByteArray("\x00\x01\x02", 3));
        QVERIFY(q.exec());

        engine.endWrite();

        SyncQuery r(engine.database());
        r.exec("SELECT i, d, t, b FROM t WHERE id = 1");
        QVERIFY(r.next());
        QCOMPARE(r.value(0).toLongLong(), 42LL);
        QVERIFY(qAbs(r.value(1).toDouble() - 3.14) < 0.001);
        QCOMPARE(r.value(2).toString(), QStringLiteral("text"));
        QCOMPARE(r.value(3).toByteArray(), QByteArray("\x00\x01\x02", 3));
    }

    void testBindNull()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "c1");
        QVERIFY(engine.start(0));
        engine.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        engine.beginWrite();

        SyncQuery q(engine.database());
        q.prepare("INSERT INTO t (id, val) VALUES (?, ?)");
        q.bindValue(0, 1);
        q.bindValue(1, QVariant());
        QVERIFY(q.exec());

        engine.endWrite();

        SyncQuery r(engine.database());
        r.exec("SELECT val FROM t WHERE id = 1");
        QVERIFY(r.next());
        QVERIFY(r.value(0).isNull());
    }

    void testMultipleRows()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "c1");
        QVERIFY(engine.start(0));
        engine.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        engine.beginWrite();
        for (int i = 1; i <= 5; ++i) {
            SyncQuery q(engine.database());
            q.prepare("INSERT INTO t (id, val) VALUES (?, ?)");
            q.bindValue(0, i);
            q.bindValue(1, QStringLiteral("row_%1").arg(i));
            QVERIFY(q.exec());
        }
        engine.endWrite();

        SyncQuery r(engine.database());
        r.exec("SELECT id, val FROM t ORDER BY id");
        int count = 0;
        while (r.next()) {
            ++count;
            QCOMPARE(r.value(0).toInt(), count);
            QCOMPARE(r.value(1).toString(), QStringLiteral("row_%1").arg(count));
        }
        QCOMPARE(count, 5);
    }

    void testColumnInfo()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "c1");
        QVERIFY(engine.start(0));
        engine.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, value REAL)");
        engine.database()->exec("INSERT INTO t VALUES (1, 'x', 2.5)");

        SyncQuery q(engine.database());
        q.exec("SELECT id, name, value FROM t");
        QCOMPARE(q.columnCount(), 3);
        QCOMPARE(q.columnName(0), QStringLiteral("id"));
        QCOMPARE(q.columnName(1), QStringLiteral("name"));
        QCOMPARE(q.columnName(2), QStringLiteral("value"));
    }

    void testNumRowsAffected()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "c1");
        QVERIFY(engine.start(0));
        engine.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");
        engine.database()->exec("INSERT INTO t VALUES (1, 'a')");
        engine.database()->exec("INSERT INTO t VALUES (2, 'b')");
        engine.database()->exec("INSERT INTO t VALUES (3, 'c')");

        SyncQuery q(engine.database());
        QVERIFY(q.exec("UPDATE t SET val = 'x'"));
        QCOMPARE(q.numRowsAffected(), 3);
    }

    void testLastInsertId()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "c1");
        QVERIFY(engine.start(0));
        engine.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        SyncQuery q(engine.database());
        q.exec("INSERT INTO t (val) VALUES ('auto')");
        QCOMPARE(q.lastInsertId(), 1LL);

        SyncQuery q2(engine.database());
        q2.exec("INSERT INTO t (val) VALUES ('auto2')");
        QCOMPARE(q2.lastInsertId(), 2LL);
    }

    void testErrorMessage()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "c1");
        QVERIFY(engine.start(0));

        SyncQuery q(engine.database());
        QVERIFY(!q.exec("SELECT * FROM nonexistent_table"));
        QVERIFY(!q.lastError().isEmpty());
    }

    void testWritesSyncToOtherEngine()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        engineA.database()->exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, score INT)");
        engineB.database()->exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, score INT)");

        // Write using SyncQuery with prepared statement
        engineA.beginWrite();

        SyncQuery q(engineA.database());
        q.prepare("INSERT INTO items (id, name, score) VALUES (?, ?, ?)");
        q.bindValue(0, 1);
        q.bindValue(1, QStringLiteral("alice"));
        q.bindValue(2, 100);
        QVERIFY(q.exec());

        engineA.endWrite();

        // Sync to B
        int applied = engineB.sync();
        QCOMPARE(applied, 1);

        SyncQuery r(engineB.database());
        r.exec("SELECT name, score FROM items WHERE id = 1");
        QVERIFY(r.next());
        QCOMPARE(r.value(0).toString(), QStringLiteral("alice"));
        QCOMPARE(r.value(1).toInt(), 100);
    }
};

QTEST_MAIN(TestSyncQuery)

#include "tst_syncquery.moc"
