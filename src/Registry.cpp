#include "Registry.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

#include "BridgeInstance.h"
#include "Json.h"
#include "Log.h"
#include "Relay.h"

namespace esb {

namespace {

bool IsScalar(uint32_t type)
{
    return type == ESB_T_I64 || type == ESB_T_F64 || type == ESB_T_BOOL;
}

bool ValidType(uint32_t t)
{
    return t >= ESB_T_I64 && t <= ESB_T_BLOB;
}

bool ValidScope(uint32_t s)
{
    return s == ESB_SCOPE_GLOBAL || s == ESB_SCOPE_AIRCRAFT;
}

// A field name is [a-z0-9_], 1..47 chars. Keeping it narrow means qualified
// names never need quoting in a dot command or an MQTT topic.
bool ValidFieldName(const char* n)
{
    if (!n || !*n)
        return false;
    size_t i = 0;
    for (; n[i]; ++i)
    {
        if (i >= 47)
            return false;
        const char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

// Provider ids additionally allow '.' and '-', for reverse-DNS style ids.
bool ValidProviderId(const char* n)
{
    if (!n || !*n)
        return false;
    size_t i = 0;
    for (; n[i]; ++i)
    {
        if (i >= 63)
            return false;
        const char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '-'))
            return false;
    }
    return true;
}

// Writes are compared, not blind: an identical write must not bump the revision
// (ARCHITECTURE.md 7). f64 goes through memcmp so a repeated NaN also compares
// equal -- with operator== it never would, and every tick would look like a
// change.
bool SameValue(const Slot& s, const ESB_Value* v)
{
    if (!s.assigned || s.type != v->type)
        return false;

    switch (v->type)
    {
    case ESB_T_I64:  return s.i64 == v->v.i64;
    case ESB_T_BOOL: return (s.i64 != 0) == (v->v.b != 0);
    case ESB_T_F64:  return std::memcmp(&s.f64, &v->v.f64, sizeof(double)) == 0;
    case ESB_T_STR:
    case ESB_T_BLOB:
        if (s.bytes.size() != v->bytes)
            return false;
        return v->bytes == 0 ||
               std::memcmp(s.bytes.data(), v->v.ptr, v->bytes) == 0;
    default:
        return false;
    }
}

// Reads copy into the caller's buffer, so out->v.ptr points at memory the
// caller already owns and no lifetime question arises.
ESB_Status ReadSlot(const Slot& s, ESB_Value* out, void* buf, uint32_t* ioBytes)
{
    if (!s.assigned)
        return ESB_E_UNSET;

    out->type = s.type;

    switch (s.type)
    {
    case ESB_T_I64:
        out->bytes = sizeof(int64_t);
        out->v.i64 = s.i64;
        return ESB_OK;
    case ESB_T_F64:
        out->bytes = sizeof(double);
        out->v.f64 = s.f64;
        return ESB_OK;
    case ESB_T_BOOL:
        out->bytes = sizeof(int32_t);
        out->v.b   = s.i64 ? 1 : 0;
        return ESB_OK;
    default:
        break;
    }

    const uint32_t need = static_cast<uint32_t>(s.bytes.size());
    out->bytes = need;
    out->v.ptr = nullptr;

    if (!ioBytes)
        return ESB_E_INVALID_ARG;      // STR/BLOB always needs a buffer

    const uint32_t have = *ioBytes;
    *ioBytes = need;
    if (!buf || have < need)
        return ESB_E_BUFFER_TOO_SMALL;

    if (need)
        std::memcpy(buf, s.bytes.data(), need);
    out->v.ptr = buf;
    return ESB_OK;
}

} // namespace

Registry::Registry()
    : m_threadId(GetCurrentThreadId())
{
}

Registry& TheRegistry()
{
    static Registry instance;
    return instance;
}

// --------------------------------------------------------------- internals

const FieldDef* Registry::Field(ESB_FieldId f) const
{
    if (f == ESB_FIELD_NONE || f > m_fields.size())
        return nullptr;
    return &m_fields[f - 1];           // ids are index+1 so 0 stays reserved
}

ProviderDef* Registry::Owner(ESB_Provider* p)
{
    ProviderDef* d = reinterpret_cast<ProviderDef*>(p);
    if (!d || d->magic != ProviderDef::kMagic || !d->live)
        return nullptr;
    return d;
}

void Registry::Store(Slot& s, const ESB_Value* v)
{
    s.type     = v->type;
    s.assigned = true;
    s.revision = ++m_clock;

    switch (v->type)
    {
    case ESB_T_I64:  s.i64 = v->v.i64;          break;
    case ESB_T_BOOL: s.i64 = v->v.b ? 1 : 0;    break;
    case ESB_T_F64:  s.f64 = v->v.f64;          break;
    default:
        s.bytes.assign(static_cast<const uint8_t*>(v->v.ptr),
                       static_cast<const uint8_t*>(v->v.ptr) + v->bytes);
        break;
    }
}

ESB_Status Registry::CheckWrite(ESB_Provider* p, ESB_FieldId f, uint32_t scope,
                                const ESB_Value* v, ProviderDef** outOwner) const
{
    if (!OnMainThread())
    {
        assert(false && "esb: called off the EuroScope main thread");
        return ESB_E_WRONG_THREAD;
    }

    ProviderDef* owner = const_cast<Registry*>(this)->Owner(p);
    if (!owner)
        return ESB_E_INVALID_ARG;

    const FieldDef* fd = Field(f);
    if (!fd)
        return ESB_E_NO_FIELD;
    if (fd->scope != scope)
        return ESB_E_INVALID_ARG;

    // Exclusive ownership: only the provider that declared the field may write
    // to it (ARCHITECTURE.md 3, D4).
    if (&*m_providers[fd->provider] != owner)
        return ESB_E_NOT_OWNER;

    if (v)
    {
        if (v->type != fd->type)
            return ESB_E_TYPE_MISMATCH;
        if (!IsScalar(v->type))
        {
            if (v->bytes && !v->v.ptr)
                return ESB_E_INVALID_ARG;
            if (fd->maxBytes && v->bytes > fd->maxBytes)
                return ESB_E_LIMIT;
        }
    }

    *outOwner = owner;
    return ESB_OK;
}

// ------------------------------------------------------------ registration

ESB_Status Registry::RegisterProvider(const ESB_ProviderDecl* decl, ESB_Provider** out)
{
    if (!OnMainThread())
    {
        assert(false && "esb: called off the EuroScope main thread");
        return ESB_E_WRONG_THREAD;
    }
    if (!decl || !out || decl->struct_size < sizeof(ESB_ProviderDecl))
        return ESB_E_INVALID_ARG;
    if (!ValidProviderId(decl->provider_id))
        return ESB_E_INVALID_ARG;
    if (decl->field_count && !decl->fields)
        return ESB_E_INVALID_ARG;

    *out = nullptr;

    const std::string id(decl->provider_id);

    auto existing = m_providerIndex.find(id);
    if (existing != m_providerIndex.end())
    {
        ProviderDef& prev = *m_providers[existing->second];

        if (prev.live && ModuleIsLoaded(prev.module, prev.modulePath))
        {
            // Same module re-registering identically is idempotent; anything
            // else is a genuine collision between two authors.
            if (prev.module == decl->module &&
                prev.major  == decl->schema_major &&
                prev.minor  == decl->schema_minor &&
                prev.fields.size() == decl->field_count)
            {
                *out = reinterpret_cast<ESB_Provider*>(&prev);
                return ESB_OK;
            }
            return prev.module == decl->module ? ESB_E_SCHEMA_CONFLICT
                                               : ESB_E_PROVIDER_TAKEN;
        }

        // The previous claimant's DLL is gone. Release the claim so a reloaded
        // plugin can take it again, and drop what it published -- field ids
        // are kept stable across a reload, so leaving the values behind would
        // hand the new instance its predecessor's stale data.
        prev.live = false;
        DropProviderValues(existing->second);
    }

    // Validate the whole schema before mutating anything, so a rejected
    // registration leaves no half-declared fields behind.
    for (uint32_t i = 0; i < decl->field_count; ++i)
    {
        const ESB_FieldDecl& fd = decl->fields[i];
        if (!ValidFieldName(fd.name) || !ValidType(fd.type) || !ValidScope(fd.scope))
            return ESB_E_INVALID_ARG;
        if (!IsScalar(fd.type) && fd.max_bytes == 0)
            return ESB_E_INVALID_ARG;          // STR/BLOB must declare a cap
        if ((fd.flags & ESB_F_PERSIST) && fd.scope != ESB_SCOPE_GLOBAL)
            return ESB_E_INVALID_ARG;          // per-aircraft persistence is meaningless

        const std::string qualified = id + "/" + fd.name;
        auto clash = m_fieldIndex.find(qualified);
        if (clash != m_fieldIndex.end() && m_fields[clash->second].type != fd.type)
            return ESB_E_SCHEMA_CONFLICT;      // a previous life declared it differently
    }

    auto def = std::make_unique<ProviderDef>();
    def->id          = id;
    def->major       = decl->schema_major;
    def->minor       = decl->schema_minor;
    def->displayName = decl->display_name ? decl->display_name : id;
    def->contact     = decl->contact ? decl->contact : "";
    def->module      = static_cast<HMODULE>(decl->module);
    def->revision    = ++m_clock;
    def->live        = true;

    if (def->module)
    {
        char path[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(def->module, path, MAX_PATH))
            def->modulePath = path;
    }

    const uint32_t providerIndex = static_cast<uint32_t>(m_providers.size());

    for (uint32_t i = 0; i < decl->field_count; ++i)
    {
        const ESB_FieldDecl& src = decl->fields[i];
        const std::string qualified = id + "/" + src.name;

        auto reuse = m_fieldIndex.find(qualified);
        uint32_t fieldIndex;

        if (reuse != m_fieldIndex.end())
        {
            // Same field from an earlier life of this provider. Keep the id
            // stable so consumers that resolved it before a plugin reload do
            // not have to resolve again.
            fieldIndex = reuse->second;
            m_fields[fieldIndex].provider = providerIndex;
            m_fields[fieldIndex].flags    = src.flags;
            m_fields[fieldIndex].maxBytes = src.max_bytes;
            m_fields[fieldIndex].doc      = src.doc ? src.doc : "";
        }
        else
        {
            FieldDef f;
            f.name      = src.name;
            f.qualified = qualified;
            f.type      = src.type;
            f.scope     = src.scope;
            f.flags     = src.flags;
            f.maxBytes  = src.max_bytes;
            f.doc       = src.doc ? src.doc : "";
            f.provider  = providerIndex;

            fieldIndex = static_cast<uint32_t>(m_fields.size());
            m_fields.push_back(std::move(f));
            m_fieldIndex.emplace(qualified, fieldIndex);
        }

        def->fields.push_back(fieldIndex);
    }

    ProviderDef* raw = def.get();
    m_providers.push_back(std::move(def));
    m_providerIndex[id] = providerIndex;

    // Anything this provider saved last session lands now, schema permitting.
    RestoreStagedFor(providerIndex);

    *out = reinterpret_cast<ESB_Provider*>(raw);
    return ESB_OK;
}

ESB_Status Registry::UnregisterProvider(ESB_Provider* p)
{
    if (!OnMainThread())
        return ESB_E_WRONG_THREAD;

    ProviderDef* d = Owner(p);
    if (!d)
        return ESB_E_INVALID_ARG;

    auto it = m_providerIndex.find(d->id);
    d->live = false;
    if (it != m_providerIndex.end())
        DropProviderValues(it->second);
    ++m_clock;
    return ESB_OK;
}

ESB_Status Registry::OwnField(ESB_Provider* p, const char* field, ESB_FieldId* out)
{
    if (!OnMainThread())
        return ESB_E_WRONG_THREAD;
    if (!field || !out)
        return ESB_E_INVALID_ARG;

    ProviderDef* d = Owner(p);
    if (!d)
        return ESB_E_INVALID_ARG;

    auto it = m_fieldIndex.find(d->id + "/" + field);
    if (it == m_fieldIndex.end() || m_fields[it->second].provider != m_providerIndex[d->id])
        return ESB_E_NO_FIELD;

    *out = it->second + 1;
    return ESB_OK;
}

// --------------------------------------------------------------- discovery

ESB_Status Registry::Resolve(const char* qualified, ESB_Type expect, ESB_FieldId* out) const
{
    if (!qualified || !out)
        return ESB_E_INVALID_ARG;

    const char* slash = std::strchr(qualified, '/');
    if (!slash)
        return ESB_E_INVALID_ARG;

    const std::string providerId(qualified, slash);
    auto pit = m_providerIndex.find(providerId);
    if (pit == m_providerIndex.end() || !m_providers[pit->second]->live)
        return ESB_E_NO_PROVIDER;

    auto it = m_fieldIndex.find(qualified);
    if (it == m_fieldIndex.end())
        return ESB_E_NO_FIELD;

    // Fail loudly rather than let a consumer built against an older schema
    // reinterpret the bytes.
    if (expect && m_fields[it->second].type != expect)
        return ESB_E_TYPE_MISMATCH;

    *out = it->second + 1;
    return ESB_OK;
}

ESB_Status Registry::ProviderVersion(const char* id, uint32_t* major, uint32_t* minor) const
{
    if (!id || !major || !minor)
        return ESB_E_INVALID_ARG;

    auto it = m_providerIndex.find(std::string(id));
    if (it == m_providerIndex.end() || !m_providers[it->second]->live)
        return ESB_E_NO_PROVIDER;

    *major = m_providers[it->second]->major;
    *minor = m_providers[it->second]->minor;
    return ESB_OK;
}

ESB_Status Registry::ListProviders(char* buf, uint32_t* ioBytes) const
{
    if (!ioBytes)
        return ESB_E_INVALID_ARG;

    std::string all;
    for (const auto& p : m_providers)
        if (p->live)
            all += p->id + "\n";

    const uint32_t need = static_cast<uint32_t>(all.size()) + 1;
    const uint32_t have = *ioBytes;
    *ioBytes = need;

    if (!buf || have < need)
        return ESB_E_BUFFER_TOO_SMALL;

    std::memcpy(buf, all.c_str(), need);
    return ESB_OK;
}

// ------------------------------------------------------------ global scope

ESB_Status Registry::GetGlobal(ESB_FieldId f, ESB_Value* out, void* buf, uint32_t* ioBytes) const
{
    if (!OnMainThread())
        return ESB_E_WRONG_THREAD;
    if (!out)
        return ESB_E_INVALID_ARG;

    const FieldDef* fd = Field(f);
    if (!fd)
        return ESB_E_NO_FIELD;
    if (fd->scope != ESB_SCOPE_GLOBAL)
        return ESB_E_INVALID_ARG;
    if (!m_providers[fd->provider]->live)
        return ESB_E_NO_PROVIDER;

    auto it = m_globals.find(f - 1);
    if (it == m_globals.end())
        return ESB_E_UNSET;

    return ReadSlot(it->second, out, buf, ioBytes);
}

ESB_Status Registry::SetGlobal(ESB_Provider* p, ESB_FieldId f, const ESB_Value* v)
{
    ProviderDef* owner = nullptr;
    const ESB_Status st = CheckWrite(p, f, ESB_SCOPE_GLOBAL, v, &owner);
    if (st != ESB_OK)
        return st;
    if (!v)
        return ESB_E_INVALID_ARG;

    Slot& s = m_globals[f - 1];
    if (SameValue(s, v))
        return ESB_OK;                 // no revision bump, no notification

    Store(s, v);
    owner->revision = s.revision;
    QueueOutbound(f - 1, ESB_AIRCRAFT_NONE);
    MarkStateDirty(*Field(f));
    Notify(f - 1, ESB_AIRCRAFT_NONE);
    return ESB_OK;
}

ESB_Status Registry::ClearGlobal(ESB_Provider* p, ESB_FieldId f)
{
    ProviderDef* owner = nullptr;
    const ESB_Status st = CheckWrite(p, f, ESB_SCOPE_GLOBAL, nullptr, &owner);
    if (st != ESB_OK)
        return st;

    auto it = m_globals.find(f - 1);
    if (it == m_globals.end() || !it->second.assigned)
        return ESB_OK;

    m_globals.erase(it);
    owner->revision = ++m_clock;
    QueueOutbound(f - 1, ESB_AIRCRAFT_NONE);
    MarkStateDirty(*Field(f));
    Notify(f - 1, ESB_AIRCRAFT_NONE);
    return ESB_OK;
}

// ---------------------------------------------------------- aircraft scope

ESB_Status Registry::AircraftHandle(const char* callsign, ESB_Aircraft* out) const
{
    if (!OnMainThread())
        return ESB_E_WRONG_THREAD;
    return m_ac.Resolve(callsign, out);
}

ESB_Status Registry::AircraftCallsign(ESB_Aircraft a, char* buf, uint32_t* ioBytes) const
{
    if (!OnMainThread())
        return ESB_E_WRONG_THREAD;
    return m_ac.CallsignOf(a, buf, ioBytes);
}

ESB_Status Registry::GetAircraft(ESB_Aircraft a, ESB_FieldId f, ESB_Value* out,
                                 void* buf, uint32_t* ioBytes) const
{
    if (!OnMainThread())
        return ESB_E_WRONG_THREAD;
    if (!out)
        return ESB_E_INVALID_ARG;

    const ESB_Status acStatus = m_ac.Validate(a);
    if (acStatus != ESB_OK)
        return acStatus;

    const FieldDef* fd = Field(f);
    if (!fd)
        return ESB_E_NO_FIELD;
    if (fd->scope != ESB_SCOPE_AIRCRAFT)
        return ESB_E_INVALID_ARG;
    if (!m_providers[fd->provider]->live)
        return ESB_E_NO_PROVIDER;

    auto it = m_aircraft.find(AircraftKey(f - 1, AircraftTable::SlotOf(a)));
    if (it == m_aircraft.end())
        return ESB_E_UNSET;

    return ReadSlot(it->second, out, buf, ioBytes);
}

ESB_Status Registry::SetAircraft(ESB_Provider* p, ESB_Aircraft a, ESB_FieldId f,
                                 const ESB_Value* v)
{
    ProviderDef* owner = nullptr;
    const ESB_Status st = CheckWrite(p, f, ESB_SCOPE_AIRCRAFT, v, &owner);
    if (st != ESB_OK)
        return st;
    if (!v)
        return ESB_E_INVALID_ARG;

    const ESB_Status acStatus = m_ac.Validate(a);
    if (acStatus != ESB_OK)
        return acStatus;

    const uint32_t slot = AircraftTable::SlotOf(a);
    const uint64_t key  = AircraftKey(f - 1, slot);

    auto it = m_aircraft.find(key);
    if (it != m_aircraft.end())
    {
        if (SameValue(it->second, v))
            return ESB_OK;                 // no revision bump, no notification
    }
    else
    {
        it = m_aircraft.emplace(key, Slot{}).first;
        m_slotFields[slot].push_back(f - 1);   // keeps slot drops O(fields)
    }

    Store(it->second, v);
    owner->revision = it->second.revision;
    QueueOutbound(f - 1, a);
    Notify(f - 1, a);
    return ESB_OK;
}

ESB_Status Registry::ClearAircraft(ESB_Provider* p, ESB_Aircraft a, ESB_FieldId f)
{
    ProviderDef* owner = nullptr;
    const ESB_Status st = CheckWrite(p, f, ESB_SCOPE_AIRCRAFT, nullptr, &owner);
    if (st != ESB_OK)
        return st;

    const ESB_Status acStatus = m_ac.Validate(a);
    if (acStatus != ESB_OK)
        return acStatus;

    const uint32_t slot = AircraftTable::SlotOf(a);

    auto it = m_aircraft.find(AircraftKey(f - 1, slot));
    if (it == m_aircraft.end())
        return ESB_OK;

    m_aircraft.erase(it);

    auto sf = m_slotFields.find(slot);
    if (sf != m_slotFields.end())
    {
        auto& fields = sf->second;
        fields.erase(std::remove(fields.begin(), fields.end(), f - 1), fields.end());
        if (fields.empty())
            m_slotFields.erase(sf);
    }

    owner->revision = ++m_clock;
    Notify(f - 1, a);
    return ESB_OK;
}

// -------------------------------------------------------------- notification

// SEH guard around a third-party function pointer. Deliberately free of any
// C++ object that would need unwinding -- MSVC rejects __try in a function
// that has one, and a guard that will not compile is worse than none.
//
// This cannot make the process safe: a subscriber shares our address space and
// can corrupt anything before it faults. What it does buy is that the common
// case -- a null deref in someone's callback -- takes down that subscription
// with a log line naming its author, instead of taking down EuroScope for
// every controller mid-session (ARCHITECTURE.md 9.5).
static bool CallSubscriberGuarded(ESB_OnChange cb, ESB_FieldId field,
                                  ESB_Aircraft ac, void* user)
{
    __try
    {
        cb(field, ac, user);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void Registry::QueuePending(uint32_t fieldIndex, ESB_Aircraft ac)
{
    // Coalesce: one delivery per (field, aircraft) per round, so a cycle costs
    // a bounded number of extra calls rather than an avalanche.
    for (const Pending& p : m_pending)
        if (p.field == fieldIndex && p.aircraft == ac)
            return;

    m_pending.push_back(Pending{ fieldIndex, ac });
}

void Registry::Dispatch(uint32_t fieldIndex, ESB_Aircraft ac)
{
    auto it = m_subsByField.find(fieldIndex);
    if (it == m_subsByField.end() || it->second.empty())
        return;

    // Snapshot: a subscriber may subscribe or unsubscribe from inside its own
    // callback, which would otherwise invalidate the vector we are walking.
    // Anything added during this dispatch simply starts with the next event.
    const std::vector<uint32_t> targets = it->second;

    ++m_dispatchDepth;

    for (uint32_t index : targets)
    {
        if (index >= m_subs.size())
            continue;

        SubscriptionDef& sub = *m_subs[index];
        if (!sub.live || !sub.callback)
            continue;

        ++sub.delivered;
        ++m_dispatched;

        if (CallSubscriberGuarded(sub.callback, fieldIndex + 1, ac, sub.user))
            continue;

        // It faulted. Drop the subscription rather than fault again next tick,
        // and name whoever owns the module so the report can be routed.
        sub.live = false;
        Log::Get().Write(ESB_LOG_ERROR,
                         "subscriber faulted on '%s' and was unsubscribed; module=%s",
                         m_fields[fieldIndex].qualified.c_str(),
                         sub.modulePath.empty() ? "<unknown>" : sub.modulePath.c_str());
    }

    --m_dispatchDepth;
}

void Registry::DrainPending()
{
    uint32_t round = 0;

    while (!m_pending.empty())
    {
        if (++round > kMaxDispatchRounds)
        {
            // Two providers are almost certainly writing in response to each
            // other. Shed the rest: a diagnosable log line beats a hang.
            std::string chain;
            for (const Pending& p : m_pending)
            {
                if (p.field < m_fields.size())
                {
                    if (!chain.empty())
                        chain += " -> ";
                    chain += m_fields[p.field].qualified;
                }
            }

            m_dropped += m_pending.size();
            Log::Get().Write(ESB_LOG_WARN,
                             "notification cycle: %u events dropped after %u rounds; chain: %s",
                             static_cast<unsigned>(m_pending.size()),
                             static_cast<unsigned>(kMaxDispatchRounds),
                             chain.c_str());
            m_pending.clear();
            return;
        }

        // Take the batch: dispatching it may queue the next round.
        std::vector<Pending> batch;
        batch.swap(m_pending);

        for (const Pending& p : batch)
            Dispatch(p.field, p.aircraft);
    }
}

void Registry::Notify(uint32_t fieldIndex, ESB_Aircraft ac)
{
    if (m_dispatchDepth > 0)
    {
        // A subscriber wrote from inside its callback. The write has already
        // been applied; only its notification is deferred.
        QueuePending(fieldIndex, ac);
        return;
    }

    Dispatch(fieldIndex, ac);
    DrainPending();
}

ESB_Status Registry::Subscribe(ESB_FieldId f, ESB_OnChange cb, void* user,
                               void* module, ESB_Sub** out)
{
    if (!OnMainThread())
        return ESB_E_WRONG_THREAD;
    if (!cb || !out)
        return ESB_E_INVALID_ARG;

    const FieldDef* fd = Field(f);
    if (!fd)
        return ESB_E_NO_FIELD;

    auto sub = std::make_unique<SubscriptionDef>();
    sub->field    = f - 1;
    sub->callback = cb;
    sub->user     = user;
    sub->module   = static_cast<HMODULE>(module);
    sub->live     = true;

    if (sub->module)
    {
        char path[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(sub->module, path, MAX_PATH))
            sub->modulePath = path;
    }

    SubscriptionDef* raw = sub.get();
    const uint32_t index = static_cast<uint32_t>(m_subs.size());
    m_subs.push_back(std::move(sub));
    m_subsByField[f - 1].push_back(index);

    *out = reinterpret_cast<ESB_Sub*>(raw);
    return ESB_OK;
}

ESB_Status Registry::Unsubscribe(ESB_Sub* s)
{
    if (!OnMainThread())
        return ESB_E_WRONG_THREAD;

    SubscriptionDef* d = reinterpret_cast<SubscriptionDef*>(s);
    if (!d || d->magic != SubscriptionDef::kMagic || !d->live)
        return ESB_E_INVALID_ARG;

    // Marked dead rather than erased: a dispatch may be walking a snapshot
    // that still names it, and the live flag is what that snapshot checks.
    d->live = false;
    return ESB_OK;
}

// -------------------------------------------------------------- change feed

uint64_t Registry::RevisionOf(ESB_FieldId f, ESB_Aircraft a) const
{
    const FieldDef* fd = Field(f);
    if (!fd)
        return 0;

    if (fd->scope == ESB_SCOPE_GLOBAL)
    {
        auto it = m_globals.find(f - 1);
        return it == m_globals.end() ? 0 : it->second.revision;
    }

    if (m_ac.Validate(a) != ESB_OK)
        return 0;

    auto it = m_aircraft.find(AircraftKey(f - 1, AircraftTable::SlotOf(a)));
    return it == m_aircraft.end() ? 0 : it->second.revision;
}

uint64_t Registry::ProviderRevision(const char* id) const
{
    if (!id)
        return 0;
    auto it = m_providerIndex.find(std::string(id));
    if (it == m_providerIndex.end() || !m_providers[it->second]->live)
        return 0;
    return m_providers[it->second]->revision;
}

// ----------------------------------------------------------------- lifecycle

ESB_Aircraft Registry::ObserveAircraft(const char* callsign, uint64_t nowMs)
{
    return m_ac.Observe(callsign, nowMs);
}

void Registry::DropAircraftSlot(uint32_t slot)
{
    DropRemoteSlot(slot);

    auto sf = m_slotFields.find(slot);
    if (sf == m_slotFields.end())
        return;

    for (uint32_t fieldIndex : sf->second)
        m_aircraft.erase(AircraftKey(fieldIndex, slot));

    m_slotFields.erase(sf);
}

void Registry::DropAircraftSlots(const std::vector<uint32_t>& slots)
{
    for (uint32_t slot : slots)
        DropAircraftSlot(slot);
}

void Registry::DropProviderValues(uint32_t providerIndex)
{
    if (providerIndex >= m_providers.size())
        return;

    for (uint32_t fieldIndex : m_providers[providerIndex]->fields)
    {
        if (m_fields[fieldIndex].scope == ESB_SCOPE_GLOBAL)
        {
            m_globals.erase(fieldIndex);
            continue;
        }

        // Aircraft-scoped: the value lives under one key per slot that carries
        // it, so walk the per-slot index rather than the whole value map.
        for (auto sf = m_slotFields.begin(); sf != m_slotFields.end(); )
        {
            auto& fields = sf->second;
            auto  found  = std::find(fields.begin(), fields.end(), fieldIndex);
            if (found != fields.end())
            {
                m_aircraft.erase(AircraftKey(fieldIndex, sf->first));
                fields.erase(found);
            }

            sf = fields.empty() ? m_slotFields.erase(sf) : std::next(sf);
        }
    }
}

void Registry::RetireAircraft(const char* callsign)
{
    uint32_t slot = 0;
    if (m_ac.Retire(callsign, &slot))
    {
        DropAircraftSlot(slot);
        ++m_clock;
    }
}

void Registry::SweepAircraft(uint64_t nowMs)
{
    const AircraftTable::SweepResult swept = m_ac.Sweep(nowMs, m_ttlMs, m_graceMs);

    if (!swept.retiredSlots.empty())
    {
        DropAircraftSlots(swept.retiredSlots);
        ++m_clock;
    }
}

void Registry::ReapDeadModules()
{
    for (uint32_t i = 0; i < m_providers.size(); ++i)
    {
        ProviderDef& p = *m_providers[i];
        if (p.live && !ModuleIsLoaded(p.module, p.modulePath))
        {
            // The DLL is gone. Release the namespace so a reloaded plugin can
            // reclaim it, and drop what it published -- nothing can read it,
            // and a reload must not inherit it (ARCHITECTURE.md 9.1).
            p.live = false;
            DropProviderValues(i);
            ++m_clock;

            Log::Get().Write(ESB_LOG_WARN,
                             "provider '%s' reaped: its module is no longer loaded (%s)",
                             p.id.c_str(),
                             p.modulePath.empty() ? "<unknown>" : p.modulePath.c_str());
        }
    }

    // The sharpest one. A subscription whose module has gone is a function
    // pointer into freed address space; the next dispatch would jump into it
    // and take EuroScope down for the whole session.
    for (auto& s : m_subs)
    {
        if (s->live && !ModuleIsLoaded(s->module, s->modulePath))
        {
            s->live = false;
            Log::Get().Write(ESB_LOG_WARN,
                             "subscription on '%s' reaped: its module is no longer loaded (%s)",
                             s->field < m_fields.size()
                                 ? m_fields[s->field].qualified.c_str() : "?",
                             s->modulePath.empty() ? "<unknown>" : s->modulePath.c_str());
        }
    }
}

void Registry::Housekeeping(uint64_t nowMs)
{
    if (!OnMainThread())
    {
        assert(false && "esb: housekeeping off the EuroScope main thread");
        return;
    }

    // Both scans touch every entry they walk, and neither needs to run at the
    // once-a-second rate OnTimer fires at.
    if (nowMs - m_lastModuleCheckMs >= kModuleCheckMs)
    {
        m_lastModuleCheckMs = nowMs;
        ReapDeadModules();
    }

    if (nowMs - m_lastAircraftSweepMs >= kAircraftSweepMs)
    {
        m_lastAircraftSweepMs = nowMs;
        SweepAircraft(nowMs);
    }

    FlushStateIfDue(nowMs);

    DrainRelay(nowMs);
}

// --------------------------------------------------------------- the relay

namespace {

// Values cross the wire as text so a topic dump is readable and a broker's
// own tooling can show something meaningful. Blobs go as hex.
std::string EncodePayload(const Slot& s)
{
    char buf[64];
    switch (s.type)
    {
    case ESB_T_I64:
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(s.i64));
        return buf;
    case ESB_T_BOOL:
        return s.i64 ? "1" : "0";
    case ESB_T_F64:
        std::snprintf(buf, sizeof buf, "%.17g", s.f64);
        return buf;
    case ESB_T_STR:
        return std::string(reinterpret_cast<const char*>(s.bytes.data()), s.bytes.size());
    default:
    {
        std::string hex;
        hex.reserve(s.bytes.size() * 2);
        for (uint8_t b : s.bytes)
        {
            char pair[3];
            std::snprintf(pair, sizeof pair, "%02x", b);
            hex += pair;
        }
        return hex;
    }
    }
}

// The inverse, applied to input from a peer we do not control. Returns false
// on anything that does not parse as the declared type.
bool DecodePayload(uint32_t type, const std::string& text, Slot* out)
{
    out->type     = type;
    out->assigned = true;

    switch (type)
    {
    case ESB_T_I64:
    {
        char* end = nullptr;
        out->i64 = std::strtoll(text.c_str(), &end, 10);
        return end && *end == '\0' && end != text.c_str();
    }
    case ESB_T_BOOL:
        if (text != "0" && text != "1")
            return false;
        out->i64 = (text == "1") ? 1 : 0;
        return true;
    case ESB_T_F64:
    {
        char* end = nullptr;
        out->f64 = std::strtod(text.c_str(), &end);
        return end && *end == '\0' && end != text.c_str();
    }
    case ESB_T_STR:
        out->bytes.assign(text.begin(), text.end());
        return true;
    case ESB_T_BLOB:
    {
        if (text.size() % 2 != 0)
            return false;
        out->bytes.clear();
        out->bytes.reserve(text.size() / 2);
        for (size_t i = 0; i < text.size(); i += 2)
        {
            char* end = nullptr;
            const std::string pair = text.substr(i, 2);
            const unsigned long b = std::strtoul(pair.c_str(), &end, 16);
            if (!end || *end != '\0')
                return false;
            out->bytes.push_back(static_cast<uint8_t>(b));
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace

void Registry::SetRelay(Relay* relay, const RelayConfig* config)
{
    m_relay       = relay;
    m_relayConfig = config;
}

// Two keys, both required (ARCHITECTURE.md 11.1). The author's flag alone is
// never enough; neither is the user's allow list.
bool Registry::SyncEligible(const FieldDef& f) const
{
    if (!m_relay || !m_relayConfig)
        return false;
    if (!(f.flags & ESB_F_SYNC))
        return false;

    return m_relayConfig->AllowsProvider(m_providers[f.provider]->id);
}

void Registry::QueueOutbound(uint32_t fieldIndex, ESB_Aircraft a)
{
    if (fieldIndex >= m_fields.size())
        return;

    const FieldDef& f = m_fields[fieldIndex];
    if (!SyncEligible(f))
        return;

    const uint32_t slot = (f.scope == ESB_SCOPE_GLOBAL)
                        ? kGlobalSlot : AircraftTable::SlotOf(a);

    // Coalesce on the key: only the newest revision per key survives to the
    // next flush, so a 20 Hz writer still produces at most publishHz packets.
    m_outbox[AircraftKey(fieldIndex, slot)] = m_clock;
}

void Registry::FlushOutbox(uint64_t nowMs)
{
    if (!m_relay || !m_relayConfig || m_outbox.empty())
        return;

    const uint64_t interval = 1000ull / (m_relayConfig->publishHz ? m_relayConfig->publishHz : 4);
    if (nowMs - m_lastOutboxMs < interval)
        return;
    m_lastOutboxMs = nowMs;

    std::map<uint64_t, uint64_t> batch;
    batch.swap(m_outbox);

    m_publishedKeys.clear();

    for (const auto& kv : batch)
    {
        const uint32_t fieldIndex = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t slot       = static_cast<uint32_t>(kv.first & 0xFFFFFFFFull);
        if (fieldIndex >= m_fields.size())
            continue;

        const FieldDef& f = m_fields[fieldIndex];
        if (!SyncEligible(f))
            continue;                  // config changed under us

        // Per-provider cap. With no authority rule, a plugin could otherwise
        // publish for every aircraft on the network (ARCHITECTURE.md 11.3).
        uint32_t& count = m_publishedKeys[f.provider];
        if (count >= m_relayConfig->maxKeysPerProvider)
        {
            ++m_relayRejected;
            continue;
        }
        ++count;

        const Slot* value = nullptr;
        if (slot == kGlobalSlot)
        {
            auto it = m_globals.find(fieldIndex);
            if (it != m_globals.end())
                value = &it->second;
        }
        else
        {
            auto it = m_aircraft.find(AircraftKey(fieldIndex, slot));
            if (it != m_aircraft.end())
                value = &it->second;
        }
        if (!value || !value->assigned)
            continue;                  // cleared before we got round to it

        Relay::Message m;
        m.qualified = f.qualified;
        m.type      = value->type;
        m.revision  = value->revision;
        m.payload   = EncodePayload(*value);

        if (slot != kGlobalSlot)
        {
            char callsign[32] = { 0 };
            uint32_t n = sizeof callsign;
            // The wire carries the callsign: ESB_Aircraft is a local handle and
            // means nothing to a peer (ARCHITECTURE.md 11.4).
            if (m_ac.CallsignOf(AircraftTable::Handle(slot, 0), callsign, &n) != ESB_OK)
            {
                // Slot went away between the write and the flush.
                if (n == 0)
                    continue;
            }
            m.callsign = callsign;
            if (m.callsign.empty())
                continue;
        }

        m_relay->PushOutbound(m);
    }
}

void Registry::TouchPeer(const std::string& callsign, uint64_t nowMs)
{
    for (PeerDef& p : m_peers)
    {
        if (p.callsign == callsign)
        {
            p.lastSeenMs = nowMs;
            return;
        }
    }

    PeerDef p;
    p.callsign   = callsign;
    p.lastSeenMs = nowMs;
    m_peers.push_back(p);
}

bool Registry::ApplyRemote(const std::string& peer, const std::string& qualified,
                           const std::string& callsign, uint32_t type,
                           const std::string& payload, uint64_t peerRevision,
                           uint64_t nowMs)
{
    TouchPeer(peer, nowMs);

    // Everything below validates input from a machine we do not control.
    auto fieldIt = m_fieldIndex.find(qualified);
    if (fieldIt == m_fieldIndex.end())
        return false;                  // unknown field: never relayed either way

    const uint32_t  fieldIndex = fieldIt->second;
    const FieldDef& f          = m_fields[fieldIndex];

    if (!m_providers[f.provider]->live)
        return false;                  // provider not loaded here
    if (!(f.flags & ESB_F_SYNC))
        return false;                  // author never marked it syncable
    if (f.type != type)
        return false;                  // type does not match our schema

    Slot value;
    if (!DecodePayload(type, payload, &value))
        return false;
    if (f.maxBytes && value.bytes.size() > f.maxBytes)
        return false;                  // oversized for the declared cap

    uint32_t slot = kGlobalSlot;
    if (f.scope == ESB_SCOPE_AIRCRAFT)
    {
        if (callsign.empty())
            return false;

        ESB_Aircraft ac = ESB_AIRCRAFT_NONE;
        if (m_ac.Resolve(callsign.c_str(), &ac) != ESB_OK)
        {
            // A TOBT can arrive before the flight plan does. Hold it briefly
            // and apply it if the aircraft shows up (ARCHITECTURE.md 11.4).
            PendingRemote pending;
            pending.peer         = peer;
            pending.qualified    = qualified;
            pending.callsign     = callsign;
            pending.type         = type;
            pending.payload      = payload;
            pending.peerRevision = peerRevision;
            pending.arrivedMs    = nowMs;
            m_pendingRemote.push_back(pending);
            return true;
        }
        slot = AircraftTable::SlotOf(ac);
    }
    else if (!callsign.empty())
    {
        return false;                  // global field with a callsign: malformed
    }

    value.revision = ++m_clock;

    std::vector<RemoteEntry>& entries = m_remote[AircraftKey(fieldIndex, slot)];
    for (RemoteEntry& e : entries)
    {
        if (e.peer == peer)
        {
            // Ordering within one publisher's stream, without trusting clocks.
            if (peerRevision && peerRevision < e.peerRevision)
                return false;
            e.value        = value;
            e.peerRevision = peerRevision;
            e.receivedMs   = nowMs;
            return true;
        }
    }

    RemoteEntry entry;
    entry.peer         = peer;
    entry.value        = value;
    entry.peerRevision = peerRevision;
    entry.receivedMs   = nowMs;
    entries.push_back(entry);
    return true;
}

void Registry::DrainRelay(uint64_t nowMs)
{
    if (!m_relay)
        return;

    Relay::Message m;
    while (m_relay->PopInbound(&m))
    {
        if (!ApplyRemote(m.peer, m.qualified, m.callsign, m.type,
                         m.payload, m.revision, nowMs))
        {
            ++m_relayRejected;
            m_relay->NoteRejected();
        }
    }

    // Retry anything that was waiting on an aircraft to appear.
    for (size_t i = 0; i < m_pendingRemote.size(); )
    {
        const PendingRemote& p = m_pendingRemote[i];

        ESB_Aircraft ac = ESB_AIRCRAFT_NONE;
        const bool known = m_ac.Resolve(p.callsign.c_str(), &ac) == ESB_OK;
        const bool stale = (nowMs - p.arrivedMs) > kPendingRemoteTtlMs;

        if (!known && !stale)
        {
            ++i;
            continue;
        }

        const PendingRemote taken = p;
        m_pendingRemote.erase(m_pendingRemote.begin() + static_cast<ptrdiff_t>(i));

        if (known)
            ApplyRemote(taken.peer, taken.qualified, taken.callsign, taken.type,
                        taken.payload, taken.peerRevision, nowMs);
    }

    FlushOutbox(nowMs);
}

void Registry::DropRemoteSlot(uint32_t slot)
{
    // Remote values participate in generational clearing exactly like local
    // ones: a recycled callsign must not inherit a peer's data either.
    for (auto it = m_remote.begin(); it != m_remote.end(); )
    {
        if (static_cast<uint32_t>(it->first & 0xFFFFFFFFull) == slot)
            it = m_remote.erase(it);
        else
            ++it;
    }
}

ESB_Status Registry::RemotePublishers(ESB_FieldId f, ESB_Aircraft a,
                                      ESB_Origin* out, uint32_t* ioCount) const
{
    if (!OnMainThread())
        return ESB_E_WRONG_THREAD;
    if (!ioCount)
        return ESB_E_INVALID_ARG;

    const uint32_t capacity = *ioCount;
    *ioCount = 0;

    const FieldDef* fd = Field(f);
    if (!fd)
        return ESB_E_NO_FIELD;

    uint32_t slot = kGlobalSlot;
    if (fd->scope == ESB_SCOPE_AIRCRAFT)
    {
        const ESB_Status st = m_ac.Validate(a);
        if (st != ESB_OK)
            return st;
        slot = AircraftTable::SlotOf(a);
    }

    auto it = m_remote.find(AircraftKey(f - 1, slot));
    if (it == m_remote.end())
        return ESB_OK;                 // nobody is publishing: not an error

    const uint32_t found = static_cast<uint32_t>(it->second.size());
    *ioCount = found;

    if (!out || capacity < found)
        return ESB_E_BUFFER_TOO_SMALL;

    for (uint32_t i = 0; i < found; ++i)
    {
        const RemoteEntry& e = it->second[i];
        std::memset(&out[i], 0, sizeof(ESB_Origin));
        std::strncpy(out[i].peer, e.peer.c_str(), sizeof(out[i].peer) - 1);
        out[i].revision    = e.peerRevision;
        out[i].received_ms = e.receivedMs;
    }

    return ESB_OK;
}

ESB_Status Registry::GetRemote(ESB_FieldId f, ESB_Aircraft a, const char* peer,
                               ESB_Value* out, void* buf, uint32_t* ioBytes) const
{
    if (!OnMainThread())
        return ESB_E_WRONG_THREAD;
    if (!out || !peer)
        return ESB_E_INVALID_ARG;

    const FieldDef* fd = Field(f);
    if (!fd)
        return ESB_E_NO_FIELD;

    uint32_t slot = kGlobalSlot;
    if (fd->scope == ESB_SCOPE_AIRCRAFT)
    {
        const ESB_Status st = m_ac.Validate(a);
        if (st != ESB_OK)
            return st;
        slot = AircraftTable::SlotOf(a);
    }

    auto it = m_remote.find(AircraftKey(f - 1, slot));
    if (it == m_remote.end())
        return ESB_E_UNSET;

    for (const RemoteEntry& e : it->second)
        if (e.peer == peer)
            return ReadSlot(e.value, out, buf, ioBytes);

    return ESB_E_UNSET;
}

ESB_Status Registry::ListPeers(ESB_Peer* out, uint32_t* ioCount) const
{
    if (!OnMainThread())
        return ESB_E_WRONG_THREAD;
    if (!ioCount)
        return ESB_E_INVALID_ARG;

    const uint32_t capacity = *ioCount;
    const uint32_t found    = static_cast<uint32_t>(m_peers.size());
    *ioCount = found;

    if (!out || capacity < found)
        return found ? ESB_E_BUFFER_TOO_SMALL : ESB_OK;

    for (uint32_t i = 0; i < found; ++i)
    {
        std::memset(&out[i], 0, sizeof(ESB_Peer));
        std::strncpy(out[i].callsign, m_peers[i].callsign.c_str(),
                     sizeof(out[i].callsign) - 1);
        out[i].last_seen_ms = m_peers[i].lastSeenMs;
    }

    return ESB_OK;
}

std::string Registry::DumpNet() const
{
    std::string out;

    if (!m_relayConfig || !m_relayConfig->enabled)
        return "relay: off — nothing leaves this machine";

    out = m_relayConfig->Describe();
    out += "\n";

    char line[320];
    std::snprintf(line, sizeof line,
                  "state: %s · peers %u · remote keys %u · pending %u · rejected %llu\n",
                  (m_relay && m_relay->Connected()) ? "connected" : "not connected",
                  static_cast<unsigned>(m_peers.size()),
                  static_cast<unsigned>(m_remote.size()),
                  static_cast<unsigned>(m_pendingRemote.size()),
                  static_cast<unsigned long long>(m_relayRejected));
    out += line;

    if (m_relay)
    {
        std::snprintf(line, sizeof line, "traffic: %llu published, %llu received\n",
                      static_cast<unsigned long long>(m_relay->Published()),
                      static_cast<unsigned long long>(m_relay->Received()));
        out += line;

        const std::string err = m_relay->LastError();
        if (!err.empty())
            out += "last error: " + err + "\n";
    }

    // Exactly which fields are eligible to leave, so a user can audit it.
    out += "syncing out:\n";
    bool any = false;
    for (const FieldDef& f : m_fields)
    {
        if (!SyncEligible(f))
            continue;
        any = true;
        out += "  " + f.qualified + "\n";
    }
    if (!any)
        out += "  (nothing — no field is both marked syncable and allowed here)\n";

    return out;
}

// -------------------------------------------------------------- persistence

void Registry::MarkStateDirty(const FieldDef& f)
{
    if ((f.flags & ESB_F_PERSIST) && f.scope == ESB_SCOPE_GLOBAL)
        m_stateDirty = true;
}

// Called after a provider registers. Anything read from disk for one of its
// fields is applied now -- but only if the schema still agrees. A provider
// that changed a field's type gets nothing rather than a reinterpreted value.
void Registry::RestoreStagedFor(uint32_t providerIndex)
{
    if (m_staged.empty() || providerIndex >= m_providers.size())
        return;

    for (uint32_t fieldIndex : m_providers[providerIndex]->fields)
    {
        const FieldDef& f = m_fields[fieldIndex];
        if (!(f.flags & ESB_F_PERSIST) || f.scope != ESB_SCOPE_GLOBAL)
            continue;

        auto it = m_staged.find(f.qualified);
        if (it == m_staged.end())
            continue;

        // By value, then erase: holding a reference into the map across the
        // erase leaves it dangling, and the vector member is the part that
        // actually gets freed.
        const StagedValue staged = it->second;
        m_staged.erase(it);

        if (staged.type != f.type)
        {
            Log::Get().Write(ESB_LOG_WARN,
                             "discarded saved value for '%s': the field is now a "
                             "different type",
                             f.qualified.c_str());
            continue;
        }
        if (!staged.bytes.empty() && f.maxBytes && staged.bytes.size() > f.maxBytes)
        {
            Log::Get().Write(ESB_LOG_WARN,
                             "discarded saved value for '%s': it exceeds the "
                             "declared max_bytes",
                             f.qualified.c_str());
            continue;
        }

        Slot& s = m_globals[fieldIndex];
        s.type     = staged.type;
        s.assigned = true;
        s.revision = ++m_clock;
        s.i64      = staged.i64;
        s.f64      = staged.f64;
        s.bytes    = staged.bytes;

        m_providers[providerIndex]->revision = s.revision;

        // Restoring counts as a change, so a consumer that subscribed before
        // the publisher loaded still learns the value.
        Notify(fieldIndex, ESB_AIRCRAFT_NONE);
    }
}

bool Registry::LoadState()
{
    if (m_statePath.empty())
        return false;

    FILE* f = std::fopen(m_statePath.c_str(), "rb");
    if (!f)
        return false;                  // no state yet is not an error

    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        text.append(buf, n);
    std::fclose(f);

    std::string error;
    const json::Value root = json::Parse(text, &error);
    if (!root.IsObject())
    {
        Log::Get().Write(ESB_LOG_WARN, "ignoring unreadable state file %s: %s",
                         m_statePath.c_str(), error.c_str());
        return false;
    }

    m_staged.clear();

    for (const auto& kv : root["fields"].Members())
    {
        const json::Value& entry = kv.second;
        if (!entry.IsObject())
            continue;

        StagedValue v;
        v.type = static_cast<uint32_t>(entry["type"].AsInt(0));

        switch (v.type)
        {
        case ESB_T_I64:
        case ESB_T_BOOL:
            v.i64 = entry["value"].AsInt(0);
            break;
        case ESB_T_F64:
            v.f64 = entry["value"].AsNumber(0.0);
            break;
        case ESB_T_STR:
        {
            const std::string& s = entry["value"].AsString();
            v.bytes.assign(s.begin(), s.end());
            break;
        }
        case ESB_T_BLOB:
        {
            // Hex, because a blob is not text and must survive a round trip
            // through a file a person might open in an editor.
            const std::string& hex = entry["value"].AsString();
            v.bytes.reserve(hex.size() / 2);
            for (size_t i = 0; i + 1 < hex.size(); i += 2)
            {
                const std::string byte = hex.substr(i, 2);
                v.bytes.push_back(static_cast<uint8_t>(
                    std::strtoul(byte.c_str(), nullptr, 16)));
            }
            break;
        }
        default:
            continue;                  // unknown type: drop it
        }

        m_staged[kv.first] = std::move(v);
    }

    Log::Get().Write(ESB_LOG_INFO, "loaded %u persisted value(s) from %s",
                     static_cast<unsigned>(m_staged.size()), m_statePath.c_str());

    // Providers that are already registered get theirs immediately; the rest
    // collect when they register.
    for (uint32_t i = 0; i < m_providers.size(); ++i)
        if (m_providers[i]->live)
            RestoreStagedFor(i);

    return true;
}

bool Registry::SaveState()
{
    if (m_statePath.empty())
        return false;

    json::Value fields = json::Value::Object();

    for (const auto& kv : m_globals)
    {
        const uint32_t fieldIndex = kv.first;
        if (fieldIndex >= m_fields.size())
            continue;

        const FieldDef& f = m_fields[fieldIndex];
        if (!(f.flags & ESB_F_PERSIST) || f.scope != ESB_SCOPE_GLOBAL)
            continue;

        const Slot& s = kv.second;
        if (!s.assigned)
            continue;

        json::Value entry = json::Value::Object();
        entry.Set("type", json::Value(static_cast<int64_t>(s.type)));

        switch (s.type)
        {
        case ESB_T_I64:
            entry.Set("value", json::Value(s.i64));
            break;
        case ESB_T_BOOL:
            entry.Set("value", json::Value(static_cast<int64_t>(s.i64 ? 1 : 0)));
            break;
        case ESB_T_F64:
            entry.Set("value", json::Value(s.f64));
            break;
        case ESB_T_STR:
            entry.Set("value", json::Value(std::string(
                reinterpret_cast<const char*>(s.bytes.data()), s.bytes.size())));
            break;
        case ESB_T_BLOB:
        {
            std::string hex;
            hex.reserve(s.bytes.size() * 2);
            for (uint8_t b : s.bytes)
            {
                char pair[3];
                std::snprintf(pair, sizeof pair, "%02x", b);
                hex += pair;
            }
            entry.Set("value", json::Value(hex));
            break;
        }
        default:
            continue;
        }

        fields.Set(f.qualified, entry);
    }

    // Values whose provider has not loaded this session are kept, not
    // silently dropped -- a user who temporarily removes a plugin should not
    // lose its saved state as a side effect.
    for (const auto& kv : m_staged)
    {
        if (fields.Has(kv.first))
            continue;

        const StagedValue& v = kv.second;
        json::Value entry = json::Value::Object();
        entry.Set("type", json::Value(static_cast<int64_t>(v.type)));

        switch (v.type)
        {
        case ESB_T_I64:
        case ESB_T_BOOL:
            entry.Set("value", json::Value(v.i64));
            break;
        case ESB_T_F64:
            entry.Set("value", json::Value(v.f64));
            break;
        case ESB_T_STR:
            entry.Set("value", json::Value(std::string(
                reinterpret_cast<const char*>(v.bytes.data()), v.bytes.size())));
            break;
        case ESB_T_BLOB:
        {
            std::string hex;
            for (uint8_t b : v.bytes)
            {
                char pair[3];
                std::snprintf(pair, sizeof pair, "%02x", b);
                hex += pair;
            }
            entry.Set("value", json::Value(hex));
            break;
        }
        default:
            continue;
        }

        fields.Set(kv.first, entry);
    }

    json::Value root = json::Value::Object();
    root.Set("version", json::Value(static_cast<int64_t>(1)));
    root.Set("fields", fields);

    const std::string text = json::Write(root, true);

    // Write to a temporary and rename, so a crash mid-write cannot leave a
    // truncated state file behind.
    const std::string temp = m_statePath + ".tmp";
    FILE* f = std::fopen(temp.c_str(), "wb");
    if (!f)
    {
        Log::Get().Write(ESB_LOG_ERROR, "cannot write %s", temp.c_str());
        return false;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);

    std::remove(m_statePath.c_str());
    if (std::rename(temp.c_str(), m_statePath.c_str()) != 0)
    {
        Log::Get().Write(ESB_LOG_ERROR, "cannot replace %s", m_statePath.c_str());
        return false;
    }

    m_stateDirty = false;
    return true;
}

void Registry::FlushStateIfDue(uint64_t nowMs)
{
    if (!m_stateDirty)
        return;
    if (nowMs - m_lastStateFlushMs < kStateFlushMs)
        return;

    m_lastStateFlushMs = nowMs;
    SaveState();
}

// --------------------------------------------------------------- diagnostics

std::string Registry::DumpProviders() const
{
    if (m_providers.empty())
        return "no providers registered";

    std::string out;
    char line[512];

    for (const auto& p : m_providers)
    {
        if (!p->live)
            continue;
        std::snprintf(line, sizeof line, "%-20s v%u.%u  %u field(s)  %s  %s\n",
                      p->id.c_str(), p->major, p->minor,
                      static_cast<unsigned>(p->fields.size()),
                      p->displayName.c_str(), p->contact.c_str());
        out += line;
    }

    return out.empty() ? "no providers registered" : out;
}

std::string Registry::DumpSchema(const char* providerId) const
{
    if (!providerId)
        return "usage: .esb schema <provider>";

    auto it = m_providerIndex.find(std::string(providerId));
    if (it == m_providerIndex.end() || !m_providers[it->second]->live)
        return std::string("no such provider: ") + providerId;

    static const char* kTypes[] = { "?", "i64", "f64", "bool", "str", "blob" };

    std::string out;
    char line[512];

    for (uint32_t fieldIndex : m_providers[it->second]->fields)
    {
        const FieldDef& f = m_fields[fieldIndex];
        std::snprintf(line, sizeof line, "%-28s %-5s %-8s %s%s %s\n",
                      f.qualified.c_str(),
                      kTypes[f.type <= ESB_T_BLOB ? f.type : 0],
                      f.scope == ESB_SCOPE_GLOBAL ? "global" : "aircraft",
                      (f.flags & ESB_F_SYNC)    ? "sync "    : "",
                      (f.flags & ESB_F_PERSIST) ? "persist " : "",
                      f.doc.c_str());
        out += line;
    }

    return out.empty() ? "provider declares no fields" : out;
}

namespace {

// Human-readable rendering of a stored value, for the dot commands and the
// dump file. Blobs and long strings are elided rather than flooding the
// message window.
std::string FormatSlot(const Slot& s)
{
    if (!s.assigned)
        return "<unset>";

    char buf[160];

    switch (s.type)
    {
    case ESB_T_I64:
        std::snprintf(buf, sizeof buf, "%lld",
                      static_cast<long long>(s.i64));
        return buf;
    case ESB_T_F64:
        std::snprintf(buf, sizeof buf, "%g", s.f64);
        return buf;
    case ESB_T_BOOL:
        return s.i64 ? "true" : "false";
    case ESB_T_STR:
    {
        std::string text(reinterpret_cast<const char*>(s.bytes.data()), s.bytes.size());
        if (text.size() > 96)
            text = text.substr(0, 93) + "...";
        // Keep it on one line: the EuroScope message window is line-oriented.
        for (char& c : text)
            if (c == '\n' || c == '\r' || c == '\t')
                c = ' ';
        return "\"" + text + "\"";
    }
    default:
        std::snprintf(buf, sizeof buf, "<blob %u bytes>",
                      static_cast<unsigned>(s.bytes.size()));
        return buf;
    }
}

} // namespace

std::string Registry::QualifiedName(ESB_FieldId f) const
{
    const FieldDef* fd = Field(f);
    return fd ? fd->qualified : std::string();
}

std::string Registry::DumpValue(const char* qualified, const char* callsign) const
{
    if (!qualified)
        return "usage: .esb get <provider>/<field> [callsign]";

    auto it = m_fieldIndex.find(std::string(qualified));
    if (it == m_fieldIndex.end())
        return std::string("no such field: ") + qualified;

    const uint32_t  fieldIndex = it->second;
    const FieldDef& fd         = m_fields[fieldIndex];
    const ProviderDef& owner   = *m_providers[fd.provider];

    if (!owner.live)
        return std::string("provider '") + owner.id + "' is not currently registered";

    const Slot* slot = nullptr;
    std::string where;

    if (fd.scope == ESB_SCOPE_GLOBAL)
    {
        auto v = m_globals.find(fieldIndex);
        if (v != m_globals.end())
            slot = &v->second;
        where = "global";
    }
    else
    {
        if (!callsign)
            return std::string(qualified) + " is aircraft-scoped: .esb get " +
                   qualified + " <callsign>";

        ESB_Aircraft ac = ESB_AIRCRAFT_NONE;
        const ESB_Status st = m_ac.Resolve(callsign, &ac);
        if (st != ESB_OK)
            return std::string("EuroScope is not showing an aircraft called ") + callsign;

        auto v = m_aircraft.find(AircraftKey(fieldIndex, AircraftTable::SlotOf(ac)));
        if (v != m_aircraft.end())
            slot = &v->second;
        where = callsign;
    }

    char line[512];
    std::snprintf(line, sizeof line, "%s [%s] = %s  (rev %llu, written by %s)",
                  qualified, where.c_str(),
                  slot ? FormatSlot(*slot).c_str() : "<unset>",
                  static_cast<unsigned long long>(slot ? slot->revision : 0),
                  owner.id.c_str());
    return line;
}

std::string Registry::DumpSubscriptions() const
{
    std::string out;
    char line[384];

    for (const auto& s : m_subs)
    {
        if (!s->live)
            continue;
        std::snprintf(line, sizeof line, "%-28s %llu delivered  %s\n",
                      s->field < m_fields.size()
                          ? m_fields[s->field].qualified.c_str() : "?",
                      static_cast<unsigned long long>(s->delivered),
                      s->modulePath.empty() ? "<unknown module>" : s->modulePath.c_str());
        out += line;
    }

    return out.empty() ? "no live subscriptions" : out;
}

std::string Registry::DumpAll() const
{
    std::string out = DumpStats() + "\n\n";

    for (const auto& p : m_providers)
    {
        if (!p->live)
            continue;

        char header[384];
        std::snprintf(header, sizeof header, "[%s] v%u.%u  %s  %s\n",
                      p->id.c_str(), p->major, p->minor,
                      p->displayName.c_str(), p->contact.c_str());
        out += header;

        for (uint32_t fieldIndex : p->fields)
        {
            const FieldDef& f = m_fields[fieldIndex];

            if (f.scope == ESB_SCOPE_GLOBAL)
            {
                auto v = m_globals.find(fieldIndex);
                out += "  " + f.qualified + " = " +
                       (v == m_globals.end() ? "<unset>" : FormatSlot(v->second)) + "\n";
                continue;
            }

            // Aircraft-scoped: one line per aircraft that actually holds a
            // value, which is normally far fewer than the aircraft on screen.
            for (const auto& sf : m_slotFields)
            {
                if (std::find(sf.second.begin(), sf.second.end(), fieldIndex) == sf.second.end())
                    continue;

                auto v = m_aircraft.find(AircraftKey(fieldIndex, sf.first));
                if (v == m_aircraft.end())
                    continue;

                char cs[32] = "?";
                uint32_t n = sizeof cs;
                m_ac.CallsignOf(AircraftTable::Handle(sf.first, 0), cs, &n);

                out += "  " + f.qualified + " [slot " + std::to_string(sf.first) + "] = " +
                       FormatSlot(v->second) + "\n";
            }
        }
        out += "\n";
    }

    out += DumpSubscriptions();
    return out;
}

std::string Registry::DumpStats() const
{
    size_t live = 0;
    for (const auto& p : m_providers)
        if (p->live)
            ++live;

    size_t liveSubs = 0;
    for (const auto& s : m_subs)
        if (s->live)
            ++liveSubs;

    char line[512];
    std::snprintf(line, sizeof line,
                  "providers %u live / %u known · fields %u · "
                  "aircraft %u live / %u slots (%u free) · "
                  "values %u global + %u per-aircraft · "
                  "subs %u live, %llu delivered, %llu shed · "
                  "clock %llu · ttl %llus grace %llus",
                  static_cast<unsigned>(live),
                  static_cast<unsigned>(m_providers.size()),
                  static_cast<unsigned>(m_fields.size()),
                  static_cast<unsigned>(m_ac.LiveCount()),
                  static_cast<unsigned>(m_ac.SlotCount()),
                  static_cast<unsigned>(m_ac.FreeCount()),
                  static_cast<unsigned>(m_globals.size()),
                  static_cast<unsigned>(m_aircraft.size()),
                  static_cast<unsigned>(liveSubs),
                  static_cast<unsigned long long>(m_dispatched),
                  static_cast<unsigned long long>(m_dropped),
                  static_cast<unsigned long long>(m_clock),
                  static_cast<unsigned long long>(m_ttlMs / 1000),
                  static_cast<unsigned long long>(m_graceMs / 1000));
    return line;
}

} // namespace esb
