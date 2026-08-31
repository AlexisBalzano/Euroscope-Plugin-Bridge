// ExampleConsumer — the other half. Reads what ExamplePublisher publishes,
// using both change-feed styles so you can see the difference:
//
//   * a CALLBACK on the global `example_cdm/airport`, delivered synchronously
//     inside the publisher's write (zero ticks);
//   * REVISION POLLING for the per-aircraft `example_cdm/tobt`, compared once
//     per OnTimer with a coarse provider-level gate first.
//
// The thing to copy from here is not the logic, it is the error handling:
// every "the other plugin is not installed" path degrades instead of failing.
//
// Build:  examples\build.cmd

#define ESB_CLIENT_SHIM          // exactly one .cpp per plugin
#include "esbridge.h"

#include <cstdio>
#include <cstring>
#include <windows.h>

#include <EuroScopePlugIn.h>

class ExampleConsumer;
static ExampleConsumer* g_plugin = nullptr;

class ExampleConsumer : public EuroScopePlugIn::CPlugIn
{
public:
    ExampleConsumer()
        : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE,
                  "Example Consumer", "1.0", "Bridge example", "Public domain")
    {
    }

    ~ExampleConsumer() override
    {
        // Mandatory, and the sharpest rule in the whole integration: a live
        // callback pointer into an unloaded DLL crashes EuroScope for every
        // controller in the session (INTEGRATION.md A10).
        if (esb_api && m_sub)
            esb_api->unsubscribe(m_sub);
    }

    void OnTimer(int) override
    {
        const ESB_Api_v1* api = ESB_Attach();
        if (!api)
            return;                          // no bridge: quietly do nothing

        if (!Resolve(api))
            return;                          // publisher not installed (yet)

        // Coarse gate: one comparison skips the entire provider when nothing
        // it owns has changed since we last looked.
        const uint64_t rev = api->provider_revision("example_cdm");
        if (rev == m_providerRevision)
            return;
        m_providerRevision = rev;

        for (EuroScopePlugIn::CFlightPlan fp = FlightPlanSelectFirst();
             fp.IsValid();
             fp = FlightPlanSelectNext(fp))
        {
            ESB_Aircraft ac = ESB_AIRCRAFT_NONE;
            if (api->aircraft(fp.GetCallsign(), &ac) != ESB_OK)
                continue;

            ESB_Value v;
            if (api->get_ac(ac, m_tobt, &v, nullptr, nullptr) != ESB_OK)
                continue;                    // UNSET, or the handle went stale

            // A real consumer would render this in a tag item. Reporting only
            // the first one keeps the example's output readable.
            if (!m_reportedOne)
            {
                char msg[160];
                std::snprintf(msg, sizeof msg, "%s TOBT %02lld:%02lld (rev %llu)",
                              fp.GetCallsign(),
                              (long long)(v.v.i64 / 60), (long long)(v.v.i64 % 60),
                              (unsigned long long)api->revision(m_tobt, ac));
                Report(msg);
                m_reportedOne = true;
            }
        }
    }

private:
    bool Resolve(const ESB_Api_v1* api)
    {
        if (m_tobt != ESB_FIELD_NONE)
            return true;

        // Keep retrying: the publisher may load after us. Resolving with the
        // expected type means a schema change is caught here rather than
        // silently misreading bytes later.
        if (api->resolve("example_cdm/tobt", ESB_T_I64, &m_tobt) != ESB_OK)
        {
            m_tobt = ESB_FIELD_NONE;
            return false;
        }

        // Gate on the schema major if the semantics matter to you.
        uint32_t major = 0, minor = 0;
        if (api->provider_version("example_cdm", &major, &minor) == ESB_OK && major != 1)
        {
            Report("example_cdm schema is a major version we do not understand");
            m_tobt = ESB_FIELD_NONE;
            return false;
        }

        // The other style: a callback, delivered on the EuroScope main thread
        // inside the publisher's own write. Keep it short and do not block.
        ESB_FieldId airport = ESB_FIELD_NONE;
        if (api->resolve("example_cdm/airport", ESB_T_STR, &airport) == ESB_OK)
            api->subscribe(airport, &ExampleConsumer::OnAirportChanged, this,
                           ESB_SelfModule(), &m_sub);

        char msg[160];
        std::snprintf(msg, sizeof msg,
                      "consuming example_cdm v%u.%u", major, minor);
        Report(msg);
        return true;
    }

    static void __cdecl OnAirportChanged(ESB_FieldId field, ESB_Aircraft, void* user)
    {
        ExampleConsumer* self = static_cast<ExampleConsumer*>(user);
        if (!self || !esb_api)
            return;

        // The callback carries no value on purpose -- read it yourself, and
        // there is no pointer-lifetime question to get wrong.
        char buf[16] = { 0 };
        uint32_t bytes = sizeof buf;
        ESB_Value v;
        if (esb_api->get_global(field, &v, buf, &bytes) != ESB_OK)
            return;

        char msg[96];
        std::snprintf(msg, sizeof msg, "airport is now %.*s", (int)v.bytes, buf);
        self->Report(msg);
    }

    void Report(const char* text)
    {
        DisplayUserMessage("Example Consumer", "", text,
                           true, false, false, false, false);
    }

    ESB_FieldId m_tobt             = ESB_FIELD_NONE;
    ESB_Sub*    m_sub              = nullptr;
    uint64_t    m_providerRevision = 0;
    bool        m_reportedOne      = false;
};

void __declspec(dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
    *ppPlugInInstance = g_plugin = new ExampleConsumer();
}

void __declspec(dllexport) EuroScopePlugInExit(void)
{
    delete g_plugin;
    g_plugin = nullptr;
}
