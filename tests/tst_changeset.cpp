#include <QTest>
#include <QDir>
#include <QTemporaryDir>

#include "syncengine/SyncableDatabase.h"
#include "syncengine/ChangesetManager.h"
#include "syncengine/SharedFolderTransport.h"

using namespace syncengine;

class TestChangeset : public QObject {
    Q_OBJECT
private slots:
    void testSessionCapturesInsert()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncableDatabase db(tmpDir.filePath("test.db"), "client1");
        QVERIFY(db.open());

        db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        QVERIFY(db.beginSession());
        db.exec("INSERT INTO t (val) VALUES ('hello')");
        QByteArray changeset = db.endSession();

        QVERIFY(!changeset.isEmpty());
    }

    void testSessionCapturesUpdate()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncableDatabase db(tmpDir.filePath("test.db"), "client1");
        QVERIFY(db.open());

        db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");
        db.exec("INSERT INTO t (val) VALUES ('hello')");

        QVERIFY(db.beginSession());
        db.exec("UPDATE t SET val = 'world' WHERE id = 1");
        QByteArray changeset = db.endSession();

        QVERIFY(!changeset.isEmpty());
    }

    void testSessionCapturesDelete()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncableDatabase db(tmpDir.filePath("test.db"), "client1");
        QVERIFY(db.open());

        db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");
        db.exec("INSERT INTO t (val) VALUES ('hello')");

        QVERIFY(db.beginSession());
        db.exec("DELETE FROM t WHERE id = 1");
        QByteArray changeset = db.endSession();

        QVERIFY(!changeset.isEmpty());
    }

    void testEmptySessionProducesEmptyChangeset()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncableDatabase db(tmpDir.filePath("test.db"), "client1");
        QVERIFY(db.open());

        db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        QVERIFY(db.beginSession());
        // No changes
        QByteArray changeset = db.endSession();

        QVERIFY(changeset.isEmpty());
    }

    void testApplyChangesetToAnotherDb()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        // Source database
        SyncableDatabase dbA(tmpDir.filePath("a.db"), "clientA");
        QVERIFY(dbA.open());
        dbA.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        // Destination database
        SyncableDatabase dbB(tmpDir.filePath("b.db"), "clientB");
        QVERIFY(dbB.open());
        dbB.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        // Insert on A
        QVERIFY(dbA.beginSession());
        dbA.exec("INSERT INTO t (id, val) VALUES (1, 'from_A')");
        QByteArray changeset = dbA.endSession();
        QVERIFY(!changeset.isEmpty());

        // Apply to B
        QVERIFY(dbB.applyChangeset(changeset));

        // Verify B has the data
        auto rows = dbB.query("SELECT val FROM t WHERE id = 1");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("from_A"));
    }

    void testFilenameRoundTrip()
    {
        QString filename = ChangesetManager::buildFilename("client-abc", 123456789ULL, 42);
        ChangesetInfo info = ChangesetManager::parseFilename(filename);

        QCOMPARE(info.clientId, QStringLiteral("client-abc"));
        QCOMPARE(info.hlc, 123456789ULL);
        QCOMPARE(info.sequence, 42ULL);
        QCOMPARE(info.filename, filename);
    }

    void testChangesetWriteAndList()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString sharedDir = tmpDir.filePath("shared");
        SyncableDatabase db(tmpDir.filePath("test.db"), "client1");
        QVERIFY(db.open());

        SharedFolderTransport transport(sharedDir);
        ChangesetManager mgr(&db, &transport);

        QByteArray fakeChangeset("changeset-data");
        QString filename = mgr.writeChangeset(fakeChangeset, 100);
        QVERIFY(!filename.isEmpty());

        QStringList files = transport.listChangesets();
        QCOMPARE(files.size(), 1);
        QCOMPARE(files[0], filename);

        // Should be marked as applied (it's our own)
        QVERIFY(db.isChangesetApplied(filename));
    }
};

QTEST_MAIN(TestChangeset)

#include "tst_changeset.moc"
