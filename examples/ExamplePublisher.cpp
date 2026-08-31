// ExamplePublisher — a complete, minimal EuroScope plugin that publishes to
// the bridge. Pair it with ExampleConsumer.cpp.
//
// It invents a "target off-block time" for every departure it can see and
// publishes it under the provider id `example_cdm`, so the consumer has
// something real to react to. The point is the wiring, not the numbers.
//
// Build:  examples\build.cmd
// Then add BOTH DLLs, plus EuroScopeBridge.dll, under
// Other Settings > Plug-ins > Load, and type  .esb providers

#define ESB_CLIENT_SHIM          // exactly one .cpp per plugin
#include "esbridge.h"

#include <cstdio>
#include <cstring>
#include <windows.h>

#include <EuroScopePlugIn.h>

namespace {

// The schema. Declared once, statically: the bridge copies what it needs, and
// the doc strings are what `.esb schema example_cdm` prints for anyone trying
// to consume this.
const ESB_FieldDecl kFields[] = {
    { "tobt",    ESB_T_I64,  ESB_SCOPE_AIRCRAFT, 0, 0,
      "Target off-block time, minutes since midnight UTC" },
    { "stand",   ESB_T_STR,  ESB_SCOPE_AIRCRAFT, 0, 8,
      "Assigned stand identifier" },
    { "airport", ESB_T_STR,  ESB_SCOPE_GLOBAL,   0, 4,
      "ICAO code this instance is sequencing" },
};

} // namespace

class ExamplePublisher : public EuroScopePlugIn::CPlugIn
{
public:
    ExamplePublisher()
        : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE,
                  "Example Publisher", "1.0", "Bridge example", "Public domain")
    {
    }

    ~ExamplePublisher() override
    {
        // Mandatory. A provider left registered after its DLL is gone is the
        // crash the bridge's module reaping exists to catch -- do not rely on
        // being caught (INTEGRATION.md A10).
        if (esb_api && m_provider)
            esb_api->unregister_provider(m_provider);
    }

    void OnTimer(int) override
    {
        // Lazy attach: plugin load order follows the user's settings file, so
        // the bridge may legitimately load after us. Cheap once attached.
        const ESB_Api_v1* api = ESB_Attach();
        if (!api)
        {
            if (++m_ticksWithoutBridge == 10)
                Complain(ESB_MISSING_MESSAGE);
            return;
        }

        if (!m_provider && !Register(api))
            return;

        // Publish once per aircraft. Note there is no dirty-tracking here on
        // purpose: the bridge compares before storing, so writing the same
        // value every second costs nothing and notifies nobody.
        for (EuroScopePlugIn::CFlightPlan fp = FlightPlanSelectFirst();
             fp.IsValid();
             fp = FlightPlanSelectNext(fp))
        {
            ESB_Aircraft ac = ESB_AIRCRAFT_NONE;
            if (api->aircraft(fp.GetCallsign(), &ac) != ESB_OK)
                continue;                    // the bridge has not seen it yet

            const int64_t tobt = FakeTobt(fp.GetCallsign());
            ESB_Value v = ESB_I64(tobt);
            api->set_ac(m_provider, ac, m_tobt, &v);

            char stand[8];
            std::snprintf(stand, sizeof stand, "%c%02d",
                          'A' + (int)(tobt % 6), (int)(tobt % 40) + 1);
            ESB_Value s = ESB_Str(stand);
            api->set_ac(m_provider, ac, m_stand, &s);
        }
    }

private:
    bool Register(const ESB_Api_v1* api)
    {
        ESB_ProviderDecl decl = { 0 };
        decl.struct_size  = sizeof decl;
        decl.provider_id  = "example_cdm";
        decl.schema_major = 1;
        decl.schema_minor = 0;
        decl.display_name = "Example CDM";
        decl.contact      = "https://github.com/AlexisBalzano/Euroscope-Plugin-Bridge";
        decl.fields       = kFields;
        decl.field_count  = sizeof kFields / sizeof kFields[0];
        decl.module       = ESB_SelfModule();   // never GetModuleHandleA(name)

        const ESB_Status st = api->register_provider(&decl, &m_provider);
        if (st != ESB_OK)
        {
            // PROVIDER_TAKEN means another author claimed this id. That is a
            // conversation with them, not something to retry around, so say so
            // once and stop trying.
            char msg[192];
            std::snprintf(msg, sizeof msg,
                          "could not claim provider id 'example_cdm' (error %d)", st);
            Complain(msg);
            m_provider = nullptr;
            return false;
        }

        api->own_field(m_provider, "tobt",    &m_tobt);
        api->own_field(m_provider, "stand",   &m_stand);
        api->own_field(m_provider, "airport", &m_airport);

        ESB_Value icao = ESB_Str("LFPG");
        api->set_global(m_provider, m_airport, &icao);

        api->log(m_provider, ESB_LOG_INFO, "example publisher registered");
        Complain("Example Publisher: registered with the bridge");
        return true;
    }

    // Stable per callsign so the value stops changing once published, which is
    // what makes the consumer's revision counter interesting to watch.
    static int64_t FakeTobt(const char* callsign)
    {
        uint32_t h = 2166136261u;
        for (const char* p = callsign; p && *p; ++p)
            h = (h ^ (uint8_t)*p) * 16777619u;
        return 6 * 60 + (h % 720);           // 06:00 UTC plus up to 12 hours
    }

    void Complain(const char* text)
    {
        DisplayUserMessage("Example Publisher", "", text,
                           true, false, false, false, false);
    }

    ESB_Provider* m_provider = nullptr;
    ESB_FieldId   m_tobt     = ESB_FIELD_NONE;
    ESB_FieldId   m_stand    = ESB_FIELD_NONE;
    ESB_FieldId   m_airport  = ESB_FIELD_NONE;
    int           m_ticksWithoutBridge = 0;
};

ExamplePublisher* g_plugin = nullptr;

void __declspec(dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
    *ppPlugInInstance = g_plugin = new ExamplePublisher();
}

void __declspec(dllexport) EuroScopePlugInExit(void)
{
    delete g_plugin;
    g_plugin = nullptr;
}
