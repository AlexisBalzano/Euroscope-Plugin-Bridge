#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "esbridge.h"
#include "AircraftTable.h"
#include "Config.h"

namespace esb {

class Relay;

// One stored value. Scalars live in the POD members; STR/BLOB in `bytes`.
struct Slot
{
    uint32_t             type     = 0;
    uint64_t             revision = 0;
    bool                 assigned = false;
    int64_t              i64      = 0;
    double               f64      = 0.0;
    std::vector<uint8_t> bytes;
};

struct FieldDef
{
    std::string name;        // "tobt"
    std::string qualified;   // "lfpg_cdm/tobt"
    uint32_t    type     = 0;
    uint32_t    scope    = 0;
    uint32_t    flags    = 0;
    uint32_t    maxBytes = 0;
    std::string doc;
    uint32_t    provider = 0;   // index into m_providers
};

// The opaque ESB_Provider* handed to plugins points at one of these. It carries
// a magic word so a garbage pointer from a buggy plugin is rejected rather than
// dereferenced.
struct ProviderDef
{
    static constexpr uint32_t kMagic = 0x45534270;   // 'ESBp'

    uint32_t    magic = kMagic;
    std::string id;
    uint32_t    major = 0;
    uint32_t    minor = 0;
    std::string displayName;
    std::string contact;
    HMODULE     module = nullptr;
    std::string modulePath;              // recorded at claim time, for liveness
    std::vector<uint32_t> fields;        // indices into m_fields
    uint64_t    revision = 0;            // coarse gate for consumers
    bool        live     = false;
};

// The opaque ESB_Sub* handed to plugins points at one of these.
struct SubscriptionDef
{
    static constexpr uint32_t kMagic = 0x45534273;   // 'ESBs'

    uint32_t     magic  = kMagic;
    uint32_t     field  = 0;             // field INDEX (not the 1-based id)
    ESB_OnChange callback = nullptr;
    void*        user   = nullptr;
    HMODULE      module = nullptr;
    std::string  modulePath;             // recorded at subscribe, for liveness
    bool         live   = false;
    uint64_t     delivered = 0;
};

// The registry itself. Single-threaded by contract: every entry point must be
// called on the EuroScope main thread (ARCHITECTURE.md 8).
class Registry
{
public:
    // ARCHITECTURE.md 6. An aircraft EuroScope stops showing us is retired
    // after the TTL, and its slot is reclaimed a grace period later.
    static constexpr uint64_t kDefaultTtlMs   = 15ull * 60 * 1000;
    static constexpr uint64_t kDefaultGraceMs =  5ull * 60 * 1000;

    // Housekeeping cadence. OnTimer fires once a second; neither of these
    // needs to run that often, and both touch every entry they scan.
    static constexpr uint64_t kModuleCheckMs  =  5ull * 1000;
    static constexpr uint64_t kAircraftSweepMs = 30ull * 1000;

    // How many times the pending queue may be drained before the bridge
    // decides two providers are notifying each other in a loop, sheds the
    // rest and logs the chain (ARCHITECTURE.md 7.1).
    static constexpr uint32_t kMaxDispatchRounds = 8;

    // Persisted state is debounced: a plugin may legitimately write a
    // PERSIST field every tick, and that must not become a disk write.
    static constexpr uint64_t kStateFlushMs = 5ull * 1000;

    Registry();

    // -- registration -----------------------------------------------------
    ESB_Status RegisterProvider(const ESB_ProviderDecl* decl, ESB_Provider** out);
    ESB_Status UnregisterProvider(ESB_Provider* p);
    ESB_Status OwnField(ESB_Provider* p, const char* field, ESB_FieldId* out);

    // -- discovery --------------------------------------------------------
    ESB_Status Resolve(const char* qualified, ESB_Type expect, ESB_FieldId* out) const;
    ESB_Status ProviderVersion(const char* id, uint32_t* major, uint32_t* minor) const;
    ESB_Status ListProviders(char* buf, uint32_t* ioBytes) const;

    // -- global scope -----------------------------------------------------
    ESB_Status GetGlobal(ESB_FieldId f, ESB_Value* out, void* buf, uint32_t* ioBytes) const;
    ESB_Status SetGlobal(ESB_Provider* p, ESB_FieldId f, const ESB_Value* v);
    ESB_Status ClearGlobal(ESB_Provider* p, ESB_FieldId f);

    // -- aircraft scope ---------------------------------------------------
    ESB_Status AircraftHandle(const char* callsign, ESB_Aircraft* out) const;
    ESB_Status AircraftCallsign(ESB_Aircraft a, char* buf, uint32_t* ioBytes) const;
    ESB_Status GetAircraft(ESB_Aircraft a, ESB_FieldId f, ESB_Value* out,
                           void* buf, uint32_t* ioBytes) const;
    ESB_Status SetAircraft(ESB_Provider* p, ESB_Aircraft a, ESB_FieldId f, const ESB_Value* v);
    ESB_Status ClearAircraft(ESB_Provider* p, ESB_Aircraft a, ESB_FieldId f);

    // -- change feed ------------------------------------------------------
    uint64_t RevisionOf(ESB_FieldId f, ESB_Aircraft a) const;
    uint64_t ProviderRevision(const char* id) const;

    ESB_Status Subscribe(ESB_FieldId f, ESB_OnChange cb, void* user, void* module,
                         ESB_Sub** out);
    ESB_Status Unsubscribe(ESB_Sub* s);

    // -- lifecycle, driven by the EuroScope layer -------------------------
    ESB_Aircraft ObserveAircraft(const char* callsign, uint64_t nowMs);
    void         RetireAircraft(const char* callsign);

    // Called once per OnTimer. Throttles its own expensive parts internally,
    // so the EuroScope layer does not have to know the cadences.
    void Housekeeping(uint64_t nowMs);

    // Forced variants, for tests and for the dot commands.
    void ReapDeadModules();
    void SweepAircraft(uint64_t nowMs);

    // -- persistence (ARCHITECTURE.md 10) ---------------------------------
    // Global scope only: per-aircraft state is meaningless across sessions.
    void SetStatePath(const std::string& path) { m_statePath = path; }
    const std::string& StatePath() const { return m_statePath; }

    bool LoadState();                    // reads the file into a staging area
    bool SaveState();                    // unconditional; ignores the debounce

    // Flipped by ~BridgePlugin once the EuroScope layer has gone. The registry
    // itself stays alive and readable, because consumers unwind after the bridge
    // does and their unregister_provider and unsubscribe calls have to keep
    // working. What it stops accepting is new state: SaveState has already run by
    // then, so a late write would be dropped on the floor rather than persisted,
    // and a late subscribe would arm a callback into a module about to unload.
    void Shutdown() { m_shuttingDown = true; }
    bool ShuttingDown() const { return m_shuttingDown; }
    void FlushStateIfDue(uint64_t nowMs);
    bool StateDirty() const { return m_stateDirty; }

    void SetAircraftTtl(uint64_t ttlMs, uint64_t graceMs)
    {
        m_ttlMs   = ttlMs;
        m_graceMs = graceMs;
    }

    // -- relay (ARCHITECTURE.md 11) ---------------------------------------
    // The registry never talks to the network. It hands outbound values to a
    // Relay and receives inbound ones back, both on the main thread.
    void SetRelay(Relay* relay, const RelayConfig* config);
    void DrainRelay(uint64_t nowMs);

    // Inbound values land in a read-only view, never merged with local state
    // (ARCHITECTURE.md 11.5). Returns false if the message was rejected --
    // unknown provider, wrong type, oversized, or not syncable.
    bool ApplyRemote(const std::string& peer, const std::string& qualified,
                     const std::string& callsign, uint32_t type,
                     const std::string& payload, uint64_t peerRevision,
                     uint64_t nowMs);

    ESB_Status RemotePublishers(ESB_FieldId f, ESB_Aircraft a,
                                ESB_Origin* out, uint32_t* ioCount) const;
    ESB_Status GetRemote(ESB_FieldId f, ESB_Aircraft a, const char* peer,
                         ESB_Value* out, void* buf, uint32_t* ioBytes) const;
    ESB_Status ListPeers(ESB_Peer* out, uint32_t* ioCount) const;

    std::string DumpNet() const;

    // -- diagnostics ------------------------------------------------------
    size_t AircraftLiveCount() const { return m_ac.LiveCount(); }
    size_t AircraftSlotCount() const { return m_ac.SlotCount(); }
    size_t AircraftFreeCount() const { return m_ac.FreeCount(); }

    std::string DumpProviders() const;
    std::string DumpSchema(const char* providerId) const;
    std::string DumpStats() const;
    std::string DumpSubscriptions() const;
    std::string DumpValue(const char* qualified, const char* callsign) const;
    std::string DumpAll() const;

    // "lfpg_cdm/tobt" for a field id, or "" if it does not resolve.
    std::string QualifiedName(ESB_FieldId f) const;

    bool OnMainThread() const { return GetCurrentThreadId() == m_threadId; }

private:
    const FieldDef*    Field(ESB_FieldId f) const;
    ProviderDef*       Owner(ESB_Provider* p);
    ESB_Status         CheckWrite(ESB_Provider* p, ESB_FieldId f, uint32_t scope,
                                  const ESB_Value* v, ProviderDef** outOwner) const;
    void               Store(Slot& s, const ESB_Value* v);

    // Drops every per-aircraft value held for `slot`, using the per-slot index
    // rather than scanning the whole value map. Disconnects are frequent and a
    // TTL sweep can retire many aircraft at once.
    void               DropAircraftSlot(uint32_t slot);
    void               DropAircraftSlots(const std::vector<uint32_t>& slots);

    // A dead provider's values must go with it. Otherwise a plugin that is
    // removed and re-added sees its own stale values from a previous life,
    // because field ids are deliberately kept stable across a reload.
    void               DropProviderValues(uint32_t providerIndex);

    // -- notification (ARCHITECTURE.md 7.1) --------------------------------
    // Notify() is called after a write that actually changed something. At
    // depth 0 it dispatches immediately, then drains anything the subscribers
    // queued. Called from inside a dispatch it only queues, so a subscriber
    // that writes cannot recurse into the stack.
    void               Notify(uint32_t fieldIndex, ESB_Aircraft ac);
    void               Dispatch(uint32_t fieldIndex, ESB_Aircraft ac);
    void               DrainPending();
    void               QueuePending(uint32_t fieldIndex, ESB_Aircraft ac);

    struct Pending
    {
        uint32_t     field;
        ESB_Aircraft aircraft;
    };

    static uint64_t    AircraftKey(uint32_t fieldIndex, uint32_t slot)
    {
        return (static_cast<uint64_t>(fieldIndex) << 32) | slot;
    }

    DWORD    m_threadId;
    uint64_t m_clock = 0;            // monotonic; every real change takes one

    uint64_t m_ttlMs   = kDefaultTtlMs;
    uint64_t m_graceMs = kDefaultGraceMs;
    uint64_t m_lastModuleCheckMs   = 0;
    uint64_t m_lastAircraftSweepMs = 0;

    std::vector<std::unique_ptr<ProviderDef>> m_providers;
    std::vector<FieldDef>                     m_fields;

    std::unordered_map<std::string, uint32_t> m_providerIndex;   // id -> index
    std::unordered_map<std::string, uint32_t> m_fieldIndex;      // qualified -> index

    std::unordered_map<uint32_t, Slot> m_globals;    // fieldIndex -> value
    std::unordered_map<uint64_t, Slot> m_aircraft;   // (fieldIndex,slot) -> value

    // slot -> field indices that currently hold a value for it.
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_slotFields;

    // -- subscriptions ----------------------------------------------------
    std::vector<std::unique_ptr<SubscriptionDef>>       m_subs;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_subsByField;

    uint32_t             m_dispatchDepth = 0;   // 0 = not inside a callback
    std::vector<Pending> m_pending;
    uint64_t             m_dispatched = 0;
    uint64_t             m_dropped    = 0;      // shed by the depth cap

    // -- persistence ------------------------------------------------------
    // Values read from disk sit here until their provider registers and its
    // schema is checked. A provider that never loads never gets its state
    // back, and a provider whose field changed type discards it.
    struct StagedValue
    {
        uint32_t             type = 0;
        int64_t              i64  = 0;
        double               f64  = 0.0;
        std::vector<uint8_t> bytes;
    };

    std::string m_statePath;
    std::map<std::string, StagedValue> m_staged;   // qualified name -> value
    bool     m_stateDirty      = false;
    uint64_t m_lastStateFlushMs = 0;
    bool     m_shuttingDown    = false;

    void MarkStateDirty(const FieldDef& f);
    void RestoreStagedFor(uint32_t providerIndex);

    // -- relay ------------------------------------------------------------
    // Global scope uses this sentinel slot so one key shape covers both.
    static constexpr uint32_t kGlobalSlot = 0xFFFFFFFFu;

    struct RemoteEntry
    {
        std::string peer;
        Slot        value;
        uint64_t    peerRevision = 0;
        uint64_t    receivedMs   = 0;
    };

    struct PeerDef
    {
        std::string callsign;
        uint64_t    lastSeenMs = 0;
    };

    // A remote value that arrived for a callsign EuroScope has not shown us
    // yet -- a TOBT can beat the flight plan here (ARCHITECTURE.md 11.4).
    struct PendingRemote
    {
        std::string peer;
        std::string qualified;
        std::string callsign;
        uint32_t    type = 0;
        std::string payload;
        uint64_t    peerRevision = 0;
        uint64_t    arrivedMs    = 0;
    };

    static constexpr uint64_t kPendingRemoteTtlMs = 60ull * 1000;
    static constexpr uint64_t kOutboxFlushMs      = 250;

    // (field, slot) -> one entry per publishing peer.
    std::map<uint64_t, std::vector<RemoteEntry>> m_remote;
    std::vector<PeerDef>       m_peers;
    std::vector<PendingRemote> m_pendingRemote;

    // Coalescing: latest value per key, flushed to the relay on a timer. A
    // plugin writing at 20 Hz must not become 20 Hz of traffic.
    std::map<uint64_t, uint64_t> m_outbox;         // key -> revision to send
    uint64_t                     m_lastOutboxMs = 0;
    std::map<uint32_t, uint32_t> m_publishedKeys; // provider -> live key count

    Relay*             m_relay       = nullptr;
    const RelayConfig* m_relayConfig = nullptr;
    uint64_t           m_relayRejected = 0;

    bool SyncEligible(const FieldDef& f) const;
    void QueueOutbound(uint32_t fieldIndex, ESB_Aircraft a);
    void FlushOutbox(uint64_t nowMs);
    void DropRemoteSlot(uint32_t slot);
    void TouchPeer(const std::string& callsign, uint64_t nowMs);

    AircraftTable m_ac;
};

// Process-wide instance. Created on first use, which may be either a client
// plugin calling ESB_GetApi or our own EuroScope layer starting up.
Registry& TheRegistry();

} // namespace esb
