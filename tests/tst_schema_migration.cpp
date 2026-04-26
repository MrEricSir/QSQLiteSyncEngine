#include <QTest>
#include <QTemporaryDir>
#include <QSignalSpy>

#include "syncengine/SyncEngine.h"

using namespace syncengine;

/// Tests that verify schema migration mismatches are detected and surfaced.
///
/// Scenario: two machines share a sync folder. Machine A upgrades to a new
/// schema (adds columns, adds tables) while Machine B is still on the old
/// schema. The sync engine must detect and report these mismatches rather
/// than silently dropping data.
class TestSchemaMigration : public QObject {
    Q_OBJECT
private slots:

    /// Machine A adds a column then writes a row. Machine B (old schema)
    /// syncs. The changeset has more columns than B's table, so it gets
    /// skipped. The sync engine must report this via syncError.
    void testNewColumnOnWriterDetectedOnOldReader()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // Both start with the same v1 schema
        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        // Machine A upgrades to v2: adds a "priority" column
        engineA.database()->exec("ALTER TABLE items ADD COLUMN priority INTEGER DEFAULT 0");

        // A writes a row (changeset now has 3 columns: id, name, priority)
        engineA.beginWrite();
        engineA.database()->exec(
            "INSERT INTO items (id, name, priority) VALUES (1, 'task1', 5)");
        engineA.endWrite();

        // B syncs -- should detect the schema mismatch and report it
        bool errorEmitted = false;
        QString errorMsg;
        QObject::connect(&engineB, &SyncEngine::syncErrorOccurred, [&](SyncEngine::SyncError, const QString &msg) {
            errorEmitted = true;
            errorMsg = msg;
        });

        int applied = engineB.sync();

        QCOMPARE(applied, 0);
        QVERIFY2(errorEmitted,
                 "Expected syncError when changeset has more columns than target table");
        QVERIFY2(errorMsg.contains("items"),
                 qPrintable("Error message should mention the table name: " + errorMsg));

        // Data is not on B -- confirmed lost, but we detected it
        auto rows = engineB.database()->query("SELECT id, name FROM items");
        QCOMPARE(rows.size(), 0);
    }

    /// Machine B (old schema) writes a row. Machine A (new schema with extra
    /// column) syncs. This direction works because the changeset has fewer
    /// columns than the target table -- trailing columns get defaults.
    void testOldWriterToNewReaderLosesDefaults()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // B has the old schema (v1)
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        // A has the new schema (v2) with a NOT NULL column and default
        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, "
            "priority INTEGER NOT NULL DEFAULT 0)");

        // B inserts a row (no priority column in changeset)
        engineB.beginWrite();
        engineB.database()->exec("INSERT INTO items (id, name) VALUES (1, 'task1')");
        engineB.endWrite();

        // A syncs -- changeset has 2 columns, table has 3. This works because
        // trailing columns get defaults. Verify the default is actually applied.
        engineA.sync();

        auto rows = engineA.database()->query(
            "SELECT id, name, priority FROM items WHERE id = 1");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][1], QStringLiteral("task1"));
        // The priority column should have the DEFAULT value (0), not NULL
        QCOMPARE(rows[0][2], QStringLiteral("0"));
    }

    /// Machine A creates a brand new table and writes data to it.
    /// Machine B (old version) doesn't have the table at all.
    /// The sync engine must detect and report this.
    void testNewTableOnWriterDetectedOnOldReader()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // Both have v1 schema
        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        // A upgrades: adds a new "tags" table
        engineA.database()->exec(
            "CREATE TABLE tags (id INTEGER PRIMARY KEY, item_id INTEGER, tag TEXT)");

        // A writes to the new table
        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO tags (id, item_id, tag) VALUES (1, 1, 'urgent')");
        engineA.endWrite();

        // B syncs -- "tags" table doesn't exist, should be reported
        bool errorEmitted = false;
        QString errorMsg;
        QObject::connect(&engineB, &SyncEngine::syncErrorOccurred, [&](SyncEngine::SyncError, const QString &msg) {
            errorEmitted = true;
            errorMsg = msg;
        });

        int applied = engineB.sync();

        QCOMPARE(applied, 0);
        QVERIFY2(errorEmitted,
                 "Expected syncError when changeset references a table that doesn't exist");
        QVERIFY2(errorMsg.contains("tags"),
                 qPrintable("Error message should mention the missing table: " + errorMsg));
    }

    /// Both machines write to the same table, but A has an extra column.
    /// The sync engine must detect that A's changesets can't be applied on B
    /// and report the mismatch. B's changesets should still apply fine on A.
    void testBidirectionalSyncWithSchemaMismatchDetected()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // A has v2 schema, B has v1
        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, priority INTEGER DEFAULT 0)");
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        // A inserts row 1
        engineA.beginWrite();
        engineA.database()->exec(
            "INSERT INTO items (id, name, priority) VALUES (1, 'from_A', 5)");
        engineA.endWrite();

        // B inserts row 2
        engineB.beginWrite();
        engineB.database()->exec("INSERT INTO items (id, name) VALUES (2, 'from_B')");
        engineB.endWrite();

        // Track errors on both sides
        QStringList errorsA, errorsB;
        QObject::connect(&engineA, &SyncEngine::syncErrorOccurred, [&](SyncEngine::SyncError, const QString &msg) {
            errorsA.append(msg);
        });
        QObject::connect(&engineB, &SyncEngine::syncErrorOccurred, [&](SyncEngine::SyncError, const QString &msg) {
            errorsB.append(msg);
        });

        // Sync both
        engineA.sync();
        engineB.sync();

        // A should have both rows (B's changeset has fewer cols -- works fine)
        auto rowsA = engineA.database()->query(
            "SELECT id, name FROM items ORDER BY id");
        QCOMPARE(rowsA.size(), 2);
        QVERIFY(errorsA.isEmpty()); // No errors on A's side

        // B should report schema mismatch for A's changeset
        QVERIFY2(!errorsB.isEmpty(),
                 "Expected syncError on B when receiving A's changeset with extra columns");

        // B only has its own row -- A's was detected as incompatible
        auto rowsB = engineB.database()->query(
            "SELECT id, name FROM items ORDER BY id");
        QCOMPARE(rowsB.size(), 1);
        QCOMPARE(rowsB[0][1], QStringLiteral("from_B"));
    }

    /// Machine A adds a NOT NULL column without a default. B's old-schema
    /// changesets applied to A will leave the column as NULL, but SQLite uses
    /// the column default. With no default and NOT NULL, this may cause issues.
    void testNotNullConstraintFromOldChangeset()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        // B has v1
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        // A has v2 with a NOT NULL column (no default)
        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, "
            "status TEXT NOT NULL)");

        // B writes a row (changeset has 2 columns)
        engineB.beginWrite();
        engineB.database()->exec("INSERT INTO items (id, name) VALUES (1, 'task1')");
        engineB.endWrite();

        // A syncs -- changeset has 2 cols, table has 3.
        int applied = engineA.sync();

        // Either the apply should fail (applied == 0) or the row should not
        // exist due to the NOT NULL violation
        auto rows = engineA.database()->query("SELECT * FROM items");
        if (!rows.isEmpty()) {
            // If it was inserted, the NOT NULL constraint was violated
            QFAIL("Row was inserted despite NOT NULL column having no value -- "
                   "constraint violation went undetected");
        }
    }

    /// Verify that the sync engine emits syncError when changesets are
    /// skipped due to schema incompatibility.
    void testErrorSignalOnSchemaMismatch()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, extra TEXT)");
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO items (id, name, extra) VALUES (1, 'x', 'y')");
        engineA.endWrite();

        // Track whether syncError was emitted
        bool errorEmitted = false;
        QObject::connect(&engineB, &SyncEngine::syncErrorOccurred, [&](SyncEngine::SyncError, const QString &) {
            errorEmitted = true;
        });

        engineB.sync();

        QVERIFY2(errorEmitted,
                 "Sync engine must emit syncError when a changeset is skipped "
                 "due to schema mismatch");
    }
};

QTEST_MAIN(TestSchemaMigration)

#include "tst_schema_migration.moc"
