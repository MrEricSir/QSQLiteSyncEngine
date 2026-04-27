#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>

#include "syncengine/SyncEngine.h"

using namespace syncengine;

static void printUsage()
{
    QTextStream err(stderr);
    err << "Usage: syncdemo <shared-folder> [--client <id>] [--db <path>]\n"
        << "\n"
        << "  shared-folder   Path to the shared sync folder (required)\n"
        << "  --client <id>   Client identifier (default: client1)\n"
        << "  --db <path>     Local database file path (default: app data dir)\n"
        << "\n"
        << "The shared folder can be on Google Drive, Dropbox, or a network drive.\n"
        << "The database file is always stored locally (never on the shared drive).\n";
}

static void printHelp()
{
    QTextStream out(stdout);
    out << "\nQSQLiteSyncEngine Demo\n";
    out << "======================\n";
    out << "Commands:\n";
    out << "  insert <name> <value>  - Insert a row into the test table\n";
    out << "  update <id> <value>    - Update a row's value\n";
    out << "  delete <id>            - Delete a row\n";
    out << "  select                 - Show all rows\n";
    out << "  sync                   - Manually trigger sync\n";
    out << "  status                 - Show sync status\n";
    out << "  help                   - Show this help\n";
    out << "  quit                   - Exit\n\n";
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Make sure the QSQLITE_SYNC plugin can be found next to the binary
    app.addLibraryPath(app.applicationDirPath());

    // Parse arguments
    QString clientId = QStringLiteral("client1");
    QString sharedFolder;
    QString dbPath;

    QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--client") && i + 1 < args.size()) {
            clientId = args[++i];
        } else if (args[i] == QStringLiteral("--db") && i + 1 < args.size()) {
            dbPath = args[++i];
        } else if (!args[i].startsWith('-') && sharedFolder.isEmpty()) {
            sharedFolder = args[i];
        }
    }

    if (sharedFolder.isEmpty()) {
        printUsage();
        return 1;
    }

    // Database must be stored locally, never on the shared/network drive.
    // SQLite WAL mode does not work on network filesystems.
    if (dbPath.isEmpty()) {
        QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dataDir);
        dbPath = dataDir + QStringLiteral("/sync_%1.db").arg(clientId);
    }

    QTextStream out(stdout);
    QTextStream in(stdin);

    out << "Starting SyncEngine as client: " << clientId << "\n";
    out << "Database: " << dbPath << "\n";
    out << "Shared folder: " << sharedFolder << "\n";

    // Open the database with the QSQLITE_SYNC driver
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE_SYNC");
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        out << "Failed to open database: " << db.lastError().text() << "\n";
        return 1;
    }

    // Create test table BEFORE starting the engine. start() runs an initial
    // sync, so the table must exist for incoming changesets to apply.
    QSqlQuery setup(db);
    setup.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS items ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  value TEXT,"
        "  modified_by TEXT"
        ")"));

    SyncEngine engine(db, sharedFolder, clientId);

    QObject::connect(&engine, &SyncEngine::changesetApplied, [&](const QString &filename) {
        out << "[sync] Applied: " << filename << "\n";
        out.flush();
    });
    QObject::connect(&engine, &SyncEngine::syncErrorOccurred,
                     [&](SyncEngine::SyncError error, const QString &msg) {
        Q_UNUSED(error);
        out << "[sync] Error: " << msg << "\n";
        out.flush();
    });
    QObject::connect(&engine, &SyncEngine::syncCompleted, [&](int count) {
        if (count > 0) {
            out << "[sync] Completed, applied " << count << " changesets\n";
            out.flush();
        }
    });

    if (!engine.start(2000)) {
        out << "Failed to start sync engine\n";
        return 1;
    }

    printHelp();
    out << "> ";
    out.flush();

    // Simple command loop using a timer to read stdin without blocking the event loop
    QTimer inputTimer;
    inputTimer.setInterval(100);
    QObject::connect(&inputTimer, &QTimer::timeout, [&]() {
        if (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty()) {
                out << "> ";
                out.flush();
                return;
            }

            QStringList parts = line.split(QChar(' '), Qt::SkipEmptyParts);
            QString cmd = parts.first().toLower();

            if (cmd == QStringLiteral("quit") || cmd == QStringLiteral("exit")) {
                app.quit();
                return;
            } else if (cmd == QStringLiteral("help")) {
                printHelp();
            } else if (cmd == QStringLiteral("insert") && parts.size() >= 3) {
                QSqlQuery q(db);
                q.prepare("INSERT INTO items (name, value, modified_by) VALUES (?, ?, ?)");
                q.bindValue(0, parts[1]);
                q.bindValue(1, parts[2]);
                q.bindValue(2, clientId);
                if (q.exec())
                    out << "Inserted (id " << q.lastInsertId().toString() << ")\n";
                else
                    out << "Insert failed: " << q.lastError().text() << "\n";
            } else if (cmd == QStringLiteral("update") && parts.size() >= 3) {
                QSqlQuery q(db);
                q.prepare("UPDATE items SET value = ?, modified_by = ? WHERE id = ?");
                q.bindValue(0, parts[2]);
                q.bindValue(1, clientId);
                q.bindValue(2, parts[1].toInt());
                if (q.exec())
                    out << "Updated " << q.numRowsAffected() << " row(s)\n";
                else
                    out << "Update failed: " << q.lastError().text() << "\n";
            } else if (cmd == QStringLiteral("delete") && parts.size() >= 2) {
                QSqlQuery q(db);
                q.prepare("DELETE FROM items WHERE id = ?");
                q.bindValue(0, parts[1].toInt());
                if (q.exec())
                    out << "Deleted " << q.numRowsAffected() << " row(s)\n";
                else
                    out << "Delete failed: " << q.lastError().text() << "\n";
            } else if (cmd == QStringLiteral("select")) {
                QSqlQuery q(db);
                q.exec("SELECT id, name, value, modified_by FROM items ORDER BY id");
                out << QString::asprintf("%-5s %-20s %-20s %-15s\n", "ID", "Name", "Value", "Modified By");
                out << QString(60, QChar('-')) << "\n";
                while (q.next()) {
                    out << QString::asprintf("%-5s %-20s %-20s %-15s\n",
                                             qPrintable(q.value(0).toString()),
                                             qPrintable(q.value(1).toString()),
                                             qPrintable(q.value(2).toString()),
                                             qPrintable(q.value(3).toString()));
                }
            } else if (cmd == QStringLiteral("sync")) {
                int n = engine.sync();
                out << "Synced " << n << " changesets\n";
            } else if (cmd == QStringLiteral("status")) {
                QSqlQuery q(db);
                q.exec("SELECT COUNT(*) FROM _sync_applied");
                q.next();
                out << "Client: " << clientId << "\n";
                out << "Applied changesets: " << q.value(0).toString() << "\n";
            } else {
                out << "Unknown command. Type 'help' for usage.\n";
            }

            out << "> ";
            out.flush();
        }
    });
    inputTimer.start();

    return app.exec();
}
