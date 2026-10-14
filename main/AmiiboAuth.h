#pragma once
#include <stdint.h>

namespace amiibo {
constexpr unsigned PWD_OFFSET = 0x85 * 4;
constexpr unsigned PACK_OFFSET = 0x86 * 4;

// For a known Amiibo dump only: physical reads mask PWD and PACK with zeros.
// UID-derived authentication behavior referenced from BruceDevices/firmware.
// Restore only an entirely masked pair; preserve explicit credentials and writes.
constexpr bool restore_masked_credentials(uint8_t* memory)
{
    for (unsigned i = PWD_OFFSET; i < PACK_OFFSET + 2; ++i) {
        if (memory[i] != 0) return false;
    }
    memory[PWD_OFFSET]     = 0xAA ^ memory[1] ^ memory[4];
    memory[PWD_OFFSET + 1] = 0x55 ^ memory[2] ^ memory[5];
    memory[PWD_OFFSET + 2] = 0xAA ^ memory[4] ^ memory[6];
    memory[PWD_OFFSET + 3] = 0x55 ^ memory[5] ^ memory[7];
    memory[PACK_OFFSET] = memory[PACK_OFFSET + 1] = 0x80;
    return true;
}
}  // namespace amiibo
