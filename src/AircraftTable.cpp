#include "AircraftTable.h"

#include <cstring>

namespace esb {

ESB_Aircraft AircraftTable::Observe(const char* callsign, uint64_t nowMs)
{
    if (!callsign || !*callsign)
        return ESB_AIRCRAFT_NONE;

    const std::string cs(callsign);

    auto it = m_index.find(cs);
    if (it != m_index.end())
    {
        Entry& e = m_slots[it->second];
        if (!e.live)
        {
            // Seen again after a retire. The generation was already bumped when
            // it went away, so the slot comes back to life under the new
            // generation and handles from the previous life stay stale.
            e.live        = true;
            e.firstSeenMs = nowMs;
        }
        e.lastSeenMs = nowMs;
        return Handle(it->second, e.generation);
    }

    uint32_t slot;
    if (!m_free.empty())
    {
        // Reuse a reclaimed slot. Its generation keeps climbing from where the
        // previous occupant left it, so an old handle can never match.
        slot = m_free.back();
        m_free.pop_back();

        Entry& e = m_slots[slot];
        e.callsign    = cs;
        ++e.generation;
        e.used        = true;
        e.live        = true;
        e.firstSeenMs = nowMs;
        e.lastSeenMs  = nowMs;

        m_index.emplace(cs, slot);
        return Handle(slot, e.generation);
    }

    Entry e;
    e.callsign    = cs;
    e.generation  = 1;              // never 0: keeps handles distinct from NONE
    e.used        = true;
    e.live        = true;
    e.firstSeenMs = nowMs;
    e.lastSeenMs  = nowMs;

    slot = static_cast<uint32_t>(m_slots.size());
    m_slots.push_back(std::move(e));
    m_index.emplace(cs, slot);
    return Handle(slot, 1);
}

bool AircraftTable::Retire(const char* callsign, uint32_t* outSlot)
{
    if (!callsign || !*callsign)
        return false;

    auto it = m_index.find(std::string(callsign));
    if (it == m_index.end())
        return false;

    Entry& e = m_slots[it->second];
    if (!e.live)
        return false;               // already retired; nothing more to drop

    e.live = false;
    ++e.generation;                 // invalidates every handle held anywhere

    if (outSlot)
        *outSlot = it->second;
    return true;
}

AircraftTable::SweepResult AircraftTable::Sweep(uint64_t nowMs, uint64_t ttlMs,
                                                uint64_t graceMs)
{
    SweepResult result;

    for (uint32_t slot = 0; slot < m_slots.size(); ++slot)
    {
        Entry& e = m_slots[slot];
        if (!e.used)
            continue;

        // Guard against a clock that has not advanced past first sight.
        const uint64_t age = (nowMs > e.lastSeenMs) ? (nowMs - e.lastSeenMs) : 0;

        if (e.live)
        {
            if (age >= ttlMs)
            {
                e.live = false;
                ++e.generation;
                result.retiredSlots.push_back(slot);
            }
            continue;
        }

        if (age >= ttlMs + graceMs)
        {
            m_index.erase(e.callsign);
            e.callsign.clear();
            e.used = false;
            m_free.push_back(slot);
            ++result.reclaimed;
        }
    }

    return result;
}

ESB_Status AircraftTable::Resolve(const char* callsign, ESB_Aircraft* out) const
{
    if (!callsign || !out)
        return ESB_E_INVALID_ARG;

    auto it = m_index.find(std::string(callsign));
    if (it == m_index.end())
        return ESB_E_UNKNOWN_AIRCRAFT;

    const Entry& e = m_slots[it->second];
    if (!e.used || !e.live)
        return ESB_E_UNKNOWN_AIRCRAFT;

    *out = Handle(it->second, e.generation);
    return ESB_OK;
}

ESB_Status AircraftTable::Validate(ESB_Aircraft h) const
{
    const uint32_t slot = SlotOf(h);
    const uint32_t gen  = GenerationOf(h);

    if (h == ESB_AIRCRAFT_NONE || slot >= m_slots.size())
        return ESB_E_UNKNOWN_AIRCRAFT;

    const Entry& e = m_slots[slot];
    if (!e.used || !e.live || e.generation != gen)
        return ESB_E_STALE_AIRCRAFT;

    return ESB_OK;
}

ESB_Status AircraftTable::CallsignOf(ESB_Aircraft h, char* buf, uint32_t* ioBytes) const
{
    const ESB_Status st = Validate(h);
    if (st != ESB_OK)
        return st;
    if (!ioBytes)
        return ESB_E_INVALID_ARG;

    const std::string& cs   = m_slots[SlotOf(h)].callsign;
    const uint32_t     need = static_cast<uint32_t>(cs.size()) + 1;   // with NUL
    const uint32_t     have = *ioBytes;

    *ioBytes = need;
    if (!buf || have < need)
        return ESB_E_BUFFER_TOO_SMALL;

    std::memcpy(buf, cs.c_str(), need);
    return ESB_OK;
}

size_t AircraftTable::LiveCount() const
{
    size_t n = 0;
    for (const Entry& e : m_slots)
        if (e.used && e.live)
            ++n;
    return n;
}

} // namespace esb
