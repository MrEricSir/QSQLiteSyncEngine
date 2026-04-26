#include <QTest>
#include <QTemporaryDir>

#include "syncengine/SyncEngine.h"
#include "syncengine/ChangesetManager.h"

using namespace syncengine;

/// Tests for schema version gating: the sync engine embeds a schema version
/// in each changeset filename and rejects changesets from a newer schema
/// version than the local database.
///
/// The application sets the schema version on the SyncEngine -- the engine
/// doesn't manage migrations or store the version itself.
class TestSchemaVersion : public QObject {
    Q_OBJECT
private slots:

    // ---------------------------------------------------------------
    // Filename format tests
    // ---------------------------------------------------------------

    /// The version should be embedded in the changeset filename and
    /// round-trip through parse/build.
    void testFilenameIncludesVersion()
    {
        QString filename = ChangesetManager::buildFilename("client1", 100, 0, 3);
        ChangesetInfo info = ChangesetManager::parseFilename(filename);

        QCOMPARE(info.clientId, QStringLiteral("client1"));
        QCOMPARE(info.hlc, 100ULL);
        QCOMPARE(info.sequence, 0ULL);
        QCOMPARE(info.schemaVersion, 3);
    }

    /// Changesets without a version field (legacy format) should parse
    /// with version 0, maintaining backward compatibility.
    void testLegacyFilenameDefaultsToVersionZero()
    {
        // Old format: {clientId}_{hlc}_{sequence}.changeset
        QString legacy = QStringLiteral("client1_00000000000000000100_0000000000.changeset");
        ChangesetInfo info = ChangesetManager::parseFilename(legacy);

        QCOMPARE(info.clientId, QStringLiteral("client1"));
        QCOMPARE(info.hlc, 100ULL);
        QCOMPARE(info.sequence, 0ULL);
        QCOMPARE(info.schemaVersion, 0);
    }

    // ---------------------------------------------------------------
    // SyncEngine version API tests
    // ---------------------------------------------------------------

    /// The engine should accept a schema version and include it in
    /// changeset filenames produced by endWrite().
    void testEndWriteEmbedsSchemaVersion()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engine(tmpDir.filePath("a.db"), shared, "clientA");
        engine.setSchemaVersion(5);
        QVERIFY(engine.start(0));

        engine.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        engine.beginWrite();
        engine.database()->exec("INSERT INTO items (id, name) VALUES (1, 'test')");
        QString filename = engine.endWrite();

        QVERIFY(!filename.isEmpty());

        ChangesetInfo info = ChangesetManager::parseFilename(filename);
        QCOMPARE(info.schemaVersion, 5);
    }

    /// Default schema version should be 0 when not explicitly set.
    void testDefaultSchemaVersionIsZero()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engine(tmpDir.filePath("a.db"), shared, "clientA");
        QVERIFY(engine.start(0));

        engine.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        engine.beginWrite();
        engine.database()->exec("INSERT INTO items (id, name) VALUES (1, 'test')");
        QString filename = engine.endWrite();

        ChangesetInfo info = ChangesetManager::parseFilename(filename);
        QCOMPARE(info.schemaVersion, 0);
    }

    // ---------------------------------------------------------------
    // Version gating: reject newer, accept equal and older
    // ---------------------------------------------------------------

    /// A client on v2 should accept changesets from another v2 client.
    void testSameVersionSyncsNormally()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineA(tmpDir.filePath("a.db"), shared, "clientA");
        SyncEngine engineB(tmpDir.filePath("b.db"), shared, "clientB");
        engineA.setSchemaVersion(2);
        engineB.setSchemaVersion(2);
        QVERIFY(engineA.start(0));
        QVERIFY(engineB.start(0));

        engineA.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineB.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");

        engineA.beginWrite();
        engineA.database()->exec("INSERT INTO items (id, name) VALUES (1, 'hello')");
        engineA.endWrite();

        int applied = engineB.sync();
        QCOMPARE(applied, 1);

        auto rows = engineB.database()->query("SELECT name FROM items WHERE id = 1");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("hello"));
    }

    /// A v3 client should accept changesets produced by a v2 client
    /// (older version → compatible, trailing columns get defaults).
    void testNewerClientAcceptsOlderChangesets()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineOld(tmpDir.filePath("old.db"), shared, "clientOld");
        SyncEngine engineNew(tmpDir.filePath("new.db"), shared, "clientNew");
        engineOld.setSchemaVersion(2);
        engineNew.setSchemaVersion(3);
        QVERIFY(engineOld.start(0));
        QVERIFY(engineNew.start(0));

        // Old client has v2 schema
        engineOld.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        // New client has v3 schema with extra column
        engineNew.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, priority INTEGER DEFAULT 0)");

        engineOld.beginWrite();
        engineOld.database()->exec("INSERT INTO items (id, name) VALUES (1, 'task')");
        engineOld.endWrite();

        // New client syncs: should accept v2 changeset
        QStringList errors;
        QObject::connect(&engineNew, &SyncEngine::syncErrorOccurred, [&](SyncEngine::SyncError, const QString &msg) {
            errors.append(msg);
        });

        int applied = engineNew.sync();
        QCOMPARE(applied, 1);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join("; ")));

        auto rows = engineNew.database()->query(
            "SELECT name, priority FROM items WHERE id = 1");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("task"));
        QCOMPARE(rows[0][1], QStringLiteral("0")); // default
    }

    /// A v2 client must reject changesets from a v3 client and emit
    /// a syncError with an actionable message telling the user to upgrade.
    void testOlderClientRejectsNewerChangesets()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineOld(tmpDir.filePath("old.db"), shared, "clientOld");
        SyncEngine engineNew(tmpDir.filePath("new.db"), shared, "clientNew");
        engineOld.setSchemaVersion(2);
        engineNew.setSchemaVersion(3);
        QVERIFY(engineOld.start(0));
        QVERIFY(engineNew.start(0));

        engineOld.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineNew.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, priority INTEGER DEFAULT 0)");

        // New client writes
        engineNew.beginWrite();
        engineNew.database()->exec(
            "INSERT INTO items (id, name, priority) VALUES (1, 'task', 5)");
        engineNew.endWrite();

        // Old client syncs; should reject v3 changeset
        QStringList errors;
        QObject::connect(&engineOld, &SyncEngine::syncErrorOccurred, [&](SyncEngine::SyncError, const QString &msg) {
            errors.append(msg);
        });

        int applied = engineOld.sync();
        QCOMPARE(applied, 0);
        QVERIFY(!errors.isEmpty());

        // Error message should be actionable: mention the version mismatch
        bool hasVersionMessage = false;
        for (const QString &msg : errors) {
            if (msg.contains("version") || msg.contains("upgrade")) {
                hasVersionMessage = true;
                break;
            }
        }
        QVERIFY2(hasVersionMessage,
                 qPrintable("Error should mention version/upgrade. Got: " + errors.join("; ")));

        // Data should NOT be on the old client
        auto rows = engineOld.database()->query("SELECT * FROM items");
        QCOMPARE(rows.size(), 0);
    }

    /// Rejected changesets should remain unapplied so they are retried
    /// after the client upgrades.
    void testRejectedChangesetsRetriedAfterUpgrade()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineOld(tmpDir.filePath("old.db"), shared, "clientOld");
        SyncEngine engineNew(tmpDir.filePath("new.db"), shared, "clientNew");
        engineOld.setSchemaVersion(2);
        engineNew.setSchemaVersion(3);
        QVERIFY(engineOld.start(0));
        QVERIFY(engineNew.start(0));

        engineOld.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineNew.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, priority INTEGER DEFAULT 0)");

        // New client writes
        engineNew.beginWrite();
        engineNew.database()->exec(
            "INSERT INTO items (id, name, priority) VALUES (1, 'task', 5)");
        engineNew.endWrite();

        // Old client syncs: rejected
        QCOMPARE(engineOld.sync(), 0);

        // "Upgrade" the old client: add the column and bump version
        engineOld.database()->exec("ALTER TABLE items ADD COLUMN priority INTEGER DEFAULT 0");
        engineOld.setSchemaVersion(3);

        // Sync again: should now succeed
        int applied = engineOld.sync();
        QCOMPARE(applied, 1);

        auto rows = engineOld.database()->query(
            "SELECT name, priority FROM items WHERE id = 1");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0][0], QStringLiteral("task"));
        QCOMPARE(rows[0][1], QStringLiteral("5"));
    }

    /// Multiple version jumps: a v1 client should reject changesets from
    /// v3 even if v2 changesets are also present. Only v1 (and v0/legacy)
    /// changesets should be applied.
    void testMultipleVersionsInSharedFolder()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engine1(tmpDir.filePath("v1.db"), shared, "client1");
        SyncEngine engine2(tmpDir.filePath("v2.db"), shared, "client2");
        SyncEngine engine3(tmpDir.filePath("v3.db"), shared, "client3");
        engine1.setSchemaVersion(1);
        engine2.setSchemaVersion(2);
        engine3.setSchemaVersion(3);
        QVERIFY(engine1.start(0));
        QVERIFY(engine2.start(0));
        QVERIFY(engine3.start(0));

        // All have the same base schema (v1 compatible)
        engine1.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engine2.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, tag TEXT)");
        engine3.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, tag TEXT, priority INT)");

        // v1 writes (compatible with everyone)
        engine1.beginWrite();
        engine1.database()->exec("INSERT INTO items (id, name) VALUES (1, 'from_v1')");
        engine1.endWrite();

        // v2 writes
        engine2.beginWrite();
        engine2.database()->exec("INSERT INTO items (id, name, tag) VALUES (2, 'from_v2', 'a')");
        engine2.endWrite();

        // v3 writes
        engine3.beginWrite();
        engine3.database()->exec(
            "INSERT INTO items (id, name, tag, priority) VALUES (3, 'from_v3', 'b', 1)");
        engine3.endWrite();

        // v1 client syncs: should only get its own and reject v2 and v3
        QStringList errors;
        QObject::connect(&engine1, &SyncEngine::syncErrorOccurred, [&](SyncEngine::SyncError, const QString &msg) {
            errors.append(msg);
        });

        int applied = engine1.sync();
        QCOMPARE(applied, 0); // v2 and v3 are both rejected (own changeset already marked)

        // Should have error messages about version 2 and version 3
        QVERIFY(errors.size() >= 2);

        auto rows = engine1.database()->query("SELECT id FROM items ORDER BY id");
        QCOMPARE(rows.size(), 1); // only its own row
        QCOMPARE(rows[0][0], QStringLiteral("1"));
    }

    /// Bidirectional sync between v2 and v3: v3 accepts v2 changesets,
    /// v2 rejects v3 changesets, both sides report appropriate status.
    void testBidirectionalVersionMismatch()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        SyncEngine engineV2(tmpDir.filePath("v2.db"), shared, "clientV2");
        SyncEngine engineV3(tmpDir.filePath("v3.db"), shared, "clientV3");
        engineV2.setSchemaVersion(2);
        engineV3.setSchemaVersion(3);
        QVERIFY(engineV2.start(0));
        QVERIFY(engineV3.start(0));

        engineV2.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineV3.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, extra TEXT DEFAULT '')");

        // Both write
        engineV2.beginWrite();
        engineV2.database()->exec("INSERT INTO items (id, name) VALUES (1, 'from_v2')");
        engineV2.endWrite();

        engineV3.beginWrite();
        engineV3.database()->exec(
            "INSERT INTO items (id, name, extra) VALUES (2, 'from_v3', 'data')");
        engineV3.endWrite();

        QStringList errorsV2, errorsV3;
        QObject::connect(&engineV2, &SyncEngine::syncErrorOccurred, [&](SyncEngine::SyncError, const QString &msg) {
            errorsV2.append(msg);
        });
        QObject::connect(&engineV3, &SyncEngine::syncErrorOccurred, [&](SyncEngine::SyncError, const QString &msg) {
            errorsV3.append(msg);
        });

        // Both sync
        engineV3.sync();
        engineV2.sync();

        // v3 should have both rows (accepted v2's changeset)
        auto rowsV3 = engineV3.database()->query("SELECT id FROM items ORDER BY id");
        QCOMPARE(rowsV3.size(), 2);
        QVERIFY(errorsV3.isEmpty());

        // v2 should only have its own row (rejected v3's changeset)
        auto rowsV2 = engineV2.database()->query("SELECT id FROM items ORDER BY id");
        QCOMPARE(rowsV2.size(), 1);
        QCOMPARE(rowsV2[0][0], QStringLiteral("1"));
        QVERIFY(!errorsV2.isEmpty());
    }

    /// Legacy changesets (version 0 / no version in filename) should be
    /// accepted by any client regardless of its schema version.
    void testLegacyChangesetsAcceptedByAllVersions()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QString shared = tmpDir.filePath("shared");

        // Engine with no version set (legacy, version 0)
        SyncEngine engineLegacy(tmpDir.filePath("legacy.db"), shared, "clientLegacy");
        // Engine on v5
        SyncEngine engineV5(tmpDir.filePath("v5.db"), shared, "clientV5");
        engineV5.setSchemaVersion(5);

        QVERIFY(engineLegacy.start(0));
        QVERIFY(engineV5.start(0));

        engineLegacy.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)");
        engineV5.database()->exec(
            "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, a TEXT, b TEXT, c TEXT, d TEXT)");

        // Legacy client writes
        engineLegacy.beginWrite();
        engineLegacy.database()->exec("INSERT INTO items (id, name) VALUES (1, 'old')");
        engineLegacy.endWrite();

        // v5 syncs: should accept legacy changeset
        QStringList errors;
        QObject::connect(&engineV5, &SyncEngine::syncErrorOccurred, [&](SyncEngine::SyncError, const QString &msg) {
            errors.append(msg);
        });

        int applied = engineV5.sync();
        QCOMPARE(applied, 1);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join("; ")));
    }
};

QTEST_MAIN(TestSchemaVersion)

#include "tst_schema_version.moc"
