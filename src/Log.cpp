#include "Log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <windows.h>

#include "BridgeInstance.h"
#include "Version.h"

namespace esb {

namespace {

const uint32_t kRotateBytes = 1024 * 1024;

const char* LevelName(int32_t level)
{
    switch (level)
    {
    case ESB_LOG_DEBUG: return "DEBUG";
    case ESB_LOG_WARN:  return "WARN ";
    case ESB_LOG_ERROR: return "ERROR";
    default:            return "INFO ";
    }
}

// Local wall clock, which is what a user reading the file alongside their
// EuroScope session will be comparing against.
void Stamp(char* out, size_t n)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::snprintf(out, n, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds);
}

} // namespace

Log& Log::Get()
{
    static Log instance;
    return instance;
}

Log::Log()
{
    // Beside the DLL, so the log travels with the thing that wrote it.
    const std::string module = BridgeInstance::Get().ModulePath();
    const size_t slash = module.find_last_of("\\/");
    m_path = (slash == std::string::npos ? std::string() : module.substr(0, slash + 1))
           + "EuroScopeBridge.log";

    m_file = std::fopen(m_path.c_str(), "a");
    if (m_file)
    {
        std::fseek(static_cast<FILE*>(m_file), 0, SEEK_END);
        m_bytes = static_cast<uint32_t>(std::ftell(static_cast<FILE*>(m_file)));
    }

    Write(ESB_LOG_INFO, "--- %s %s (build %d) started ---",
          ESB_BRIDGE_NAME, ESB_BRIDGE_VERSION_STRING, ESB_BRIDGE_BUILD);
}

Log::~Log()
{
    if (m_file)
    {
        std::fclose(static_cast<FILE*>(m_file));
        m_file = nullptr;
    }
}

void Log::Rotate()
{
    if (m_file)
    {
        std::fclose(static_cast<FILE*>(m_file));
        m_file = nullptr;
    }

    const std::string previous = m_path + ".1";
    std::remove(previous.c_str());
    std::rename(m_path.c_str(), previous.c_str());

    m_file  = std::fopen(m_path.c_str(), "w");
    m_bytes = 0;
}

void Log::Write(int32_t level, const char* fmt, ...)
{
    if (level < ESB_LOG_DEBUG) level = ESB_LOG_DEBUG;
    if (level > ESB_LOG_ERROR) level = ESB_LOG_ERROR;
    ++m_counts[level];

    if (!m_file)
        return;

    if (m_bytes >= kRotateBytes)
        Rotate();
    if (!m_file)
        return;

    char stamp[32];
    Stamp(stamp, sizeof stamp);

    char body[1024];
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(body, sizeof body, fmt, args);
    va_end(args);
    if (written < 0)
        return;

    const int n = std::fprintf(static_cast<FILE*>(m_file), "%s  %s  %s\n",
                               stamp, LevelName(level), body);
    if (n > 0)
        m_bytes += static_cast<uint32_t>(n);

    // Flushed every line on purpose: the failures this file exists to explain
    // are the ones that take EuroScope down with them, and a buffered tail
    // would be exactly the part that never reached disk.
    std::fflush(static_cast<FILE*>(m_file));
}

uint32_t Log::Count(int32_t level) const
{
    if (level < ESB_LOG_DEBUG || level > ESB_LOG_ERROR)
        return 0;
    return m_counts[level];
}

} // namespace esb
