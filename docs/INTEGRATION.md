# Integration Checklist — for plugin developers

Everything you need to do to talk to the bridge, in the order you need to do it. Design rationale lives in [ARCHITECTURE.md](ARCHITECTURE.md); the contract is [`include/esbridge.h`](../include/esbridge.h).

> **Status: pre-implementation.** `EuroScopeBridge.dll` does not exist yet — it is what milestone M1 produces ([§15](ARCHITECTURE.md)). This checklist is the contract it will be built against, so it is worth reading before you design around the bridge, but none of it runs today.

Three phases, each usable on its own:

| Phase | You get | Depends on |
| --- | --- | --- |
| **A — Attach** | A working `ESB_Api_v1*`, and a plugin that survives the bridge being absent | nothing |
| **B — Bridged data** | Publishing fields other plugins read, and reading theirs | A |
| **C — Remote data** | Selected fields mirrored to and from other controllers | A + B |

---

## Phase A — Get the bridge attached

### Setup

- [ ] **A1.** Copy `esbridge.h` into your project. No `.lib`, no linker settings, no package manager — it is one header with no build-system footprint.
- [ ] **A2.** `#define ESB_CLIENT_SHIM` before including it in **exactly one** `.cpp`. The shim defines file-static state; two definitions means two attach states.
- [ ] **A3.** **Do not ship the bridge DLL with your plugin.** It is released standalone at [github.com/AlexisBalzano/Euroscope-Plugin-Bridge/releases](https://github.com/AlexisBalzano/Euroscope-Plugin-Bridge/releases) — link your users there, and tell them to add it through *Other Settings → Plug-ins → Load*. Windows maps only one module of a given base name per process, so bundled copies silently collapse onto whichever the user listed first, possibly a much older one than yours. A newer bridge always serves older clients, so "install the latest" is always right (§14.1).

### Attach

- [ ] **A4.** Call `ESB_Attach()` from **`OnTimer`, not your constructor.** EuroScope's plugin load order follows the user's settings file; the bridge may legitimately load after you.
- [ ] **A5.** Handle `NULL` on every call. Run with your bridge features off — never crash, never open a dialog.
- [ ] **A6.** Never `LoadLibrary` the bridge yourself. A bridge outside EuroScope's plugin list gets no `OnTimer` and no aircraft lifecycle events, so it silently never collects garbage, reaps dead modules, or drains the relay.
- [ ] **A7.** If your plugin genuinely cannot function without the bridge, `DisplayUserMessage(ESB_MISSING_MESSAGE)` **once** after ~10 failed ticks, then stop trying. Use the macro rather than your own wording — a user running three bridge-aware plugins should see one instruction, not three.

### Rules you cannot break

- [ ] **A8.** Call `esb_*` **only from the EuroScope main thread.** If you have a worker thread — an HTTP fetch, a file watcher — queue its results and apply them in `OnTimer`.
- [ ] **A9.** Use `ESB_SelfModule()` for your `HMODULE`, never `GetModuleHandleA("YourPlugin.dll")`. Users rename DLLs, and a wrong handle means the bridge reaps your registration while you are still alive.
- [ ] **A10.** Release everything in your destructor: `unregister_provider()`, and `unsubscribe()` for every subscription. A live callback pointer into an unloaded DLL crashes EuroScope for everyone.

### Verify

- [ ] **A11.** `.esb providers` lists your plugin, with your version and contact string.

---

## Phase B — Add bridged data

### B1 · Publishing a field

- [ ] **B1.1.** Choose a provider id. Lowercase, stable **forever**, and distinctive — it is an exclusive claim, so pick something no one else will plausibly want. Reverse-DNS (`org.vatsim-fr.cdm`) if you are unsure.
- [ ] **B1.2.** Declare your fields as a `static const ESB_FieldDecl[]`. Pick the narrowest type that fits; set `max_bytes` on every `STR`/`BLOB`.
- [ ] **B1.3.** **Write the `doc` string properly.** It is what `.esb schema` prints and, in practice, the only documentation a consumer will ever read.
- [ ] **B1.4.** Start at `schema_major = 1`, `schema_minor = 0`.
- [ ] **B1.5.** Fill `ESB_ProviderDecl` — including `struct_size = sizeof d` and `module = ESB_SelfModule()`.
- [ ] **B1.6.** Call `register_provider()` once, after attach succeeds. Handle `ESB_E_PROVIDER_TAKEN`: someone else claimed your id, which is a conflict to resolve with that author, not a condition to retry around.
- [ ] **B1.7.** Resolve your own field ids once via `own_field()` and cache them. Never call it in a hot path.
- [ ] **B1.8.** Write with `set_global` / `set_ac`, passing your `ESB_Provider*`.
- [ ] **B1.9.** **Do not add your own dirty-tracking.** Writing a value identical to the one already stored does not bump the revision or fire callbacks — the bridge compares first. Write on every tick if it is simpler.
- [ ] **B1.10.** Do not mirror what EuroScope already exposes. Callsign, altitude, route and squawk are available to every plugin directly; copying them creates a second, staler source of truth.
- [ ] **B1.11.** Bump `schema_minor` when you add a field; bump `schema_major` for anything else — type change, removal, or a change in what the value *means*.

```cpp
#define ESB_CLIENT_SHIM              // exactly one .cpp in your project
#include "esbridge.h"

static const ESB_FieldDecl kFields[] = {
  { "tobt",  ESB_T_I64, ESB_SCOPE_AIRCRAFT, 0, 0,
    "Target off-block time, minutes since midnight UTC" },
  { "stand", ESB_T_STR, ESB_SCOPE_AIRCRAFT, 0, 8,
    "Assigned stand identifier" },
};

static ESB_Provider *g_prov  = 0;
static ESB_FieldId   g_tobt  = ESB_FIELD_NONE;

void MyPlugin::OnTimer(int)
{
    const ESB_Api_v1 *api = ESB_Attach();
    if (!api) return;                            // bridge not loaded (yet)

    if (!g_prov) {
        ESB_ProviderDecl d = {0};
        d.struct_size  = sizeof d;
        d.provider_id  = "lfpg_cdm";
        d.schema_major = 1;
        d.schema_minor = 0;
        d.display_name = "LFPG CDM";
        d.contact      = "https://github.com/you/lfpg-cdm";
        d.fields       = kFields;
        d.field_count  = 2;
        d.module       = ESB_SelfModule();      // never GetModuleHandleA(name)

        if (api->register_provider(&d, &g_prov) != ESB_OK) return;
        api->own_field(g_prov, "tobt", &g_tobt);
    }

    ESB_Aircraft ac;
    if (api->aircraft("AFR1234", &ac) != ESB_OK) return;

    ESB_Value v = {0};
    v.type = ESB_T_I64; v.bytes = sizeof(int64_t); v.v.i64 = 742;
    api->set_ac(g_prov, ac, g_tobt, &v);        // no-op if unchanged
}

MyPlugin::~MyPlugin()
{
    if (esb_api && g_prov) esb_api->unregister_provider(g_prov);
}
```

### B2 · Consuming a field

- [ ] **B2.1.** `resolve("provider/field", EXPECTED_TYPE, &id)` once — but keep retrying alongside `ESB_Attach()`, because the provider may load after you.
- [ ] **B2.2.** **Handle `ESB_E_NO_PROVIDER` and keep working.** The plugin you read from may simply not be installed. This is mandatory, not defensive style.
- [ ] **B2.3.** Handle `ESB_E_TYPE_MISMATCH` — the provider changed their schema out from under you. Do not read the field.
- [ ] **B2.4.** Gate on `provider_version()` if you depend on specific semantics.
- [ ] **B2.5.** Pick your change feed:
  - **Poll** — store the last revision, compare in `OnTimer`. Use `provider_revision()` as a coarse gate to skip a whole provider in one comparison.
  - **Subscribe** — `subscribe(field, cb, user, ESB_SelfModule(), &sub)`. Delivered on the main thread, synchronously inside the publisher's write.
- [ ] **B2.6.** If you subscribe: keep the callback short, do not block, and expect no ordering guarantee across different fields. Writes inside a callback are allowed; their notifications are deferred until the outermost dispatch unwinds.
- [ ] **B2.7.** For `STR`/`BLOB`, pass a buffer and handle `ESB_E_BUFFER_TOO_SMALL` by resizing to the returned `*io_bytes`. Scalars need no buffer — pass `NULL, 0`.
- [ ] **B2.8.** Distinguish `ESB_E_UNSET` (field declared, no value yet) from `ESB_E_NO_PROVIDER` (plugin absent). They call for different behaviour.
- [ ] **B2.9.** On `ESB_E_STALE_AIRCRAFT`, re-resolve the handle from the callsign. Never treat a cached `ESB_Aircraft` as valid indefinitely.
- [ ] **B2.10.** `unsubscribe()` every subscription in your destructor.

```cpp
static ESB_FieldId g_tobt = ESB_FIELD_NONE;
static uint64_t    g_seen = 0;

void MyPlugin::OnTimer(int)
{
    const ESB_Api_v1 *api = ESB_Attach();
    if (!api) return;

    if (g_tobt == ESB_FIELD_NONE &&
        api->resolve("lfpg_cdm/tobt", ESB_T_I64, &g_tobt) != ESB_OK)
        return;                        // not installed — degrade, don't fail

    ESB_Aircraft ac;
    if (api->aircraft("AFR1234", &ac) != ESB_OK) return;

    uint64_t rev = api->revision(g_tobt, ac);
    if (rev == g_seen) return;                   // nothing new
    g_seen = rev;

    ESB_Value v;
    if (api->get_ac(ac, g_tobt, &v, 0, 0) == ESB_OK)
        UseTobt(v.v.i64);
}
```

### Verify

- [ ] **B3.** `.esb schema <your-id>` prints your fields with your doc strings.
- [ ] **B4.** `.esb get <provider>/<field> <callsign>` shows the value, its revision, and who wrote it.
- [ ] **B5.** `.esb watch <key>` while you exercise the plugin — confirms writes land when you expect and *only* when you expect.
- [ ] **B6.** Test with the other plugin uninstalled. Yours must still start, run, and shut down cleanly.

---

## Phase C — Add remote data

Everything here is opt-in and off by default. Assume most of your users will never turn it on.

### Publishing

- [ ] **C1.** Decide **per field** whether it should ever leave the machine. Default is no. Add `ESB_F_SYNC` only where cross-controller sync is genuinely the point.
- [ ] **C2.** Understand that you cannot force sync on. It takes two keys: your flag, **and** the user enabling the relay and your provider in `bridge.json`.
- [ ] **C3.** Never put anything sensitive in a synced field. It reaches a broker you do not control and lands on other controllers' machines.
- [ ] **C4.** Publish about any aircraft you like — **assumed or not.** There is no tracking-controller rule. A CDM plugin publishes TOBT for departures nobody tracks yet; a sequencer publishes arrival order for traffic in the next sector down. That is the intended use.
- [ ] **C5.** But **scope what you publish** to what you are actually responsible for. There is a per-provider cap on live published keys; `ESB_E_LIMIT` means you are publishing too much, not that the cap is wrong.
- [ ] **C6.** Do not write at high frequency. Outbound is coalesced (~4 Hz, latest value only), so anything faster is work you are throwing away.

### Consuming

- [ ] **C7.** Read in two steps: `remote_publishers(field, aircraft, origins, &count)` to see who is publishing, then `get_remote(field, aircraft, peer, ...)` for the one you want.
- [ ] **C8.** **Decide your arbitration rule and write it in your own docs.** Most recent `received_ms`? Highest `revision`? A preferred peer? The bridge deliberately will not choose for you.
- [ ] **C9.** Handle `count > 1`. It is normally 0 or 1, because usually only one controller runs your plugin for a given aircraft — but that is a tendency, not a guarantee.
- [ ] **C10.** Never merge remote into local. `get_ac()` is always something a local plugin wrote. If you want a combined view, build it in your own code and be explicit about which half is which.
- [ ] **C11.** Expect remote values to arrive for aircraft you have not seen yet — a TOBT can beat the flight plan to you. The bridge holds those briefly and applies them when the aircraft appears, so do not assume ordering against flight-plan arrival.
- [ ] **C12.** Peers come and go. `list_peers()` tells you who is connected; a peer's values disappear when they drop.
- [ ] **C13.** **Test with the relay off.** That is the default state for most of your users. Zero peers must be a fully working configuration, not a degraded one.

```cpp
ESB_Origin who[4];
uint32_t   n = 4;
if (api->remote_publishers(g_tobt, ac, who, &n) != ESB_OK || n == 0)
    return;                             // nobody is publishing this

// Your rule, not the bridge's. This one takes the most recently received.
uint32_t best = 0;
for (uint32_t i = 1; i < n; ++i)
    if (who[i].received_ms > who[best].received_ms) best = i;

ESB_Value v;
if (api->get_remote(g_tobt, ac, who[best].peer, &v, 0, 0) == ESB_OK)
    UseRemoteTobt(v.v.i64, who[best].peer);
```

### Verify

- [ ] **C14.** `.esb net` shows the relay state, connected peers, and exactly which fields are syncing in each direction.

---

## Mistakes that will bite you

| Mistake | What happens |
| --- | --- |
| `LoadLibrary` instead of `GetModuleHandle` | A bridge that never ticks: no GC, no module reaping, no relay drain. Silent, and it looks like it works. |
| Attaching in the constructor | Works on your machine, fails for any user whose plugin order differs. |
| `GetModuleHandleA("MyPlugin.dll")` | Breaks the moment a user renames your DLL, and the bridge reaps you while you are still running. |
| Forgetting `unsubscribe()` | Callback pointer into an unloaded DLL. Crashes EuroScope for everyone, in a way that looks like the bridge's fault. |
| Calling from a worker thread | Corruption you will not reproduce. Catch it with a debug build, which returns `ESB_E_WRONG_THREAD`. |
| Caching `ESB_Aircraft` indefinitely | Reads fail with `ESB_E_STALE_AIRCRAFT` after a reconnect, or silently pick up nothing. |
| Mirroring EuroScope data into the bridge | A second source of truth, staler than the first. |
| JSON in a `BLOB` to dodge declaring a schema | Defeats validation, defeats `.esb schema`, and makes your data unreadable to everyone but you. |
| Assuming you are the only remote publisher | Works until the day a second controller runs your plugin. Handle `count > 1`. |
| Treating "provider missing" as an error | Your plugin refuses to start because an unrelated optional plugin is not installed. |
