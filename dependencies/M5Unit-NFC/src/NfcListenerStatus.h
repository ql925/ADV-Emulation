#pragma once
#include <stdint.h>

namespace m5 {
namespace nfc {
namespace detail {
// Values stored in RAM diagnostic event 14; no frame contents are recorded.
enum class ListenerIoStep : uint8_t {
    Interrupt = 1, LoadConfig, Start, Off, Idle, Bitrate, PassiveTarget,
    Ready, Active, Halt, ReceiveRecovery, Oscillator, Stop
};
constexpr const char* listener_io_step_name(uint32_t step)
{
    switch (static_cast<ListenerIoStep>(step)) {
        case ListenerIoStep::Interrupt: return "IRQ";
        case ListenerIoStep::LoadConfig: return "PT_CONFIG";
        case ListenerIoStep::Start: return "START";
        case ListenerIoStep::Off: return "OFF";
        case ListenerIoStep::Idle: return "IDLE";
        case ListenerIoStep::Bitrate: return "BITRATE";
        case ListenerIoStep::PassiveTarget: return "PTA";
        case ListenerIoStep::Ready: return "READY";
        case ListenerIoStep::Active: return "ACTIVE";
        case ListenerIoStep::Halt: return "HALT";
        case ListenerIoStep::ReceiveRecovery: return "RX_RECOVERY";
        case ListenerIoStep::Oscillator: return "OSCILLATOR";
        case ListenerIoStep::Stop: return "STOP";
        default: return "none";
    }
}
}  // namespace detail
}  // namespace nfc
}  // namespace m5
