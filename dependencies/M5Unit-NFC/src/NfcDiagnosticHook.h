#pragma once
#include <stdint.h>
#define M5_NFC_RAM_DIAGNOSTICS_VERSION 3
// Project-local observer. Never read registers, print, allocate or write Flash here.
extern "C" void m5_nfc_ram_event(uint8_t, uint32_t, uint32_t, uint32_t) __attribute__((weak));
inline void nfc_ram_event(uint8_t kind, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0)
{
    if (m5_nfc_ram_event) m5_nfc_ram_event(kind, a, b, c);
}
