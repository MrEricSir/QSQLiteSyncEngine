# QSQLiteSyncEngine

[![Unit Tests](https://github.com/MrEricSir/QSQLiteSyncEngine/workflows/CI/badge.svg)](https://github.com/MrEricSir/QSQLiteSyncEngine/actions)

SQLite sync engine for Qt applications. Syncs multiple database instances over a shared folder (Google Drive, Dropbox, local network drive), etc. for integration in your application.

## Under the Hood

Clients write to their own local SQLite database. When changes are made, the engine captures a binary changeset using the SQLite Session Extension, annotates the changeset with a hybrid logical clock (HLC) timestamp and the client's schema version, and writes it to a shared folder. Other clients watch the folder and apply incoming changesets with last-write-wins conflict resolution.

Basic sync strategy:
```
Local Write -> Session Changeset -> Shared Folder -> Remote Clients Apply
```

## Quick Start

The library ships a `QSQLITE_SYNC` driver plugin which works almost exactly like Qt's built-in `QSQLITE` driver. The difference? SQLite Session Extension support.

Once set up, use `QSqlDatabase` and `QSqlQuery` as normal and the database changes will push any local changes to the shared folder automatically.

To pull and apply changes from remote clients via `SyncEngine` users can either configure it to happen automatically via the `start()` method, or call the `sync()` method manually.

```cpp
#include <QSqlDatabase>
#include <QSqlQuery>
#include "syncengine/SyncEngine.h"

using namespace syncengine;

// Open with the QSQLITE_SYNC driver.
QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE_SYNC");
db.setDatabaseName("myapp.db");
db.open();

// Create the sync engine with a shared folder and client ID.
SyncEngine engine(db, "/shared/folder", "client-id");
engine.setSchemaVersion(1);
engine.start();

// Use QSqlQuery as you normally would for database operations. Writes are pushed to the shared folder automatically.
QSqlQuery q(db);
q.exec("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, name TEXT)");
q.exec("INSERT INTO items (name) VALUES ('hello')");
q.prepare("INSERT INTO items (name) VALUES (?)");
q.bindValue(0, "world");
q.exec();

// Call sync() to pull and apply changes from other clients.
engine.sync();
```

Transactions, `QSqlTableModel`, and all other Qt SQL classes work as expected.

## API

### SyncEngine

| Method | Description |
|--------|-------------|
| `start(intervalMs)` | Open the database and begin capturing changes. If `intervalMs > 0`, also pulls remote changes on a timer. Pass `0` to pull manually via `sync()`. See "Pushing and Pulling" below. |
| `stop()` | Stop syncing and close the database. |
| `sync()` | Pull and apply remote changesets from the shared folder. Returns the number applied. See "Pushing and Pulling" below. |
| `setSchemaVersion(v)` | Set the schema version for outgoing changesets. |
| `isRunning()` | Whether the engine is currently running. |

### Signals

| Signal | Description |
|--------|-------------|
| `changesetApplied(filename)` | A remote changeset was successfully applied. |
| `syncErrorOccurred(error, message)` | A sync error occurred. `error` is a `SyncEngine::SyncError` error type enum; `message` is an error string. |
| `syncCompleted(count)` | A sync cycle finished, `count` changesets were applied. |

### SyncError Enum

| Value | Meaning |
|-------|---------|
| `DatabaseError` | Failed to open or access the local database. |
| `TransportError` | Failed to read or write changeset files on the shared folder. |
| `VersionMismatch` | A remote changeset requires a newer schema version. The user should upgrade to resolve. |
| `SchemaMismatch` | A remote changeset was skipped due to incompatible table schema. |
| `ChangesetError` | A remote changeset could not be applied (general failure). |

See also: [the full documentation](https://mrericsir.github.io/QSQLiteSyncEngine/syncengine.html).

## Pushing and Pulling

Sync has two directions -- pushing local changes out and pulling remote changes
in -- and they work differently:

**Pushing (automatic).** When users write to the database via `QSqlQuery`, the
engine's commit hook captures the changes and writes a changeset file to the
shared folder immediately.

**Pulling (manual or timed).** Remote changesets sitting in the shared folder
are NOT applied automatically in the background. Users control when they're
applied by calling `sync()`, either manually or on a timer:

```cpp
// Option 1: auto-pull on a timer.
// start() accepts an interval in milliseconds (default: 1000ms).
engine.start(5000);  // pull every 5 seconds

// Option 2: pull manually whenever you want.
engine.start(0);     // disable the timer
engine.sync();       // pull now
```

Users should generally prefer explicit pulling. This is because applying remote changes modifies the local database, which could affect in-progress queries or UI state. By controlling the timing, users can pull at safe points, for example before or after a UI update.

Listen for the `syncCompleted` signal to be notified of potential changes:

```cpp
connect(&engine, &SyncEngine::syncCompleted, [&](int count) {
    if (count > 0)
        model->select();  // refresh QSqlTableModel
});
```

## Schema Versioning

The engine embeds a schema version in each changeset filename. Clients reject changesets from a newer schema version and emit `syncErrorOccurred` with `VersionMismatch` and an actionable upgrade message. Rejected changesets remain in the shared folder and are automatically retried after the client upgrades.

```cpp
engine.setSchemaVersion(2);  // Ideally set before start()
```

## QSQLITE_SYNC Driver

The `QSQLITE_SYNC` driver plugin is built automatically when Qt source is
available. It is functionally identical to Qt's `QSQLITE` driver but compiled
against a SQLite build with the Session Extension enabled. This allows the sync
engine to share the same database connection as `QSqlQuery`, enabling automatic
change capture without any special write API.

If the driver plugin is not available (e.g., Qt source tree not installed), the
engine falls back to a parallel connection and requires `beginWrite()` /
`endWrite()` calls to track changes.

## How To Include

### Git Submodule

```bash
git submodule add https://github.com/MrEricSir/QSQLiteSyncEngine.git external/QSQLiteSyncEngine
git add .gitmodules
git commit -m "Add QSQLiteSyncEngine submodule"
```

Then in your `CMakeLists.txt`:

```cmake
add_subdirectory(external/QSQLiteSyncEngine)

target_link_libraries(YourApp PRIVATE syncengine)
```

## Building and Running the Tests

```bash
cmake -B build
cmake --build build
ctest --test-dir build
```

## Documentation

QDoc documentation can be generated with:

```bash
cmake --build build --target QSQLiteSyncEngine_docs
```

The HTML output will be written to `docs/html/`.

## Contributing

Found a bug? Want to propose an improvement? File
[a ticket](https://github.com/MrEricSir/QSQLiteSyncEngine/issues) and/or submit
[a pull request](https://github.com/MrEricSir/QSQLiteSyncEngine/pulls).

Contributions are welcome!
