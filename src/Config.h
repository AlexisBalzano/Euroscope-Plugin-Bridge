#pragma once

#include <cstdint>
#include <set>
#include <string>

namespace esb {

// bridge.json, read from beside the DLL. See ARCHITECTURE.md 11.
//
// The whole file exists to hold one half of a two-key consent: a field reaches
// the network only when its AUTHOR declared ESB_F_SYNC *and* the USER enabled
// the relay and that provider here. Author intent alone never puts a
// controller's data on the wire, which is why `enabled` defaults to false and
// `providers` defaults to empty.
struct RelayConfig
{
    bool        enabled = false;
    std::string host;
    uint16_t    port    = 8883;
    bool        tls     = true;

    std::string username;
    std::string password;

    // Peers only exchange data inside a room, so unrelated vACCs never see
    // each other's traffic even on a shared broker.
    std::string room = "default";

    // Defaults to the EuroScope connection callsign when left blank.
    std::string controller;

    // The user's allow list. Empty means nothing syncs, which is the point.
    std::set<std::string> providers;

    // Outbound coalescing: at most this many publishes per key per second.
    uint32_t publishHz = 4;

    // Per-provider cap on live published keys (ARCHITECTURE.md 11.3).
    uint32_t maxKeysPerProvider = 500;

    bool AllowsProvider(const std::string& id) const
    {
        return enabled && providers.find(id) != providers.end();
    }

    // Rendered for `.esb net`, so a user can audit what is configured.
    std::string Describe() const;
};

struct Config
{
    RelayConfig relay;

    // Missing file is not an error: the default is a bridge that never opens
    // a socket. *error is set only for a file that exists but is malformed.
    static Config Load(const std::string& path, std::string* error);

    // Written on first run so users have something to edit rather than a blank
    // page. Never overwrites an existing file.
    static bool WriteDefault(const std::string& path);

    std::string Describe() const;     // for `.esb net`
};

} // namespace esb
