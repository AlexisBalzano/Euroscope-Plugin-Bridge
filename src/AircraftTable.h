#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "esbridge.h"

namespace esb {

// Generational handles for aircraft. See ARCHITECTURE.md 6.
//
// A handle is (generation << 32) | slot. Generations start at 1, so a valid
// handle is never 0 and can never be confused with ESB_AIRCRAFT_NONE. When a
// callsign goes away the generation is bumped, which invalidates every handle
// any plugin is still holding -- that is what stops a recycled callsign from
// inheriting the previous occupant's data.
//
// A slot moves through three states:
//   used + live    EuroScope can currently see it
//   used + dead    retired; kept briefly so a reconnect lands on the same slot
//   unused         reclaimed onto the free list, ready for a different callsign
class AircraftTable
{
public:
    static uint32_t     SlotOf(ESB_Aircraft h) { return static_cast<uint32_t>(h & 0xFFFFFFFFull); }
    static uint32_t     GenerationOf(ESB_Aircraft h) { return static_cast<uint32_t>(h >> 32); }
    static ESB_Aircraft Handle(uint32_t slot, uint32_t gen)
    {
        return (static_cast<ESB_Aircraft>(gen) << 32) | slot;
    }

    struct SweepResult
    {
        std::vector<uint32_t> retiredSlots;   // whose values must now be dropped
        size_t                reclaimed = 0;  // entries returned to the free list
    };

    // Called by the EuroScope layer for every callsign it can currently see.
    ESB_Aircraft Observe(const char* callsign, uint64_t nowMs);

    // Bumps the generation and marks the slot dead. Returns false if the
    // callsign was not tracked. On success *outSlot receives the slot whose
    // values the registry must drop.
    bool Retire(const char* callsign, uint32_t* outSlot);

    // EuroScope does not always deliver a clean disconnect, so anything that
    // stops being observed has to age out on its own.
    //
    //   ttlMs    a live entry unseen this long is retired, exactly as if a
    //            disconnect had arrived
    //   graceMs  a dead entry lingers this long before its slot is reclaimed,
    //            so a quick reconnect keeps the same slot and its first-seen
    SweepResult Sweep(uint64_t nowMs, uint64_t ttlMs, uint64_t graceMs);

    // Resolve only -- never creates. Mirrors ESB_Api_v1::aircraft.
    ESB_Status Resolve(const char* callsign, ESB_Aircraft* out) const;

    ESB_Status CallsignOf(ESB_Aircraft h, char* buf, uint32_t* ioBytes) const;

    // Usable only while the slot is in use AND live AND the generation still
    // matches. Anything else is ESB_E_STALE_AIRCRAFT.
    ESB_Status Validate(ESB_Aircraft h) const;

    size_t LiveCount() const;
    size_t SlotCount() const { return m_slots.size(); }
    size_t FreeCount() const { return m_free.size(); }

private:
    struct Entry
    {
        std::string callsign;
        uint32_t    generation  = 0;
        bool        used        = false;
        bool        live        = false;
        uint64_t    firstSeenMs = 0;
        uint64_t    lastSeenMs  = 0;
    };

    std::vector<Entry>                       m_slots;
    std::vector<uint32_t>                    m_free;
    std::unordered_map<std::string, uint32_t> m_index;   // callsign -> slot
};

} // namespace esb
