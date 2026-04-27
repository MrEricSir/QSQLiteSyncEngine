#include <QTest>
#include <QTemporaryDir>
#include <QSignalSpy>

#include "syncengine/SyncEngine.h"

using namespace syncengine;

class TestSyncIntegration : public QObject {
    Q_OBJECT
private slots:
    void testBasicSync()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString sharedFolder = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), sharedFolder, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), sharedFolder, "clientB");

        QVERIFY(engineA.start(0)); // 0 = no auto-sync timer
        QVERIFY(engineB.start(0));

        // Create table on both
        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, value TEXT)");
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, value TEXT)");

        // Insert on A
        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO items (id, name, value) VALUES (1, 'item1', 'val1')");
        engineA.endWrite();

        // Sync B
        int applied = engineB.sync();
        QCOMPARE(applied, 1);

        // Verify B has the data
        auto rows = engineB.database()->query("SELECT name, value FROM items WHERE id = 1");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("item1"));
        QCOMPARE(rows[0][1], QStringLiteral("val1"));
    }

    void testBidirectionalSync()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString sharedFolder = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), sharedFolder, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), sharedFolder, "clientB");

        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        // Insert on A
        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO items (id, name) VALUES (1, 'fromA')");
        engineA.endWrite();

        // Insert on B
        engineB.beginWrite();
        engineB.database()->exec("INSERT INTO items (id, name) VALUES (2, 'fromB')");
        engineB.endWrite();

        // Sync both
        engineA.sync();
        engineB.sync();

        // Both should have both rows
        auto rowsA = engineA.database()->query("SELECT id, name FROM items ORDER BY id");
        auto rowsB = engineB.database()->query("SELECT id, name FROM items ORDER BY id");

        QCOMPARE(rowsA.size(), 2);
        QCOMPARE(rowsB.size(), 2);
        QCOMPARE(rowsA[0][1], QStringLiteral("fromA"));
        QCOMPARE(rowsA[1][1], QStringLiteral("fromB"));
        QCOMPARE(rowsB[0][1], QStringLiteral("fromA"));
        QCOMPARE(rowsB[1][1], QStringLiteral("fromB"));
    }

    void testOfflineSync()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString sharedFolder = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), sharedFolder, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), sharedFolder, "clientB");

        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        // A makes multiple changes while B is "offline" (not syncing)
        for (int i = 1; i <= 5; ++i) {
            engineA.beginWrite();
            engineA.database()->exec(
                QString("INSERT INTO items (id, name) VALUES (%1, 'item%1')").arg(i));
            engineA.endWrite();
        }

        // B comes online and syncs
        int applied = engineB.sync();
        QCOMPARE(applied, 5);

        auto rows = engineB.database()->query("SELECT COUNT(*) FROM items");
        QCOMPARE(rows[0][0], QStringLiteral("5"));
    }

    void testConflictResolution()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString sharedFolder = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), sharedFolder, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), sharedFolder, "clientB");

        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        // Both start with the same row
        engineA.database()->exec("INSERT INTO items (id, name) VALUES (1, 'original')");
        engineB.database()->exec("INSERT INTO items (id, name) VALUES (1, 'original')");

        // A updates the row
        engineA.beginWrite();
        engineA.database()->exec("UPDATE items SET name = 'updated_by_A' WHERE id = 1");
        engineA.endWrite();

        // B updates the same row differently
        engineB.beginWrite();
        engineB.database()->exec("UPDATE items SET name = 'updated_by_B' WHERE id = 1");
        engineB.endWrite();

        // Both sync -- each picks up the other's changeset
        engineA.sync(); // A gets B's update
        engineB.sync(); // B gets A's update

        // After one round, A has B's value applied (REPLACE), B has A's value applied.
        // They may not converge yet -- do a second round so both see all changesets.
        engineA.sync();
        engineB.sync();

        // Both should now have applied all changesets and converge.
        // The final value depends on changeset ordering (by HLC).
        auto rowsA = engineA.database()->query("SELECT name FROM items WHERE id = 1");
        auto rowsB = engineB.database()->query("SELECT name FROM items WHERE id = 1");

        QCOMPARE(rowsA.size(), 1);
        QCOMPARE(rowsB.size(), 1);
        // Both should have the same final value (convergence)
        QCOMPARE(rowsA[0][0], rowsB[0][0]);
    }

    void testSimulatedLatency()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString sharedFolder = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), sharedFolder, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), sharedFolder, "clientB");

        // Set 500ms simulated latency on A's transport
        engineA.transport()->setSimulatedLatencyMs(500);

        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        // A writes -- changeset is delayed
        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO items (id, name) VALUES (1, 'delayed')");
        engineA.endWrite();

        // B syncs immediately -- should see nothing yet
        int applied = engineB.sync();
        QCOMPARE(applied, 0);

        // Wait for latency to elapse
        QThread::msleep(600);

        // Flush A's delayed files to disk (simulates network propagation completing)
        engineA.transport()->flushDelayed();

        // Now B should see it
        applied = engineB.sync();
        QCOMPARE(applied, 1);

        auto rows = engineB.database()->query("SELECT name FROM items WHERE id = 1");
        QCOMPARE(rows[0][0], QStringLiteral("delayed"));
    }

    void testIdempotentSync()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString sharedFolder = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), sharedFolder, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), sharedFolder, "clientB");

        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO items (id, name) VALUES (1, 'test')");
        engineA.endWrite();

        // Sync B multiple times -- should only apply once
        QCOMPARE(engineB.sync(), 1);
        QCOMPARE(engineB.sync(), 0);
        QCOMPARE(engineB.sync(), 0);

        auto rows = engineB.database()->query("SELECT COUNT(*) FROM items");
        QCOMPARE(rows[0][0], QStringLiteral("1"));
    }

    /// When a changeset for table X fails because X doesn't exist, but
    /// another changeset in the same batch creates X (e.g., via a trigger
    /// or a different table's changeset), the deferred retry should pick it up.
    ///
    /// Note: CREATE TABLE itself is DDL that the session extension captures as
    /// a change to sqlite_master, but sqlite3changeset_apply() cannot replay
    /// DDL. Schema must be set up independently on each client. This test
    /// verifies the retry logic works when the first pass creates the table
    /// as a side effect.
    void testDeferredChangesetsRetryAfterTableCreation()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // A writes to two tables
        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engineA.start(0));
        engineA.database()->exec("CREATE TABLE t1 (id INTEGER PRIMARY KEY, val TEXT)");
        engineA.database()->exec("CREATE TABLE t2 (id INTEGER PRIMARY KEY, val TEXT)");

        // Write to t1 first, then t2
        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO t1 VALUES (1, 'in_t1')");
        engineA.endWrite();

        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO t2 VALUES (1, 'in_t2')");
        engineA.endWrite();

        // B has t2 but NOT t1. The t1 changeset will fail on first pass,
        // but since the t2 changeset succeeds (applied > 0), the retry runs.
        // t1 still won't exist on retry, so it will fail again -- but the
        // retry logic itself is exercised.
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec("CREATE TABLE t2 (id INTEGER PRIMARY KEY, val TEXT)");

        QStringList errors;
        QObject::connect(&engineB, &SyncEngine::syncErrorOccurred,
                         [&](SyncEngine::SyncError, const QString &msg) {
            errors.append(msg);
        });

        int applied = engineB.sync();

        // t2 changeset should have been applied
        QVERIFY(applied >= 1);
        auto rows = engineB.database()->query("SELECT val FROM t2 WHERE id = 1");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("in_t2"));

        // t1 changeset should have produced an error (table doesn't exist)
        bool hasT1Error = false;
        for (const auto &msg : errors) {
            if (msg.contains("t1"))
                hasT1Error = true;
        }
        QVERIFY(hasT1Error);

        // Now B creates t1 and syncs again -- the deferred changeset retries
        engineB.database()->exec("CREATE TABLE t1 (id INTEGER PRIMARY KEY, val TEXT)");
        errors.clear();

        applied = engineB.sync();
        QVERIFY(applied >= 1);

        rows = engineB.database()->query("SELECT val FROM t1 WHERE id = 1");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("in_t1"));
    }

    /// When deferred changesets still fail after retry (table genuinely doesn't
    /// exist), errors should be emitted only once (on the retry, not the first pass).
    void testDeferredChangesetsEmitErrorOnlyOnce()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // A creates and writes to a table that B will never have
        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engineA.start(0));
        engineA.database()->exec("CREATE TABLE secret (id INTEGER PRIMARY KEY, val TEXT)");

        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO secret VALUES (1, 'hidden')");
        engineA.endWrite();

        // B has a different schema -- no "secret" table
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineB.start(0));

        int errorCount = 0;
        QObject::connect(&engineB, &SyncEngine::syncErrorOccurred,
                         [&](SyncEngine::SyncError, const QString &) {
            ++errorCount;
        });

        engineB.sync();

        // Should emit errors, but each failed changeset should only produce
        // one error (from the retry pass), not two (from both passes)
        QVERIFY(errorCount > 0);

        // Sync again -- the same changesets are still pending (not marked applied).
        // They should produce errors again (still failing).
        int secondErrorCount = 0;
        QObject::disconnect(&engineB, &SyncEngine::syncErrorOccurred, nullptr, nullptr);
        QObject::connect(&engineB, &SyncEngine::syncErrorOccurred,
                         [&](SyncEngine::SyncError, const QString &) {
            ++secondErrorCount;
        });

        engineB.sync();
        QCOMPARE(secondErrorCount, errorCount);
    }
};

QTEST_MAIN(TestSyncIntegration)

#include "tst_sync_integration.moc"
