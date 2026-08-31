#pragma once

// EuroScopePlugIn.h uses POINT, RECT, COLORREF and NULL without including
// anything itself, so windows.h has to come first.
#include <string>
#include <vector>

#include <windows.h>

#include <EuroScopePlugIn.h>

#include "esbridge.h"
#include "Config.h"
#include "Relay.h"

namespace esb {

// The EuroScope side of the bridge. Its job is small on purpose: keep the
// aircraft table in step with what EuroScope can see, drive housekeeping once
// a second, and answer dot commands. All the actual registry logic lives in
// Registry, which knows nothing about EuroScope and can be tested without it.
//
// Being a real registered plugin is not optional -- it is the only way to get
// OnTimer and the aircraft lifecycle events. See ARCHITECTURE.md 9.3.
class BridgePlugin : public EuroScopePlugIn::CPlugIn
{
public:
    BridgePlugin();
    virtual ~BridgePlugin();

    void OnTimer(int counter) override;
    void OnFlightPlanDisconnect(EuroScopePlugIn::CFlightPlan flightPlan) override;
    bool OnCompileCommand(const char* command) override;

private:
    std::string ControllerCallsign();

    void Say(const char* text);
    void SayLines(const std::string& text);

    // `.esb watch` is implemented with the same public subscribe() a plugin
    // author would use, so the feature exercises its own change feed.
    bool ToggleWatch(const char* qualified);
    static void __cdecl OnWatchFired(ESB_FieldId field, ESB_Aircraft ac, void* user);

    struct Watch
    {
        ESB_Sub*    sub = nullptr;
        ESB_FieldId field = ESB_FIELD_NONE;
        std::string qualified;
    };

    std::vector<Watch> m_watches;

    Config m_config;
    Relay  m_relay;
};

} // namespace esb
