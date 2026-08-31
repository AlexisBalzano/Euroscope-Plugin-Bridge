#pragma once

#include <cstdint>
#include <string>

#include "esbridge.h"

namespace esb {

// Rotating log file, written beside the bridge DLL.
//
// The point of this is attribution. A plugin author whose subscriber faulted,
// or whose provider id collided, needs to find out without attaching a
// debugger to EuroScope -- and a user reporting "the bridge broke" needs a
// file to attach. Every line a provider causes carries that provider's id and
// contact string, so a report routes to the right author (ARCHITECTURE.md 12).
class Log
{
public:
    static Log& Get();

    // Rotates once the file passes ~1 MB; keeps one previous generation.
    void Write(int32_t level, const char* fmt, ...);

    const std::string& Path() const { return m_path; }
    uint32_t           Count(int32_t level) const;

private:
    Log();
    ~Log();

    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void Rotate();

    std::string m_path;
    void*       m_file  = nullptr;     // FILE*, kept opaque to avoid <cstdio>
    uint32_t    m_bytes = 0;
    uint32_t    m_counts[4] = { 0, 0, 0, 0 };
};

} // namespace esb
