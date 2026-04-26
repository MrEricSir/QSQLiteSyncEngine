#include <QTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QSqlDriver>
#include <QSqlTableModel>
#include <QSignalSpy>
#include <QCoreApplication>

#include "syncengine/SyncEngine.h"
#include "sqlite3.h"

using namespace syncengine;

/// Comprehensive tests for the QSQLITE_SYNC driver and auto-capture integration.
///
/// Organized in three sections:
///   1. Driver smoke tests -- verify the plugin works as a standalone QSqlDatabase
///   2. Single-instance smoke tests -- engine + driver on one database, no sync
///   3. Multi-instance sync tests -- auto-captured changes sync between engines
class TestQSqliteSyncDriver : public QObject {
    Q_OBJECT

private:
    static int s_connId;
    QSqlDatabase createDb(const QString &path) {
        QString connName = QStringLiteral("syncdriver_%1").arg(++s_connId);
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE_SYNC", connName);
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

    // =================================================================
    // Section 1: Driver smoke tests (no SyncEngine)
    // =================================================================

    /// The QSQLITE_SYNC driver should be discoverable.
    void testDriverAvailable()
    {
        QVERIFY2(QSqlDatabase::drivers().contains("QSQLITE_SYNC"),
                 "QSQLITE_SYNC driver not found");
    }

    /// Open, create table, insert, select.
    void testBasicOpenAndQuery()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        QSqlQuery q(db);
        QVERIFY(q.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)"));
        QVERIFY(q.exec("INSERT INTO t (id, val) VALUES (1, 'hello')"));

        QVERIFY(q.exec("SELECT val FROM t WHERE id = 1"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("hello"));
        QVERIFY(!q.next()); // only one row
    }

    /// Prepared statements with positional bindings.
    void testPreparedPositionalBindings()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        QSqlQuery q(db);
        q.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL)");

        q.prepare("INSERT INTO t (id, name, score) VALUES (?, ?, ?)");
        q.bindValue(0, 1);
        q.bindValue(1, QStringLiteral("alice"));
        q.bindValue(2, 99.5);
        QVERIFY(q.exec());

        q.prepare("SELECT name, score FROM t WHERE id = ?");
        q.bindValue(0, 1);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("alice"));
        QVERIFY(qAbs(q.value(1).toDouble() - 99.5) < 0.01);
    }

    /// Prepared statements with named bindings.
    void testPreparedNamedBindings()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        QSqlQuery q(db);
        q.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)");

        q.prepare("INSERT INTO t (id, name) VALUES (:id, :name)");
        q.bindValue(":id", 1);
        q.bindValue(":name", QStringLiteral("bob"));
        QVERIFY(q.exec());

        q.exec("SELECT name FROM t WHERE id = 1");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("bob"));
    }

    /// INSERT, UPDATE, DELETE, verify with SELECT.
    void testInsertUpdateDelete()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        QSqlQuery q(db);
        q.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        q.exec("INSERT INTO t (id, val) VALUES (1, 'original')");
        q.exec("UPDATE t SET val = 'updated' WHERE id = 1");
        q.exec("SELECT val FROM t WHERE id = 1");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("updated"));

        q.exec("DELETE FROM t WHERE id = 1");
        q.exec("SELECT COUNT(*) FROM t");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
    }

    /// Transaction commit and rollback without SyncEngine.
    void testTransactionCommitAndRollback()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        QSqlQuery q(db);
        q.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        // Commit
        QVERIFY(db.transaction());
        q.exec("INSERT INTO t VALUES (1, 'a')");
        q.exec("INSERT INTO t VALUES (2, 'b')");
        QVERIFY(db.commit());

        q.exec("SELECT COUNT(*) FROM t");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 2);

        // Rollback
        QVERIFY(db.transaction());
        q.exec("INSERT INTO t VALUES (3, 'c')");
        QVERIFY(db.rollback());

        q.exec("SELECT COUNT(*) FROM t");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 2);
    }

    /// NULL, blob, integer, real, text types round-trip correctly.
    void testDataTypes()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        QSqlQuery q(db);
        q.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, i INT, r REAL, t TEXT, b BLOB)");

        q.prepare("INSERT INTO t VALUES (?, ?, ?, ?, ?)");
        q.bindValue(0, 1);
        q.bindValue(1, 42);
        q.bindValue(2, 3.14);
        q.bindValue(3, QStringLiteral("text"));
        q.bindValue(4, QByteArray("\x00\x01\x02", 3));
        QVERIFY(q.exec());

        // Insert a row with NULL
        q.prepare("INSERT INTO t (id, i) VALUES (?, ?)");
        q.bindValue(0, 2);
        q.bindValue(1, QVariant(QMetaType::fromType<int>()));
        QVERIFY(q.exec());

        q.exec("SELECT i, r, t, b FROM t WHERE id = 1");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toLongLong(), 42LL);
        QVERIFY(qAbs(q.value(1).toDouble() - 3.14) < 0.001);
        QCOMPARE(q.value(2).toString(), QStringLiteral("text"));
        QCOMPARE(q.value(3).toByteArray(), QByteArray("\x00\x01\x02", 3));

        q.exec("SELECT i FROM t WHERE id = 2");
        QVERIFY(q.next());
        QVERIFY(q.value(0).isNull());
    }

    /// lastInsertId and numRowsAffected work.
    void testLastInsertIdAndRowsAffected()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        QSqlQuery q(db);
        q.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");

        q.exec("INSERT INTO t (val) VALUES ('a')");
        QCOMPARE(q.lastInsertId().toLongLong(), 1LL);

        q.exec("INSERT INTO t (val) VALUES ('b')");
        QCOMPARE(q.lastInsertId().toLongLong(), 2LL);

        q.exec("UPDATE t SET val = 'x'");
        QCOMPARE(q.numRowsAffected(), 2);
    }

    /// QSqlRecord / column metadata works.
    void testRecordMetadata()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        QSqlQuery q(db);
        q.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL)");
        q.exec("INSERT INTO t VALUES (1, 'x', 2.5)");
        q.exec("SELECT id, name, score FROM t");
        QVERIFY(q.next());

        QSqlRecord rec = q.record();
        QCOMPARE(rec.count(), 3);
        QCOMPARE(rec.fieldName(0), QStringLiteral("id"));
        QCOMPARE(rec.fieldName(1), QStringLiteral("name"));
        QCOMPARE(rec.fieldName(2), QStringLiteral("score"));
    }

    /// QSqlTableModel works with the driver.
    void testQSqlTableModel()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        QSqlQuery q(db);
        q.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        q.exec("INSERT INTO items VALUES (1, 'hello')");
        q.exec("INSERT INTO items VALUES (2, 'world')");

        QSqlTableModel model(nullptr, db);
        model.setTable("items");
        QVERIFY(model.select());
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0, 1)).toString(), QStringLiteral("hello"));
    }

    /// The session extension API is callable on the driver's handle.
    void testSessionExtensionOnHandle()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        auto *handle = *static_cast<sqlite3 **>(db.driver()->handle().data());

        // Create session, track all tables, make changes, extract changeset
        sqlite3_session *session = nullptr;
        QCOMPARE(sqlite3session_create(handle, "main", &session), SQLITE_OK);
        QCOMPARE(sqlite3session_attach(session, nullptr), SQLITE_OK);

        QSqlQuery q(db);
        q.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");
        q.exec("INSERT INTO t VALUES (1, 'hello')");

        int nChangeset = 0;
        void *pChangeset = nullptr;
        QCOMPARE(sqlite3session_changeset(session, &nChangeset, &pChangeset), SQLITE_OK);
        QVERIFY(nChangeset > 0);

        sqlite3_free(pChangeset);
        sqlite3session_delete(session);
    }

    /// Error handling -- bad SQL produces an error, not a crash.
    void testErrorOnBadSql()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        QSqlQuery q(db);
        QVERIFY(!q.exec("SELECT * FROM nonexistent_table"));
        QVERIFY(q.lastError().isValid());
    }

    // =================================================================
    // Section 2: Single-instance smoke tests (engine + driver, no sync)
    // =================================================================

    /// SyncEngine constructed from QSQLITE_SYNC QSqlDatabase starts OK.
    void testEngineStartsFromDriver()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        SyncEngine engine(db, tmpDir.filePath("shared"), "client1");
        QVERIFY(engine.start(0));
        QVERIFY(engine.isRunning());
        engine.stop();
    }

    /// Writes via QSqlQuery on the shared handle are visible via the engine's
    /// database() accessor (same connection).
    void testQSqlQueryWritesVisibleOnEngineHandle()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        SyncEngine engine(db, tmpDir.filePath("shared"), "client1");
        QVERIFY(engine.start(0));

        QSqlQuery q(db);
        q.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        q.exec("INSERT INTO items VALUES (1, 'hello')");
        QCoreApplication::processEvents();

        // engine.database() points to the same handle -- should see the data
        auto rows = engine.database()->query("SELECT name FROM items WHERE id = 1");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("hello"));
    }

    /// Writes via engine.database()->exec() are visible via QSqlQuery on the
    /// same QSqlDatabase (same handle, should be immediate).
    void testEngineExecVisibleOnQSqlQuery()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        SyncEngine engine(db, tmpDir.filePath("shared"), "client1");
        QVERIFY(engine.start(0));

        QSqlQuery setup(db);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        engine.database()->exec("INSERT INTO items VALUES (1, 'from_engine')");

        QSqlQuery q(db);
        q.exec("SELECT name FROM items WHERE id = 1");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("from_engine"));
    }

    /// Auto-capture fires on autocommit writes -- verify changeset file is
    /// created in the shared folder.
    void testAutoCaptureCreatesChangesetFile()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        SyncEngine engine(db, shared, "client1");
        QVERIFY(engine.start(0));

        QSqlQuery q(db);
        q.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        QCoreApplication::processEvents();

        q.exec("INSERT INTO items VALUES (1, 'test')");
        QCoreApplication::processEvents();

        // Shared folder should have changeset files
        QDir sharedDir(shared);
        QStringList files = sharedDir.entryList({"*.changeset"}, QDir::Files);
        QVERIFY2(!files.isEmpty(),
                 "Expected changeset files in shared folder after auto-capture");
    }

    /// QSqlTableModel submitAll() triggers auto-capture.
    void testQSqlTableModelSubmitTriggersAutoCapture()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        SyncEngine engine(db, shared, "client1");
        QVERIFY(engine.start(0));

        QSqlQuery setup(db);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        QCoreApplication::processEvents();

        QSqlTableModel model(nullptr, db);
        model.setTable("items");
        model.setEditStrategy(QSqlTableModel::OnManualSubmit);
        model.select();

        int row = model.rowCount();
        model.insertRow(row);
        model.setData(model.index(row, 0), 1);
        model.setData(model.index(row, 1), "from_model");
        QVERIFY(model.submitAll());
        QCoreApplication::processEvents();

        QDir sharedDir(shared);
        QStringList files = sharedDir.entryList({"*.changeset"}, QDir::Files);
        QVERIFY2(!files.isEmpty(),
                 "Expected at least one changeset after QSqlTableModel::submitAll()");
    }

    /// beginWrite/endWrite still works alongside auto-capture.
    void testManualWriteCoexistsWithAutoCapture()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        SyncEngine engine(db, shared, "client1");
        QVERIFY(engine.start(0));

        QSqlQuery setup(db);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        QCoreApplication::processEvents();

        // Manual write
        engine.beginWrite();
        engine.database()->exec("INSERT INTO items VALUES (1, 'manual')");
        QString filename = engine.endWrite();
        QVERIFY(!filename.isEmpty());

        // Auto-capture write
        QSqlQuery q(db);
        q.exec("INSERT INTO items VALUES (2, 'auto')");
        QCoreApplication::processEvents();

        // Both rows should exist
        QSqlQuery verify(db);
        verify.exec("SELECT COUNT(*) FROM items");
        QVERIFY(verify.next());
        QCOMPARE(verify.value(0).toInt(), 2);
    }

    // =================================================================
    // Section 3: Multi-instance sync tests (auto-capture → sync)
    // =================================================================

    /// Auto-capture: autocommit QSqlQuery INSERT syncs to another engine.
    void testAutoCaptureSyncsAutocommitInsert()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase userDb = createDb(tmpDir.filePath("a.db"));
        QVERIFY(userDb.open());

        SyncEngine engineA(userDb, shared, "clientA");
        QVERIFY(engineA.start(0));

        QSqlQuery setup(userDb);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        QCoreApplication::processEvents();

        QSqlQuery q(userDb);
        QVERIFY(q.exec("INSERT INTO items (id, name) VALUES (1, 'auto_captured')"));
        QCoreApplication::processEvents();

        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        int applied = engineB.sync();
        QVERIFY(applied > 0);

        auto rows = engineB.database()->query("SELECT name FROM items WHERE id = 1");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("auto_captured"));
    }

    /// Auto-capture: explicit transaction groups multiple writes into one changeset.
    void testAutoCaptureExplicitTransaction()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase userDb = createDb(tmpDir.filePath("a.db"));
        QVERIFY(userDb.open());

        SyncEngine engineA(userDb, shared, "clientA");
        QVERIFY(engineA.start(0));

        QSqlQuery setup(userDb);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        QCoreApplication::processEvents();

        QVERIFY(userDb.transaction());
        QSqlQuery q(userDb);
        q.exec("INSERT INTO items VALUES (1, 'first')");
        q.exec("INSERT INTO items VALUES (2, 'second')");
        q.exec("INSERT INTO items VALUES (3, 'third')");
        QVERIFY(userDb.commit());
        QCoreApplication::processEvents();

        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        engineB.sync();

        auto rows = engineB.database()->query("SELECT COUNT(*) FROM items");
        QCOMPARE(rows[0][0], QStringLiteral("3"));
    }

    /// Auto-capture: prepared query with bindings syncs correctly.
    void testAutoCapturePreparedQuerySyncs()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase userDb = createDb(tmpDir.filePath("a.db"));
        QVERIFY(userDb.open());

        SyncEngine engineA(userDb, shared, "clientA");
        QVERIFY(engineA.start(0));

        QSqlQuery setup(userDb);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, score INT)");
        QCoreApplication::processEvents();

        QSqlQuery q(userDb);
        q.prepare("INSERT INTO items (id, name, score) VALUES (?, ?, ?)");
        q.bindValue(0, 1);
        q.bindValue(1, QStringLiteral("alice"));
        q.bindValue(2, 100);
        QVERIFY(q.exec());
        QCoreApplication::processEvents();

        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, score INT)");

        QVERIFY(engineB.sync() > 0);

        auto rows = engineB.database()->query("SELECT name, score FROM items WHERE id = 1");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("alice"));
        QCOMPARE(rows[0][1], QStringLiteral("100"));
    }

    /// Rolled-back transaction does NOT sync.
    void testAutoCaptureRollbackNotSynced()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase userDb = createDb(tmpDir.filePath("a.db"));
        QVERIFY(userDb.open());

        SyncEngine engineA(userDb, shared, "clientA");
        QVERIFY(engineA.start(0));

        QSqlQuery setup(userDb);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        QCoreApplication::processEvents();

        QVERIFY(userDb.transaction());
        QSqlQuery q(userDb);
        q.exec("INSERT INTO items VALUES (1, 'should_not_sync')");
        QVERIFY(userDb.rollback());
        QCoreApplication::processEvents();

        QSqlQuery q2(userDb);
        q2.exec("INSERT INTO items VALUES (2, 'should_sync')");
        QCoreApplication::processEvents();

        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        engineB.sync();

        auto rows = engineB.database()->query("SELECT id, name FROM items ORDER BY id");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("2"));
        QCOMPARE(rows[0][1], QStringLiteral("should_sync"));
    }

    /// Bidirectional sync: both engines use QSQLITE_SYNC with auto-capture.
    void testBidirectionalAutoCapture()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase dbA = createDb(tmpDir.filePath("a.db"));
        QSqlDatabase dbB = createDb(tmpDir.filePath("b.db"));
        QVERIFY(dbA.open());
        QVERIFY(dbB.open());

        SyncEngine engineA(dbA, shared, "clientA");
        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        QSqlQuery setupA(dbA);
        setupA.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        QSqlQuery setupB(dbB);
        setupB.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        QCoreApplication::processEvents();

        // A writes via QSqlQuery
        QSqlQuery qA(dbA);
        qA.exec("INSERT INTO items VALUES (1, 'from_A')");
        QCoreApplication::processEvents();

        // B writes via QSqlQuery
        QSqlQuery qB(dbB);
        qB.exec("INSERT INTO items VALUES (2, 'from_B')");
        QCoreApplication::processEvents();

        // Sync both
        engineA.sync();
        engineB.sync();

        // A should have both
        QSqlQuery verifyA(dbA);
        verifyA.exec("SELECT COUNT(*) FROM items");
        QVERIFY(verifyA.next());
        QCOMPARE(verifyA.value(0).toInt(), 2);

        // B should have both
        QSqlQuery verifyB(dbB);
        verifyB.exec("SELECT COUNT(*) FROM items");
        QVERIFY(verifyB.next());
        QCOMPARE(verifyB.value(0).toInt(), 2);
    }

    /// syncCompleted signal fires after auto-captured changes are applied.
    void testSyncCompletedSignal()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase dbA = createDb(tmpDir.filePath("a.db"));
        QVERIFY(dbA.open());

        SyncEngine engineA(dbA, shared, "clientA");
        QVERIFY(engineA.start(0));

        QSqlQuery setup(dbA);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        QCoreApplication::processEvents();

        QSqlQuery q(dbA);
        q.exec("INSERT INTO items VALUES (1, 'test')");
        QCoreApplication::processEvents();

        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        QSignalSpy spy(&engineB, &SyncEngine::syncCompleted);
        engineB.sync();
        QCoreApplication::processEvents();

        QVERIFY(!spy.isEmpty());
        QVERIFY(spy.first().first().toInt() > 0);
    }

    /// UPDATE and DELETE operations sync correctly via auto-capture.
    void testAutoCaptureUpdateAndDelete()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase dbA = createDb(tmpDir.filePath("a.db"));
        QVERIFY(dbA.open());

        SyncEngine engineA(dbA, shared, "clientA");
        QVERIFY(engineA.start(0));

        QSqlQuery setup(dbA);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        setup.exec("INSERT INTO items VALUES (1, 'original')");
        setup.exec("INSERT INTO items VALUES (2, 'to_delete')");
        QCoreApplication::processEvents();

        // Sync initial state to B
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineB.sync();

        // Now A does UPDATE and DELETE
        QSqlQuery q(dbA);
        q.exec("UPDATE items SET name = 'updated' WHERE id = 1");
        q.exec("DELETE FROM items WHERE id = 2");
        QCoreApplication::processEvents();

        // B syncs again
        int applied = engineB.sync();
        QVERIFY(applied > 0);

        auto rows = engineB.database()->query("SELECT id, name FROM items ORDER BY id");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("1"));
        QCOMPARE(rows[0][1], QStringLiteral("updated"));
    }
};

int TestQSqliteSyncDriver::s_connId = 0;

QTEST_MAIN(TestQSqliteSyncDriver)

#include "tst_qsqlite_sync_driver.moc"
