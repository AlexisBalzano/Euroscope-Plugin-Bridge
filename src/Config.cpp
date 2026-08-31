#include "Config.h"

#include <cstdio>
#include <cstring>

#include "Json.h"

namespace esb {

namespace {

const char* kTemplate =
"{\n"
"  // EuroScope Plugin Bridge configuration.\n"
"  //\n"
"  // Nothing leaves this machine unless you turn the relay on AND list the\n"
"  // providers you want shared. A plugin author cannot enable either.\n"
"  \"relay\": {\n"
"    \"enabled\": false,\n"
"\n"
"    \"host\": \"\",\n"
"    \"port\": 8883,\n"
"    \"tls\": true,\n"
"    \"username\": \"\",\n"
"    \"password\": \"\",\n"
"\n"
"    // Peers only exchange data within the same room.\n"
"    \"room\": \"default\",\n"
"\n"
"    // Blank means: use your EuroScope connection callsign.\n"
"    \"controller\": \"\",\n"
"\n"
"    // Provider ids you allow to publish. Run `.esb providers` to see them.\n"
"    // Only fields their author marked syncable are eligible even then.\n"
"    \"providers\": [],\n"
"\n"
"    \"publish_hz\": 4,\n"
"    \"max_keys_per_provider\": 500\n"
"  }\n"
"}\n";

std::string ReadFile(const std::string& path, bool* found)
{
    *found = false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return std::string();

    *found = true;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        text.append(buf, n);
    std::fclose(f);
    return text;
}

} // namespace

Config Config::Load(const std::string& path, std::string* error)
{
    Config config;
    if (error)
        error->clear();

    bool found = false;
    const std::string text = ReadFile(path, &found);
    if (!found)
        return config;                 // no file: relay stays off

    std::string parseError;
    const json::Value root = json::Parse(text, &parseError);
    if (!root.IsObject())
    {
        // A broken config must not silently fall back to "enabled" -- the
        // default object we return has the relay off, which is the safe read.
        if (error)
            *error = parseError.empty() ? "not a JSON object" : parseError;
        return config;
    }

    const json::Value& relay = root["relay"];
    if (!relay.IsObject())
        return config;

    RelayConfig& r = config.relay;
    r.enabled    = relay["enabled"].AsBool(false);
    r.host       = relay["host"].AsString();
    r.port       = static_cast<uint16_t>(relay["port"].AsInt(8883));
    r.tls        = relay["tls"].AsBool(true);
    r.username   = relay["username"].AsString();
    r.password   = relay["password"].AsString();
    r.controller = relay["controller"].AsString();

    if (relay.Has("room") && !relay["room"].AsString().empty())
        r.room = relay["room"].AsString();

    for (const json::Value& p : relay["providers"].Items())
        if (p.IsString() && !p.AsString().empty())
            r.providers.insert(p.AsString());

    const int64_t hz = relay["publish_hz"].AsInt(4);
    r.publishHz = static_cast<uint32_t>(hz < 1 ? 1 : (hz > 50 ? 50 : hz));

    const int64_t cap = relay["max_keys_per_provider"].AsInt(500);
    r.maxKeysPerProvider = static_cast<uint32_t>(cap < 1 ? 1 : cap);

    // Enabled but unusable is a misconfiguration worth reporting rather than
    // retrying against an empty hostname forever.
    if (r.enabled && r.host.empty())
    {
        r.enabled = false;
        if (error)
            *error = "relay.enabled is true but host is empty; relay stays off";
    }

    return config;
}

bool Config::WriteDefault(const std::string& path)
{
    FILE* existing = std::fopen(path.c_str(), "rb");
    if (existing)
    {
        std::fclose(existing);
        return false;                  // never overwrite a user's file
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;

    std::fwrite(kTemplate, 1, std::strlen(kTemplate), f);
    std::fclose(f);
    return true;
}

std::string RelayConfig::Describe() const
{
    if (!enabled)
        return "relay: off (nothing leaves this machine)";

    std::string allowed;
    for (const std::string& p : providers)
    {
        if (!allowed.empty())
            allowed += ", ";
        allowed += p;
    }
    if (allowed.empty())
        allowed = "<none allowed>";

    char line[512];
    std::snprintf(line, sizeof line,
                  "relay: %s:%u %s | room '%s' | as '%s' | %u Hz | providers: %s",
                  host.c_str(), static_cast<unsigned>(port),
                  tls ? "TLS" : "PLAINTEXT",
                  room.c_str(),
                  controller.empty() ? "<euroscope callsign>" : controller.c_str(),
                  publishHz,
                  allowed.c_str());
    return line;
}

std::string Config::Describe() const
{
    return relay.Describe();
}

} // namespace esb
