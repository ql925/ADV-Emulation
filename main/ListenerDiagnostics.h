#pragma once
#include <stdint.h>

namespace amiibo {
// States match EmulationLayerA's None/Off/Idle/Ready/Active/Halt. These are
// loop-observed transitions, not IRQ counts or a measurement of RF timing.
struct ListenerDiagnostics {
    uint32_t ready_to_active{}, ready_to_off{}, active_to_off{}, active_off_no_command{};
    uint32_t max_ready_ms{}, max_active_ms{};
    uint32_t max_service_us[6]{};
    uint8_t state{};
    uint32_t since{};
    bool had_command{};

    constexpr void clear_batch(uint32_t now)
    {
        ready_to_active = ready_to_off = active_to_off = active_off_no_command = 0;
        max_ready_ms = max_active_ms = 0;
        for (uint8_t i = 0; i < 6; ++i) max_service_us[i] = 0;
        since = now;
        had_command = false;
    }
    constexpr void command() { had_command = true; }
    constexpr uint32_t observe(uint8_t next, uint32_t now)
    {
        if (next == state) return 0;
        const uint32_t elapsed = now - since;
        if (state == 3) {
            if (elapsed > max_ready_ms) max_ready_ms = elapsed;
            if (next == 4) ++ready_to_active;
            if (next == 1) ++ready_to_off;
        }
        if (state == 4) {
            if (elapsed > max_active_ms) max_active_ms = elapsed;
            if (next == 1) {
                ++active_to_off;
                if (!had_command) ++active_off_no_command;
            }
        }
        if (next != 4) had_command = false;
        state = next;
        since = now;
        return elapsed;
    }
    constexpr void service(uint8_t previous_state, uint32_t elapsed)
    {
        if (previous_state < 6 && elapsed > max_service_us[previous_state])
            max_service_us[previous_state] = elapsed;
    }
};
}  // namespace amiibo
