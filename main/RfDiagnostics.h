#pragma once
#include <stdint.h>

namespace amiibo {
// Fixed-size, allocation-free recorder. Values contain metadata, never frame data.
struct RfDiagnostics {
    enum Kind : uint8_t { Irq = 1, Trace, Bitrate, Pta, FifoSize, FifoRead,
                          RxError, Expiry, IrqFailure, IdleReceive, SoftwareCrc, EofRxe, RxAccepted, ListenerIoFailure };
    struct Event { uint32_t us, a, b, c; uint8_t kind, state; };
    static constexpr uint16_t CAPACITY = 256;
    Event events[CAPACITY]{};
    uint16_t next{}, count{};
    uint32_t total{}, irq_bits[32]{}, irq_reads{}, irq_failures{}, rx_errors{}, expiry{};
    uint32_t fifo_failures{}, max_units_us{}, max_listener_us{}, max_gap_us{}, slow_services{};
    uint32_t eof_rxe{}, rx_accepted{};
    uint32_t unselected_rx_discarded{}, io_failures{}, last_io_step{};
    uint32_t previous_start{};
    bool has_previous{};

    constexpr void clear()
    {
        next = count = 0;
        total = irq_reads = irq_failures = rx_errors = expiry = fifo_failures = 0;
        max_units_us = max_listener_us = max_gap_us = slow_services = 0;
        eof_rxe = rx_accepted = 0;
        unselected_rx_discarded = io_failures = last_io_step = 0;
        has_previous = false;
        for (auto& value : irq_bits) value = 0;
    }
    constexpr void record(uint32_t us, uint8_t state, uint8_t kind,
                          uint32_t a, uint32_t b, uint32_t c)
    {
        events[next] = {us, a, b, c, kind, state};
        next = (next + 1) % CAPACITY;
        if (count < CAPACITY) ++count;
        ++total;
        if (kind == Irq) {
            ++irq_reads;
            for (unsigned i = 0; i < 32; ++i) if (a & (uint32_t(1) << i)) ++irq_bits[i];
        }
        if (kind == IrqFailure) ++irq_failures;
        if (kind == RxError) ++rx_errors;
        if (kind == Expiry) ++expiry;
        if (kind == EofRxe) ++eof_rxe;
        if (kind == RxAccepted) ++rx_accepted;
        if (kind == IdleReceive) ++unselected_rx_discarded;
        if (kind == ListenerIoFailure) { ++io_failures; last_io_step = a; }
        if ((kind == FifoSize || kind == FifoRead) && !a) ++fifo_failures;
    }
    constexpr uint32_t irq_count(uint32_t mask) const
    {
        uint32_t sum = 0;
        for (unsigned i = 0; i < 32; ++i) if (mask & (uint32_t(1) << i)) sum += irq_bits[i];
        return sum;
    }
    constexpr const Event& at(uint16_t index) const
    {
        return events[(next + CAPACITY - count + index) % CAPACITY];
    }
    constexpr void service(uint32_t start, uint32_t units_us, uint32_t listener_us, bool running)
    {
        if (!running) { has_previous = false; return; }
        if (has_previous) {
            const uint32_t gap = start - previous_start;
            if (gap > max_gap_us) max_gap_us = gap;
        }
        previous_start = start;
        has_previous = true;
        if (units_us > max_units_us) max_units_us = units_us;
        if (listener_us > max_listener_us) max_listener_us = listener_us;
        if (units_us + listener_us >= 2000) ++slow_services;
    }
};
static_assert(sizeof(RfDiagnostics::Event) <= 20, "Bound RF recorder RAM cost");
}  // namespace amiibo
