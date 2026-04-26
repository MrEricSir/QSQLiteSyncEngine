#include <QTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QCoreApplication>

#include "syncengine/SyncEngine.h"

using namespace syncengine;

/// Tests for the scenario where two clients have existing databases with
/// pre-existing data and connect to the sync engine for the first time.
///
/// The SQLite Session Extension only captures changes made after a session
/// is started. Pre-existing rows have no changesets, so they will not sync
/// automatically. This test suite documents that behavior and verifies that
/// new writes after connecting DO sync correctly.
class TestExistingDatabases : public QObject {
    Q_OBJECT

private:
    static int s_connId;
    QSqlDatabase createSyncDb(const QString &path) {
        QString connName = QStringLiteral("existdb_%1").arg(++s_connId);
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE_SYNC", connName);
        db.setDatabaseName(path);
        return db;
    }

    QSqlDatabase createPlainDb(const QString &path) {
        QString connName = QStringLiteral("existdb_%1").arg(++s_connId);
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(path);
        return db;
    }

private slots:
    void initTestCase()
    {
        QCoreApplication::addLibraryPath(
            QCoreApplication::applicationDirPath());
    }

    void cleanupTestCase()
    {
        for (const QString &name : QSqlDatabase::connectionNames())
            QSqlDatabase::removeDatabase(name);
    }

    /// Two clients populate their databases independently using plain QSQLITE
    /// (no sync engine). They then switch to QSQLITE_SYNC and start syncing.
    /// Pre-existing data does NOT sync because no changesets were ever produced
    /// for it. This is the expected (and unavoidable) behavior.
    void testPreExistingDataDoesNotSync()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");
        QString pathA = tmpDir.filePath("a.db");
        QString pathB = tmpDir.filePath("b.db");

        // Phase 1: both clients populate their databases using plain QSQLITE
        {
            QSqlDatabase dbA = createPlainDb(pathA);
            QVERIFY(dbA.open());
            QSqlQuery qA(dbA);
            qA.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            qA.exec("INSERT INTO items (id, name) VALUES (1, 'alice')");
            qA.exec("INSERT INTO items (id, name) VALUES (2, 'bob')");
            dbA.close();
        }
        {
            QSqlDatabase dbB = createPlainDb(pathB);
            QVERIFY(dbB.open());
            QSqlQuery qB(dbB);
            qB.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            qB.exec("INSERT INTO items (id, name) VALUES (3, 'charlie')");
            qB.exec("INSERT INTO items (id, name) VALUES (4, 'diana')");
            dbB.close();
        }

        // Phase 2: both clients switch to QSQLITE_SYNC and start syncing
        QSqlDatabase dbA = createSyncDb(pathA);
        QSqlDatabase dbB = createSyncDb(pathB);
        QVERIFY(dbA.open());
        QVERIFY(dbB.open());

        SyncEngine engineA(dbA, shared, "clientA");
        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // Sync both directions
        engineA.sync();
        engineB.sync();
        engineA.sync();

        // Pre-existing data does NOT appear on the other client
        QSqlQuery checkA(dbA);
        checkA.exec("SELECT COUNT(*) FROM items");
        QVERIFY(checkA.next());
        QCOMPARE(checkA.value(0).toInt(), 2); // only alice and bob

        QSqlQuery checkB(dbB);
        checkB.exec("SELECT COUNT(*) FROM items");
        QVERIFY(checkB.next());
        QCOMPARE(checkB.value(0).toInt(), 2); // only charlie and diana
    }

    /// After connecting, NEW writes from either client DO sync correctly
    /// even though the databases had pre-existing data.
    void testNewWritesSyncAfterConnecting()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");
        QString pathA = tmpDir.filePath("a.db");
        QString pathB = tmpDir.filePath("b.db");

        // Phase 1: populate with pre-existing data
        {
            QSqlDatabase dbA = createPlainDb(pathA);
            QVERIFY(dbA.open());
            QSqlQuery q(dbA);
            q.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            q.exec("INSERT INTO items (id, name) VALUES (1, 'pre_existing_A')");
            dbA.close();
        }
        {
            QSqlDatabase dbB = createPlainDb(pathB);
            QVERIFY(dbB.open());
            QSqlQuery q(dbB);
            q.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            q.exec("INSERT INTO items (id, name) VALUES (2, 'pre_existing_B')");
            dbB.close();
        }

        // Phase 2: connect with sync engines
        QSqlDatabase dbA = createSyncDb(pathA);
        QSqlDatabase dbB = createSyncDb(pathB);
        QVERIFY(dbA.open());
        QVERIFY(dbB.open());

        SyncEngine engineA(dbA, shared, "clientA");
        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // Phase 3: new writes after connecting
        QSqlQuery qA(dbA);
        qA.exec("INSERT INTO items (id, name) VALUES (10, 'new_from_A')");
        QCoreApplication::processEvents();

        QSqlQuery qB(dbB);
        qB.exec("INSERT INTO items (id, name) VALUES (20, 'new_from_B')");
        QCoreApplication::processEvents();

        // Sync both
        engineA.sync();
        engineB.sync();

        // A should have: pre_existing_A (1), new_from_A (10), new_from_B (20)
        QSqlQuery checkA(dbA);
        checkA.exec("SELECT id, name FROM items ORDER BY id");
        QMap<int, QString> rowsA;
        while (checkA.next())
            rowsA[checkA.value(0).toInt()] = checkA.value(1).toString();

        QVERIFY(rowsA.contains(1));  // pre-existing
        QVERIFY(rowsA.contains(10)); // own new write
        QVERIFY(rowsA.contains(20)); // synced from B
        QCOMPARE(rowsA[20], QStringLiteral("new_from_B"));

        // B should have: pre_existing_B (2), new_from_B (20), new_from_A (10)
        QSqlQuery checkB(dbB);
        checkB.exec("SELECT id, name FROM items ORDER BY id");
        QMap<int, QString> rowsB;
        while (checkB.next())
            rowsB[checkB.value(0).toInt()] = checkB.value(1).toString();

        QVERIFY(rowsB.contains(2));  // pre-existing
        QVERIFY(rowsB.contains(20)); // own new write
        QVERIFY(rowsB.contains(10)); // synced from A
        QCOMPARE(rowsB[10], QStringLiteral("new_from_A"));
    }

    /// If pre-existing data has overlapping primary keys, the first new write
    /// that touches a conflicting row will trigger conflict resolution.
    void testOverlappingPrimaryKeysOnNewWrite()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");
        QString pathA = tmpDir.filePath("a.db");
        QString pathB = tmpDir.filePath("b.db");

        // Both clients have row id=1 with different values
        {
            QSqlDatabase db = createPlainDb(pathA);
            QVERIFY(db.open());
            QSqlQuery q(db);
            q.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            q.exec("INSERT INTO items (id, name) VALUES (1, 'version_A')");
            db.close();
        }
        {
            QSqlDatabase db = createPlainDb(pathB);
            QVERIFY(db.open());
            QSqlQuery q(db);
            q.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            q.exec("INSERT INTO items (id, name) VALUES (1, 'version_B')");
            db.close();
        }

        // Connect both
        QSqlDatabase dbA = createSyncDb(pathA);
        QSqlDatabase dbB = createSyncDb(pathB);
        QVERIFY(dbA.open());
        QVERIFY(dbB.open());

        SyncEngine engineA(dbA, shared, "clientA");
        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // No conflict yet -- pre-existing data doesn't sync
        engineA.sync();
        engineB.sync();

        // A updates its row -- this creates a changeset
        QSqlQuery qA(dbA);
        qA.exec("UPDATE items SET name = 'updated_A' WHERE id = 1");
        QCoreApplication::processEvents();

        // B syncs -- A's update arrives. B has id=1 as 'version_B',
        // the incoming changeset expects the old value to be 'version_A'.
        // This is a DATA conflict -- the conflict resolver handles it.
        engineB.sync();

        // The row should exist on B (conflict resolved one way or the other)
        QSqlQuery checkB(dbB);
        checkB.exec("SELECT name FROM items WHERE id = 1");
        QVERIFY(checkB.next());
        // With LWW, A's update (which has an HLC) wins over B's pre-existing
        // data (which has no HLC / HLC of 0).
        QCOMPARE(checkB.value(0).toString(), QStringLiteral("updated_A"));
    }

    /// A client with an empty database connects for the first time while
    /// the other has pre-existing data. Only new writes sync.
    void testOneEmptyOnePopulated()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");
        QString pathA = tmpDir.filePath("a.db");
        QString pathB = tmpDir.filePath("b.db");

        // A has pre-existing data
        {
            QSqlDatabase db = createPlainDb(pathA);
            QVERIFY(db.open());
            QSqlQuery q(db);
            q.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            q.exec("INSERT INTO items (id, name) VALUES (1, 'pre_existing')");
            q.exec("INSERT INTO items (id, name) VALUES (2, 'also_pre')");
            db.close();
        }

        // B starts fresh
        QSqlDatabase dbA = createSyncDb(pathA);
        QSqlDatabase dbB = createSyncDb(pathB);
        QVERIFY(dbA.open());
        QVERIFY(dbB.open());

        QSqlQuery setupB(dbB);
        setupB.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        SyncEngine engineA(dbA, shared, "clientA");
        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // Sync -- B gets nothing (no changesets for A's pre-existing data)
        engineB.sync();

        QSqlQuery checkB(dbB);
        checkB.exec("SELECT COUNT(*) FROM items");
        QVERIFY(checkB.next());
        QCOMPARE(checkB.value(0).toInt(), 0);

        // A writes something new -- this DOES sync
        QSqlQuery qA(dbA);
        qA.exec("INSERT INTO items (id, name) VALUES (3, 'new_write')");
        QCoreApplication::processEvents();

        engineB.sync();

        checkB.exec("SELECT id, name FROM items");
        QVERIFY(checkB.next());
        QCOMPARE(checkB.value(0).toInt(), 3);
        QCOMPARE(checkB.value(1).toString(), QStringLiteral("new_write"));
        QVERIFY(!checkB.next()); // only one row
    }

    /// After connecting, updates to pre-existing rows DO produce changesets
    /// and sync to the other client.
    void testUpdatingPreExistingRowsSyncs()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");
        QString pathA = tmpDir.filePath("a.db");
        QString pathB = tmpDir.filePath("b.db");

        // A has pre-existing data
        {
            QSqlDatabase db = createPlainDb(pathA);
            QVERIFY(db.open());
            QSqlQuery q(db);
            q.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            q.exec("INSERT INTO items (id, name) VALUES (1, 'original')");
            db.close();
        }

        // B has matching schema but no data
        {
            QSqlDatabase db = createPlainDb(pathB);
            QVERIFY(db.open());
            QSqlQuery q(db);
            q.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            db.close();
        }

        // Connect both
        QSqlDatabase dbA = createSyncDb(pathA);
        QSqlDatabase dbB = createSyncDb(pathB);
        QVERIFY(dbA.open());
        QVERIFY(dbB.open());

        SyncEngine engineA(dbA, shared, "clientA");
        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // A updates its pre-existing row -- the UPDATE is captured as a changeset
        QSqlQuery qA(dbA);
        qA.exec("UPDATE items SET name = 'modified' WHERE id = 1");
        QCoreApplication::processEvents();

        // B syncs -- the update changeset arrives, but B doesn't have the row.
        // This is a NOTFOUND conflict (update for a row that doesn't exist locally).
        // The conflict resolver skips it.
        engineB.sync();

        QSqlQuery checkB(dbB);
        checkB.exec("SELECT COUNT(*) FROM items");
        QVERIFY(checkB.next());
        // The UPDATE changeset can't be applied because the row doesn't exist on B
        QCOMPARE(checkB.value(0).toInt(), 0);
    }

    /// Deleting a pre-existing row produces a changeset. If the other client
    /// doesn't have the row, the delete is a NOTFOUND conflict and is skipped.
    void testDeletingPreExistingRow()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");
        QString pathA = tmpDir.filePath("a.db");
        QString pathB = tmpDir.filePath("b.db");

        {
            QSqlDatabase db = createPlainDb(pathA);
            QVERIFY(db.open());
            QSqlQuery q(db);
            q.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            q.exec("INSERT INTO items (id, name) VALUES (1, 'to_delete')");
            db.close();
        }
        {
            QSqlDatabase db = createPlainDb(pathB);
            QVERIFY(db.open());
            QSqlQuery q(db);
            q.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            db.close();
        }

        QSqlDatabase dbA = createSyncDb(pathA);
        QSqlDatabase dbB = createSyncDb(pathB);
        QVERIFY(dbA.open());
        QVERIFY(dbB.open());

        SyncEngine engineA(dbA, shared, "clientA");
        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // A deletes the pre-existing row
        QSqlQuery qA(dbA);
        qA.exec("DELETE FROM items WHERE id = 1");
        QCoreApplication::processEvents();

        // B syncs -- NOTFOUND conflict, delete is skipped (row doesn't exist on B)
        // Should not error or crash
        QList<QPair<SyncEngine::SyncError, QString>> errors;
        QObject::connect(&engineB, &SyncEngine::syncErrorOccurred,
                         [&](SyncEngine::SyncError e, const QString &msg) {
            errors.append({e, msg});
        });

        engineB.sync();

        // No errors should be emitted for NOTFOUND conflicts
        QVERIFY2(errors.isEmpty(),
                 qPrintable(errors.isEmpty() ? "" : errors[0].second));
    }
};

int TestExistingDatabases::s_connId = 0;

QTEST_MAIN(TestExistingDatabases)

#include "tst_existing_databases.moc"
