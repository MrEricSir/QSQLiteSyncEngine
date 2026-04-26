#include <QTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlTableModel>
#include <QSignalSpy>

#include "syncengine/SyncEngine.h"

using namespace syncengine;

/// Tests for QSqlDatabase integration: the SyncEngine is constructed from a
/// QSqlDatabase, opens a parallel sync connection to the same file, and the
/// user's QSqlDatabase sees changes applied by remote sync via WAL mode.
class TestQSqlDatabaseIntegration : public QObject {
    Q_OBJECT

private:
    static int s_connId;
    QSqlDatabase createDb(const QString &path) {
        QString connName = QStringLiteral("testconn_%1").arg(++s_connId);
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(path);
        return db;
    }

private slots:
    void cleanupTestCase()
    {
        for (const QString &name : QSqlDatabase::connectionNames())
            QSqlDatabase::removeDatabase(name);
    }

    /// SyncEngine should accept a QSqlDatabase and start successfully.
    void testConstructFromQSqlDatabase()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase db = createDb(tmpDir.filePath("test.db"));
        QVERIFY(db.open());

        SyncEngine engine(db, shared, "client1");
        QVERIFY(engine.start(0));

        engine.stop();
    }

    /// Writes through the engine's beginWrite/endWrite should be visible
    /// on the user's QSqlDatabase (same file, WAL mode).
    void testEngineWritesVisibleOnQSqlDatabase()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase userDb = createDb(tmpDir.filePath("test.db"));
        QVERIFY(userDb.open());

        // Create table via user's connection
        QSqlQuery setup(userDb);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        SyncEngine engine(userDb, shared, "client1");
        QVERIFY(engine.start(0));

        // Write through the engine
        engine.beginWrite();
        engine.database()->exec("INSERT INTO items (id, name) VALUES (1, 'from_engine')");
        engine.endWrite();

        // Read through the user's QSqlDatabase -- should see it (WAL mode)
        QSqlQuery q(userDb);
        q.exec("SELECT name FROM items WHERE id = 1");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("from_engine"));
    }

    /// Remote changesets applied by sync should be visible on the user's
    /// QSqlDatabase.
    void testSyncedChangesVisibleOnQSqlDatabase()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // Engine A writes via path-based constructor
        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engineA.start(0));
        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO items (id, name) VALUES (1, 'from_A')");
        engineA.endWrite();

        // Engine B uses QSqlDatabase constructor
        QSqlDatabase userDb = createDb(tmpDir.filePath("b.db"));
        QVERIFY(userDb.open());

        SyncEngine engineB(userDb, shared, "clientB");
        QVERIFY(engineB.start(0));

        // Create table on both connections (engine's and user's)
        engineB.database()->exec(
            "CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, name TEXT)");
        QSqlQuery setup(userDb);
        setup.exec("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, name TEXT)");

        // Sync B -- should pull A's changeset
        int applied = engineB.sync();
        QVERIFY(applied > 0);

        // User's QSqlDatabase should see the synced row
        QSqlQuery q(userDb);
        q.exec("SELECT name FROM items WHERE id = 1");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("from_A"));
    }

    /// QSqlTableModel backed by the user's QSqlDatabase should see
    /// changes from both engine writes and remote sync.
    void testQSqlTableModelSeesChanges()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // Create table and engine
        QSqlDatabase userDb = createDb(tmpDir.filePath("test.db"));
        QVERIFY(userDb.open());
        QSqlQuery setup(userDb);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        SyncEngine engine(userDb, shared, "client1");
        QVERIFY(engine.start(0));

        // Set up a model
        QSqlTableModel model(nullptr, userDb);
        model.setTable("items");
        model.select();
        QCOMPARE(model.rowCount(), 0);

        // Write through engine
        engine.beginWrite();
        engine.database()->exec("INSERT INTO items (id, name) VALUES (1, 'hello')");
        engine.endWrite();

        // Refresh model -- should see the new row
        model.select();
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0, 1)).toString(), QStringLiteral("hello"));
    }

    /// Bidirectional sync between a QSqlDatabase-based engine and a
    /// path-based engine.
    void testBidirectionalSync()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // Engine A: path-based
        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engineA.start(0));
        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        // Engine B: QSqlDatabase-based
        QSqlDatabase userDb = createDb(tmpDir.filePath("b.db"));
        QVERIFY(userDb.open());
        QSqlQuery setup(userDb);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        SyncEngine engineB(userDb, shared, "clientB");
        QVERIFY(engineB.start(0));

        // A writes
        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO items (id, name) VALUES (1, 'from_A')");
        engineA.endWrite();

        // B writes
        engineB.beginWrite();
        engineB.database()->exec("INSERT INTO items (id, name) VALUES (2, 'from_B')");
        engineB.endWrite();

        // Both sync
        engineA.sync();
        engineB.sync();

        // Both should have both rows
        auto rowsA = engineA.database()->query("SELECT id FROM items ORDER BY id");
        QCOMPARE(rowsA.size(), 2);

        QSqlQuery q(userDb);
        q.exec("SELECT COUNT(*) FROM items");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 2);
    }

    /// The beginWrite/endWrite API should still work on a QSqlDatabase-based
    /// engine and produce synced changesets.
    void testBeginEndWriteProducesChangeset()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase userDb = createDb(tmpDir.filePath("a.db"));
        QVERIFY(userDb.open());
        QSqlQuery setup(userDb);
        setup.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        SyncEngine engineA(userDb, shared, "clientA");
        QVERIFY(engineA.start(0));

        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO items (id, name) VALUES (1, 'tracked')");
        QString filename = engineA.endWrite();
        QVERIFY(!filename.isEmpty());

        // Verify B can sync it
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        QVERIFY(engineB.start(0));
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        int applied = engineB.sync();
        QCOMPARE(applied, 1);

        auto rows = engineB.database()->query("SELECT name FROM items WHERE id = 1");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("tracked"));
    }
};

int TestQSqlDatabaseIntegration::s_connId = 0;

QTEST_MAIN(TestQSqlDatabaseIntegration)

#include "tst_qsqldatabase_integration.moc"
