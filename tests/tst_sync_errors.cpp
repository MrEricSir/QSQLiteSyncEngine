#include <QTest>
#include <QTemporaryDir>
#include <QFile>

#include "syncengine/SyncEngine.h"

using namespace syncengine;

/// Tests that every SyncError enum value is emitted with the correct type
/// under the conditions that trigger it.
class TestSyncErrors : public QObject {
    Q_OBJECT

private:
    struct CapturedError {
        SyncEngine::SyncError type = SyncEngine::NoError;
        QString message;
    };

    /// Connect to syncErrorOccurred and capture the first error.
    static void captureError(SyncEngine &engine, CapturedError &out)
    {
        QObject::connect(&engine, &SyncEngine::syncErrorOccurred,
                         [&](SyncEngine::SyncError error, const QString &msg) {
            if (out.type == SyncEngine::NoError) {
                out.type = error;
                out.message = msg;
            }
        });
    }

    /// Connect and capture ALL errors into a list.
    static void captureAllErrors(SyncEngine &engine, QList<CapturedError> &out)
    {
        QObject::connect(&engine, &SyncEngine::syncErrorOccurred,
                         [&](SyncEngine::SyncError error, const QString &msg) {
            out.append({error, msg});
        });
    }

private slots:

    // =================================================================
    // DatabaseError
    // =================================================================

    /// start() on an invalid path should emit DatabaseError.
    void testDatabaseErrorOnBadPath()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        // Use an empty string as the database path.
        // sqlite3_open("") opens a temporary database that gets deleted on close,
        // but our initSyncMetadata uses PRAGMA journal_mode=WAL which can fail
        // on a temp db. Use a path with null bytes to guarantee failure.
        QString badPath = tmpDir.filePath("sub1/sub2/sub3/sub4/sub5/test.db");
        // Don't create the parent directories -- sqlite3_open will fail

        SyncEngine engine(badPath, tmpDir.filePath("shared"), "client1");

        CapturedError err;
        captureError(engine, err);

        bool started = engine.start(0);
        QVERIFY(!started);
        QCOMPARE(err.type, SyncEngine::DatabaseError);
        QVERIFY(!err.message.isEmpty());
    }

    // =================================================================
    // TransportError
    // =================================================================

    /// endWrite() should emit TransportError if the shared folder is not
    /// writable (e.g., path doesn't exist and can't be created).
    void testTransportErrorOnUnwritableSharedFolder()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        // Create a regular file, then try to use a path inside it as the
        // shared folder. mkpath will fail because you can't create a directory
        // inside a file. This works on all platforms.
        QString blocker = tmpDir.filePath("blocker");
        QFile(blocker).open(QIODevice::WriteOnly); // create a regular file
        QString badShared = blocker + "/impossible/shared";
        SyncEngine engine(tmpDir.filePath("test.db"), badShared, "client1");

        CapturedError err;
        captureError(engine, err);

        QVERIFY(engine.start(0));

        engine.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        engine.beginWrite();
        engine.database()->exec("INSERT INTO t VALUES (1, 'test')");
        QString result = engine.endWrite();

        QVERIFY(result.isEmpty());
        QCOMPARE(err.type, SyncEngine::TransportError);
    }

    // =================================================================
    // VersionMismatch
    // =================================================================

    /// Sync should emit VersionMismatch when a remote changeset has a higher
    /// schema version than the local client.
    void testVersionMismatchOnNewerRemoteChangeset()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // Writer on v3
        SyncEngine writer(tmpDir.filePath("writer.db"), shared, "writer");
        writer.setSchemaVersion(3);
        QVERIFY(writer.start(0));
        writer.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");
        writer.beginWrite();
        writer.database()->exec("INSERT INTO t VALUES (1, 'hello')");
        writer.endWrite();

        // Reader on v2
        SyncEngine reader(tmpDir.filePath("reader.db"), shared, "reader");
        reader.setSchemaVersion(2);
        QVERIFY(reader.start(0));
        reader.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        CapturedError err;
        captureError(reader, err);

        int applied = reader.sync();

        QCOMPARE(applied, 0);
        QCOMPARE(err.type, SyncEngine::VersionMismatch);
        QVERIFY(err.message.contains("version"));
    }

    /// VersionMismatch should NOT be emitted when the remote changeset has
    /// an equal or lower schema version.
    void testNoVersionMismatchOnSameOrOlderVersion()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // Writer on v2
        SyncEngine writer(tmpDir.filePath("writer.db"), shared, "writer");
        writer.setSchemaVersion(2);
        QVERIFY(writer.start(0));
        writer.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");
        writer.beginWrite();
        writer.database()->exec("INSERT INTO t VALUES (1, 'hello')");
        writer.endWrite();

        // Reader on v3 (newer)
        SyncEngine reader(tmpDir.filePath("reader.db"), shared, "reader");
        reader.setSchemaVersion(3);
        QVERIFY(reader.start(0));
        reader.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        QList<CapturedError> errors;
        captureAllErrors(reader, errors);

        int applied = reader.sync();

        QCOMPARE(applied, 1);
        for (const auto &e : errors)
            QVERIFY2(e.type != SyncEngine::VersionMismatch,
                     "Should not emit VersionMismatch for same or older version");
    }

    // =================================================================
    // SchemaMismatch
    // =================================================================

    /// Sync should emit SchemaMismatch when a remote changeset has more
    /// columns than the local table.
    void testSchemaMismatchOnColumnCountDifference()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // Writer has 3 columns
        SyncEngine writer(tmpDir.filePath("writer.db"), shared, "writer");
        QVERIFY(writer.start(0));
        writer.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a TEXT, b TEXT)");
        writer.beginWrite();
        writer.database()->exec("INSERT INTO t VALUES (1, 'x', 'y')");
        writer.endWrite();

        // Reader has 2 columns
        SyncEngine reader(tmpDir.filePath("reader.db"), shared, "reader");
        QVERIFY(reader.start(0));
        reader.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a TEXT)");

        CapturedError err;
        captureError(reader, err);

        int applied = reader.sync();

        QCOMPARE(applied, 0);
        QCOMPARE(err.type, SyncEngine::SchemaMismatch);
        QVERIFY(err.message.contains("t")); // table name in message
    }

    /// Sync should emit SchemaMismatch when a remote changeset references
    /// a table that doesn't exist locally.
    void testSchemaMismatchOnMissingTable()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine writer(tmpDir.filePath("writer.db"), shared, "writer");
        QVERIFY(writer.start(0));
        writer.database()->exec("CREATE TABLE newtable (id INTEGER PRIMARY KEY, val TEXT)");
        writer.beginWrite();
        writer.database()->exec("INSERT INTO newtable VALUES (1, 'data')");
        writer.endWrite();

        // Reader doesn't have newtable
        SyncEngine reader(tmpDir.filePath("reader.db"), shared, "reader");
        QVERIFY(reader.start(0));

        CapturedError err;
        captureError(reader, err);

        reader.sync();

        QCOMPARE(err.type, SyncEngine::SchemaMismatch);
        QVERIFY(err.message.contains("newtable"));
    }

    /// SchemaMismatch should NOT be emitted when the remote changeset has
    /// fewer columns (trailing columns get defaults -- this is compatible).
    void testNoSchemaMismatchOnFewerRemoteColumns()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // Writer has 2 columns
        SyncEngine writer(tmpDir.filePath("writer.db"), shared, "writer");
        QVERIFY(writer.start(0));
        writer.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a TEXT)");
        writer.beginWrite();
        writer.database()->exec("INSERT INTO t VALUES (1, 'x')");
        writer.endWrite();

        // Reader has 3 columns (superset)
        SyncEngine reader(tmpDir.filePath("reader.db"), shared, "reader");
        QVERIFY(reader.start(0));
        reader.database()->exec(
            "CREATE TABLE t (id INTEGER PRIMARY KEY, a TEXT, b TEXT DEFAULT 'def')");

        QList<CapturedError> errors;
        captureAllErrors(reader, errors);

        int applied = reader.sync();

        QCOMPARE(applied, 1);
        for (const auto &e : errors)
            QVERIFY2(e.type != SyncEngine::SchemaMismatch,
                     "Should not emit SchemaMismatch when local has more columns");
    }

    // =================================================================
    // ChangesetError
    // =================================================================

    /// ChangesetError should be emitted when a changeset file is corrupt
    /// (not a valid sqlite changeset).
    void testChangesetErrorOnCorruptFile()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // Create a corrupt changeset file manually
        QDir().mkpath(shared);
        QFile corrupt(shared + "/writer_00000000000000000001_0000000000_v0.changeset");
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("this is not a valid changeset");
        corrupt.close();

        SyncEngine reader(tmpDir.filePath("reader.db"), shared, "reader");
        QVERIFY(reader.start(0));

        CapturedError err;
        captureError(reader, err);

        reader.sync();

        QCOMPARE(err.type, SyncEngine::ChangesetError);
    }

    // =================================================================
    // NoError -- verify clean operations don't emit errors
    // =================================================================

    /// A normal write + sync cycle should not emit any errors.
    void testNoErrorOnCleanSync()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        engineA.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");
        engineB.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO t VALUES (1, 'hello')");
        engineA.endWrite();

        QList<CapturedError> errors;
        captureAllErrors(engineB, errors);

        int applied = engineB.sync();

        QCOMPARE(applied, 1);
        QVERIFY2(errors.isEmpty(),
                 qPrintable(QStringLiteral("Unexpected error: %1")
                     .arg(errors.isEmpty() ? "" : errors[0].message)));
    }

    // =================================================================
    // Multiple errors in one sync cycle
    // =================================================================

    /// A single sync() call that encounters multiple problems should emit
    /// multiple syncErrorOccurred signals with appropriate types.
    void testMultipleErrorsInOneSyncCycle()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // Writer 1: schema version 5
        SyncEngine w1(tmpDir.filePath("w1.db"), shared, "writer1");
        w1.setSchemaVersion(5);
        QVERIFY(w1.start(0));
        w1.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a TEXT, b TEXT)");
        w1.beginWrite();
        w1.database()->exec("INSERT INTO t VALUES (1, 'x', 'y')");
        w1.endWrite();

        // Writer 2: schema version 1, different table schema
        SyncEngine w2(tmpDir.filePath("w2.db"), shared, "writer2");
        w2.setSchemaVersion(1);
        QVERIFY(w2.start(0));
        w2.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a TEXT, b TEXT, c TEXT)");
        w2.beginWrite();
        w2.database()->exec("INSERT INTO t VALUES (2, 'a', 'b', 'c')");
        w2.endWrite();

        // Reader: version 2, only 2 columns
        SyncEngine reader(tmpDir.filePath("reader.db"), shared, "reader");
        reader.setSchemaVersion(2);
        QVERIFY(reader.start(0));
        reader.database()->exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a TEXT)");

        QList<CapturedError> errors;
        captureAllErrors(reader, errors);

        reader.sync();

        // Should have at least one VersionMismatch (v5 > v2) and at least
        // one SchemaMismatch (w2's 4 cols > reader's 2 cols)
        bool hasVersionMismatch = false;
        bool hasSchemaMismatch = false;
        for (const auto &e : errors) {
            if (e.type == SyncEngine::VersionMismatch) hasVersionMismatch = true;
            if (e.type == SyncEngine::SchemaMismatch) hasSchemaMismatch = true;
        }
        QVERIFY2(hasVersionMismatch, "Expected VersionMismatch for v5 changeset");
        QVERIFY2(hasSchemaMismatch, "Expected SchemaMismatch for 4-column changeset");
    }
};

QTEST_MAIN(TestSyncErrors)

#include "tst_sync_errors.moc"
