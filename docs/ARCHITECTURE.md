# EuroScope Plugin Bridge — Architecture

**Status:** M1–M5 implemented · **ABI:** v1 · **Canonical contract:** [`include/esbridge.h`](../include/esbridge.h)

> **The relay (§11) is built but must not ship yet.** Two things gate it: the §11.6 policy question with VATSIM/vACC, and the SChannel TLS path, which compiles and follows the standard handshake loop but has never been run against a live broker. Everything in §1–10 is implemented and tested.

A shared, schema-declared registry that lets independent EuroScope plugins publish and consume each other's data, with an optional relay for mirroring selected fields between controllers.

---

## 1. The problem

EuroScope offers no supported channel for one plugin to read another's data. In practice the community works around this by abusing the two fields the network already carries:

| Workaround | Why people use it | Why it hurts |
| --- | --- | --- |
| Scratchpad string | Every plugin can read/write it | ~100 chars total, shared by *all* plugins, transmitted to every controller, collides silently, visible in other clients |
| Flight strip annotations | 9 slots, per-aircraft | Same collision problem, also network-transmitted, no types, no discovery |
| Re-parsing the same source twice | No coordination needed | Duplicated work, duplicated bugs, divergent answers between plugins |

All three push local coordination data across the VATSIM network, where it becomes other controllers' problem. The bridge exists so that plugin-to-plugin data never has to leave the process unless someone deliberately chooses to send it.

## 2. Why there is no local server

**Every EuroScope plugin is a DLL that EuroScope loads into its own process.** All plugins share one address space and one thread. That single fact removes the entire transport problem for the local case.

A localhost server would add, for zero benefit: a listening socket, a firewall prompt on first run, a port-collision failure mode, JSON or protobuf encode/decode on every access, a worker thread whose results must be marshalled back to the EuroScope main thread (the EuroScope API is not thread-safe), and a second process to install, supervise and version.

An in-process registry behind an exported C ABI has none of those. A read is a bounds check and a load. The bridge does not need to be reachable, authenticated, or serialized, because the caller is already inside it.

The clarification that settled this: **companion apps talk to their own EuroScope plugin, which talks to the bridge.** That path is already in-process at the point the bridge is involved, so it needs no gateway. The bridge never opens a socket for local traffic — only the online relay in §11 opens one, and only when the user configures it.

### 2.1 Options considered

| Option | Verdict |
| --- | --- |
| **Exported C ABI in a bridge DLL** | **Chosen.** Zero copy, zero latency, no ports, no threads. Cost: strict ABI discipline (§4). |
| Named shared memory + mutex | Solves a cross-process problem we do not have; adds pointer-relocation and locking for nothing. |
| Localhost HTTP / WebSocket | Rejected for local. See above. |
| COM in-process server | Gives ABI stability for free, but registry entries, `IUnknown` ceremony, and threading-model rules make it far heavier to adopt than a header. |
| C++ interface class exported directly | Only safe if every plugin uses one identical toolset and CRT. Third parties will not. |
| Registered window message broadcast | Fine for notification, useless as storage; still needs a registry behind it. |

## 3. Locked decisions

| # | Decision | Rationale |
| --- | --- | --- |
| D1 | **In-process only for local; relay only for online** | §2. Local participants are already inside the process. |
| D2 | **Revision counters + opt-in main-thread callbacks** | Pollers stay trivially safe; latency-sensitive consumers get zero-tick delivery (§7). |
| D3 | **Registered schemas** | Every field is declared with a type, scope, size cap and doc string before it can hold a value. Makes the registry self-describing, lets the bridge validate untrusted relay input, and gives `.esb schema` something real to print. |
| D4 | **Exclusive ownership** | A provider id is claimed by exactly one live module. Only the owner writes. Two plugins fighting over a key becomes a startup error rather than a heisenbug. |

D3 and D4 collapse into a single act: **registration is the claim.** A plugin declares its namespace and its schema in one call and receives a write-authority token.

## 4. ABI discipline

The bridge is a public integration point for plugins built by other people, with other compilers, other CRTs, other STL versions, in debug and release. The boundary is therefore pure C:

- C linkage, explicit `__cdecl`, POD types only. No `std::string`, no `std::vector`, no exceptions crossing the line.
- One exported symbol: `ESB_GetApi(uint32_t abi_version)` returns a `const ESB_Api_v1*` or `NULL`.
- Growable structs lead with `uint32_t struct_size`; readers gate optional members on it.
- Released members are never reordered, retyped or removed. New capability is appended, or lands in `ESB_Api_v2` served by the same entry point.
- Memory never changes owner. Reads copy into a caller buffer, returning `ESB_E_BUFFER_TOO_SMALL` with the required size in `*io_bytes`.
- All functions are `noexcept` and report through `ESB_Status`.

**Inside** the bridge, ordinary modern C++ and the STL are fine. The rule is about the boundary, not the implementation.

## 5. Data model

```
scope     ::= global | aircraft
provider  ::= claimed namespace         "ccams", "topsky", "org.vatsim-fr.cdm"
field     ::= name declared in that provider's schema
key       ::= provider "/" field        "ccams/assigned_squawk"
```

An aircraft-scoped value is addressed by `(ESB_FieldId, ESB_Aircraft)`; a global one by `ESB_FieldId` alone.

Consumers call `resolve("ccams/assigned_squawk", ESB_T_I64, &id)` **once at startup** and reuse the `ESB_FieldId` forever — string lookup never appears in a hot path. `resolve` also fails loudly on `ESB_E_TYPE_MISMATCH`, so a consumer compiled against an older schema finds out immediately instead of misreading bytes.

### 5.1 What the bridge does *not* store

The bridge is not a mirror of the EuroScope API. Callsign, altitude, route, squawk and controller assignments are already available to every plugin directly, and duplicating them would create a second, staler source of truth. The bridge's own `core/` provider carries only what EuroScope does not expose: `first_seen`, `last_seen`, `generation`.

### 5.2 Schema versioning

`schema_minor` bumps on additive change (new field). `schema_major` bumps on anything else (type change, removal, semantic change). Consumers gate with `provider_version()` and must tolerate `ESB_E_NO_PROVIDER` — the plugin they read from may simply not be installed. Degrading gracefully when a provider is absent is a hard requirement, not a nicety.

## 6. Aircraft identity and lifecycle

Callsign is the only key every plugin already has, and it is **reusable**: a pilot disconnects and someone else connects as `AFR1234` twenty minutes later. Keying per-aircraft data on the raw string leaks the previous occupant's state into the new one.

`ESB_Aircraft` is therefore a **generational handle** — a slot index packed with a generation counter:

```
 63                    32 31                     0
+------------------------+------------------------+
|      generation        |       slot index       |
+------------------------+------------------------+
```

- The bridge bumps the generation and clears every per-aircraft value for that slot on `OnFlightPlanDisconnect`, and on any reconnect it observes.
- A read through a handle whose generation no longer matches returns `ESB_E_STALE_AIRCRAFT` rather than someone else's data.
- Handles are cheap to hold across ticks — but must be re-resolved from callsign whenever a plugin sees `ESB_E_STALE_AIRCRAFT`.

**Garbage collection.** EuroScope does not always deliver a clean disconnect, so anything that stops being observed has to age out on its own. The bridge refreshes `last_seen` for every aircraft during its `OnTimer` pass over the flight-plan and radar-target lists, and sweeps in two phases — because "gone" and "forgettable" are different things:

| Phase | When | Effect |
| --- | --- | --- |
| **Retire** | unseen for the TTL (default 15 min) | Generation bumped, values dropped — identical to a disconnect |
| **Reclaim** | a further grace period (default 5 min) | Entry removed, slot returned to a free list for a different callsign |

The grace window means a pilot who reconnects shortly after a dropout lands on the same slot and keeps their `first_seen`. Reclaimed slots are reused, and a reused slot's generation keeps climbing from its previous occupant — which is what stops a handle to the old aircraft from ever matching the new one.

Cadence: the aircraft sweep runs every 30 s and module-liveness reaping every 5 s. `OnTimer` fires every second and both throttle themselves, so the EuroScope layer does not have to know either interval. At a busy sector's few hundred aircraft the work is negligible either way.

## 7. Change propagation

Two mechanisms, deliberately:

**Revision counters.** Every value carries a monotonic `uint64` that never repeats and is `0` while unset. A polling consumer stores the revision it last acted on and compares in its own `OnTimer`. `provider_revision()` gives a coarse gate so a consumer can skip an entire provider in one comparison. This path cannot deadlock, cannot re-enter, and cannot be broken by a badly written subscriber.

**Writes are compared, not blind.** Setting a field to the value it already holds does not bump the revision and does not fire callbacks — the bridge compares first, blob contents included. A publisher can therefore write on every tick without generating notification traffic, which removes the commonest source of accidental storms and means no plugin needs its own dirty-tracking.

**Callbacks.** `subscribe()` registers a function pointer against a field. Because all local writes happen on the EuroScope main thread, notification is **synchronous at write time** — local plugin-to-plugin latency is zero, not one tick.

The callback carries only `(field, aircraft, user)`, never the value. The subscriber reads it, which removes every pointer-lifetime question and makes coalescing trivial.

### 7.1 Re-entrancy

A subscriber may legitimately write in response to a notification. The bridge handles this with a dispatch-depth counter:

1. Writes made inside a notification are applied immediately, but *their* notifications are queued.
2. The queue drains when the outermost dispatch unwinds.
3. Notifications coalesce per `(subscriber, field, aircraft)`, so a cycle produces at most one extra call per pair, not an avalanche.
4. Depth is capped (8). Exceeding it logs the offending provider chain and drops the tail — a cycle becomes a diagnosable log line rather than a stack overflow inside EuroScope.

## 8. Threading

**All `esb_*` calls happen on the EuroScope main thread.** Debug builds assert the thread id and return `ESB_E_WRONG_THREAD`; release builds assume it.

The relay is the only thread in the system, and it never touches the registry. It owns two single-producer/single-consumer queues:

```
  main thread                          relay thread
  -----------                          ------------
  set_ac() ──► registry ──► outbound ──────►  encode ──► wss://
                              queue

  registry ◄── validate ◄── inbound  ◄────── decode ◄── wss://
   (OnTimer)                  queue
```

The bridge's `OnTimer` drains `inbound`, validates each message against the local schema, and applies it. Nothing else ever writes to the registry. From a plugin author's point of view the bridge is single-threaded, which is the correct model for EuroScope and removes an entire category of bug from third-party code.

Consequence: local changes propagate in **0 ticks**, remote changes in **≤1 tick** (≤1 s).

## 9. Failure modes

### 9.1 A plugin's DLL is unloaded

The sharpest hazard in the design. EuroScope lets a user remove a plugin at runtime; a registered callback pointer then points into freed address space and the next dispatch crashes EuroScope for everyone.

Mitigation, and the reason `ESB_ProviderDecl::module` and `subscribe(..., module, ...)` exist: every provider and every subscription records the caller's `HMODULE`, along with its path at claim time. The bridge periodically verifies those modules are still mapped at that path and reaps registrations whose module has vanished, releasing the provider claim so a reloaded plugin can re-register.

**A dead provider's values die with it.** Field ids are deliberately kept stable across a reload, so a consumer that resolved `foo/bar` before the reload need not resolve again. That is exactly why the values cannot be left behind: a plugin removed and re-added would otherwise find its own stale data from a previous life still sitting there, indistinguishable from something it had just written. Reaping drops the provider's global and per-aircraft values along with the claim.

This is a correctness requirement, not hardening.

### 9.2 The bridge is not installed

`ESB_Attach()` returns `NULL` and the client plugin runs with its bridge features disabled. Never a crash, never a dialog. A plugin that cannot function at all without the bridge should say so once via `DisplayUserMessage` and stop, rather than retry-spamming.

### 9.3 The bridge loads after its clients

EuroScope's plugin load order follows the user's settings file and is not guaranteed. `ESB_Attach()` is therefore lazy and idempotent: call it every tick until it succeeds. Once attached it is a single pointer test.

Clients must use `GetModuleHandle`, **never** `LoadLibrary`. A bridge loaded outside EuroScope's plugin list never receives `OnTimer` or the aircraft lifecycle events, so it would silently never garbage-collect, never reap dead modules, and never drain the relay.

### 9.4 Two copies of the bridge DLL

Two folders, two filenames — and the obvious fix turned out to be wrong.

An earlier draft had the first instance to claim a named mutex serve, with losers resolving the winner's `ESB_GetApi` and forwarding to it. Implementing it showed that forwarding solves a problem that cannot arise while creating one that can. Clients find the bridge with `GetModuleHandleA("EuroScopeBridge.dll")` — **by base name**. Windows maps only one module of a given base name per process, so a second copy can only exist if somebody renamed it, and a renamed copy is unreachable by every client no matter which one loaded first. Being first to load does not make an instance the right server. Forwarding would also leave the loser handing clients a pointer into a module whose lifetime it does not control.

The rule therefore follows from how clients actually look the bridge up: **the instance named `EuroScopeBridge.dll` serves; anything else stands down.** Load order is irrelevant.

A stood-down instance runs no sweeps and answers no dot commands — otherwise the user sees every response twice with no way to tell which copy produced it — and its `ESB_GetApi` returns `NULL`, so anything that did reach it takes the documented no-bridge path instead of silently talking to a registry that will never be populated. It reports once, in the message window, which file is inert and where it sits, so the user can delete it.

The named mutex `Local\EuroScopeBridge.v1.<pid>` survives as belt and braces for the remaining case: if two identically named modules ever do get mapped (side-by-side activation contexts can manage it), the first to claim it serves and the second stands down the same way.

### 9.5 A subscriber faults

Same process, so there is no true isolation. Pragmatically: dispatch is wrapped in SEH, a faulting subscription is auto-unsubscribed and logged with its provider id and contact string. EuroScope survives, and the log names who to email.

## 10. Persistence

Fields flagged `ESB_F_PERSIST` (global scope only — per-aircraft state is meaningless across sessions) are written to `bridge-state.json` beside the config, debounced to at most one flush every 5 s plus one on shutdown. Restored at startup only after the owning provider registers, and only if the schema still matches — a changed type discards the stored value rather than resurrecting garbage.

## 11. Online relay

Opt-in, off by default, and layered strictly on top of the local registry.

### 11.1 Consent is two-key

A field reaches the network only when **both** are true: its author declared `ESB_F_SYNC`, **and** the user enabled the relay and that provider in `bridge.json`. Author intent alone never puts a controller's data on the wire. The list of what is currently syncing is printable with `.esb net`.

### 11.2 Transport: MQTT over TLS

Recommended for v1, because the registry model maps onto MQTT almost exactly:

- **Retained messages** give current-value-per-key semantics to late joiners for free — precisely what a registry is.
- **Topic wildcards** give subscription filtering without server code.
- **Last Will and Testament** cleans up a controller's data when they drop, which is otherwise fiddly to get right.
- Mature brokers (Mosquitto, EMQX) with per-topic ACLs exist, so there is no server to write or operate.

Topic layout: `esb/v1/<room>/<controller>/<provider>/<field>[/<callsign>]`.

A custom WebSocket server is the alternative, and is only worth it if the relay needs server-side logic — arbitration, derived values, history. It costs you a service to build, host, secure and keep online.

### 11.3 Provenance, not authority

One rule: **every synced value is published under its author's own topic prefix**, for aircraft scope exactly as for global scope. Nothing collides, and consumers read the set.

An earlier draft made the tracking controller authoritative for aircraft-scoped fields. That was wrong. A plugin routinely has something to say about an aircraft it does not have assumed — a CDM plugin publishing TOBT/TSAT for a departure nobody is tracking yet, a code-assignment plugin publishing before pickup, a sequencer publishing arrival order for traffic still in the sector next door. Gating publication on tracking would silence exactly the cases the relay exists for, and it also made the `peer` argument of `get_remote()` meaningless for aircraft fields.

So **the bridge arbitrates nothing.** `remote_publishers()` returns every peer currently publishing a given field for a given aircraft, each with an `ESB_Origin` carrying the peer, the publisher's revision, and local receipt time. The consumer applies its own rule — most recent, a preferred peer, or something domain-specific. In practice the count is 0 or 1, because usually only one controller runs the publishing plugin for a given aircraft; the API simply declines to pretend that is guaranteed.

Ordering within one publisher's stream uses `(revision, publisher_id)` — no reliance on synchronised clocks.

**Consequence — publish scope.** With no authority rule, twenty controllers running the same plugin could each publish for the same two hundred aircraft. Providers are expected to scope what they publish to what they are actually responsible for, and the bridge enforces a per-provider cap on live published keys, returning `ESB_E_LIMIT` beyond it.

### 11.4 Aircraft identity on the wire

`ESB_Aircraft` is a local handle and means nothing to a peer, so the wire carries the **callsign** and each bridge resolves it against its own aircraft table. Two consequences follow:

- A remote value can arrive for a callsign this client has not seen yet — a TOBT published before the flight plan reaches you. Those are held in a small pending map keyed by callsign with a short TTL, and applied if and when the aircraft appears.
- The remote view participates in generational clearing (§6). When a callsign disconnects and its generation bumps, remote values for it are dropped alongside local ones, so a recycled callsign never inherits a previous occupant's remote data either.

### 11.5 Remote values are a separate view

Inbound data lands in a **read-only remote view** reached through `remote_publishers()` and `get_remote()`. It is never merged into local storage. A plugin that reads `get_ac()` is always reading something a local plugin wrote — which eliminates the entire class of "why did my value change on its own" bug, and means a hostile or buggy peer cannot corrupt local state.

### 11.6 Hardening

- Bind nothing; the bridge is a client, never a listener.
- TLS with certificate validation; per-user credentials; broker-side ACLs scoped to the user's own topic prefix.
- Inbound messages are validated against the local schema — type, `max_bytes`, known provider — and dropped otherwise.
- Outbound is coalesced per key at a configurable rate (default 4 Hz, latest value only). A plugin writing at 20 Hz must not become 20 Hz of traffic.
- Unknown providers are never relayed, in either direction.

**Open policy question:** sharing controller data out-of-band between clients touches VATSIM network policy. That needs a conversation with VATSIM and the relevant vACC before the relay ships. It is a governance question, not a technical one, and it does not block §1–10.

## 12. Diagnostics

Third-party adoption lives or dies on debuggability. The bridge registers dot commands:

| Command | Shows |
| --- | --- |
| `.esb providers` | Registered providers, versions, owning module, contact |
| `.esb schema <provider>` | Declared fields, types, scopes, flags, doc strings |
| `.esb get <key> [callsign]` | Current value, revision, last writer |
| `.esb watch <key>` | Live change log to the message window |
| `.esb net` | Relay state, peers, what is syncing out and in |
| `.esb stats` | Provider, field, aircraft-slot and value counts, plus the TTL in force |
| `.esb dump` | Full registry snapshot to file |

Plus a rotating log file, with each provider's `contact` string attached to its errors so problems route to the right author.

## 13. Sizing

500 aircraft × 20 providers × 10 aircraft-scoped fields, values typically under 256 bytes, gives a worst case in the low tens of MB and a realistic sparse footprint of a few MB. Reads are O(1): fields declared `ESB_F_DENSE` get a flat array indexed by aircraft slot; everything else uses a hash map. The `OnTimer` sweep is O(aircraft) once per second. None of this is a performance problem at EuroScope's scale — the design work is all in identity, ownership and lifetime, not throughput.

## 14. Distribution and installation

The bridge is a runtime dependency shared by plugins from different authors, which makes "where does the DLL come from" a design question rather than a packaging afterthought.

### 14.1 One copy, shipped standalone

`EuroScopeBridge.dll` is released on its own from the project's GitHub Releases page. **Plugins must not bundle it.**

Bundling looks convenient and is a trap. Windows maps only one module of a given base name per process, so if three plugins each ship their own `EuroScopeBridge.dll` in their own folder, exactly one of them ends up loaded — whichever the user listed first — and the other two silently bind to it. A user who installed plugin A a year ago and plugin C yesterday gets last year's bridge, with nothing to indicate anything is wrong. (The double-load mutex in §9.4 catches the remaining case, where a copy has been renamed and genuinely does load twice.)

Because clients negotiate through `ESB_GetApi(version)` and released members never change, **a newer bridge always serves older clients.** That makes "install the latest" unconditionally correct advice, and removes any reason for a second copy to exist.

### 14.2 Version reporting

`ESB_Api_v1` carries two numbers. `abi_version` is the shape of the struct — what you negotiate on. `bridge_build` is which build filled it in — what a plugin gates on if it needs a specific fix, and what a user quotes in a bug report. `.esb providers` prints both.

### 14.3 The installation step is the adoption cost

The user must add the DLL to their plugin list themselves, through *Other Settings → Plug-ins → Load*. There is no way around it: a plugin that `LoadLibrary`s the bridge gets one that never receives `OnTimer`, and therefore never collects garbage, reaps dead modules, or drains the relay (§9.3).

What the design does instead is make the failure loud and self-diagnosing. `ESB_MISSING_MESSAGE` in the client header gives every plugin the same wording and the same link, so a user running three bridge-aware plugins sees one instruction rather than three different ones, and learns it once.

**Revisit if adoption stalls.** If the manual step turns out to be what kills uptake, the fallback worth investigating is a bridge that can bootstrap itself when `LoadLibrary`d, taking main-thread time from the EuroScope window's message loop instead of from `OnTimer`. That is more invasive and more fragile than it sounds, and is not worth building before there is evidence the simple path has failed.

## 15. Build order

| Milestone | Contents |
| --- | --- |
| **M1 — Core** ✅ | Bridge plugin skeleton, `ESB_GetApi`, provider registration with exclusive claim, schema declaration, global + aircraft get/set, revision counters. Enough for two plugins to talk. |
| **M2 — Safety** ✅ | Generational aircraft handles, TTL garbage collection, module-liveness reaping, double-load mutex. The correctness milestone; nothing ships publicly before it. |
| **M3 — Ergonomics** ✅ | Callbacks with re-entrancy handling, dot commands, logging, the `esbridge.h` client shim, a worked two-plugin example. |
| **M4 — Persistence** ✅ | `ESB_F_PERSIST`, debounced flush, schema-checked restore. |
| **M5 — Relay** ⚠ | `bridge.json`, MQTT client on its own thread, SPSC queues, remote view, coalescing, ACLs. Gated on the §11.6 policy question. |

## 16. What to revisit

- **Exclusive ownership** is right for v1 because it converts silent conflicts into startup errors. If two legitimate plugins ever need to write the same field, revisit as priority-layered writes rather than relaxing to last-write-wins.
- **The 1 s `OnTimer` tick** bounds remote latency. If that proves too slow, pump the inbound queue from a radar screen refresh as well — but only if a real use case demands it.
- **`ESB_T_BLOB`** will get used as an escape hatch for structured data, defeating the schema. Watch for it; if it becomes common, that is the signal for a nested/record type in v2.
- **Schema registration is the adoption cost.** If third-party authors push back on declaring fields up front, the answer is better tooling (a schema-from-header generator), not dropping D3.
