#include "syncengine/SchemaLogCapture.h"

namespace syncengine {

thread_local SchemaLogCapture::Guard *SchemaLogCapture::s_activeGuard = nullptr;

void SchemaLogCapture::install()
{
    static std::once_flag flag;
    std::call_once(flag, []() {
        sqlite3_shutdown();
        sqlite3_config(SQLITE_CONFIG_LOG, logCallback, nullptr);
        sqlite3_initialize();
    });
}

void SchemaLogCapture::logCallback(void * /*userData*/, int errorCode, const char *message)
{
    if (errorCode != SQLITE_SCHEMA)
        return;

    Guard *guard = s_activeGuard;
    if (!guard)
        return;

    guard->m_warnings.append({errorCode, QString::fromUtf8(message)});
}

SchemaLogCapture::Guard::Guard()
{
    s_activeGuard = this;
}

SchemaLogCapture::Guard::~Guard()
{
    s_activeGuard = nullptr;
}

} // namespace syncengine
