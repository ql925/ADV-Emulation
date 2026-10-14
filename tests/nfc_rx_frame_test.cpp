#include "../dependencies/M5Unit-NFC/src/NfcRxFrame.h"
using m5::nfc::detail::active_payload_size;
constexpr bool rx_boundaries()
{
    for (unsigned bytes = 0; bytes <= 600; ++bytes) {
        const auto n = active_payload_size(bytes, 0, 256);
        if (n != (bytes >= 3 && bytes <= 258 ? bytes - 2 : 0)) return false;
        for (unsigned bits = 1; bits <= 7; ++bits)
            if (active_payload_size(bytes, bits, 256)) return false;
    }
    return active_payload_size(3, 0, 1) == 1 &&
           active_payload_size(8, 0, 6) == 6 &&
           active_payload_size(512, 0, 510) == 510 &&
           active_payload_size(513, 0, 600) == 0 &&
           active_payload_size(3, 0, 0) == 0;
}
static_assert(rx_boundaries(), "CRC removal, GET_VERSION/WRITE, partial bytes, capacity and hardware limits");
