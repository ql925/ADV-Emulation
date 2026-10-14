#include "../main/RfDiagnostics.h"
constexpr bool rf_recording()
{
    amiibo::RfDiagnostics d;
    for (uint32_t i = 0; i < 300; ++i) d.record(i, 3, d.Irq, 0x80000001u, 0, 0);
    if (d.total != 300 || d.count != 256 || d.at(0).us != 44 || d.at(255).us != 299) return false;
    if (d.irq_count(1) != 300 || d.irq_count(0x80000000u) != 300) return false;
    d.record(301, 4, d.FifoRead, 0, 0, 16);
    d.record(302, 4, d.IrqFailure, 0, 0, 0);
    d.record(303, 4, d.RxError, 1, 2, 3);
    if (d.fifo_failures != 1 || d.irq_failures != 1 || d.rx_errors != 1) return false;
    d.service(0xFFFFFF00u, 100, 200, true);
    d.service(0x100u, 1200, 900, true);
    if (d.max_gap_us != 512 || d.max_units_us != 1200 || d.max_listener_us != 900 || d.slow_services != 1) return false;
    d.service(10000, 0, 0, false);
    d.service(20000, 100, 100, true);
    if (d.max_gap_us != 512) return false;
    for (unsigned i = 0; i < 300; ++i) {
        d.record(i, 4, d.EofRxe, 0x10080000, 0, 0);
        d.record(i, 4, d.RxAccepted, 1, 0, 0);
        d.record(i, 2, d.IdleReceive, 0x10080000, 0, 0);
        d.record(i, 4, d.ListenerIoFailure, 6, 0, 0);
    }
    if (d.eof_rxe != 300 || d.rx_accepted != 300) return false;
    if (d.unselected_rx_discarded != 300 || d.io_failures != 300 || d.last_io_step != 6) return false;
    d.clear();
    if (d.eof_rxe || d.rx_accepted) return false;
    if (d.unselected_rx_discarded || d.io_failures || d.last_io_step) return false;
    return !d.count && !d.total && !d.irq_count(1) && !d.fifo_failures && !d.has_previous;
}
static_assert(rf_recording(), "RF ring rollover, aggregate counters, timing wrap and maintenance exclusion");
