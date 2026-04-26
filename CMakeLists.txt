cmake_minimum_required(VERSION 3.16)

project(QSQLiteSyncEngine VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Sql)
find_package(Qt6SqlPrivate QUIET)

# --- SQLite with session extension enabled ---
# Qt bundles SQLite source in its 3rdparty directory but does not compile it
# with SQLITE_ENABLE_SESSION. We reuse Qt's copy of the amalgamation so we
# don't ship a separate download, but compile it ourselves with the flags we
# need. If Qt's source tree is not available, fall back to a local copy.
set(QT_SQLITE_SOURCE "" CACHE PATH "Path to Qt's bundled sqlite3.c (auto-detected)")
if(NOT QT_SQLITE_SOURCE)
    # Derive the Qt source tree from CMAKE_PREFIX_PATH or the Qt6 package dir.
    # Qt online installer places sources at <qt_root>/<version>/Src/
    # and the SDK at <qt_root>/<version>/<platform>/.
    if(Qt6_DIR)
        # Qt6_DIR is something like <root>/6.10.0/macos/lib/cmake/Qt6
        get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
        get_filename_component(_qt_version_root "${_qt_prefix}/.." ABSOLUTE)
    elseif(CMAKE_PREFIX_PATH)
        list(GET CMAKE_PREFIX_PATH 0 _qt_prefix)
        get_filename_component(_qt_version_root "${_qt_prefix}/.." ABSOLUTE)
    else()
        set(_qt_version_root "")
    endif()
    set(_qt_sqlite_dir "${_qt_version_root}/Src/qtbase/src/3rdparty/sqlite")
    if(_qt_version_root AND EXISTS "${_qt_sqlite_dir}/sqlite3.c")
        set(QT_SQLITE_SOURCE "${_qt_sqlite_dir}")
        message(STATUS "Using Qt's bundled SQLite from: ${QT_SQLITE_SOURCE}")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c")
        set(QT_SQLITE_SOURCE "${CMAKE_SOURCE_DIR}/third_party/sqlite")
        message(STATUS "Using local SQLite from: ${QT_SQLITE_SOURCE}")
    else()
        message(FATAL_ERROR
            "Could not find SQLite amalgamation source.\n"
            "Either install Qt sources or place sqlite3.c/sqlite3.h in third_party/sqlite/.\n"
            "You can also set -DQT_SQLITE_SOURCE=/path/to/dir containing sqlite3.c")
    endif()
endif()

add_library(sqlite3 STATIC "${QT_SQLITE_SOURCE}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${QT_SQLITE_SOURCE}")
target_compile_definitions(sqlite3 PUBLIC
    SQLITE_ENABLE_SESSION
    SQLITE_ENABLE_PREUPDATE_HOOK
    SQLITE_ENABLE_COLUMN_METADATA
    SQLITE_ENABLE_FTS5
)
# Silence warnings in third-party code
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /w)
else()
    target_compile_options(sqlite3 PRIVATE -w)
endif()
set_target_properties(sqlite3 PROPERTIES AUTOMOC OFF)

# --- Sync engine library ---
add_library(syncengine STATIC
    src/syncengine/HybridLogicalClock.cpp
    src/syncengine/SyncableDatabase.cpp
    src/syncengine/SharedFolderTransport.cpp
    src/syncengine/ChangesetManager.cpp
    src/syncengine/ConflictResolver.cpp
    src/syncengine/SyncEngine.cpp
    src/syncengine/SchemaLogCapture.cpp
    src/syncengine/SyncQuery.cpp
)

add_library(syncengine::syncengine ALIAS syncengine)

target_include_directories(syncengine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(syncengine PUBLIC Qt6::Core Qt6::Sql sqlite3)

# --- QSQLITE_SYNC driver plugin ---
# Recompiles Qt's QSQLITE driver source against our sqlite3 (with session
# extension enabled) and registers it as the "QSQLITE_SYNC" driver.
# Requires Qt source tree for the driver source files and private headers.
# --- QSQLITE_SYNC driver plugin ---
# Recompiles Qt's QSQLITE driver source against our sqlite3 (with session
# extension enabled) and registers it as the "QSQLITE_SYNC" driver.
if(TARGET Qt6::SqlPrivate)
    qt_add_plugin(qsqlite_sync
        PLUGIN_TYPE sqldrivers
        CLASS_NAME QSQLiteSyncDriverPlugin
        src/driver/qsql_sqlite.cpp
        src/driver/qsql_sqlite_p.h
        src/driver/qsql_sqlite_vfs.cpp
        src/driver/qsql_sqlite_vfs_p.h
        src/driver/smain.cpp
    )
    target_include_directories(qsqlite_sync PRIVATE
        src/driver
        "${QT_SQLITE_SOURCE}"
    )
    target_link_libraries(qsqlite_sync PRIVATE Qt6::Core Qt6::Sql Qt6::SqlPrivate sqlite3)

    # Place the plugin where QSqlDatabase can discover it at runtime
    add_custom_command(TARGET qsqlite_sync POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_BINARY_DIR}/sqldrivers"
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:qsqlite_sync>
            "${CMAKE_BINARY_DIR}/sqldrivers/$<TARGET_FILE_NAME:qsqlite_sync>"
        COMMENT "Copying QSQLITE_SYNC plugin to build/sqldrivers/"
    )
    message(STATUS "QSQLITE_SYNC driver plugin enabled")
else()
    message(STATUS "QSQLITE_SYNC driver plugin disabled (Qt6::SqlPrivate not found)")
endif()

# Tests, demo, and documentation (only when building standalone)
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    # --- Demo CLI ---
    add_executable(syncdemo demo/main.cpp)
    target_link_libraries(syncdemo PRIVATE syncengine)

    # --- Tests ---
    # Each test is a separate executable so Qt Creator's auto-test runner can
    # discover and run them individually.
    enable_testing()

    function(add_sync_test TEST_NAME TEST_SOURCE)
        add_executable(${TEST_NAME} ${TEST_SOURCE})
        target_link_libraries(${TEST_NAME} PRIVATE syncengine Qt6::Test)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endfunction()

    add_sync_test(tst_hlc              tests/tst_hlc.cpp)
    add_sync_test(tst_changeset        tests/tst_changeset.cpp)
    add_sync_test(tst_conflict         tests/tst_conflict.cpp)
    add_sync_test(tst_sync_integration tests/tst_sync_integration.cpp)
    add_sync_test(tst_schema_migration tests/tst_schema_migration.cpp)
    add_sync_test(tst_schema_log       tests/tst_schema_log.cpp)
    add_sync_test(tst_schema_version   tests/tst_schema_version.cpp)
    add_sync_test(tst_qsqldatabase_integration tests/tst_qsqldatabase_integration.cpp)
    add_sync_test(tst_syncquery        tests/tst_syncquery.cpp)
    add_sync_test(tst_qsqlite_sync_driver tests/tst_qsqlite_sync_driver.cpp)
    add_sync_test(tst_sync_errors     tests/tst_sync_errors.cpp)
    add_sync_test(tst_robustness      tests/tst_robustness.cpp)
    add_sync_test(tst_existing_databases tests/tst_existing_databases.cpp)
    add_sync_test(tst_fts             tests/tst_fts.cpp)

    # --- Documentation ---
    find_program(QDOC_EXE qdoc)
    if(QDOC_EXE)
        find_program(QMAKE_EXE NAMES qmake6 qmake)
        if(QMAKE_EXE)
            execute_process(COMMAND ${QMAKE_EXE} -query QT_INSTALL_HEADERS OUTPUT_VARIABLE QT_INSTALL_HEADERS OUTPUT_STRIP_TRAILING_WHITESPACE)
            execute_process(COMMAND ${QMAKE_EXE} -query QT_INSTALL_DOCS OUTPUT_VARIABLE QT_INSTALL_DOCS OUTPUT_STRIP_TRAILING_WHITESPACE)
        endif()

        add_custom_target(QSQLiteSyncEngine_docs
            COMMAND ${CMAKE_COMMAND} -E env
                "QT_INSTALL_HEADERS=${QT_INSTALL_HEADERS}"
                "QT_INSTALL_DOCS=${QT_INSTALL_DOCS}"
                ${QDOC_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/docs/qsqlitesyncengine.qdocconf
                --outputdir ${CMAKE_CURRENT_BINARY_DIR}/docs
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating QDoc documentation"
        )
    endif()
endif()
