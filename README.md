# EuroScope Plugin Bridge

A shared, schema-declared registry that lets independent EuroScope plugins publish and read each other's data — without a server, a socket, or anything crossing the VATSIM network.

Every EuroScope plugin is a DLL loaded into the EuroScope process, so plugin-to-plugin data never needs to leave that process. The bridge is one small DLL exporting a stable C ABI that other plugins reach with `GetModuleHandle` + `GetProcAddress`.

| | |
| --- | --- |
| **Status** | M1–M5 implemented. Relay built but gated — see below. |
| **ABI** | v1 |
| **Target** | Win32 (EuroScope is 32-bit), v143 toolset, static CRT |
| **Dependencies** | None. The EuroScope SDK is vendored in `lib/`. |

## Documentation

| Document | For |
| --- | --- |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Why it is built this way — decisions, hazards, trade-offs |
| [docs/INTEGRATION.md](docs/INTEGRATION.md) | Plugin authors: a 50-step checklist to integrate |
| [include/esbridge.h](include/esbridge.h) | The contract. One header, no linker dependency. |

## Building

Open `EuroScopeBridge.sln` in Visual Studio and build **Release | Win32**, or:

```bash
msbuild EuroScopeBridge.sln -p:Configuration=Release -p:Platform=Win32
```

Output lands in `build\Release\EuroScopeBridge.dll`.

Win32 is not a preference — EuroScope is a 32-bit application and will not load an x64 plugin. The static CRT (`/MT`) means the DLL ships with no redistributable.

## Installing

In EuroScope: **Other Settings → Plug-ins → Load**, and pick `EuroScopeBridge.dll`.

It must be registered as a plugin. A bridge loaded any other way never receives `OnTimer` and so never tracks aircraft or collects garbage — see [ARCHITECTURE.md §9.3](docs/ARCHITECTURE.md).

Verify with `.esb providers` in the EuroScope command line.

## Testing

```bash
tests\build-and-run.cmd
```

And the worked example pair, which builds two real EuroScope plugins against nothing but `esbridge.h`:

```bash
examples\build.cmd
```

The test links the core translation units directly rather than loading the DLL, because the built plugin imports `EuroScopePlugInDll.dll` and cannot be loaded outside EuroScope. That it links at all is itself a check: `Registry`, `AircraftTable` and `ApiExports` must have no EuroScope dependency.

Export names are verified separately — `dumpbin /exports` must show `ESB_GetApi` **undecorated**, or `GetProcAddress` in client plugins will not find it.

## What works today

Provider registration with exclusive namespace claim · schema declaration and validation · global and per-aircraft values · revision counters with compare-before-write · generational aircraft handles surviving callsign recycling · two-phase TTL garbage collection with slot reclamation · module-liveness reaping of providers **and subscriptions** · duplicate-instance stand-down · synchronous change callbacks with deferred re-entrant writes and cycle shedding · SEH-guarded dispatch that unsubscribes a faulting callback · rotating log file · `.esb providers, schema, stats, get, watch, subs, dump, log`.

**The relay is built but not cleared to ship.** It is off by default and needs two things before it should be turned on: the VATSIM/vACC policy conversation in ARCHITECTURE.md §11.6, and a run against a live TLS broker — the SChannel path compiles and follows the standard handshake but has never talked to a real server. Everything else is implemented and covered by 165 tests.

## Layout

```
include/esbridge.h     public C ABI — the only file plugin authors need
src/Registry.*         the registry. No EuroScope dependency.
src/AircraftTable.*    generational handles. No EuroScope dependency.
src/ApiExports.cpp     the exported vtable. No EuroScope dependency.
src/Log.*              rotating log file. No EuroScope dependency.
src/BridgePlugin.*     the EuroScope adapter — the only file that knows about it
lib/                   vendored EuroScope SDK
examples/              a worked publisher + consumer pair, buildable
src/Relay.*            the relay thread. Never touches the registry.
src/Transport.*        sockets + TLS. The only file that opens one.
src/MqttCodec.*        MQTT 3.1.1 framing. No I/O, fully testable.
src/Config.*, Json.*   bridge.json and bridge-state.json
tests/smoke.cpp        165 checks across M1-M5
```

The split is deliberate: everything except `BridgePlugin` is testable, and reviewable, without EuroScope in the picture.

## A note on `lib/`

`EuroScopePlugIn.h` and `EuroScopePlugInDll.lib` are Gergely Csernák's EuroScope SDK, vendored here so the project builds out of the box (the same thing CCAMS and vSMR do). If you would rather not redistribute them, delete `lib/`, add it to `.gitignore`, and point `AdditionalIncludeDirectories` / `AdditionalLibraryDirectories` at your own copy.
