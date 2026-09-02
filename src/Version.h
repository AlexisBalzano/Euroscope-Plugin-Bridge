#pragma once

// Monotonic build number, surfaced through ESB_Api_v1::bridge_build. Bump on
// every release. Plugins gate on it when they need a specific fix; users quote
// it in bug reports. See ARCHITECTURE.md 14.2.
#define ESB_BRIDGE_BUILD          2

#define ESB_BRIDGE_NAME           "EuroScope Plugin Bridge"
#define ESB_BRIDGE_VERSION_STRING "1.0.0"
#define ESB_BRIDGE_AUTHOR         "Alexis Balzano"
#define ESB_BRIDGE_LICENSE        "Open source, see LICENSE"
