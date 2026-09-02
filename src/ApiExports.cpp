// The public C ABI surface. Everything here is a thin, noexcept forwarder onto
// the Registry -- no logic, no allocation the caller can see, no C++ type
// crossing the boundary. See ARCHITECTURE.md 4.

#include "esbridge.h"
#include "BridgeInstance.h"
#include "Log.h"
#include "Registry.h"
#include "Version.h"

using esb::TheRegistry;

namespace {

// The one condition a forwarder tests. After ~BridgePlugin has run the registry
// is deliberately still answering: EuroScope unloads the bridge before the
// plugins that consume it, so their destructors call unregister_provider and
// unsubscribe on the way out and those have to succeed. What it stops taking is
// new state -- SaveState has already run, so a write arriving now would be lost
// rather than persisted, and a subscription would arm a callback into a module
// that is about to unload.
bool AcceptingState()
{
    return !TheRegistry().ShuttingDown();
}

// -- registration ---------------------------------------------------------

ESB_Status __cdecl Api_register_provider(const ESB_ProviderDecl* decl, ESB_Provider** out)
{
    if (!AcceptingState())
        return ESB_E_SHUTDOWN;
    return TheRegistry().RegisterProvider(decl, out);
}

ESB_Status __cdecl Api_unregister_provider(ESB_Provider* p)
{
    return TheRegistry().UnregisterProvider(p);
}

ESB_Status __cdecl Api_own_field(ESB_Provider* p, const char* field, ESB_FieldId* out)
{
    if (!AcceptingState())
        return ESB_E_SHUTDOWN;
    return TheRegistry().OwnField(p, field, out);
}

// -- discovery ------------------------------------------------------------

ESB_Status __cdecl Api_resolve(const char* qualified, ESB_Type expect, ESB_FieldId* out)
{
    return TheRegistry().Resolve(qualified, expect, out);
}

ESB_Status __cdecl Api_provider_version(const char* id, uint32_t* major, uint32_t* minor)
{
    return TheRegistry().ProviderVersion(id, major, minor);
}

ESB_Status __cdecl Api_list_providers(char* buf, uint32_t* ioBytes)
{
    return TheRegistry().ListProviders(buf, ioBytes);
}

// -- global scope ---------------------------------------------------------

ESB_Status __cdecl Api_get_global(ESB_FieldId f, ESB_Value* out, void* buf, uint32_t* ioBytes)
{
    return TheRegistry().GetGlobal(f, out, buf, ioBytes);
}

ESB_Status __cdecl Api_set_global(ESB_Provider* p, ESB_FieldId f, const ESB_Value* v)
{
    if (!AcceptingState())
        return ESB_E_SHUTDOWN;
    return TheRegistry().SetGlobal(p, f, v);
}

ESB_Status __cdecl Api_clear_global(ESB_Provider* p, ESB_FieldId f)
{
    if (!AcceptingState())
        return ESB_E_SHUTDOWN;
    return TheRegistry().ClearGlobal(p, f);
}

// -- aircraft scope -------------------------------------------------------

ESB_Status __cdecl Api_aircraft(const char* callsign, ESB_Aircraft* out)
{
    return TheRegistry().AircraftHandle(callsign, out);
}

ESB_Status __cdecl Api_aircraft_callsign(ESB_Aircraft a, char* buf, uint32_t* ioBytes)
{
    return TheRegistry().AircraftCallsign(a, buf, ioBytes);
}

ESB_Status __cdecl Api_get_ac(ESB_Aircraft a, ESB_FieldId f, ESB_Value* out,
                              void* buf, uint32_t* ioBytes)
{
    return TheRegistry().GetAircraft(a, f, out, buf, ioBytes);
}

ESB_Status __cdecl Api_set_ac(ESB_Provider* p, ESB_Aircraft a, ESB_FieldId f, const ESB_Value* v)
{
    if (!AcceptingState())
        return ESB_E_SHUTDOWN;
    return TheRegistry().SetAircraft(p, a, f, v);
}

ESB_Status __cdecl Api_clear_ac(ESB_Provider* p, ESB_Aircraft a, ESB_FieldId f)
{
    if (!AcceptingState())
        return ESB_E_SHUTDOWN;
    return TheRegistry().ClearAircraft(p, a, f);
}

// -- change feed ----------------------------------------------------------

uint64_t __cdecl Api_revision(ESB_FieldId f, ESB_Aircraft a)
{
    return TheRegistry().RevisionOf(f, a);
}

uint64_t __cdecl Api_provider_revision(const char* id)
{
    return TheRegistry().ProviderRevision(id);
}

ESB_Status __cdecl Api_subscribe(ESB_FieldId f, ESB_OnChange cb, void* user,
                                 void* module, ESB_Sub** out)
{
    if (!AcceptingState())
        return ESB_E_SHUTDOWN;
    return TheRegistry().Subscribe(f, cb, user, module, out);
}

ESB_Status __cdecl Api_unsubscribe(ESB_Sub* s)
{
    return TheRegistry().Unsubscribe(s);
}

// -- remote view (read-only; never merged with local state) ---------------

ESB_Status __cdecl Api_remote_publishers(ESB_FieldId f, ESB_Aircraft a,
                                         ESB_Origin* out, uint32_t* ioCount)
{
    return TheRegistry().RemotePublishers(f, a, out, ioCount);
}

ESB_Status __cdecl Api_get_remote(ESB_FieldId f, ESB_Aircraft a, const char* peer,
                                  ESB_Value* out, void* buf, uint32_t* ioBytes)
{
    return TheRegistry().GetRemote(f, a, peer, out, buf, ioBytes);
}

ESB_Status __cdecl Api_list_peers(ESB_Peer* out, uint32_t* ioCount)
{
    return TheRegistry().ListPeers(out, ioCount);
}

// -- diagnostics ----------------------------------------------------------

// A provider's own log line carries its id and contact, so a user's log file
// tells them who to report a problem to (ARCHITECTURE.md 12).
void __cdecl Api_log(ESB_Provider* p, int32_t level, const char* msg)
{
    if (!msg)
        return;

    const esb::ProviderDef* d = reinterpret_cast<const esb::ProviderDef*>(p);
    const bool known = d && d->magic == esb::ProviderDef::kMagic;

    esb::Log::Get().Write(level, "[%s] %s%s%s",
                          known ? d->id.c_str() : "?",
                          msg,
                          (known && !d->contact.empty()) ? "  -- " : "",
                          (known && !d->contact.empty()) ? d->contact.c_str() : "");
}

ESB_Status __cdecl Api_last_error(char* buf, uint32_t* ioBytes)
{
    if (!ioBytes)
        return ESB_E_INVALID_ARG;
    const uint32_t have = *ioBytes;
    *ioBytes = 1;
    if (!buf || have < 1)
        return ESB_E_BUFFER_TOO_SMALL;
    buf[0] = '\0';
    return ESB_OK;
}

// The vtable itself. Built once, immutable, handed out by pointer. Member
// order is frozen: see the ABI rules in esbridge.h.
const ESB_Api_v1 kApiV1 = {
    sizeof(ESB_Api_v1),
    ESB_ABI_VERSION,
    ESB_BRIDGE_BUILD,
    0,                              // reserved_

    Api_register_provider,
    Api_unregister_provider,
    Api_own_field,

    Api_resolve,
    Api_provider_version,
    Api_list_providers,

    Api_get_global,
    Api_set_global,
    Api_clear_global,

    Api_aircraft,
    Api_aircraft_callsign,
    Api_get_ac,
    Api_set_ac,
    Api_clear_ac,

    Api_revision,
    Api_provider_revision,
    Api_subscribe,
    Api_unsubscribe,

    Api_remote_publishers,
    Api_get_remote,
    Api_list_peers,

    Api_log,
    Api_last_error,
};

// Keeps this DLL mapped for the life of the process.
//
// Clients cache the table below for as long as they live and have no way to
// learn when it stops being mapped: the shim attaches with GetModuleHandle and
// holds no reference by design, and EuroScope unloads the bridge BEFORE the
// plugins that consume it. Without this, every consumer that calls
// unregister_provider or unsubscribe from its destructor -- which is the obvious
// way to write one -- reads a function pointer out of freed address space and
// takes an access violation on the way out.
//
// Pinning rather than a plain reference because there is no moment at which it
// would be safe to drop one: the last consumer to unwind is by definition still
// holding the table. The cost is that the DLL cannot be replaced without
// restarting EuroScope, which for a shared bridge is the right trade.
void PinSelf()
{
    static bool pinned = false;
    if (pinned)
        return;

    HMODULE self = nullptr;
    pinned = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCSTR>(&kApiV1), &self) != FALSE;

    if (!pinned)
        esb::Log::Get().Write(ESB_LOG_WARN,
                              "could not pin the bridge module; consumers that call "
                              "the api from their destructor may fault on shutdown");
}

} // namespace

// The one exported symbol clients resolve. Returning NULL for an unknown
// version is what lets a future v2 bridge keep serving v1 clients.
extern "C" __declspec(dllexport)
const ESB_Api_v1* __cdecl ESB_GetApi(uint32_t abi_version)
{
    if (abi_version != ESB_ABI_VERSION)
        return nullptr;

    // A duplicate copy that stood down must refuse rather than hand out an
    // empty registry it will never populate. NULL puts the client on its
    // documented no-bridge path, which fails visibly instead of silently.
    if (!esb::BridgeInstance::Get().Serving())
        return nullptr;

    // Only once a client is actually about to hold the pointer, and only on the
    // copy that serves: a stood-down duplicate has no business staying resident.
    PinSelf();

    return &kApiV1;
}
