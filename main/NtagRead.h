#pragma once
#include <stdint.h>
#include "AmiiboAuth.h"

namespace amiibo {
constexpr uint16_t TAG_BYTES = 540;
constexpr uint16_t PAGE_COUNT = TAG_BYTES / 4;
constexpr uint16_t AUTH0_OFFSET = 0x83 * 4 + 3;
constexpr uint16_t ACCESS_OFFSET = 0x84 * 4;
constexpr uint16_t DYNAMIC_LOCK_OFFSET = 0x82 * 4;

// READ/FAST_READ never disclose PWD or PACK, even after PWD_AUTH (NXP
// NTAG213/215/216, sections 8.8 and 10.2/10.3). Page 0x86's last two bytes
// are RFUI, not dynamic lock bytes; dynamic locks are in page 0x82.
constexpr unsigned PROTECTED_FROM = PWD_OFFSET;
constexpr unsigned PROTECTED_TO   = PACK_OFFSET + 2;  // exclusive

constexpr bool protected_offset(uint16_t offset)
{
    return offset >= PROTECTED_FROM && offset < PROTECTED_TO;
}

constexpr uint16_t fast_read_size(uint16_t first, uint16_t last)
{
    return first <= last && last < PAGE_COUNT ? 4 * (last - first + 1) : 0;
}

constexpr uint8_t public_byte(const uint8_t* memory, uint16_t offset)
{
    offset %= TAG_BYTES;
    return protected_offset(offset) ? 0 : memory[offset];
}

constexpr void read_pages(const uint8_t* memory, uint16_t first, uint8_t* out, uint16_t count,
                          uint16_t accessible_pages = PAGE_COUNT)
{
    // Callers pass a valid NTAG215 page and at most TAG_BYTES bytes. Normalize
    // once, then wrap the cursor without a division for every response byte.
    if (!accessible_pages || accessible_pages > PAGE_COUNT) return;
    const uint16_t accessible_bytes = 4 * accessible_pages;
    uint16_t offset = (4 * first) % accessible_bytes;
    for (uint16_t i = 0; i < count; ++i) {
        out[i] = protected_offset(offset) ? 0 : memory[offset];
        if (++offset == accessible_bytes) offset = 0;
    }
}

// FAST_READ masks the same bytes as READ: the copied range is the raw memory, and only
// the protected bytes inside that range are cleared. This is deliberately not a page-wise
// operation, because a range starting mid-page must keep the bytes that precede the
// protected window.
constexpr void mask_protected(uint8_t* out, uint16_t first_offset, uint16_t count)
{
    const uint32_t end = static_cast<uint32_t>(first_offset) + count;
    const uint32_t from = first_offset > PROTECTED_FROM ? first_offset : PROTECTED_FROM;
    const uint32_t to = end < PROTECTED_TO ? end : PROTECTED_TO;
    for (uint32_t offset = from; offset < to; ++offset) out[offset - first_offset] = 0;
}
}  // namespace amiibo
