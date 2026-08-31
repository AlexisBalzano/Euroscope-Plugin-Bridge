#pragma once

#include <string>

#include <windows.h>

namespace esb {

// Decides whether this loaded copy of the bridge is the one that should serve.
// See ARCHITECTURE.md 9.4.
//
// Clients find the bridge with GetModuleHandleA("EuroScopeBridge.dll") -- by
// base name. That single fact drives the whole rule:
//
//   * Windows maps only one module of a given base name per process, so a
//     second copy can only exist if someone renamed it.
//   * A renamed copy is unreachable by every client, no matter which copy
//     loaded first. Being first to load does not make it the right server.
//
// So the instance that serves is the one actually named EuroScopeBridge.dll,
// not the one that won a race. A copy that is not named that stands down: it
// runs no sweeps, answers no dot commands, and its ESB_GetApi returns NULL so
// that anything which did reach it takes the documented no-bridge path instead
// of silently talking to an empty registry.
//
// The mutex is the remaining belt-and-braces: if two identically named modules
// ever do get mapped (side-by-side activation contexts can do it), the first
// to claim it serves and the second stands down.
class BridgeInstance
{
public:
    static BridgeInstance& Get();

    bool               Serving() const { return m_serving; }
    const std::string& ModulePath() const { return m_modulePath; }

    // Null while serving; otherwise a one-line explanation for the user.
    const char* StandDownReason() const { return m_reason; }

private:
    BridgeInstance();
    ~BridgeInstance();

    BridgeInstance(const BridgeInstance&) = delete;
    BridgeInstance& operator=(const BridgeInstance&) = delete;

    bool        m_serving    = false;
    std::string m_modulePath;
    const char* m_reason     = nullptr;
    HANDLE      m_mutex      = nullptr;
};

// True if `module` is still mapped at `path`. Used to reap registrations whose
// DLL the user has removed -- a dangling callback into an unloaded module is
// the sharpest crash in the design (ARCHITECTURE.md 9.1).
bool ModuleIsLoaded(HMODULE module, const std::string& path);

// Full path of the module containing the given address, or "" on failure.
std::string ModulePathOf(const void* addressInModule);

} // namespace esb
