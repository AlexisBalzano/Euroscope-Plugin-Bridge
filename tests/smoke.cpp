// M1 smoke test.
//
// The bridge DLL imports from EuroScopePlugInDll.dll, so it cannot be
// LoadLibrary'd outside EuroScope (you get ERROR_MOD_NOT_FOUND). Instead this
// links the three core translation units directly and calls ESB_GetApi the
// same way a client would. That works precisely because Registry, AircraftTable
// and ApiExports have no EuroScope dependency at all -- if this test ever stops
// linking, that separation has been broken, which is a finding in itself.
//
// The one thing this cannot cover is GetProcAddress resolution of the export.
// That is verified separately:  dumpbin /exports EuroScopeBridge.dll
// must show `ESB_GetApi` undecorated.
//
//   tests\build-and-run.cmd
//
// Built x86 to match the DLL's target.

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

#include "esbridge.h"
#include "MqttCodec.h"
#include "Registry.h"
#include "Relay.h"
#include "Transport.h"

// Defined in ApiExports.cpp, linked in directly here.
extern "C" __declspec(dllexport) const ESB_Api_v1* __cdecl ESB_GetApi(uint32_t abi_version);

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

static void CheckStatus(ESB_Status got, ESB_Status want, const char* what)
{
    if (got == want)
        std::printf("  [pass] %s\n", what);
    else
    {
        std::printf("  [FAIL] %s (wanted %d, got %d)\n", what, want, got);
        ++g_failures;
    }
}

// --------------------------------------------------------------- callbacks
// File-scope because ESB_OnChange is a plain C function pointer -- which is
// the point: no C++ type crosses the boundary.

static const ESB_Api_v1* g_api          = nullptr;
static ESB_Provider*     g_prov         = nullptr;
static ESB_FieldId       g_counterField = ESB_FIELD_NONE;
static ESB_FieldId       g_ratioField   = ESB_FIELD_NONE;
static ESB_FieldId       g_pingField    = ESB_FIELD_NONE;
static ESB_FieldId       g_pongField    = ESB_FIELD_NONE;

static int          g_hits        = 0;
static ESB_FieldId  g_lastField   = ESB_FIELD_NONE;
static ESB_Aircraft g_lastAircraft = ESB_AIRCRAFT_NONE;
static ESB_Status   g_writeStatus = 0;
static int          g_cycleHits   = 0;

static void __cdecl CountingCallback(ESB_FieldId field, ESB_Aircraft ac, void*)
{
    ++g_hits;
    g_lastField    = field;
    g_lastAircraft = ac;
}

// Writes from inside a notification. Legal, and its own notification must be
// deferred to the end of the outermost dispatch rather than recursing.
static void __cdecl WritingCallback(ESB_FieldId, ESB_Aircraft, void*)
{
    ESB_Value v;
    v.v.i64 = 0;
    v.type = ESB_T_F64; v.bytes = (uint32_t)sizeof(double); v.v.f64 = 2.5;
    g_writeStatus = g_api->set_global(g_prov, g_ratioField, &v);
}

// A deliberate loop: each side writes the other's field with a changing value,
// so compare-before-write never damps it. Only the depth cap stops this.
static void __cdecl PingCallback(ESB_FieldId, ESB_Aircraft, void*)
{
    ++g_cycleHits;
    ESB_Value v;
    v.v.i64 = 0;
    v.type = ESB_T_F64; v.bytes = (uint32_t)sizeof(double);
    v.v.f64 = (double)g_cycleHits + 0.5;
    g_api->set_global(g_prov, g_pongField, &v);
}

static void __cdecl PongCallback(ESB_FieldId, ESB_Aircraft, void*)
{
    ++g_cycleHits;
    ESB_Value v;
    v.v.i64 = 0;
    v.type = ESB_T_I64; v.bytes = (uint32_t)sizeof(int64_t);
    v.v.i64 = 1000 + g_cycleHits;
    g_api->set_global(g_prov, g_pingField, &v);
}

// ------------------------------------------------------------- fake broker
// Enough of a broker to exercise the real client: it answers CONNECT with
// CONNACK, SUBSCRIBE with SUBACK, PINGREQ with PINGRESP, records what was
// published, and lets the test inject a peer's PUBLISH. Protocol knowledge
// lives here rather than in the shipping DLL.
class FakeBroker : public esb::Transport
{
public:
    bool Connect(const std::string&, uint16_t, bool, std::string*) override
    {
        m_connected = true;
        return true;
    }

    int Send(const uint8_t* data, size_t size) override
    {
        if (!m_connected)
            return -1;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.insert(m_pending.end(), data, data + size);

        for (;;)
        {
            esb::mqtt::Packet p;
            size_t consumed = 0;
            const esb::mqtt::DecodeResult r =
                esb::mqtt::Decode(m_pending.data(), m_pending.size(), &p, &consumed);
            if (r != esb::mqtt::DecodeResult::Ok)
                break;

            const uint8_t type = static_cast<uint8_t>(m_pending[0] >> 4);
            m_pending.erase(m_pending.begin(), m_pending.begin() + (ptrdiff_t)consumed);

            switch (type)
            {
            case esb::mqtt::kConnect:
            {
                // CONNACK: session-present 0, return code 0.
                const uint8_t connack[] = { 0x20, 0x02, 0x00, 0x00 };
                m_out.insert(m_out.end(), connack, connack + sizeof connack);
                break;
            }
            case esb::mqtt::kSubscribe:
            {
                const uint8_t suback[] = { 0x90, 0x03, 0x00, 0x01, 0x00 };
                m_out.insert(m_out.end(), suback, suback + sizeof suback);
                break;
            }
            case esb::mqtt::kPingReq:
            {
                const uint8_t pingresp[] = { 0xD0, 0x00 };
                m_out.insert(m_out.end(), pingresp, pingresp + sizeof pingresp);
                break;
            }
            case esb::mqtt::kPublish:
                m_publishedTopics += p.topic + " ";
                break;
            default:
                break;
            }
        }

        return static_cast<int>(size);
    }

    int Recv(uint8_t* buffer, size_t size) override
    {
        if (!m_connected)
            return -1;
        std::lock_guard<std::mutex> lock(m_mutex);
        const size_t n = size < m_out.size() ? size : m_out.size();
        if (n)
        {
            std::memcpy(buffer, m_out.data(), n);
            m_out.erase(m_out.begin(), m_out.begin() + (ptrdiff_t)n);
        }
        return static_cast<int>(n);
    }

    void Close() override { m_connected = false; }
    bool Connected() const override { return m_connected; }

    // --- test side ---
    void Deliver(const std::vector<uint8_t>& packet)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_out.insert(m_out.end(), packet.begin(), packet.end());
    }

    std::string TakePublishedTopics()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string out;
        out.swap(m_publishedTopics);
        return out;
    }

private:
    std::mutex           m_mutex;
    std::vector<uint8_t> m_pending;   // client -> broker, awaiting a full packet
    std::vector<uint8_t> m_out;       // broker -> client
    std::string          m_publishedTopics;
    bool                 m_connected = false;
};

int main()
{
    const ESB_Api_v1* api = ESB_GetApi(ESB_ABI_VERSION);
    if (!api)
    {
        std::printf("bridge refused ABI v%u\n", ESB_ABI_VERSION);
        return 2;
    }

    std::printf("abi v%u, bridge build %u, vtable %u bytes\n\n",
                api->abi_version, api->bridge_build, api->struct_size);

    std::printf("version negotiation\n");
    Check(ESB_GetApi(9999) == nullptr, "unknown ABI version is refused");
    Check(api->struct_size == sizeof(ESB_Api_v1), "vtable size matches the header");

    // ---------------------------------------------------------- registration
    std::printf("\nregistration and exclusive claim\n");

    static const ESB_FieldDecl kFields[] = {
        { "counter", ESB_T_I64, ESB_SCOPE_GLOBAL,   0, 0,  "a number"      },
        { "label",   ESB_T_STR, ESB_SCOPE_GLOBAL,   0, 16, "a short label" },
        { "ratio",   ESB_T_F64, ESB_SCOPE_GLOBAL,   0, 0,  "a double"      },
        { "tobt",    ESB_T_I64, ESB_SCOPE_AIRCRAFT, 0, 0,  "per aircraft"  },
    };

    HMODULE self  = GetModuleHandleA(nullptr);          // this exe
    HMODULE other = GetModuleHandleA("kernel32.dll");   // a different module

    ESB_ProviderDecl decl = { 0 };
    decl.struct_size  = sizeof decl;
    decl.provider_id  = "smoke";
    decl.schema_major = 1;
    decl.schema_minor = 0;
    decl.display_name = "Smoke Test";
    decl.contact      = "https://example.invalid";
    decl.fields       = kFields;
    decl.field_count  = 4;
    decl.module       = self;

    ESB_Provider* prov = nullptr;
    CheckStatus(api->register_provider(&decl, &prov), ESB_OK, "provider registers");
    Check(prov != nullptr, "provider handle returned");

    ESB_ProviderDecl rival = decl;
    rival.module       = other;
    rival.schema_minor = 7;
    ESB_Provider* dup = nullptr;
    CheckStatus(api->register_provider(&rival, &dup), ESB_E_PROVIDER_TAKEN,
                "a different module cannot claim a taken id");

    ESB_Provider* again = nullptr;
    CheckStatus(api->register_provider(&decl, &again), ESB_OK,
                "same module re-registering is idempotent");
    Check(again == prov, "idempotent re-registration returns the same handle");

    ESB_ProviderDecl bad = decl;
    bad.provider_id = "Smoke Test";                     // spaces and capitals
    ESB_Provider* rejected = nullptr;
    CheckStatus(api->register_provider(&bad, &rejected), ESB_E_INVALID_ARG,
                "malformed provider id is rejected");

    ESB_ProviderDecl uncapped = decl;
    static const ESB_FieldDecl kNoCap[] = {
        { "text", ESB_T_STR, ESB_SCOPE_GLOBAL, 0, 0, "no max_bytes declared" },
    };
    uncapped.provider_id = "nocap";
    uncapped.fields      = kNoCap;
    uncapped.field_count = 1;
    CheckStatus(api->register_provider(&uncapped, &rejected), ESB_E_INVALID_ARG,
                "a STR field with no max_bytes is rejected");

    // ------------------------------------------------------------- discovery
    std::printf("\ndiscovery\n");

    ESB_FieldId counter = ESB_FIELD_NONE, bogus = ESB_FIELD_NONE;
    CheckStatus(api->resolve("smoke/counter", ESB_T_I64, &counter), ESB_OK,
                "resolve a declared field");
    Check(counter != ESB_FIELD_NONE, "field id is not NONE");
    CheckStatus(api->resolve("smoke/counter", ESB_T_STR, &bogus), ESB_E_TYPE_MISMATCH,
                "resolving with the wrong type fails loudly");
    CheckStatus(api->resolve("nobody/here", ESB_T_I64, &bogus), ESB_E_NO_PROVIDER,
                "absent provider reports NO_PROVIDER");
    CheckStatus(api->resolve("smoke/nosuch", ESB_T_I64, &bogus), ESB_E_NO_FIELD,
                "undeclared field reports NO_FIELD");

    uint32_t major = 0, minor = 0;
    CheckStatus(api->provider_version("smoke", &major, &minor), ESB_OK,
                "provider version readable");
    Check(major == 1 && minor == 0, "provider version is 1.0");

    // ----------------------------------------------------------- global scope
    std::printf("\nglobal get and set\n");

    ESB_Value v = { 0 };
    v.type = ESB_T_I64; v.bytes = sizeof(int64_t); v.v.i64 = 42;
    CheckStatus(api->set_global(prov, counter, &v), ESB_OK, "write an i64");

    ESB_Value got = { 0 };
    CheckStatus(api->get_global(counter, &got, nullptr, nullptr), ESB_OK,
                "read back with no buffer (scalar)");
    Check(got.type == ESB_T_I64 && got.v.i64 == 42, "value round-trips");

    ESB_FieldId ratio = ESB_FIELD_NONE;
    api->resolve("smoke/ratio", ESB_T_F64, &ratio);
    ESB_Value unset = { 0 };
    CheckStatus(api->get_global(ratio, &unset, nullptr, nullptr), ESB_E_UNSET,
                "declared but unwritten field reports UNSET");

    ESB_Value wrong = { 0 };
    wrong.type = ESB_T_F64; wrong.bytes = sizeof(double); wrong.v.f64 = 1.0;
    CheckStatus(api->set_global(prov, counter, &wrong), ESB_E_TYPE_MISMATCH,
                "writing the wrong type is rejected");

    // -------------------------------------------- revisions and comparison
    std::printf("\nrevision counters\n");

    const uint64_t rev1 = api->revision(counter, ESB_AIRCRAFT_NONE);
    Check(rev1 != 0, "a written field has a non-zero revision");

    CheckStatus(api->set_global(prov, counter, &v), ESB_OK, "rewrite the same value");
    Check(api->revision(counter, ESB_AIRCRAFT_NONE) == rev1,
          "identical write does NOT bump the revision");

    v.v.i64 = 43;
    api->set_global(prov, counter, &v);
    const uint64_t rev2 = api->revision(counter, ESB_AIRCRAFT_NONE);
    Check(rev2 > rev1, "a real change does bump the revision");
    Check(api->revision(ratio, ESB_AIRCRAFT_NONE) == 0, "unset field reads revision 0");
    Check(api->provider_revision("smoke") >= rev2, "provider revision tracks its fields");
    Check(api->provider_revision("nobody") == 0, "absent provider reads revision 0");

    // A repeated NaN must compare equal, or every tick would look like a change.
    ESB_Value nan1 = { 0 };
    nan1.type = ESB_T_F64; nan1.bytes = sizeof(double);
    *reinterpret_cast<uint64_t*>(&nan1.v.f64) = 0x7FF8000000000001ull;
    api->set_global(prov, ratio, &nan1);
    const uint64_t nanRev = api->revision(ratio, ESB_AIRCRAFT_NONE);
    api->set_global(prov, ratio, &nan1);
    Check(api->revision(ratio, ESB_AIRCRAFT_NONE) == nanRev,
          "rewriting an identical NaN does not bump the revision");

    // ------------------------------------------------- string buffer protocol
    std::printf("\nstring buffer protocol\n");

    ESB_FieldId label = ESB_FIELD_NONE;
    api->resolve("smoke/label", ESB_T_STR, &label);

    const char* text = "GND";
    ESB_Value sv = { 0 };
    sv.type = ESB_T_STR; sv.bytes = 3; sv.v.ptr = text;
    CheckStatus(api->set_global(prov, label, &sv), ESB_OK, "write a string");

    ESB_Value sgot = { 0 };
    uint32_t need = 0;
    CheckStatus(api->get_global(label, &sgot, nullptr, &need), ESB_E_BUFFER_TOO_SMALL,
                "null buffer reports BUFFER_TOO_SMALL");
    Check(need == 3, "required size is reported back");

    char buf[8] = { 0 };
    uint32_t cap = sizeof buf;
    CheckStatus(api->get_global(label, &sgot, buf, &cap), ESB_OK, "read with a buffer");
    Check(sgot.bytes == 3 && std::memcmp(buf, "GND", 3) == 0, "string round-trips");
    Check(sgot.v.ptr == buf, "value points at the caller's own buffer");

    char tiny[2] = { 0 };
    uint32_t tinyCap = sizeof tiny;
    CheckStatus(api->get_global(label, &sgot, tiny, &tinyCap), ESB_E_BUFFER_TOO_SMALL,
                "undersized buffer is refused");
    Check(tinyCap == 3, "and reports the size actually needed");

    const char* tooLong = "THIS IS DEFINITELY LONGER THAN SIXTEEN";
    ESB_Value big = { 0 };
    big.type = ESB_T_STR; big.bytes = (uint32_t)std::strlen(tooLong); big.v.ptr = tooLong;
    CheckStatus(api->set_global(prov, label, &big), ESB_E_LIMIT,
                "a write past the declared max_bytes is refused");

    // ------------------------------------------------------------- ownership
    std::printf("\nownership\n");

    static const ESB_FieldDecl kOther[] = {
        { "thing", ESB_T_I64, ESB_SCOPE_GLOBAL, 0, 0, "another provider's field" },
    };
    ESB_ProviderDecl second = { 0 };
    second.struct_size  = sizeof second;
    second.provider_id  = "smoke2";
    second.schema_major = 1;
    second.fields       = kOther;
    second.field_count  = 1;
    second.module       = self;

    ESB_Provider* prov2 = nullptr;
    CheckStatus(api->register_provider(&second, &prov2), ESB_OK, "second provider registers");
    CheckStatus(api->set_global(prov2, counter, &v), ESB_E_NOT_OWNER,
                "a provider cannot write another's field");

    ESB_FieldId mine = ESB_FIELD_NONE;
    CheckStatus(api->own_field(prov2, "thing", &mine), ESB_OK, "own_field finds own field");
    CheckStatus(api->own_field(prov2, "counter", &mine), ESB_E_NO_FIELD,
                "own_field refuses another provider's field");

    // --------------------------------------------------------- aircraft scope
    // Driven through Registry directly, standing in for what BridgePlugin's
    // OnTimer does from EuroScope's flight-plan and radar-target lists.
    std::printf("\naircraft scope\n");

    esb::Registry& reg = esb::TheRegistry();
    ESB_FieldId tobt = ESB_FIELD_NONE;
    api->resolve("smoke/tobt", ESB_T_I64, &tobt);

    ESB_Aircraft none = ESB_AIRCRAFT_NONE;
    CheckStatus(api->aircraft("AFR1234", &none), ESB_E_UNKNOWN_AIRCRAFT,
                "callsign EuroScope has not shown us is unknown");

    reg.ObserveAircraft("AFR1234", 1000);

    ESB_Aircraft ac = ESB_AIRCRAFT_NONE;
    CheckStatus(api->aircraft("AFR1234", &ac), ESB_OK, "observed callsign resolves");
    Check(ac != ESB_AIRCRAFT_NONE, "handle is not NONE");

    ESB_Value tv = { 0 };
    tv.type = ESB_T_I64; tv.bytes = sizeof(int64_t); tv.v.i64 = 742;
    CheckStatus(api->set_ac(prov, ac, tobt, &tv), ESB_OK, "write a per-aircraft value");

    ESB_Value tgot = { 0 };
    CheckStatus(api->get_ac(ac, tobt, &tgot, nullptr, nullptr), ESB_OK, "read it back");
    Check(tgot.v.i64 == 742, "per-aircraft value round-trips");

    const uint64_t acRev = api->revision(tobt, ac);
    api->set_ac(prov, ac, tobt, &tv);
    Check(api->revision(tobt, ac) == acRev, "identical per-aircraft write is a no-op");

    CheckStatus(api->set_ac(prov, ac, counter, &v), ESB_E_INVALID_ARG,
                "a global field cannot be written in aircraft scope");
    CheckStatus(api->get_ac(ac, counter, &tgot, nullptr, nullptr), ESB_E_INVALID_ARG,
                "nor read in aircraft scope");

    char csBuf[16] = { 0 };
    uint32_t csCap = sizeof csBuf;
    CheckStatus(api->aircraft_callsign(ac, csBuf, &csCap), ESB_OK, "callsign reads back");
    Check(std::strcmp(csBuf, "AFR1234") == 0, "and matches");

    // ---- the property the whole generational-handle design exists for ----
    std::printf("\ncallsign recycling (ARCHITECTURE.md 6)\n");

    reg.RetireAircraft("AFR1234");

    CheckStatus(api->get_ac(ac, tobt, &tgot, nullptr, nullptr), ESB_E_STALE_AIRCRAFT,
                "a handle held across a disconnect goes stale");
    ESB_Aircraft gone = ESB_AIRCRAFT_NONE;
    CheckStatus(api->aircraft("AFR1234", &gone), ESB_E_UNKNOWN_AIRCRAFT,
                "the callsign stops resolving");

    // A different pilot connects under the same callsign.
    reg.ObserveAircraft("AFR1234", 2000);

    ESB_Aircraft ac2 = ESB_AIRCRAFT_NONE;
    CheckStatus(api->aircraft("AFR1234", &ac2), ESB_OK, "the callsign resolves again");
    Check(ac2 != ac, "but to a DIFFERENT handle (generation bumped)");
    CheckStatus(api->get_ac(ac2, tobt, &tgot, nullptr, nullptr), ESB_E_UNSET,
                "the new aircraft does NOT inherit the old one's value");
    CheckStatus(api->get_ac(ac, tobt, &tgot, nullptr, nullptr), ESB_E_STALE_AIRCRAFT,
                "and the old handle stays stale, it never silently retargets");

    CheckStatus(api->set_ac(prov, 0xDEADBEEFull, tobt, &tv), ESB_E_UNKNOWN_AIRCRAFT,
                "a fabricated handle is rejected");

    // ------------------------------------------------ M2: TTL garbage collection
    std::printf("\nTTL garbage collection (ARCHITECTURE.md 6)\n");

    // Short horizons so the sweep is testable: retire after 10s unseen,
    // reclaim the slot 5s after that.
    reg.SetAircraftTtl(10 * 1000, 5 * 1000);

    reg.ObserveAircraft("BAW99", 100000);
    ESB_Aircraft baw = ESB_AIRCRAFT_NONE;
    CheckStatus(api->aircraft("BAW99", &baw), ESB_OK, "observe a second aircraft");
    api->set_ac(prov, baw, tobt, &tv);
    CheckStatus(api->get_ac(baw, tobt, &tgot, nullptr, nullptr), ESB_OK, "it holds a value");

    reg.SweepAircraft(105000);   // 5s later: inside the TTL
    CheckStatus(api->get_ac(baw, tobt, &tgot, nullptr, nullptr), ESB_OK,
                "a sweep inside the TTL leaves it alone");

    // EuroScope never fired a disconnect; the aircraft simply stopped appearing.
    reg.SweepAircraft(115000);   // 15s after last seen: past the TTL
    CheckStatus(api->get_ac(baw, tobt, &tgot, nullptr, nullptr), ESB_E_STALE_AIRCRAFT,
                "past the TTL it is retired exactly like a disconnect");
    ESB_Aircraft after = ESB_AIRCRAFT_NONE;
    CheckStatus(api->aircraft("BAW99", &after), ESB_E_UNKNOWN_AIRCRAFT,
                "and stops resolving");

    // Still inside the grace window: a reconnect lands on the same slot.
    reg.ObserveAircraft("BAW99", 116000);
    ESB_Aircraft bawAgain = ESB_AIRCRAFT_NONE;
    CheckStatus(api->aircraft("BAW99", &bawAgain), ESB_OK, "it can come back");
    Check(esb::AircraftTable::SlotOf(bawAgain) == esb::AircraftTable::SlotOf(baw),
          "within the grace window it reuses the same slot");
    Check(bawAgain != baw, "but under a new generation");
    CheckStatus(api->get_ac(bawAgain, tobt, &tgot, nullptr, nullptr), ESB_E_UNSET,
                "and its values did not survive");

    std::printf("\nslot reclamation\n");

    reg.ObserveAircraft("KLM10", 200000);
    ESB_Aircraft klm = ESB_AIRCRAFT_NONE;
    api->aircraft("KLM10", &klm);
    const uint32_t klmSlot = esb::AircraftTable::SlotOf(klm);

    // Past TTL + grace with nothing else in flight: the slot goes on the free
    // list and the next new callsign takes it.
    reg.SweepAircraft(230000);
    reg.SweepAircraft(260000);
    ESB_Aircraft goneKlm = ESB_AIRCRAFT_NONE;
    CheckStatus(api->aircraft("KLM10", &goneKlm), ESB_E_UNKNOWN_AIRCRAFT,
                "past TTL + grace the entry is gone entirely");

    Check(reg.AircraftFreeCount() > 0, "the reclaimed slot lands on the free list");

    // Which free slot gets reused is not a guarantee -- the free list is LIFO
    // and several aircraft aged out together. What must hold is that the table
    // does not grow, and that the reused slot's generation keeps climbing.
    const size_t slotsBefore = reg.AircraftSlotCount();
    reg.ObserveAircraft("DLH20", 260000);
    ESB_Aircraft dlh = ESB_AIRCRAFT_NONE;
    api->aircraft("DLH20", &dlh);

    Check(reg.AircraftSlotCount() == slotsBefore,
          "a new callsign reuses a reclaimed slot instead of growing the table");
    Check(esb::AircraftTable::GenerationOf(dlh) > 1,
          "the reused slot's generation continues from its previous occupant");
    CheckStatus(api->get_ac(klm, tobt, &tgot, nullptr, nullptr), ESB_E_STALE_AIRCRAFT,
                "and a handle to the reclaimed slot stays stale, never retargeted");
    (void)klmSlot;

    reg.SetAircraftTtl(esb::Registry::kDefaultTtlMs, esb::Registry::kDefaultGraceMs);

    // ------------------------------------- M2: a dead provider takes its data
    std::printf("\ndead provider drops its values (ARCHITECTURE.md 9.1)\n");

    static const ESB_FieldDecl kDoomed[] = {
        { "gv", ESB_T_I64, ESB_SCOPE_GLOBAL,   0, 0, "global value"   },
        { "av", ESB_T_I64, ESB_SCOPE_AIRCRAFT, 0, 0, "aircraft value" },
    };
    ESB_ProviderDecl doomed = { 0 };
    doomed.struct_size  = sizeof doomed;
    doomed.provider_id  = "doomed";
    doomed.schema_major = 1;
    doomed.fields       = kDoomed;
    doomed.field_count  = 2;
    doomed.module       = self;

    ESB_Provider* dprov = nullptr;
    CheckStatus(api->register_provider(&doomed, &dprov), ESB_OK, "doomed provider registers");

    ESB_FieldId gv = ESB_FIELD_NONE, av = ESB_FIELD_NONE;
    api->resolve("doomed/gv", ESB_T_I64, &gv);
    api->resolve("doomed/av", ESB_T_I64, &av);

    ESB_Value dv = { 0 };
    dv.type = ESB_T_I64; dv.bytes = sizeof(int64_t); dv.v.i64 = 111;
    api->set_global(dprov, gv, &dv);
    api->set_ac(dprov, dlh, av, &dv);
    CheckStatus(api->get_global(gv, &tgot, nullptr, nullptr), ESB_OK, "it publishes a global");
    CheckStatus(api->get_ac(dlh, av, &tgot, nullptr, nullptr), ESB_OK, "and a per-aircraft value");

    api->unregister_provider(dprov);

    // The plugin comes back -- a reload, or the user re-adding it. Field ids
    // are deliberately stable across that, so stale values would resurface if
    // they were not dropped when the provider died.
    ESB_Provider* reborn = nullptr;
    CheckStatus(api->register_provider(&doomed, &reborn), ESB_OK, "and registers again");
    ESB_FieldId gv2 = ESB_FIELD_NONE;
    CheckStatus(api->resolve("doomed/gv", ESB_T_I64, &gv2), ESB_OK, "its fields resolve again");
    Check(gv2 == gv, "to the same field id (consumers need not re-resolve)");
    CheckStatus(api->get_global(gv2, &tgot, nullptr, nullptr), ESB_E_UNSET,
                "but the global value did NOT survive the death");
    CheckStatus(api->get_ac(dlh, av, &tgot, nullptr, nullptr), ESB_E_UNSET,
                "nor did the per-aircraft one");
    api->unregister_provider(reborn);

    // ------------------------------------------------ M3: the callback feed
    std::printf("\ncallbacks (ARCHITECTURE.md 7)\n");

    g_api = api;
    g_counterField = counter;

    ESB_Sub* sub = nullptr;
    CheckStatus(api->subscribe(counter, &CountingCallback, nullptr, self, &sub), ESB_OK,
                "subscribe to a field");
    Check(sub != nullptr, "subscription handle returned");

    g_hits = 0;
    v.v.i64 = 100;
    api->set_global(prov, counter, &v);
    Check(g_hits == 1, "a write fires the callback synchronously, in the same call");
    Check(g_lastField == counter, "with the field that changed");
    Check(g_lastAircraft == ESB_AIRCRAFT_NONE, "and NONE for a global field");

    // The compare-before-write guarantee has to extend to notifications, or
    // every publisher would need dirty-tracking after all.
    g_hits = 0;
    api->set_global(prov, counter, &v);
    Check(g_hits == 0, "an identical write fires nothing");

    g_hits = 0;
    api->clear_global(prov, counter);
    Check(g_hits == 1, "clearing fires too");

    // A fresh aircraft: everything observed earlier was deliberately aged out
    // by the TTL tests above, so those handles are stale by design.
    reg.ObserveAircraft("EZY77", 300000);
    ESB_Aircraft cbAc = ESB_AIRCRAFT_NONE;
    CheckStatus(api->aircraft("EZY77", &cbAc), ESB_OK, "observe an aircraft to watch");

    g_hits = 0;
    ESB_Value acv2 = { 0 };
    acv2.type = ESB_T_I64; acv2.bytes = sizeof(int64_t); acv2.v.i64 = 5;
    ESB_Sub* acSub = nullptr;
    api->subscribe(tobt, &CountingCallback, nullptr, self, &acSub);
    CheckStatus(api->set_ac(prov, cbAc, tobt, &acv2), ESB_OK, "write a per-aircraft value");
    Check(g_hits == 1, "aircraft-scoped writes fire too");
    Check(g_lastAircraft == cbAc, "carrying the aircraft handle");
    api->unsubscribe(acSub);

    CheckStatus(api->unsubscribe(sub), ESB_OK, "unsubscribe");
    g_hits = 0;
    v.v.i64 = 101;
    api->set_global(prov, counter, &v);
    Check(g_hits == 0, "an unsubscribed callback stops firing");
    CheckStatus(api->unsubscribe(sub), ESB_E_INVALID_ARG, "double unsubscribe is refused");

    std::printf("\nre-entrancy (ARCHITECTURE.md 7.1)\n");

    // A subscriber that writes from inside its own callback. The write must
    // land immediately; only its notification is deferred.
    ESB_Sub* writer = nullptr;
    ESB_Sub* watcher = nullptr;
    g_prov = prov;
    g_ratioField = ratio;
    api->subscribe(counter, &WritingCallback, nullptr, self, &writer);
    api->subscribe(ratio, &CountingCallback, nullptr, self, &watcher);

    g_hits = 0;
    g_writeStatus = 999;
    v.v.i64 = 202;
    api->set_global(prov, counter, &v);

    CheckStatus(g_writeStatus, ESB_OK, "a write from inside a callback succeeds");
    Check(g_hits == 1, "and its own notification is delivered once, after unwinding");

    ESB_Value ratioGot = { 0 };
    CheckStatus(api->get_global(ratio, &ratioGot, nullptr, nullptr), ESB_OK,
                "the value written from inside the callback is readable");
    Check(ratioGot.v.f64 == 2.5, "and correct");

    api->unsubscribe(writer);
    api->unsubscribe(watcher);

    std::printf("\nnotification cycle is shed, not fatal\n");

    // Two fields whose subscribers write to each other: without the depth cap
    // this is an unbounded drain loop inside EuroScope.
    ESB_Sub* ping = nullptr;
    ESB_Sub* pong = nullptr;
    g_pingField = counter;
    g_pongField = ratio;
    api->subscribe(counter, &PingCallback, nullptr, self, &ping);
    api->subscribe(ratio, &PongCallback, nullptr, self, &pong);

    g_cycleHits = 0;
    v.v.i64 = 303;
    api->set_global(prov, counter, &v);

    Check(g_cycleHits > 0, "the cycle runs");
    Check(g_cycleHits < 100, "but is shed after the depth cap rather than looping forever");

    api->unsubscribe(ping);
    api->unsubscribe(pong);

    // ------------------------------------------------- M4: persisted state
    std::printf("\npersistence (ARCHITECTURE.md 10)\n");

    static const ESB_FieldDecl kPersist[] = {
        { "sticky",   ESB_T_I64, ESB_SCOPE_GLOBAL, ESB_F_PERSIST, 0, "survives a restart"  },
        { "note",     ESB_T_STR, ESB_SCOPE_GLOBAL, ESB_F_PERSIST, 32, "a persisted string" },
        { "volatile", ESB_T_I64, ESB_SCOPE_GLOBAL, 0,             0, "not persisted"       },
    };
    ESB_ProviderDecl keeper = { 0 };
    keeper.struct_size  = sizeof keeper;
    keeper.provider_id  = "keeper";
    keeper.schema_major = 1;
    keeper.fields       = kPersist;
    keeper.field_count  = 3;
    keeper.module       = self;

    // A per-aircraft field flagged PERSIST is meaningless and must be refused
    // at declaration rather than silently ignored later.
    static const ESB_FieldDecl kBadPersist[] = {
        { "nope", ESB_T_I64, ESB_SCOPE_AIRCRAFT, ESB_F_PERSIST, 0, "per-aircraft persist" },
    };
    ESB_ProviderDecl badPersist = keeper;
    badPersist.provider_id = "badpersist";
    badPersist.fields      = kBadPersist;
    badPersist.field_count = 1;
    ESB_Provider* refused = nullptr;
    CheckStatus(api->register_provider(&badPersist, &refused), ESB_E_INVALID_ARG,
                "PERSIST on an aircraft-scoped field is refused");

    ESB_Provider* kprov = nullptr;
    CheckStatus(api->register_provider(&keeper, &kprov), ESB_OK, "keeper registers");

    ESB_FieldId sticky = ESB_FIELD_NONE, note = ESB_FIELD_NONE, vol = ESB_FIELD_NONE;
    api->resolve("keeper/sticky",   ESB_T_I64, &sticky);
    api->resolve("keeper/note",     ESB_T_STR, &note);
    api->resolve("keeper/volatile", ESB_T_I64, &vol);

    ESB_Value sv2 = { 0 };
    sv2.type = ESB_T_I64; sv2.bytes = sizeof(int64_t); sv2.v.i64 = 4242;
    api->set_global(kprov, sticky, &sv2);
    const char* noteText = "remember me";
    ESB_Value nv = { 0 };
    nv.type = ESB_T_STR; nv.bytes = (uint32_t)std::strlen(noteText); nv.v.ptr = noteText;
    api->set_global(kprov, note, &nv);
    sv2.v.i64 = 7;
    api->set_global(kprov, vol, &sv2);

    Check(reg.StateDirty(), "writing a PERSIST field marks the state dirty");

    char statePath[MAX_PATH] = { 0 };
    GetTempPathA(MAX_PATH, statePath);
    std::strcat(statePath, "esb-smoke-state.json");
    std::remove(statePath);
    reg.SetStatePath(statePath);

    Check(reg.SaveState(), "state saves");
    Check(!reg.StateDirty(), "and the dirty flag clears");

    // Read the file back as a client would never have to -- but the test does,
    // to prove PERSIST is what actually gates inclusion.
    {
        FILE* f = std::fopen(statePath, "rb");
        Check(f != nullptr, "the state file exists");
        std::string text;
        if (f)
        {
            char rb[1024];
            size_t got;
            while ((got = std::fread(rb, 1, sizeof rb, f)) > 0)
                text.append(rb, got);
            std::fclose(f);
        }
        Check(text.find("keeper/sticky") != std::string::npos,
              "a PERSIST field is written");
        Check(text.find("keeper/note") != std::string::npos,
              "including strings");
        Check(text.find("keeper/volatile") == std::string::npos,
              "a field without PERSIST is NOT written");
        Check(text.find("smoke/counter") == std::string::npos,
              "and neither is anything from a provider that never asked");
    }

    std::printf("\nschema-checked restore\n");

    // Simulate a restart: clear the live value, then load from disk.
    api->clear_global(kprov, sticky);
    api->clear_global(kprov, note);
    CheckStatus(api->get_global(sticky, &tgot, nullptr, nullptr), ESB_E_UNSET,
                "value cleared before the reload");

    Check(reg.LoadState(), "state loads");
    CheckStatus(api->get_global(sticky, &tgot, nullptr, nullptr), ESB_OK,
                "an already-registered provider gets its value back immediately");
    Check(tgot.v.i64 == 4242, "with the right value");

    char noteBuf[64] = { 0 };
    uint32_t noteCap = sizeof noteBuf;
    CheckStatus(api->get_global(note, &tgot, noteBuf, &noteCap), ESB_OK,
                "strings restore too");
    std::printf("       (restored note: bytes=%u \"%.*s\")\n",
                tgot.bytes, (int)tgot.bytes, noteBuf);
    Check(std::memcmp(noteBuf, "remember me", 11) == 0, "with their bytes intact");

    // A provider whose field changed type must not be handed the old bytes.
    static const ESB_FieldDecl kRetyped[] = {
        { "sticky",   ESB_T_STR, ESB_SCOPE_GLOBAL, ESB_F_PERSIST, 16, "now a string" },
        { "note",     ESB_T_STR, ESB_SCOPE_GLOBAL, ESB_F_PERSIST, 32, "unchanged"    },
        { "volatile", ESB_T_I64, ESB_SCOPE_GLOBAL, 0,             0, "unchanged"     },
    };
    api->unregister_provider(kprov);

    ESB_ProviderDecl retyped = keeper;
    retyped.provider_id  = "keeper2";
    retyped.fields       = kRetyped;
    retyped.schema_major = 2;

    // Rewrite the state file so it claims keeper2/sticky was an i64.
    {
        FILE* f = std::fopen(statePath, "wb");
        const char* forged =
            "{\n \"version\": 1,\n \"fields\": {\n"
            "  \"keeper2/sticky\": { \"type\": 1, \"value\": 999 }\n }\n}\n";
        if (f) { std::fwrite(forged, 1, std::strlen(forged), f); std::fclose(f); }
    }
    Check(reg.LoadState(), "a state file for a not-yet-registered provider loads");

    ESB_Provider* rprov = nullptr;
    CheckStatus(api->register_provider(&retyped, &rprov), ESB_OK, "keeper2 registers");
    ESB_FieldId sticky2 = ESB_FIELD_NONE;
    api->resolve("keeper2/sticky", ESB_T_STR, &sticky2);
    CheckStatus(api->get_global(sticky2, &tgot, noteBuf, &noteCap), ESB_E_UNSET,
                "a saved value whose type no longer matches is DISCARDED, not reinterpreted");
    api->unregister_provider(rprov);

    std::printf("\ndebounce\n");

    ESB_Provider* dbprov = nullptr;
    ESB_ProviderDecl deb = keeper;
    deb.provider_id = "debounce";
    api->register_provider(&deb, &dbprov);
    ESB_FieldId dbField = ESB_FIELD_NONE;
    api->resolve("debounce/sticky", ESB_T_I64, &dbField);

    std::remove(statePath);
    sv2.v.i64 = 1;
    api->set_global(dbprov, dbField, &sv2);
    reg.FlushStateIfDue(0);
    Check(reg.StateDirty(), "a write inside the debounce window does not hit disk");

    reg.FlushStateIfDue(esb::Registry::kStateFlushMs + 1);
    Check(!reg.StateDirty(), "and is flushed once the window passes");
    api->unregister_provider(dbprov);
    std::remove(statePath);

    // ---------------------------------------------------------- M5: the relay
    std::printf("\nMQTT codec (ARCHITECTURE.md 11.2)\n");
    {
        // The varint is the one piece of MQTT framing that is easy to get
        // subtly wrong, and every packet depends on it.
        struct { uint32_t value; size_t bytes; } cases[] = {
            { 0, 1 }, { 127, 1 }, { 128, 2 }, { 16383, 2 },
            { 16384, 3 }, { 2097151, 3 }, { 2097152, 4 },
        };
        bool allOk = true;
        for (const auto& c : cases)
        {
            std::vector<uint8_t> encoded;
            esb::mqtt::EncodeVarInt(c.value, encoded);
            uint32_t decoded = 0;
            size_t   used    = 0;
            if (encoded.size() != c.bytes ||
                !esb::mqtt::DecodeVarInt(encoded.data(), encoded.size(), &decoded, &used) ||
                decoded != c.value || used != c.bytes)
                allOk = false;
        }
        Check(allOk, "remaining-length varints round-trip at every boundary");

        const std::vector<uint8_t> pub =
            esb::mqtt::EncodePublish("esb/v1/room/CTR/prov/field", "1:2:42", true);
        esb::mqtt::Packet decoded;
        size_t consumed = 0;
        Check(esb::mqtt::Decode(pub.data(), pub.size(), &decoded, &consumed) ==
                  esb::mqtt::DecodeResult::Ok,
              "a PUBLISH round-trips");
        Check(decoded.type == esb::mqtt::kPublish, "as a PUBLISH");
        Check(decoded.topic == "esb/v1/room/CTR/prov/field", "with its topic");
        Check(decoded.payload == "1:2:42", "and its payload");
        Check(decoded.retain, "and the retain flag that late joiners depend on");
        Check(consumed == pub.size(), "consuming exactly the packet");

        // A partial packet must ask for more, never guess.
        esb::mqtt::Packet partial;
        size_t used = 0;
        Check(esb::mqtt::Decode(pub.data(), pub.size() - 3, &partial, &used) ==
                  esb::mqtt::DecodeResult::NeedMore,
              "a truncated packet reports NeedMore rather than misparsing");
    }

    std::printf("\nconsent is two-key (ARCHITECTURE.md 11.1)\n");

    static const ESB_FieldDecl kSyncFields[] = {
        { "shared",  ESB_T_I64, ESB_SCOPE_GLOBAL,   ESB_F_SYNC, 0, "syncable"     },
        { "private", ESB_T_I64, ESB_SCOPE_GLOBAL,   0,          0, "not syncable" },
        { "ac",      ESB_T_I64, ESB_SCOPE_AIRCRAFT, ESB_F_SYNC, 0, "per aircraft" },
    };
    ESB_ProviderDecl syncer = { 0 };
    syncer.struct_size  = sizeof syncer;
    syncer.provider_id  = "syncer";
    syncer.schema_major = 1;
    syncer.fields       = kSyncFields;
    syncer.field_count  = 3;
    syncer.module       = self;

    ESB_Provider* sprov = nullptr;
    CheckStatus(api->register_provider(&syncer, &sprov), ESB_OK, "syncer registers");

    ESB_FieldId shared = ESB_FIELD_NONE, priv = ESB_FIELD_NONE, acField = ESB_FIELD_NONE;
    api->resolve("syncer/shared",  ESB_T_I64, &shared);
    api->resolve("syncer/private", ESB_T_I64, &priv);
    api->resolve("syncer/ac",      ESB_T_I64, &acField);

    esb::RelayConfig relayConfig;
    relayConfig.enabled    = true;
    relayConfig.room       = "testroom";
    relayConfig.controller = "LFPG_TWR";
    relayConfig.publishHz  = 1000;            // effectively no debounce, for the test
    relayConfig.host       = "loopback";

    esb::Relay relay;
    auto brokerOwned = std::unique_ptr<FakeBroker>(new FakeBroker());
    FakeBroker* broker = brokerOwned.get();

    // With the relay wired but NO provider on the user's allow list, nothing
    // may leave -- even though the author marked the field syncable.
    reg.SetRelay(&relay, &relayConfig);

    ESB_Value shv = { 0 };
    shv.type = ESB_T_I64; shv.bytes = sizeof(int64_t); shv.v.i64 = 11;
    api->set_global(sprov, shared, &shv);
    reg.DrainRelay(1000000);

    esb::Relay::Message probe;
    Check(!relay.PopInbound(&probe), "nothing inbound yet");
    Check(reg.DumpNet().find("syncer/shared") == std::string::npos,
          "an author-flagged field is NOT syncing while the user has not allowed it");

    // Now the user allows the provider: the second key turns.
    relayConfig.providers.insert("syncer");
    Check(reg.DumpNet().find("syncer/shared") != std::string::npos,
          "allowing the provider makes the flagged field eligible");
    Check(reg.DumpNet().find("syncer/private") == std::string::npos,
          "but a field its author never flagged still never syncs");

    std::printf("\noutbound path\n");

    relay.Start(relayConfig, std::move(brokerOwned));
    for (int i = 0; i < 50 && !relay.Connected(); ++i)
        Sleep(20);
    Check(relay.Connected(), "the relay completes CONNECT / CONNACK / SUBSCRIBE");

    shv.v.i64 = 22;
    api->set_global(sprov, shared, &shv);
    shv.v.i64 = 33;
    api->set_global(sprov, priv, &shv);        // must never reach the wire
    reg.DrainRelay(2000000);
    Sleep(120);

    {
        const std::string topics = broker->TakePublishedTopics();
        Check(topics.find("esb/v1/testroom/LFPG_TWR/syncer/shared") != std::string::npos,
              "an allowed, flagged field is published");
        Check(topics.find("syncer/private") == std::string::npos,
              "an unflagged field never reaches the wire");
    }

    std::printf("\ninbound is validated, and stays in the remote view\n");

    // Feed a peer's PUBLISH in through the transport, exactly as a broker would.
    auto deliverRemote = [&](const char* topic, const char* payload) {
        broker->Deliver(esb::mqtt::EncodePublish(topic, payload, true));
        Sleep(80);
        reg.DrainRelay(3000000);
    };

    deliverRemote("esb/v1/testroom/EDDF_TWR/syncer/shared", "1:7:555");

    ESB_Origin origins[4];
    uint32_t n = 4;
    CheckStatus(api->remote_publishers(shared, ESB_AIRCRAFT_NONE, origins, &n), ESB_OK,
                "remote_publishers succeeds");
    Check(n == 1, "one peer is publishing");
    Check(std::strcmp(origins[0].peer, "EDDF_TWR") == 0, "named correctly");
    Check(origins[0].revision == 7, "carrying the publisher's revision");

    ESB_Value remoteVal = { 0 };
    CheckStatus(api->get_remote(shared, ESB_AIRCRAFT_NONE, "EDDF_TWR",
                                &remoteVal, nullptr, nullptr), ESB_OK,
                "the remote value reads back");
    Check(remoteVal.v.i64 == 555, "with the right value");

    // The rule the whole remote view exists for.
    ESB_Value localVal = { 0 };
    CheckStatus(api->get_global(shared, &localVal, nullptr, nullptr), ESB_OK,
                "the local value still reads");
    Check(localVal.v.i64 == 22,
          "and is UNCHANGED -- remote data is never merged into local state");

    ESB_Peer peers[4];
    uint32_t peerCount = 4;
    CheckStatus(api->list_peers(peers, &peerCount), ESB_OK, "peers list");
    Check(peerCount == 1 && std::strcmp(peers[0].callsign, "EDDF_TWR") == 0,
          "showing the publishing controller");

    std::printf("\ninbound from an untrusted peer is rejected on every axis\n");

    const uint64_t before = relay.Rejected();
    deliverRemote("esb/v1/testroom/EDDF_TWR/syncer/private", "1:8:1");
    Check(relay.Rejected() > before, "a field its author never flagged syncable is rejected");

    const uint64_t beforeType = relay.Rejected();
    deliverRemote("esb/v1/testroom/EDDF_TWR/syncer/shared", "4:9:hello");
    Check(relay.Rejected() > beforeType, "a type that disagrees with our schema is rejected");
    CheckStatus(api->get_remote(shared, ESB_AIRCRAFT_NONE, "EDDF_TWR",
                                &remoteVal, nullptr, nullptr), ESB_OK,
                "and the previous good value survives");
    Check(remoteVal.v.i64 == 555, "unmodified");

    const uint64_t beforeUnknown = relay.Rejected();
    deliverRemote("esb/v1/testroom/EDDF_TWR/nosuch/field", "1:1:1");
    Check(relay.Rejected() > beforeUnknown, "an unknown provider is rejected");

    const uint64_t beforeGarbage = relay.Rejected();
    deliverRemote("esb/v1/testroom/EDDF_TWR/syncer/shared", "1:10:notanumber");
    Check(relay.Rejected() > beforeGarbage, "a payload that does not parse is rejected");

    std::printf("\nremote values follow the aircraft generation\n");

    reg.ObserveAircraft("SAS55", 3000000);
    deliverRemote("esb/v1/testroom/EDDF_TWR/syncer/ac/SAS55", "1:3:88");

    ESB_Aircraft sas = ESB_AIRCRAFT_NONE;
    CheckStatus(api->aircraft("SAS55", &sas), ESB_OK, "the aircraft resolves");
    n = 4;
    CheckStatus(api->remote_publishers(acField, sas, origins, &n), ESB_OK,
                "a per-aircraft remote value arrives");
    Check(n == 1, "from one peer");

    reg.RetireAircraft("SAS55");
    reg.ObserveAircraft("SAS55", 3100000);
    ESB_Aircraft sas2 = ESB_AIRCRAFT_NONE;
    api->aircraft("SAS55", &sas2);
    n = 4;
    api->remote_publishers(acField, sas2, origins, &n);
    Check(n == 0,
          "a recycled callsign does NOT inherit the peer's data either");

    std::printf("\nvalues arriving before the aircraft does\n");

    deliverRemote("esb/v1/testroom/EDDF_TWR/syncer/ac/QTR99", "1:4:123");
    ESB_Aircraft qtr = ESB_AIRCRAFT_NONE;
    Check(api->aircraft("QTR99", &qtr) == ESB_E_UNKNOWN_AIRCRAFT,
          "EuroScope has not shown us the aircraft yet");

    reg.ObserveAircraft("QTR99", 3200000);
    reg.DrainRelay(3200000);
    CheckStatus(api->aircraft("QTR99", &qtr), ESB_OK, "then it appears");
    n = 4;
    api->remote_publishers(acField, qtr, origins, &n);
    Check(n == 1, "and the held value is applied (a TOBT can beat the flight plan)");

    relay.Stop();
    reg.SetRelay(nullptr, nullptr);
    api->unregister_provider(sprov);

    // ---------------------------------------------------------------- teardown
    std::printf("\nteardown\n");
    CheckStatus(api->unregister_provider(prov2), ESB_OK, "unregister second provider");
    CheckStatus(api->resolve("smoke2/thing", ESB_T_I64, &bogus), ESB_E_NO_PROVIDER,
                "its fields stop resolving");
    CheckStatus(api->unregister_provider(prov), ESB_OK, "unregister first provider");

    std::printf("\n%s (%d failure%s)\n",
                g_failures ? "FAILED" : "ALL CHECKS PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
