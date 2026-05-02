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
/// On first start, snapshotIfNeeded() generates a changeset for all existing
/// rows so that pre-existing data syncs to other clients automatically.
class TestExistingDatabases : public QObject {
    Q_OBJECT

private:
    static int connId;
    QSqlDatabase createSyncDb(const QString &path) {
        QString connName = QStringLiteral("existdb_%1").arg(++connId);
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE_SYNC", connName);
        db.setDatabaseName(path);
        return db;
    }

    QSqlDatabase createPlainDb(const QString &path) {
        QString connName = QStringLiteral("existdb_%1").arg(++connId);
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
        for (const QString &name : QSqlDatabase::connectionNames()) {
            QSqlDatabase::removeDatabase(name);
        }
    }

    /// Two clients populate their databases independently using plain QSQLITE
    /// (no sync engine). They then switch to QSQLITE_SYNC and start syncing.
    /// Pre-existing data syncs via the automatic snapshot on first start.
    void testPreExistingDataSyncsViaSnapshot()
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

        // Phase 2: both clients switch to QSQLITE_SYNC and start syncing.
        // start() snapshots each client's pre-existing data.
        QSqlDatabase dbA = createSyncDb(pathA);
        QSqlDatabase dbB = createSyncDb(pathB);
        QVERIFY(dbA.open());
        QVERIFY(dbB.open());

        SyncEngine engineA(dbA, shared, "clientA");
        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // Three syncs: A picks up B's snapshot, B picks up A's snapshot,
        // then A picks up anything new from B. Two rounds are needed
        // because each client's snapshot is only visible after start().
        engineA.sync();
        engineB.sync();
        engineA.sync();

        // Both clients now have all 4 rows via snapshot exchange
        QSqlQuery checkA(dbA);
        checkA.exec("SELECT COUNT(*) FROM items");
        QVERIFY(checkA.next());
        QCOMPARE(checkA.value(0).toInt(), 4);

        QSqlQuery checkB(dbB);
        checkB.exec("SELECT COUNT(*) FROM items");
        QVERIFY(checkB.next());
        QCOMPARE(checkB.value(0).toInt(), 4);
    }

    /// After connecting, new writes sync alongside the snapshot data.
    /// Both clients end up with pre-existing rows from both sides plus
    /// any new writes.
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

        // Phase 2: connect with sync engines (snapshots happen on start)
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

        // Three syncs to fully exchange snapshots and new writes.
        engineA.sync();
        engineB.sync();
        engineA.sync();

        // A has: pre_existing_A (1), pre_existing_B (2), new_from_A (10), new_from_B (20)
        QSqlQuery checkA(dbA);
        checkA.exec("SELECT id, name FROM items ORDER BY id");
        QMap<int, QString> rowsA;
        while (checkA.next()) {
            rowsA[checkA.value(0).toInt()] = checkA.value(1).toString();
        }

        QCOMPARE(rowsA.size(), 4);
        QVERIFY(rowsA.contains(1));
        QVERIFY(rowsA.contains(2));
        QVERIFY(rowsA.contains(10));
        QVERIFY(rowsA.contains(20));

        // B has the same 4 rows
        QSqlQuery checkB(dbB);
        checkB.exec("SELECT id, name FROM items ORDER BY id");
        QMap<int, QString> rowsB;
        while (checkB.next()) {
            rowsB[checkB.value(0).toInt()] = checkB.value(1).toString();
        }

        QCOMPARE(rowsB.size(), 4);
        QVERIFY(rowsB.contains(1));
        QVERIFY(rowsB.contains(2));
        QVERIFY(rowsB.contains(10));
        QVERIFY(rowsB.contains(20));
    }

    /// If pre-existing data has overlapping primary keys, the snapshots
    /// trigger conflict resolution. After syncing, both clients converge
    /// to the same value.
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

        // Connect both - snapshots cause a CONFLICT on id=1
        QSqlDatabase dbA = createSyncDb(pathA);
        QSqlDatabase dbB = createSyncDb(pathB);
        QVERIFY(dbA.open());
        QVERIFY(dbB.open());

        SyncEngine engineA(dbA, shared, "clientA");
        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // Three syncs to fully exchange snapshots and resolve conflicts.
        engineA.sync();
        engineB.sync();
        engineA.sync();

        // Both clients must agree on id=1 (convergence)
        QSqlQuery checkA(dbA);
        checkA.exec("SELECT name FROM items WHERE id = 1");
        QVERIFY(checkA.next());
        QString valueA = checkA.value(0).toString();

        QSqlQuery checkB(dbB);
        checkB.exec("SELECT name FROM items WHERE id = 1");
        QVERIFY(checkB.next());
        QString valueB = checkB.value(0).toString();

        QCOMPARE(valueA, valueB);
    }

    /// A client with an empty database connects while the other has
    /// pre-existing data. The snapshot delivers A's data to B.
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

        // B gets A's pre-existing data via snapshot
        engineB.sync();

        QSqlQuery checkB(dbB);
        checkB.exec("SELECT COUNT(*) FROM items");
        QVERIFY(checkB.next());
        QCOMPARE(checkB.value(0).toInt(), 2);

        // New writes from A also sync
        QSqlQuery qA(dbA);
        qA.exec("INSERT INTO items (id, name) VALUES (3, 'new_write')");
        QCoreApplication::processEvents();

        engineB.sync();

        checkB.exec("SELECT COUNT(*) FROM items");
        QVERIFY(checkB.next());
        QCOMPARE(checkB.value(0).toInt(), 3);
    }

    /// Updates to pre-existing rows produce changesets and sync. B now
    /// has the row via snapshot, so the UPDATE applies cleanly.
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

        // Connect both - A's snapshot delivers the row to B
        QSqlDatabase dbA = createSyncDb(pathA);
        QSqlDatabase dbB = createSyncDb(pathB);
        QVERIFY(dbA.open());
        QVERIFY(dbB.open());

        SyncEngine engineA(dbA, shared, "clientA");
        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // B syncs to pick up A's snapshot
        engineB.sync();

        // B now has the row via snapshot
        QSqlQuery checkB(dbB);
        checkB.exec("SELECT COUNT(*) FROM items");
        QVERIFY(checkB.next());
        QCOMPARE(checkB.value(0).toInt(), 1);

        // A updates its pre-existing row
        QSqlQuery qA(dbA);
        qA.exec("UPDATE items SET name = 'modified' WHERE id = 1");
        QCoreApplication::processEvents();

        // B syncs - the UPDATE applies cleanly since B has the row
        engineB.sync();

        checkB.exec("SELECT name FROM items WHERE id = 1");
        QVERIFY(checkB.next());
        QCOMPARE(checkB.value(0).toString(), QStringLiteral("modified"));
    }

    /// Deleting a pre-existing row produces a changeset. B has the row
    /// via snapshot, so the DELETE applies and removes the row.
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

        // B picks up A's snapshot (has the row now)
        engineB.sync();
        {
            QSqlQuery check(dbB);
            check.exec("SELECT COUNT(*) FROM items");
            QVERIFY(check.next());
            QCOMPARE(check.value(0).toInt(), 1);
        }

        // A deletes the pre-existing row
        QSqlQuery qA(dbA);
        qA.exec("DELETE FROM items WHERE id = 1");
        QCoreApplication::processEvents();

        // B syncs - DELETE applies and removes the row
        QList<QPair<SyncEngine::SyncError, QString>> errors;
        QObject::connect(&engineB, &SyncEngine::syncErrorOccurred,
                         [&](SyncEngine::SyncError e, const QString &msg) {
            errors.append({e, msg});
        });

        engineB.sync();

        // No errors
        QVERIFY2(errors.isEmpty(),
                 qPrintable(errors.isEmpty() ? "" : errors[0].second));

        // Row is gone
        QSqlQuery checkB(dbB);
        checkB.exec("SELECT COUNT(*) FROM items");
        QVERIFY(checkB.next());
        QCOMPARE(checkB.value(0).toInt(), 0);
    }
};

int TestExistingDatabases::connId = 0;

QTEST_MAIN(TestExistingDatabases)

#include "tst_existing_databases.moc"
