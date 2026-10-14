#pragma once
#include <stdint.h>
namespace m5 { namespace nfc { namespace detail {
// Active NFC-A command frames include two CRC bytes and must be byte aligned.
// Capacity applies to the payload; CRC remains in FIFO until transmit/cleanup.
constexpr uint16_t active_payload_size(uint16_t bytes, uint8_t bits, uint16_t capacity)
{
    return bits == 0 && bytes > 2 && bytes <= 512 && bytes - 2 <= capacity
        ? static_cast<uint16_t>(bytes - 2) : 0;
}
}}}
