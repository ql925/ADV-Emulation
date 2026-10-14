#include "../main/ListenerTiming.h"
struct TimingIO {
    uint8_t passive{0xAD}, control{0xB7}, mask{};
    unsigned calls{}, fail_at{};
    bool corrupt{};
    constexpr bool ok() { return ++calls != fail_at; }
    constexpr bool read_passive(uint8_t& v) { if (!ok()) return false; v = passive; return true; }
    constexpr bool read_control(uint8_t& v) { if (!ok()) return false; v = control; return true; }
    constexpr bool read_mask(uint8_t& v) { if (!ok()) return false; v = mask; return true; }
    constexpr bool write_passive(uint8_t v) { if (!ok()) return false; passive = v; return true; }
    constexpr bool write_control(uint8_t v) { if (!ok()) return false; control = v; return true; }
    constexpr bool write_mask(uint8_t v) { if (!ok()) return false; mask = corrupt ? 0 : v; return true; }
};
constexpr bool timing_profiles()
{
    const amiibo::ListenerTiming profiles[] = {amiibo::PAPER_TIMING, amiibo::SHORT_TIMING};
    for (auto timing : profiles) {
        TimingIO io;
        amiibo::TimingSnapshot snapshot;
        if (!amiibo::apply_listener_timing(io, timing, snapshot) || !snapshot.valid || snapshot.passive != 0x5D ||
            (snapshot.control & 0xF7) != 0xB7 || snapshot.mask != timing.mask || io.calls != 8) return false;
        if ((snapshot.control & 8) != (timing.long_step ? 8 : 0)) return false;
        for (unsigned fail = 1; fail <= 8; ++fail) {
            TimingIO bad; bad.fail_at = fail; snapshot.valid = true;
            if (amiibo::apply_listener_timing(bad, timing, snapshot) || snapshot.valid || bad.calls != fail) return false;
        }
        TimingIO corrupt; corrupt.corrupt = true;
        if (amiibo::apply_listener_timing(corrupt, timing, snapshot) || snapshot.valid) return false;
        TimingIO unchanged; unchanged.passive = 0x5C;
        if (!amiibo::apply_listener_timing(unchanged, timing, snapshot) || unchanged.calls != 7) return false;
    }
    return amiibo::PAPER_TIMING.step_fc() == 512 && amiibo::SHORT_TIMING.step_fc() == 64;
}
static_assert(timing_profiles(), "Paired FDEL/MRT profiles preserve other bits, read back and fail closed on I/O errors");
