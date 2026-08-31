#include "BridgeInstance.h"

#include <cstdio>
#include <cstring>

namespace esb {

namespace {

const char* kServingModuleName = "EuroScopeBridge.dll";

const char* BaseName(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

} // namespace

std::string ModulePathOf(const void* addressInModule)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(addressInModule), &module))
        return std::string();

    char path[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(module, path, MAX_PATH))
        return std::string();

    return std::string(path);
}

bool ModuleIsLoaded(HMODULE module, const std::string& path)
{
    if (!module || path.empty())
        return true;                 // nothing to check against; assume alive

    HMODULE now = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            path.c_str(), &now))
        return false;

    // A different handle at the same path means the original was unloaded and
    // something else took the name. Either way the registration is dead.
    return now == module;
}

BridgeInstance& BridgeInstance::Get()
{
    static BridgeInstance instance;
    return instance;
}

BridgeInstance::BridgeInstance()
{
    m_modulePath = ModulePathOf(reinterpret_cast<const void*>(&kServingModuleName));

    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&kServingModuleName), &self);

    // Linked straight into an executable rather than loaded as the plugin DLL:
    // that is the test harness, or a future embedding. There is no other copy
    // to arbitrate with, so serve.
    const bool inExecutable = (self != nullptr && self == GetModuleHandleA(nullptr));

    if (!inExecutable)
    {
        if (m_modulePath.empty())
        {
            m_reason = "the bridge could not identify its own module";
            return;
        }

        if (_stricmp(BaseName(m_modulePath), kServingModuleName) != 0)
        {
            // Clients resolve the bridge by base name, so nothing can ever
            // reach this copy. Standing down keeps it from running a second,
            // invisible registry and a second set of dot commands.
            m_reason = "this copy is not named EuroScopeBridge.dll, so no plugin can reach it";
            return;
        }
    }

    char mutexName[64];
    std::snprintf(mutexName, sizeof mutexName,
                  "Local\\EuroScopeBridge.v1.%lu", GetCurrentProcessId());

    m_mutex = CreateMutexA(nullptr, TRUE, mutexName);
    if (!m_mutex)
    {
        m_reason = "the bridge could not create its single-instance mutex";
        return;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(m_mutex);
        m_mutex  = nullptr;
        m_reason = "another copy of the bridge is already serving this EuroScope session";
        return;
    }

    m_serving = true;
}

BridgeInstance::~BridgeInstance()
{
    if (m_mutex)
    {
        ReleaseMutex(m_mutex);
        CloseHandle(m_mutex);
        m_mutex = nullptr;
    }
}

} // namespace esb
