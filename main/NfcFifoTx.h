#pragma once
#include <stdint.h>

namespace amiibo {
// Queued is deliberately not called TX complete: the listener owns the IRQ cache.
enum class TxResult : uint8_t { Queued, Invalid, Setup, Status, FifoError, Empty, Timeout, Write };
constexpr uint16_t FIFO_BYTES = 512;
constexpr uint16_t TX_CHUNK = 128;
constexpr uint32_t REFILL_TIMEOUT_US = 60000;

// IO exposes only non-destructive FIFO status reads; never consume reader IRQs.
// A short preload reduces I2C response startup latency. Refill while RF drains
// the FIFO, as in RFAL, without loading more than its current free capacity.
template <class IO>
constexpr TxResult queue_frame(IO& io, const uint8_t* data, uint16_t size)
{
    if (!data || !size || size > 540) return TxResult::Invalid;
    uint16_t loaded = size < TX_CHUNK ? size : TX_CHUNK;
    if (!io.clear() || !io.length(size) || !io.write(data, loaded) || !io.start()) {
        io.abort();
        return TxResult::Setup;
    }
    const uint32_t started = io.now();
    while (loaded < size) {
        if (static_cast<uint32_t>(io.now() - started) >= REFILL_TIMEOUT_US) {
            io.abort();
            return TxResult::Timeout;
        }
        uint16_t status = 0;
        if (!io.status(status)) { io.abort(); return TxResult::Status; }
        // M5 readFIFOStatus returns register 1 in the high byte, register 2 low.
        const uint16_t used = (status >> 8) | ((status & 0xC0) << 2);
        if ((status & 0x30) || used > FIFO_BYTES) { io.abort(); return TxResult::FifoError; }
        if (!used) { io.abort(); return TxResult::Empty; }
        uint16_t count = size - loaded;
        if (count > TX_CHUNK) count = TX_CHUNK;
        if (count > FIFO_BYTES - used) continue;
        if (!io.write(data + loaded, count)) { io.abort(); return TxResult::Write; }
        loaded += count;
    }
    return TxResult::Queued;
}
}  // namespace amiibo
