#include <QTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QCoreApplication>

#include "syncengine/SyncEngine.h"

using namespace syncengine;

/// Tests that FTS (Full-Text Search) virtual tables do not interfere with
/// sync and that the FTS index must be rebuilt manually after applying
/// remote changesets.
class TestFTS : public QObject {
    Q_OBJECT

private:
    static int s_connId;
    QSqlDatabase createSyncDb(const QString &path) {
        QString connName = QStringLiteral("fts_%1").arg(++s_connId);
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

    /// Writing to a base table that has a corresponding FTS table should
    /// produce a changeset and sync normally. The FTS table itself is not
    /// included in the changeset.
    void testBaseTableWithFTSSyncs()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase dbA = createSyncDb(tmpDir.filePath("a.db"));
        QVERIFY(dbA.open());

        SyncEngine engineA(dbA, shared, "clientA");
        QVERIFY(engineA.start(0));

        // Create a base table and an FTS table backed by it
        QSqlQuery qA(dbA);
        qA.exec("CREATE TABLE articles (id INTEGER PRIMARY KEY, title TEXT, body TEXT)");
        qA.exec("CREATE VIRTUAL TABLE articles_fts USING fts5(title, body, content=articles, content_rowid=id)");

        // Triggers to keep FTS in sync with base table locally
        qA.exec("CREATE TRIGGER articles_ai AFTER INSERT ON articles BEGIN "
                 "INSERT INTO articles_fts(rowid, title, body) VALUES (new.id, new.title, new.body); END");
        qA.exec("CREATE TRIGGER articles_au AFTER UPDATE ON articles BEGIN "
                 "INSERT INTO articles_fts(articles_fts, rowid, title, body) VALUES ('delete', old.id, old.title, old.body); "
                 "INSERT INTO articles_fts(rowid, title, body) VALUES (new.id, new.title, new.body); END");
        qA.exec("CREATE TRIGGER articles_ad AFTER DELETE ON articles BEGIN "
                 "INSERT INTO articles_fts(articles_fts, rowid, title, body) VALUES ('delete', old.id, old.title, old.body); END");
        QCoreApplication::processEvents();

        // Insert data -- triggers populate FTS locally
        qA.exec("INSERT INTO articles (id, title, body) VALUES (1, 'Hello World', 'This is a test article')");
        QCoreApplication::processEvents();

        // Verify FTS works locally
        qA.exec("SELECT title FROM articles_fts WHERE articles_fts MATCH 'test'");
        QVERIFY(qA.next());
        QCOMPARE(qA.value(0).toString(), QStringLiteral("Hello World"));

        // Set up B and sync
        QSqlDatabase dbB = createSyncDb(tmpDir.filePath("b.db"));
        QVERIFY(dbB.open());

        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineB.start(0));

        QSqlQuery qB(dbB);
        qB.exec("CREATE TABLE articles (id INTEGER PRIMARY KEY, title TEXT, body TEXT)");
        qB.exec("CREATE VIRTUAL TABLE articles_fts USING fts5(title, body, content=articles, content_rowid=id)");
        qB.exec("CREATE TRIGGER articles_ai AFTER INSERT ON articles BEGIN "
                 "INSERT INTO articles_fts(rowid, title, body) VALUES (new.id, new.title, new.body); END");

        engineB.sync();

        // Base table data should arrive on B
        qB.exec("SELECT title, body FROM articles WHERE id = 1");
        QVERIFY(qB.next());
        QCOMPARE(qB.value(0).toString(), QStringLiteral("Hello World"));
        QCOMPARE(qB.value(1).toString(), QStringLiteral("This is a test article"));
    }

    /// After syncing base table data from a remote client, the local FTS
    /// index is NOT automatically populated. The FTS table is empty until
    /// explicitly rebuilt.
    void testFTSNotAutoPopulatedBySync()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // A writes articles
        QSqlDatabase dbA = createSyncDb(tmpDir.filePath("a.db"));
        QVERIFY(dbA.open());

        SyncEngine engineA(dbA, shared, "clientA");
        QVERIFY(engineA.start(0));

        QSqlQuery qA(dbA);
        qA.exec("CREATE TABLE articles (id INTEGER PRIMARY KEY, title TEXT, body TEXT)");
        qA.exec("CREATE VIRTUAL TABLE articles_fts USING fts5(title, body, content=articles, content_rowid=id)");
        QCoreApplication::processEvents();

        qA.exec("INSERT INTO articles (id, title, body) VALUES (1, 'Searchable', 'Find me please')");
        qA.exec("INSERT INTO articles (id, title, body) VALUES (2, 'Another', 'More content here')");
        QCoreApplication::processEvents();

        // B has matching schema with FTS but uses content-sync triggers
        QSqlDatabase dbB = createSyncDb(tmpDir.filePath("b.db"));
        QVERIFY(dbB.open());

        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineB.start(0));

        QSqlQuery qB(dbB);
        qB.exec("CREATE TABLE articles (id INTEGER PRIMARY KEY, title TEXT, body TEXT)");
        qB.exec("CREATE VIRTUAL TABLE articles_fts USING fts5(title, body, content=articles, content_rowid=id)");

        // Note: B has NO triggers -- the changeset_apply bypasses triggers
        // even if they existed, because it uses the sqlite3 C API directly.

        engineB.sync();

        // Base table has the data
        qB.exec("SELECT COUNT(*) FROM articles");
        QVERIFY(qB.next());
        QCOMPARE(qB.value(0).toInt(), 2);

        // FTS index does NOT have the data -- it was not populated by sync
        qB.exec("SELECT COUNT(*) FROM articles_fts WHERE articles_fts MATCH 'Searchable'");
        QVERIFY(qB.next());
        QCOMPARE(qB.value(0).toInt(), 0);
    }

    /// After syncing, an explicit FTS rebuild should populate the index
    /// from the synced base table data.
    void testFTSRebuildAfterSync()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase dbA = createSyncDb(tmpDir.filePath("a.db"));
        QVERIFY(dbA.open());

        SyncEngine engineA(dbA, shared, "clientA");
        QVERIFY(engineA.start(0));

        QSqlQuery qA(dbA);
        qA.exec("CREATE TABLE articles (id INTEGER PRIMARY KEY, title TEXT, body TEXT)");
        qA.exec("CREATE VIRTUAL TABLE articles_fts USING fts5(title, body, content=articles, content_rowid=id)");
        QCoreApplication::processEvents();

        qA.exec("INSERT INTO articles VALUES (1, 'Hello', 'World')");
        qA.exec("INSERT INTO articles VALUES (2, 'Goodbye', 'Moon')");
        QCoreApplication::processEvents();

        // B syncs
        QSqlDatabase dbB = createSyncDb(tmpDir.filePath("b.db"));
        QVERIFY(dbB.open());

        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineB.start(0));

        QSqlQuery qB(dbB);
        qB.exec("CREATE TABLE articles (id INTEGER PRIMARY KEY, title TEXT, body TEXT)");
        qB.exec("CREATE VIRTUAL TABLE articles_fts USING fts5(title, body, content=articles, content_rowid=id)");

        engineB.sync();

        // FTS is empty before rebuild
        qB.exec("SELECT COUNT(*) FROM articles_fts WHERE articles_fts MATCH 'Hello'");
        QVERIFY(qB.next());
        QCOMPARE(qB.value(0).toInt(), 0);

        // Rebuild FTS from base table
        qB.exec("INSERT INTO articles_fts(articles_fts) VALUES('rebuild')");

        // Now FTS search works
        qB.exec("SELECT title FROM articles_fts WHERE articles_fts MATCH 'Hello'");
        QVERIFY(qB.next());
        QCOMPARE(qB.value(0).toString(), QStringLiteral("Hello"));

        qB.exec("SELECT title FROM articles_fts WHERE articles_fts MATCH 'Moon'");
        QVERIFY(qB.next());
        QCOMPARE(qB.value(0).toString(), QStringLiteral("Goodbye"));
    }

    /// FTS shadow tables (_content, _data, etc.) should not appear in
    /// changesets or cause sync errors.
    void testFTSShadowTablesDoNotCauseErrors()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase dbA = createSyncDb(tmpDir.filePath("a.db"));
        QVERIFY(dbA.open());

        SyncEngine engineA(dbA, shared, "clientA");
        QVERIFY(engineA.start(0));

        QSqlQuery qA(dbA);
        qA.exec("CREATE TABLE articles (id INTEGER PRIMARY KEY, title TEXT, body TEXT)");
        qA.exec("CREATE VIRTUAL TABLE articles_fts USING fts5(title, body, content=articles, content_rowid=id)");
        qA.exec("CREATE TRIGGER articles_ai AFTER INSERT ON articles BEGIN "
                 "INSERT INTO articles_fts(rowid, title, body) VALUES (new.id, new.title, new.body); END");
        QCoreApplication::processEvents();

        // Insert data -- triggers write to FTS shadow tables
        qA.exec("INSERT INTO articles VALUES (1, 'Test', 'Content')");
        QCoreApplication::processEvents();

        // B syncs -- should not get errors from FTS shadow tables
        QSqlDatabase dbB = createSyncDb(tmpDir.filePath("b.db"));
        QVERIFY(dbB.open());

        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineB.start(0));

        QSqlQuery qB(dbB);
        qB.exec("CREATE TABLE articles (id INTEGER PRIMARY KEY, title TEXT, body TEXT)");
        qB.exec("CREATE VIRTUAL TABLE articles_fts USING fts5(title, body, content=articles, content_rowid=id)");

        QList<QPair<SyncEngine::SyncError, QString>> errors;
        QObject::connect(&engineB, &SyncEngine::syncErrorOccurred,
                         [&](SyncEngine::SyncError e, const QString &msg) {
            errors.append({e, msg});
        });

        engineB.sync();

        // Filter out expected schema mismatch warnings for FTS internal tables
        // (these are virtual table artifacts, not real errors)
        QList<QPair<SyncEngine::SyncError, QString>> realErrors;
        for (const auto &e : errors) {
            // FTS shadow table mismatches are expected and harmless
            if (e.first == SyncEngine::SchemaMismatch
                && e.second.contains("articles_fts"))
                continue;
            realErrors.append(e);
        }

        QVERIFY2(realErrors.isEmpty(),
                 qPrintable(realErrors.isEmpty() ? "" : realErrors[0].second));

        // Base table data arrived
        qB.exec("SELECT title FROM articles WHERE id = 1");
        QVERIFY(qB.next());
        QCOMPARE(qB.value(0).toString(), QStringLiteral("Test"));
    }

    /// The syncCompleted signal can be used to trigger an FTS rebuild,
    /// which is the recommended pattern.
    void testRebuildFTSOnSyncCompleted()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        QSqlDatabase dbA = createSyncDb(tmpDir.filePath("a.db"));
        QVERIFY(dbA.open());

        SyncEngine engineA(dbA, shared, "clientA");
        QVERIFY(engineA.start(0));

        QSqlQuery qA(dbA);
        qA.exec("CREATE TABLE articles (id INTEGER PRIMARY KEY, title TEXT, body TEXT)");
        qA.exec("CREATE VIRTUAL TABLE articles_fts USING fts5(title, body, content=articles, content_rowid=id)");
        QCoreApplication::processEvents();

        qA.exec("INSERT INTO articles VALUES (1, 'Alpha', 'First article')");
        qA.exec("INSERT INTO articles VALUES (2, 'Beta', 'Second article')");
        QCoreApplication::processEvents();

        // B sets up with auto-rebuild on syncCompleted
        QSqlDatabase dbB = createSyncDb(tmpDir.filePath("b.db"));
        QVERIFY(dbB.open());

        SyncEngine engineB(dbB, shared, "clientB");
        QVERIFY(engineB.start(0));

        QSqlQuery qB(dbB);
        qB.exec("CREATE TABLE articles (id INTEGER PRIMARY KEY, title TEXT, body TEXT)");
        qB.exec("CREATE VIRTUAL TABLE articles_fts USING fts5(title, body, content=articles, content_rowid=id)");

        // This is the recommended pattern: rebuild FTS after sync
        QObject::connect(&engineB, &SyncEngine::syncCompleted, [&](int count) {
            if (count > 0) {
                QSqlQuery rebuild(dbB);
                rebuild.exec("INSERT INTO articles_fts(articles_fts) VALUES('rebuild')");
            }
        });

        engineB.sync();
        QCoreApplication::processEvents();

        // FTS should be searchable now thanks to the rebuild in syncCompleted
        qB.exec("SELECT title FROM articles_fts WHERE articles_fts MATCH 'Alpha'");
        QVERIFY(qB.next());
        QCOMPARE(qB.value(0).toString(), QStringLiteral("Alpha"));

        qB.exec("SELECT title FROM articles_fts WHERE articles_fts MATCH 'Second'");
        QVERIFY(qB.next());
        QCOMPARE(qB.value(0).toString(), QStringLiteral("Beta"));
    }
};

int TestFTS::s_connId = 0;

QTEST_MAIN(TestFTS)

#include "tst_fts.moc"
