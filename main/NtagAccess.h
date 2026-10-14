#pragma once
#include "NtagRead.h"

namespace amiibo {
// NXP NTAG213/215/216: static locks cover pages 3..15; NTAG215 dynamic
// locks cover 16..129 in 16-page groups (the final group has two pages).
constexpr bool permanently_locked(const uint8_t* memory, uint8_t page)
{
    if (page >= 3 && page <= 7) return memory[10] & (1u << page);
    if (page >= 8 && page <= 15) return memory[11] & (1u << (page - 8));
    if (page >= 16 && page <= 129) return memory[DYNAMIC_LOCK_OFFSET] & (1u << ((page - 16) / 16));
    return false;
}
constexpr bool password_required(const uint8_t* memory, uint8_t page, bool authenticated)
{
    return !authenticated && memory[AUTH0_OFFSET] < PAGE_COUNT && page >= memory[AUTH0_OFFSET];
}
constexpr void write_static_locks(uint8_t* memory, uint8_t low, uint8_t high)
{
    const uint8_t previous = memory[10];
    uint8_t low_mask = 0xFF, high_mask = 0xFF;
    if (previous & 1) low_mask &= 0xF7;                     // Freeze L_CC.
    if (previous & 2) { low_mask &= 0x0F; high_mask &= 0xFC; } // Freeze L4..L9.
    if (previous & 4) high_mask &= 0x03;                    // Freeze L10..L15.
    memory[10] |= low & low_mask;
    memory[11] |= high & high_mask;
}
constexpr void write_dynamic_locks(uint8_t* memory, uint8_t locks, uint8_t blocks)
{
    const uint8_t previous_blocks = memory[DYNAMIC_LOCK_OFFSET + 2];
    uint8_t mask = 0xFF;
    for (uint8_t group = 0; group < 4; ++group)
        if (previous_blocks & (1u << group)) mask &= static_cast<uint8_t>(~(3u << (group * 2)));
    memory[DYNAMIC_LOCK_OFFSET] |= locks & mask;
    memory[DYNAMIC_LOCK_OFFSET + 2] |= blocks & 0x0F;
    // Byte 1 and byte 3 are RFUI; retain the supplied dump's values.
}
}  // namespace amiibo
