#pragma once
#include <stdint.h>

namespace amiibo {
enum class RebuildStep : uint8_t { None, Configure, Begin, Timing, Recover };

// Startup and maintenance use the same checked sequence. IO records failures and
// stops a partially initialized listener before the caller schedules a retry.
template <class IO>
constexpr bool rebuild_listener(IO& io)
{
    RebuildStep failed = RebuildStep::None;
    if (!io.configure()) failed = RebuildStep::Configure;
    else if (!io.begin()) failed = RebuildStep::Begin;
    else if (!io.timing()) failed = RebuildStep::Timing;
    if (failed == RebuildStep::None) return true;
    io.failed(failed);
    if (!io.end()) io.failed(RebuildStep::Recover);
    return false;
}
}  // namespace amiibo
