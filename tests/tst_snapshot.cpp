#include <QTest>
#include <QTemporaryDir>

#include "syncengine/SyncEngine.h"

using namespace syncengine;

/// Tests for the automatic snapshot feature: when a SyncEngine starts for the
/// first time against a shared folder, it snapshots all existing user-table
/// rows so that other clients can pick them up.
class TestSnapshot : public QObject {
    Q_OBJECT

private slots:
    /// Engine with pre-existing data writes a snapshot changeset on first start.
    void testSnapshotOnFirstStart()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engine(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engine.start(0));

        engine.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engine.stop();

        // Re-populate outside the engine so rows are pre-existing
        {
            SyncableDatabase db(tmpDir.filePath("a.db"), "clientA");
            QVERIFY(db.open());
            db.exec("INSERT INTO items (id, name) VALUES (1, 'alpha')");
            db.exec("INSERT INTO items (id, name) VALUES (2, 'beta')");
            db.close();
        }

        // Start a fresh engine on the same db but a NEW shared folder
        // so it looks like a first start.
        QString shared2 = tmpDir.filePath("shared2");
        SyncEngine engine2(tmpDir.filePath("a.db"), shared2, "clientA");
        QVERIFY(engine2.start(0));

        // A second client should see the snapshot
        SyncEngine engineB(tmpDir.filePath("b.db"), shared2, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        int applied = engineB.sync();
        QVERIFY(applied > 0);

        auto rows = engineB.database()->query("SELECT COUNT(*) FROM items");
        QCOMPARE(rows[0][0], QStringLiteral("2"));
    }

    /// Stopping and restarting with the same shared folder does not produce
    /// a second snapshot (the client already has changesets there).
    void testSnapshotNotRepeated()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engine(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engine.start(0));

        engine.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engine.beginWrite();
        engine.database()->exec("INSERT INTO items (id, name) VALUES (1, 'one')");
        engine.endWrite();
        engine.stop();

        // Restart against the same shared folder
        SyncEngine engine2(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engine2.start(0));

        // Client B should see exactly 1 changeset (the original write),
        // not a duplicate snapshot.
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        int applied = engineB.sync();
        QCOMPARE(applied, 1);

        auto rows = engineB.database()->query("SELECT COUNT(*) FROM items");
        QCOMPARE(rows[0][0], QStringLiteral("1"));
    }

    /// Client B gets A's pre-existing data via the automatic snapshot.
    void testSnapshotSyncsToSecondClient()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // A creates table and inserts data, then stops
        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engineA.start(0));
        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineA.stop();

        // Insert rows outside the engine (pre-existing data)
        {
            SyncableDatabase db(tmpDir.filePath("a.db"), "clientA");
            QVERIFY(db.open());
            db.exec("INSERT INTO items (id, name) VALUES (1, 'hello')");
            db.exec("INSERT INTO items (id, name) VALUES (2, 'world')");
            db.close();
        }

        // Restart A with a new shared folder so snapshot fires
        QString shared2 = tmpDir.filePath("shared2");
        SyncEngine engineA2(tmpDir.filePath("a.db"), shared2, "clientA");
        QVERIFY(engineA2.start(0));

        // B connects and syncs
        SyncEngine engineB(tmpDir.filePath("b.db"), shared2, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        engineB.sync();

        auto rows = engineB.database()->query(
            "SELECT id, name FROM items ORDER BY id");
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0][0], QStringLiteral("1"));
        QCOMPARE(rows[0][1], QStringLiteral("hello"));
        QCOMPARE(rows[1][0], QStringLiteral("2"));
        QCOMPARE(rows[1][1], QStringLiteral("world"));
    }

    /// Snapshot includes all user tables, not just the first one.
    void testSnapshotWithMultipleTables()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engineA.start(0));
        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineA.database()->exec(
            "CREATE TABLE tags (id INTEGER PRIMARY KEY, label TEXT)");
        engineA.stop();

        // Pre-existing data in both tables
        {
            SyncableDatabase db(tmpDir.filePath("a.db"), "clientA");
            QVERIFY(db.open());
            db.exec("INSERT INTO items (id, name) VALUES (1, 'item1')");
            db.exec("INSERT INTO tags (id, label) VALUES (1, 'tag1')");
            db.exec("INSERT INTO tags (id, label) VALUES (2, 'tag2')");
            db.close();
        }

        QString shared2 = tmpDir.filePath("shared2");
        SyncEngine engineA2(tmpDir.filePath("a.db"), shared2, "clientA");
        QVERIFY(engineA2.start(0));

        SyncEngine engineB(tmpDir.filePath("b.db"), shared2, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineB.database()->exec(
            "CREATE TABLE tags (id INTEGER PRIMARY KEY, label TEXT)");

        engineB.sync();

        auto items = engineB.database()->query("SELECT COUNT(*) FROM items");
        QCOMPARE(items[0][0], QStringLiteral("1"));

        auto tags = engineB.database()->query("SELECT COUNT(*) FROM tags");
        QCOMPARE(tags[0][0], QStringLiteral("2"));
    }

    /// An empty database (no user tables) produces no snapshot changeset.
    void testSnapshotSkipsEmptyDatabase()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // A starts with no tables at all
        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engineA.start(0));

        // B should see nothing
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineB.start(0));

        int applied = engineB.sync();
        QCOMPARE(applied, 0);
    }

    /// The snapshot excludes internal _sync_* metadata tables.
    void testSnapshotExcludesSyncMetadata()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engineA.start(0));
        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO items (id, name) VALUES (1, 'x')");
        engineA.endWrite();
        engineA.stop();

        // _sync_applied, _sync_client etc. now have rows. Restart with a
        // new shared folder so a snapshot is generated.
        QString shared2 = tmpDir.filePath("shared2");
        SyncEngine engineA2(tmpDir.filePath("a.db"), shared2, "clientA");
        QVERIFY(engineA2.start(0));

        // B only has the items table - if sync metadata leaked into the
        // snapshot, apply would fail or insert unexpected rows.
        SyncEngine engineB(tmpDir.filePath("b.db"), shared2, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        engineB.sync();

        auto rows = engineB.database()->query("SELECT COUNT(*) FROM items");
        QCOMPARE(rows[0][0], QStringLiteral("1"));
    }

    /// FTS5 shadow tables (e.g., MyFTS_content) are excluded from the snapshot.
    void testSnapshotExcludesFTSShadowTables()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engineA.start(0));
        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineA.database()->exec(
            "CREATE VIRTUAL TABLE items_fts USING fts5(name, content=items, content_rowid=id)");
        engineA.stop();

        {
            SyncableDatabase db(tmpDir.filePath("a.db"), "clientA");
            QVERIFY(db.open());
            db.exec("INSERT INTO items (id, name) VALUES (1, 'searchable')");
            // Populate FTS index
            db.exec("INSERT INTO items_fts(items_fts) VALUES('rebuild')");
            db.close();
        }

        QString shared2 = tmpDir.filePath("shared2");
        SyncEngine engineA2(tmpDir.filePath("a.db"), shared2, "clientA");
        QVERIFY(engineA2.start(0));

        // B has the items table but NOT the FTS table.
        // If shadow tables leaked into the snapshot, apply would fail.
        SyncEngine engineB(tmpDir.filePath("b.db"), shared2, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        QStringList errors;
        QObject::connect(&engineB, &SyncEngine::syncErrorOccurred,
                         [&](SyncEngine::SyncError, const QString &msg) {
            errors.append(msg);
        });

        engineB.sync();

        auto rows = engineB.database()->query("SELECT COUNT(*) FROM items");
        QCOMPARE(rows[0][0], QStringLiteral("1"));
        QVERIFY2(errors.isEmpty(),
                 qPrintable(errors.isEmpty() ? "" : errors[0]));
    }

    /// Both clients have pre-existing data with overlapping keys. Both
    /// produce snapshots on start, and conflict resolution converges.
    void testSnapshotWithOverlappingKeys()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // Set up A with pre-existing data
        {
            SyncableDatabase db(tmpDir.filePath("a.db"), "clientA");
            QVERIFY(db.open());
            db.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            db.exec("INSERT INTO items (id, name) VALUES (1, 'from_A')");
            db.close();
        }

        // Set up B with overlapping key
        {
            SyncableDatabase db(tmpDir.filePath("b.db"), "clientB");
            QVERIFY(db.open());
            db.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
            db.exec("INSERT INTO items (id, name) VALUES (1, 'from_B')");
            db.close();
        }

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // Three syncs to fully exchange snapshots and resolve conflicts.
        engineA.sync();
        engineB.sync();
        engineA.sync();

        // Both must converge to the same value
        auto rowsA = engineA.database()->query(
            "SELECT name FROM items WHERE id = 1");
        auto rowsB = engineB.database()->query(
            "SELECT name FROM items WHERE id = 1");
        QCOMPARE(rowsA.size(), 1);
        QCOMPARE(rowsB.size(), 1);
        QCOMPARE(rowsA[0][0], rowsB[0][0]);
    }

    /// Unit-test SyncableDatabase::generateSnapshot() directly.
    void testGenerateSnapshotDirect()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncableDatabase db(tmpDir.filePath("test.db"), "client1");
        QVERIFY(db.open());

        db.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        db.exec("INSERT INTO items (id, name) VALUES (1, 'one')");
        db.exec("INSERT INTO items (id, name) VALUES (2, 'two')");
        db.exec("INSERT INTO items (id, name) VALUES (3, 'three')");

        QByteArray snapshot = db.generateSnapshot({"items"});
        QVERIFY(!snapshot.isEmpty());

        // Apply the snapshot to a second database
        SyncableDatabase db2(tmpDir.filePath("test2.db"), "client2");
        QVERIFY(db2.open());
        db2.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        QVERIFY(db2.applyChangeset(snapshot));

        auto rows = db2.query("SELECT COUNT(*) FROM items");
        QCOMPARE(rows[0][0], QStringLiteral("3"));

        // Empty table list produces empty snapshot
        QByteArray emptySnap = db.generateSnapshot({});
        QVERIFY(emptySnap.isEmpty());
    }
};

QTEST_MAIN(TestSnapshot)

#include "tst_snapshot.moc"
