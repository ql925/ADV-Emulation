#pragma once
#include <stdint.h>

// 0: paper_name_plate baseline (512/fc, MRT=4); 1: previous short-mask experiment (64/fc, MRT=2).
#ifndef AMIIBO_RX_PROFILE
#define AMIIBO_RX_PROFILE 0
#endif
#if AMIIBO_RX_PROFILE != 0 && AMIIBO_RX_PROFILE != 1
#error AMIIBO_RX_PROFILE must be 0 (Paper) or 1 (short-mask comparison)
#endif

namespace amiibo {
struct ListenerTiming {
    uint8_t fdel, mask;
    bool long_step;
    constexpr uint16_t step_fc() const { return long_step ? 512 : 64; }
};
constexpr ListenerTiming PAPER_TIMING{5, 4, true};
constexpr ListenerTiming SHORT_TIMING{5, 2, false};
constexpr ListenerTiming LISTENER_TIMING = AMIIBO_RX_PROFILE == 0 ? PAPER_TIMING : SHORT_TIMING;
constexpr const char* RX_PROFILE_NAME = AMIIBO_RX_PROFILE == 0 ? "PAPER_512_4" : "SHORT_64_2";
struct TimingSnapshot { uint8_t passive{}, control{}, mask{}; bool valid{}; };

// Configure/read back once at startup or an off-field resume. Do not probe these
// registers for logging while a reader is waiting. All unrelated bits are retained.
template <class IO>
constexpr bool apply_listener_timing(IO& io, ListenerTiming timing, TimingSnapshot& snapshot)
{
    snapshot.valid = false;
    if (timing.fdel > 15) return false;
    uint8_t passive{}, control{};
    if (!io.read_passive(passive) || !io.read_control(control)) return false;
    const uint8_t target = (passive & 0x0F) | (timing.fdel << 4);
    if (target != passive && !io.write_passive(target)) return false;
    const uint8_t updated = (control & 0xF7) | (timing.long_step ? 0x08 : 0);
    if (!io.write_control(updated) || !io.write_mask(timing.mask)) return false;
    if (!io.read_passive(snapshot.passive) || !io.read_control(snapshot.control) || !io.read_mask(snapshot.mask))
        return false;
    snapshot.valid = (snapshot.passive >> 4) == timing.fdel &&
                     (snapshot.control & 0x08) == (updated & 0x08) && snapshot.mask == timing.mask;
    return snapshot.valid;
}
}  // namespace amiibo
