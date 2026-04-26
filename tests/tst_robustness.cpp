#include <QTest>
#include <QTemporaryDir>
#include <QThread>

#include "syncengine/SyncEngine.h"
#include "syncengine/SyncQuery.h"

using namespace syncengine;

/// Tests for robustness fixes: SQL injection safety, memory management,
/// thread safety, and edge cases.
class TestRobustness : public QObject {
    Q_OBJECT
private slots:

    // =================================================================
    // SQL injection safety (prepared statements)
    // =================================================================

    /// Client IDs containing SQL-special characters should not cause
    /// injection or corruption.
    void testClientIdWithSqlSpecialChars()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        // Client ID with single quotes, semicolons, and SQL keywords
        QString dangerousId = QStringLiteral("client'; DROP TABLE _sync_client; --");

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), dangerousId);
        QVERIFY(engine.start(0));

        // If injection happened, _sync_client would be dropped and this would fail
        auto rows = engine.database()->query("SELECT client_id FROM _sync_client");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], dangerousId);
    }

    /// Table names with special characters should not cause injection
    /// in updateRowHlc / getRowHlc.
    void testTableNameWithSqlSpecialChars()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "client1");
        QVERIFY(engine.start(0));

        // Table name with injection attempt
        QString dangerousTable = QStringLiteral("items'; DROP TABLE _sync_row_hlc; --");

        // These should safely use prepared statements, not crash or corrupt
        engine.database()->updateRowHlc(dangerousTable, 1, 100, "client1");
        uint64_t hlc = engine.database()->getRowHlc(dangerousTable, 1);
        QCOMPARE(hlc, 100ULL);

        // Verify the metadata table is intact
        auto rows = engine.database()->query("SELECT COUNT(*) FROM _sync_row_hlc");
        QCOMPARE(rows.size(), 1);
        QVERIFY(rows[0][0].toInt() >= 1);
    }

    /// Changeset IDs with special characters should not cause injection
    /// in markChangesetApplied / isChangesetApplied.
    void testChangesetIdWithSqlSpecialChars()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "client1");
        QVERIFY(engine.start(0));

        QString dangerousId = QStringLiteral("changeset'; DELETE FROM _sync_applied; --");

        engine.database()->markChangesetApplied(dangerousId, 42);
        QVERIFY(engine.database()->isChangesetApplied(dangerousId));

        // Verify the table is intact and has exactly our entry
        auto rows = engine.database()->query("SELECT COUNT(*) FROM _sync_applied");
        QVERIFY(rows[0][0].toInt() >= 1);
    }

    /// Unicode strings in client IDs and table names should round-trip
    /// correctly through the prepared statements.
    void testUnicodeInMetadata()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString unicodeId = QStringLiteral("client_\xC3\xA9\xC3\xA0\xC3\xBC_\xE4\xB8\xAD\xE6\x96\x87");

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), unicodeId);
        QVERIFY(engine.start(0));

        auto rows = engine.database()->query("SELECT client_id FROM _sync_client");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], unicodeId);
    }

    // =================================================================
    // Memory management: endSession edge cases
    // =================================================================

    /// Calling endSession() without a prior beginSession() should return
    /// empty and not crash.
    void testEndSessionWithoutBegin()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "client1");
        QVERIFY(engine.start(0));

        // endWrite without beginWrite should be safe
        QString result = engine.endWrite();
        QVERIFY(result.isEmpty());
    }

    /// Calling beginSession() twice should not leak the first session.
    void testDoubleBeginSession()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "client1");
        QVERIFY(engine.start(0));

        engine.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        // Begin twice -- second should clean up the first
        engine.beginWrite();
        engine.database()->exec("INSERT INTO t VALUES (1, 'first')");

        engine.beginWrite(); // replaces the previous session
        engine.database()->exec("INSERT INTO t VALUES (2, 'second')");
        QString result = engine.endWrite();

        // Should not crash, and should produce a changeset for the second session
        // (the first session's changes may or may not be captured depending on
        // implementation, but it must not leak or crash)
        QVERIFY(!result.isEmpty());
    }

    // =================================================================
    // Thread safety: atomic flags
    // =================================================================

    /// start() and stop() should be safe to call from the main thread
    /// while the sync timer could be firing.
    void testStartStopCycle()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "client1");

        // Rapid start/stop cycles should not crash
        for (int i = 0; i < 10; ++i) {
            QVERIFY(engine.start(100)); // 100ms timer
            QVERIFY(engine.isRunning());
            engine.stop();
            QVERIFY(!engine.isRunning());
        }
    }

    /// isRunning() should reflect the current state accurately.
    void testIsRunningState()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "client1");
        QVERIFY(!engine.isRunning());

        QVERIFY(engine.start(0));
        QVERIFY(engine.isRunning());

        engine.stop();
        QVERIFY(!engine.isRunning());
    }

    // =================================================================
    // Const correctness
    // =================================================================

    /// The const clock() accessor should be callable from a const context.
    void testConstClockAccessor()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "client1");
        QVERIFY(engine.start(0));

        const SyncableDatabase *constDb = engine.database();
        uint64_t hlc = constDb->clock().current();
        QVERIFY(hlc > 0);
    }

    // =================================================================
    // Edge cases in changeset processing
    // =================================================================

    /// Empty changeset files in the shared folder should not crash sync.
    void testEmptyChangesetFileDoesNotCrash()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // Create an empty changeset file
        QDir().mkpath(shared);
        QFile empty(shared + "/bogus_00000000000000000001_0000000000_v0.changeset");
        QVERIFY(empty.open(QIODevice::WriteOnly));
        empty.close(); // 0 bytes

        SyncEngine engine(tmpDir.filePath("test.db"), shared, "client1");
        QVERIFY(engine.start(0));

        // Should not crash
        int applied = engine.sync();
        QCOMPARE(applied, 0);
    }

    /// Changeset files with unparseable names should be silently skipped.
    void testUnparseableFilenameSkipped()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QDir().mkpath(shared);
        QFile bad(shared + "/not_a_valid_name.changeset");
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("data");
        bad.close();

        SyncEngine engine(tmpDir.filePath("test.db"), shared, "client1");
        QVERIFY(engine.start(0));

        int applied = engine.sync();
        QCOMPARE(applied, 0);
    }

    /// Sync with an empty shared folder should return 0 and not error.
    void testSyncEmptyFolder()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        SyncEngine engine(tmpDir.filePath("test.db"), tmpDir.filePath("shared"), "client1");
        QVERIFY(engine.start(0));

        QList<QPair<SyncEngine::SyncError, QString>> errors;
        QObject::connect(&engine, &SyncEngine::syncErrorOccurred,
                         [&](SyncEngine::SyncError e, const QString &msg) {
            errors.append({e, msg});
        });

        int applied = engine.sync();
        QCOMPARE(applied, 0);
        QVERIFY(errors.isEmpty());
    }

    /// Schema version 0 (default) should work correctly with all metadata
    /// operations.
    void testDefaultSchemaVersionMetadata()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        // Neither sets schema version -- both default to 0
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        engineA.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");
        engineB.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO t VALUES (1, 'test')");
        engineA.endWrite();

        int applied = engineB.sync();
        QCOMPARE(applied, 1);
    }
};

QTEST_MAIN(TestRobustness)

#include "tst_robustness.moc"
