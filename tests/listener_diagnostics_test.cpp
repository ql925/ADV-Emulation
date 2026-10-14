#include "../main/ListenerDiagnostics.h"

constexpr bool failed_activation()
{
    amiibo::ListenerDiagnostics d;
    d.observe(2, 26368);
    d.observe(3, 26373);
    if (d.observe(1, 26420) != 47 || d.ready_to_off != 1 || d.max_ready_ms != 47) return false;
    d.observe(2, 28693);
    d.observe(3, 28699);
    d.observe(4, 28773);
    d.observe(1, 28840);
    return d.ready_to_active == 1 && d.active_to_off == 1 && d.active_off_no_command == 1 &&
           d.max_ready_ms == 74 && d.max_active_ms == 67;
}
static_assert(failed_activation(), "Real log: Ready dropouts and Active without an application command");

constexpr bool commands_and_new_batches()
{
    amiibo::ListenerDiagnostics d;
    d.observe(3, 100);
    // A callback may occur in the same update that transitions Ready to Active.
    d.command();
    d.observe(4, 110);
    d.service(3, 4652);
    d.observe(1, 150);
    if (d.active_off_no_command || d.max_service_us[3] != 4652) return false;
    d.clear_batch(200);
    if (d.ready_to_active || d.active_to_off || d.max_service_us[3]) return false;
    d.observe(3, 0xFFFFFFF0);
    d.observe(4, 0x10);
    d.observe(1, 0x30);
    return d.max_ready_ms == 32 && d.max_active_ms == 32 && d.active_off_no_command == 1;
}
static_assert(commands_and_new_batches(), "Callback ordering, batch reset, per-state service and clock wrap");

constexpr bool new_selection_after_idle()
{
    amiibo::ListenerDiagnostics d;
    d.observe(4, 100);
    d.command();
    d.observe(2, 110);
    d.observe(3, 120);
    d.observe(4, 130);
    d.observe(1, 140);
    return d.active_off_no_command == 1;
}
static_assert(new_selection_after_idle(), "An earlier reader command cannot hide an empty later activation");
