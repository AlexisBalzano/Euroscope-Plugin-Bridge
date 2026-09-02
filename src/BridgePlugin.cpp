#include "BridgePlugin.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "BridgeInstance.h"
#include "Log.h"
#include "Registry.h"
#include "Version.h"

namespace esb {

namespace {

// Skips leading spaces and returns the remainder, or nullptr if there is none.
const char* NextArg(const char* s)
{
    if (!s)
        return nullptr;
    while (*s == ' ')
        ++s;
    return *s ? s : nullptr;
}

// Everything the bridge writes lives next to the DLL, so the log, the dump
// and the state file travel with the thing that produced them.
std::string BesideModule(const char* name)
{
    const std::string module = BridgeInstance::Get().ModulePath();
    const size_t slash = module.find_last_of("\\/");
    return (slash == std::string::npos ? std::string() : module.substr(0, slash + 1)) +
           (name ? name : "");
}

// A full snapshot is far too big for the message window, so it goes to a file
// beside the log and the command reports the path.
std::string WriteDump(const std::string& text)
{
    const std::string path = BesideModule("EuroScopeBridge-dump.txt");

    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return std::string();

    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return path;
}

} // namespace

BridgePlugin::BridgePlugin()
    : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE,
              ESB_BRIDGE_NAME,
              ESB_BRIDGE_VERSION_STRING,
              ESB_BRIDGE_AUTHOR,
              ESB_BRIDGE_LICENSE)
{
    const BridgeInstance& instance = BridgeInstance::Get();

    char msg[512];

    if (!instance.Serving())
    {
        // A duplicate copy. It does no work and serves no clients; say clearly
        // which file is inert so the user can remove it (ARCHITECTURE.md 9.4).
        std::snprintf(msg, sizeof msg,
                      "INACTIVE: %s. This file does nothing and can be removed: %s",
                      instance.StandDownReason(), instance.ModulePath().c_str());
        Say(msg);
        return;
    }

    // Touch the registry now so its main-thread id is captured here, on the
    // EuroScope thread, rather than by whichever client happens to call first.
    Registry& registry = TheRegistry();

    const std::string dir = BesideModule("");
    registry.SetStatePath(dir + "bridge-state.json");
    registry.LoadState();

    // The relay is opt-in and stays off unless bridge.json says otherwise.
    const std::string configPath = dir + "bridge.json";
    Config::WriteDefault(configPath);          // first run only; never overwrites

    std::string configError;
    m_config = Config::Load(configPath, &configError);
    if (!configError.empty())
    {
        std::snprintf(msg, sizeof msg, "bridge.json: %s", configError.c_str());
        Say(msg);
        Log::Get().Write(ESB_LOG_WARN, "bridge.json: %s", configError.c_str());
    }

    if (m_config.relay.enabled)
    {
        if (m_config.relay.controller.empty())
            m_config.relay.controller = ControllerCallsign();

        registry.SetRelay(&m_relay, &m_config.relay);
        m_relay.Start(m_config.relay, std::unique_ptr<Transport>(new TcpTransport()));

        std::snprintf(msg, sizeof msg, "relay enabled — %s",
                      m_config.Describe().c_str());
        Say(msg);
    }

    std::snprintf(msg, sizeof msg, "%s %s (build %d) ready",
                  ESB_BRIDGE_NAME, ESB_BRIDGE_VERSION_STRING, ESB_BRIDGE_BUILD);
    Say(msg);
}

BridgePlugin::~BridgePlugin()
{
    if (!BridgeInstance::Get().Serving())
        return;

    // Stop the relay thread before anything it might reference goes away.
    TheRegistry().SetRelay(nullptr, nullptr);
    m_relay.Stop();

    // The debounce means there is almost always something not yet on disk.
    // Shutdown is the one moment it must be flushed unconditionally.
    TheRegistry().SaveState();

    // The registry outlives this object: the module is pinned, so consumers that
    // EuroScope unloads after us still find a live api table to unregister and
    // unsubscribe against. It must not take new state past this point, though -
    // the flush above has already happened, so a later write would just be lost.
    TheRegistry().Shutdown();
}

// The relay publishes under the controller's position, so peers can tell whose
// data they are looking at. Falls back to the machine name when disconnected,
// which is enough to keep a test session distinguishable.
std::string BridgePlugin::ControllerCallsign()
{
    EuroScopePlugIn::CController me = ControllerMyself();
    if (me.IsValid() && me.GetCallsign() && *me.GetCallsign())
        return me.GetCallsign();

    char name[64] = { 0 };
    DWORD n = sizeof name;
    if (GetComputerNameA(name, &n) && name[0])
        return std::string("local-") + name;

    return "local";
}

void BridgePlugin::Say(const char* text)
{
    DisplayUserMessage("EuroScope Bridge", "", text, true, false, false, false, false);
}

// The message window is line-oriented; a multi-line dump arrives as one
// unreadable run unless it is split here.
void BridgePlugin::SayLines(const std::string& text)
{
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        if (end > start)
            Say(text.substr(start, end - start).c_str());
        start = end + 1;
    }
}

void __cdecl BridgePlugin::OnWatchFired(ESB_FieldId field, ESB_Aircraft ac, void* user)
{
    BridgePlugin* self = static_cast<BridgePlugin*>(user);
    if (!self)
        return;

    Registry& registry = TheRegistry();
    const std::string qualified = registry.QualifiedName(field);

    char callsign[32] = { 0 };
    uint32_t n = sizeof callsign;
    const bool haveCallsign =
        (ac != ESB_AIRCRAFT_NONE) &&
        registry.AircraftCallsign(ac, callsign, &n) == ESB_OK;

    self->Say(registry.DumpValue(qualified.c_str(),
                                 haveCallsign ? callsign : nullptr).c_str());
}

bool BridgePlugin::ToggleWatch(const char* qualified)
{
    Registry& registry = TheRegistry();

    for (size_t i = 0; i < m_watches.size(); ++i)
    {
        if (m_watches[i].qualified == qualified)
        {
            registry.Unsubscribe(m_watches[i].sub);
            m_watches.erase(m_watches.begin() + i);
            Say((std::string("stopped watching ") + qualified).c_str());
            return true;
        }
    }

    ESB_FieldId field = ESB_FIELD_NONE;
    const ESB_Status st = registry.Resolve(qualified, 0, &field);
    if (st != ESB_OK)
    {
        Say((std::string("cannot watch ") + qualified +
             (st == ESB_E_NO_PROVIDER ? ": no such provider" : ": no such field")).c_str());
        return false;
    }

    Watch w;
    w.field     = field;
    w.qualified = qualified;

    // Subscribed with our own module handle, so this watch is reaped along
    // with the bridge exactly like any plugin's subscription would be.
    if (registry.Subscribe(field, &BridgePlugin::OnWatchFired, this,
                           GetModuleHandleA(ESB_MODULE_NAME), &w.sub) != ESB_OK)
    {
        Say("could not start the watch");
        return false;
    }

    m_watches.push_back(w);
    Say((std::string("watching ") + qualified + " (repeat the command to stop)").c_str());
    return true;
}

void BridgePlugin::OnTimer(int /*counter*/)
{
    if (!BridgeInstance::Get().Serving())
        return;

    Registry& registry = TheRegistry();
    const uint64_t now = GetTickCount64();

    // Refresh the aircraft table from whatever EuroScope currently knows.
    // Radar targets first, then flight plans: a departure can have a plan long
    // before it has a track, and either is enough to make a callsign
    // addressable. TTL-based reaping of anything that stops appearing here is
    // M2's job.
    for (EuroScopePlugIn::CRadarTarget rt = RadarTargetSelectFirst();
         rt.IsValid();
         rt = RadarTargetSelectNext(rt))
    {
        registry.ObserveAircraft(rt.GetCallsign(), now);
    }

    for (EuroScopePlugIn::CFlightPlan fp = FlightPlanSelectFirst();
         fp.IsValid();
         fp = FlightPlanSelectNext(fp))
    {
        registry.ObserveAircraft(fp.GetCallsign(), now);
    }

    // Reap providers whose DLL the user has removed, and age out aircraft that
    // stopped appearing above without a clean disconnect. Both throttle
    // themselves; calling this every tick is correct.
    registry.Housekeeping(now);
}

void BridgePlugin::OnFlightPlanDisconnect(EuroScopePlugIn::CFlightPlan flightPlan)
{
    if (!BridgeInstance::Get().Serving())
        return;

    if (flightPlan.IsValid())
        TheRegistry().RetireAircraft(flightPlan.GetCallsign());
}

bool BridgePlugin::OnCompileCommand(const char* command)
{
    if (!command || std::strncmp(command, ".esb", 4) != 0)
        return false;

    // A stood-down duplicate must not answer, or the user sees every response
    // twice and cannot tell which copy produced it.
    if (!BridgeInstance::Get().Serving())
        return false;

    const char* arg = NextArg(command + 4);
    if (!arg)
    {
        char msg[192];
        std::snprintf(msg, sizeof msg,
                      "%s build %d — providers, schema <id>, stats, get, watch, subs, dump, log, net",
                      ESB_BRIDGE_VERSION_STRING, ESB_BRIDGE_BUILD);
        Say(msg);
        return true;
    }

    Registry& registry = TheRegistry();

    if (std::strncmp(arg, "providers", 9) == 0)
    {
        SayLines(registry.DumpProviders());
        return true;
    }

    if (std::strncmp(arg, "schema", 6) == 0)
    {
        SayLines(registry.DumpSchema(NextArg(arg + 6)));
        return true;
    }

    if (std::strncmp(arg, "stats", 5) == 0)
    {
        Say(registry.DumpStats().c_str());
        return true;
    }

    if (std::strncmp(arg, "get", 3) == 0)
    {
        // .esb get <provider>/<field> [callsign]
        const char* key = NextArg(arg + 3);
        if (!key)
        {
            Say("usage: .esb get <provider>/<field> [callsign]");
            return true;
        }

        std::string qualified(key);
        std::string callsign;
        const size_t space = qualified.find(' ');
        if (space != std::string::npos)
        {
            const char* rest = NextArg(key + space);
            callsign = rest ? rest : "";
            qualified.resize(space);
        }

        Say(registry.DumpValue(qualified.c_str(),
                               callsign.empty() ? nullptr : callsign.c_str()).c_str());
        return true;
    }

    if (std::strncmp(arg, "watch", 5) == 0)
    {
        const char* key = NextArg(arg + 5);
        if (!key)
        {
            Say("usage: .esb watch <provider>/<field>   (repeat to stop)");
            return true;
        }
        ToggleWatch(key);
        return true;
    }

    if (std::strncmp(arg, "subs", 4) == 0)
    {
        SayLines(registry.DumpSubscriptions());
        return true;
    }

    if (std::strncmp(arg, "dump", 4) == 0)
    {
        const std::string path = WriteDump(registry.DumpAll());
        Say(path.empty() ? "could not write the dump file"
                         : ("registry written to " + path).c_str());
        return true;
    }

    if (std::strncmp(arg, "log", 3) == 0)
    {
        Say(("log file: " + Log::Get().Path()).c_str());
        return true;
    }

    if (std::strncmp(arg, "net", 3) == 0)
    {
        SayLines(registry.DumpNet());
        return true;
    }


    Say("unknown .esb command (providers, schema <id>, stats, get, watch, subs, dump, log, net)");
    return true;
}

} // namespace esb

// --------------------------------------------------------------------------
// EuroScope entry points. The SDK header already declares these dllexport.

esb::BridgePlugin* g_plugin = nullptr;

void __declspec(dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
    *ppPlugInInstance = g_plugin = new esb::BridgePlugin();
}

void __declspec(dllexport) EuroScopePlugInExit(void)
{
    delete g_plugin;
    g_plugin = nullptr;
}
