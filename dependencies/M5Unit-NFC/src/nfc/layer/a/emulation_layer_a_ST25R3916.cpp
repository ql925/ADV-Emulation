#include "NfcDiagnosticHook.h"
#include "NfcRxFrame.h"
#include "NfcListenerStatus.h"
/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
/*!
  @file emulation_layer_a_ST25R3916.cpp
  @brief ST25R3916 NFC-A emulation adapter for common layer
*/
#include "nfc/layer/a/emulation_layer_a.hpp"
#include "nfc/layer/emulation_trace.hpp"
#include "unit/unit_ST25R3916.hpp"
#include <M5Utility.hpp>

using namespace m5::unit;
using namespace m5::unit::st25r3916;
using namespace m5::unit::st25r3916::command;
using namespace m5::unit::st25r3916::regval;
using namespace m5::nfc;
using namespace m5::nfc::a;
using namespace m5::nfc::a::mifare;
using namespace m5::nfc::a::mifare::classic;
using m5::nfc::detail::ListenerIoStep;

// clang-format off
#pragma GCC optimize("O3")
// clang-format on

namespace {
// An ISO14443-4 reader may send up to its frame size (256 bytes by default), so a listener that
// hands unknown commands to the application has to be able to hold one
constexpr uint16_t RX_BUFFER_SIZE{256};

// Events kept by the trace, see emulation_trace.hpp
enum : uint8_t { EV_OFF, EV_IDLE, EV_READY, EV_ACTIVE, EV_HALT, EV_HALT_IRQ, EV_READY_IRQ, EV_PTA };
constexpr const char* trace_names[] = {"OFF", "IDLE", "READY", "ACTIVE", "HALT", "halt_irq", "ready_irq", "PTA"};

inline bool is_eof(const uint32_t irq)
{
    return (irq & I_eof32);
}

constexpr uint8_t mode_mask{0xFB};  // targ,om0123,ar01
constexpr uint8_t mode_bitrate_detection = targ | (0x09u << 3);
constexpr uint8_t mode_listen_nfc_a      = targ | (0x01 << 3);

constexpr uint32_t mode_irq    = I_wu_a32 | I_wu_ax32 | I_rxe_pta32;
// RXE can arrive together with SELECT completion before software reaches Active.
constexpr uint32_t default_irq = I_nfct32 | I_rxs32 | I_rxe32 | I_eon32 | I_eof32 | I_crc32 | I_err132 | I_err232 | I_par32;
}  // namespace

namespace m5 {
namespace nfc {
struct ListenerST25R3916ForA final : EmulationLayerA::Adapter {
    explicit ListenerST25R3916ForA(EmulationLayerA& layer, UnitST25R3916& ref) : _layer{layer}, _u{ref}
    {
    }
    virtual bool start_emulation(const m5::nfc::a::PICC& picc) override;
    virtual bool stop_emulation() override;
    virtual bool transmit(const uint8_t* tx, const uint16_t tx_len, const uint32_t timeout_ms) override;
    EmulationLayerA::State initial_state() const override { return _initial_state; }

    //
    virtual EmulationLayerA::State update_off() override;
    virtual EmulationLayerA::State update_idle() override;
    virtual EmulationLayerA::State update_ready() override;
    virtual EmulationLayerA::State update_active() override;
    virtual EmulationLayerA::State update_halt() override;

    inline virtual EmulationLayerA::State reset_to_off() override
    {
        return goto_off();
    }

    inline virtual bool consume_rf_activity() override
    {
        const bool ret = _rf_activity;
        _rf_activity   = false;
        return ret;
    }

    //
    EmulationLayerA::State goto_state(const EmulationLayerA::State s);
    EmulationLayerA::State goto_off();
    EmulationLayerA::State goto_idle();
    EmulationLayerA::State goto_ready();
    EmulationLayerA::State goto_active();
    EmulationLayerA::State goto_halt();

    bool load_config(const m5::nfc::a::PICC& picc);

    //
    bool get_irq(const uint32_t mask_bits, uint32_t& irq);

    bool is_extra_field(bool& present);
    EmulationLayerA::State failure(ListenerIoStep step);
    bool recover_receive(bool disable_tx = false);
    bool read_bitrate();
    void diagnostic_trace(uint8_t event, uint32_t irq = 0, uint8_t detail = 0)
    {
        nfc_ram_event(2, event, irq, detail);
        _trace.record(event, irq, detail);
    }
    bool diagnostic_fifo_size(uint16_t& bytes, uint8_t& bits)
    {
        const bool ok = _u.readFIFOSize(bytes, bits);
        nfc_ram_event(5, ok, bytes, bits);
        return ok;
    }
    //    enum State{ None, Off, Idle, Ready, Active, Halt };
    Bitrate _bitrate{Bitrate::Invalid};
    bool _wakeup{};
    bool _rf_activity{};
    bool _failed{};
    bool _auto_response_disabled{};
    EmulationLayerA::State _initial_state{EmulationLayerA::State::Off};
    m5::nfc::emulation::Trace _trace{trace_names, (uint8_t)(sizeof(trace_names) / sizeof(trace_names[0]))};

    EmulationLayerA& _layer;
    UnitST25R3916& _u;
};

bool ListenerST25R3916ForA::get_irq(const uint32_t bits, uint32_t& irq32)
{
    irq32 = 0;
    // Unit::update is the I2C IRQ owner. A partial clear-on-read failure invalidates
    // the cached snapshot even when a later read succeeds; only recovery clears it.
    if (_failed || _u.interruptReadFailed()) return false;
    if (_u._interrupt_occurred) {  // In I2C, Unit::update acquires IRQ each time.
        _u._interrupt_occurred = false;
        uint32_t v{};
        if (!_u.readInterrupts(v)) return false;
        _u._stored_irq = _u._stored_irq | v;
    }
    irq32 = _u._stored_irq & bits;
    if (irq32) {
        _u._stored_irq = _u._stored_irq & ~irq32;
        // Every frame the chip takes part in passes through here, the ones it answers on its own
        // included, which makes this the one place that sees the whole of the RF traffic
        _rf_activity = true;
    }
    return true;
}

EmulationLayerA::State ListenerST25R3916ForA::failure(const ListenerIoStep step)
{
    if (!_failed) nfc_ram_event(14, static_cast<uint8_t>(step));
    _failed = true;
    _bitrate = Bitrate::Invalid;
    _wakeup = _rf_activity = false;
    // Both cleanup operations are attempted. Failure still stops software dispatch;
    // the application will rebuild only after it can confirm the external field is absent.
    (void)_u.writeDirectCommand(CMD_STOP_ALL_ACTIVITIES);
    (void)_u.writeDirectCommand(CMD_CLEAR_FIFO);
    return EmulationLayerA::State::None;
}

bool ListenerST25R3916ForA::recover_receive(const bool disable_tx)
{
    bool ok = _u.writeDirectCommand(CMD_CLEAR_FIFO);
    ok = _u.writeDirectCommand(CMD_UNMASK_RECEIVE_DATA) && ok;
    if (disable_tx) ok = _u.clear_bit_register8(REG_OPERATION_CONTROL, tx_en) && ok;
    if (!ok) failure(ListenerIoStep::ReceiveRecovery);
    return ok;
}

bool ListenerST25R3916ForA::read_bitrate()
{
    uint8_t br{};
    const bool ok = _u.readBitrateDetectionDisplay(br);
    nfc_ram_event(3, ok, br);
    if (!ok) {
        failure(ListenerIoStep::Bitrate);
        return false;
    }
    br = (br >> 4) & 0x03;
    _bitrate = static_cast<Bitrate>(br > 2 ? 2 : br);
    return true;
}

bool ListenerST25R3916ForA::load_config(const m5::nfc::a::PICC& picc)
{
    if (!picc.valid() || picc.size == 10) {  // 10 bytes uid not supported
        M5_LIB_LOGE("Invalid PICC");
        return false;
    }

    uint8_t wbuf[15]{};
    uint32_t offset{};

    // UID
    memcpy(wbuf, picc.uid, picc.size);
    offset += 10;
    // ATQA
    wbuf[offset++] = picc.atqa & 0xFF;
    wbuf[offset++] = picc.atqa >> 8;
    // SAK (lv1-3)
    wbuf[offset++] = (picc.size == 4) ? (picc.sak & ~0x04) : (picc.sak | 0x04 /* incomplete */);
    wbuf[offset++] = (picc.sak & ~0x04);
    wbuf[offset++] = (picc.sak & ~0x04);

    constexpr uint8_t uid_4{0x00};
    constexpr uint8_t uid_7{0x10};
    return _u.change_bit_register8(REG_AUXILIARY_DEFINITION, ((picc.size == 7) ? uid_7 : uid_4), 0x30) &&
           _u.writePtMemoryA(wbuf, offset);
}

bool ListenerST25R3916ForA::start_emulation(const m5::nfc::a::PICC& picc)
{
    if (!_u.isNFCMode(NFC::A)) {
        M5_LIB_LOGE("Unit is not configured for NFC-A emulation");
        return false;
    }

    _failed = _rf_activity = false;
    _initial_state = EmulationLayerA::State::None;
    if (!load_config(picc)) {
        failure(ListenerIoStep::LoadConfig);
        return false;
    }

    // Auto response only NFC-A
    if (!_u.change_bit_register8(REG_NFCIP_1_PASSIVE_TARGET_DEFINITION, d_ac_ap2p | d_212_424_1r,
                                d_ac_ap2p | d_212_424_1r | d_106_ac_a) ||
        !_u.change_bit_register8(REG_TIMER_AND_EMV_CONTROL, 0x00, 0xE0) ||  // Disable GPT trigger
        !_u.set_bit_register8(REG_TIMER_AND_EMV_CONTROL, mrt_step) ||     // 512/fc steps
        !_u.write_register8(REG_MASK_RECEIVER_TIMER, calculate_mrt(100, true)) ||
        !_u.clear_bit_register8(REG_ISO14443A_SETTINGS, no_tx_par | no_rx_par | nfc_f0) ||
        !_u.writeAntennaTuningControl1(0x00) || !_u.writeAntennaTuningControl2(0xFF) ||
        !_u.writeOvershootProtectionConfiguration1(0x00) || !_u.writeOvershootProtectionConfiguration2(0x00) ||
        !_u.writeUndershootProtectionConfiguration1(0x00) || !_u.writeUndershootProtectionConfiguration2(0x00) ||
        !_u.writeDirectCommand(CMD_UNMASK_RECEIVE_DATA)) {
        failure(ListenerIoStep::Start);
        return false;
    }
    _initial_state = goto_off();
    return _initial_state != EmulationLayerA::State::None;
}

bool ListenerST25R3916ForA::stop_emulation()
{
    ///    /*Check if Observation Mode was enabled and disable it on ST25R391x */
    //    rfalCheckDisableObsMode();

    // Stopping must attempt every cleanup step even when the first one fails.
    bool ok = _u.enable_osc();
    ok = _u.disable_field() && ok;
    ok = _u.set_bit_register8(REG_NFCIP_1_PASSIVE_TARGET_DEFINITION, d_ac_ap2p | d_212_424_1r | d_106_ac_a) && ok;
    ok = _u.writeModeDefinition(0x00) && ok;
    if (!ok) nfc_ram_event(14, static_cast<uint8_t>(ListenerIoStep::Stop));
    return ok;
}

bool ListenerST25R3916ForA::transmit(const uint8_t* tx, const uint16_t tx_len, const uint32_t /*timeout_ms*/)
{
    return _u.nfcaEmulationTransmit(tx, tx_len);
}

// ------------------------------------------------------------
EmulationLayerA::State ListenerST25R3916ForA::goto_state(const EmulationLayerA::State s)
{
    switch (s) {
        case EmulationLayerA::State::None:
            return failure(ListenerIoStep::Active);
        case EmulationLayerA::State::Off:
            return goto_off();
        case EmulationLayerA::State::Idle:
            return goto_idle();
        case EmulationLayerA::State::Ready:
            return goto_ready();
        case EmulationLayerA::State::Active:
            return goto_active();
        case EmulationLayerA::State::Halt:
            return goto_halt();
        default:
            break;
    }
    return goto_off();
}

EmulationLayerA::State ListenerST25R3916ForA::goto_off()
{
    diagnostic_trace(EV_OFF);
    _trace.dump();  // Compiles away unless serial trace is explicitly enabled.
    _bitrate      = Bitrate::Invalid;
    _wakeup       = false;

    if (!_u.writeDirectCommand(CMD_STOP_ALL_ACTIVITIES) || !_u.set_bit_register8(REG_OPERATION_CONTROL, rx_en) ||
        !_u.clear_bit_register8(REG_NFCIP_1_PASSIVE_TARGET_DEFINITION, d_106_ac_a) ||
        !_u.writeDirectCommand(CMD_GO_TO_SENSE) || !_u.clear_bit_register8(REG_ISO14443A_SETTINGS, nfc_f0) ||
        !_u.writeMaskInterrupts(0xFFFFFFFF) || !_u.clearInterrupts() ||
        !_u.enable_interrupts(I_osc32 | default_irq | mode_irq) ||
        !_u.set_bit_register8(REG_AUXILIARY_DEFINITION, no_crc_rx) ||
        !_u.change_bit_register8(REG_MODE_DEFINITION, mode_bitrate_detection, 0x78)) {
        return failure(ListenerIoStep::Off);
    }
    _auto_response_disabled = false;
    bool field{};
    if (!is_extra_field(field)) return failure(ListenerIoStep::Off);
    if (field) {
        return goto_idle();
    }
    if (!_u.clear_bit_register8(REG_OPERATION_CONTROL, tx_en | rx_en | en)) return failure(ListenerIoStep::Off);
    return EmulationLayerA::State::Off;
}

EmulationLayerA::State ListenerST25R3916ForA::goto_idle()
{
    uint8_t v{}, aux{};

    diagnostic_trace(EV_IDLE);
    if (!_u.readOperationControl(v)) return failure(ListenerIoStep::Idle);
    if ((v & en) == 0) {
        if (!_u.set_bit_register8(REG_OPERATION_CONTROL, (en | rx_en)) || !_u.readAuxiliaryDisplay(aux))
            return failure(ListenerIoStep::Idle);
        if ((aux & osc_ok) == 0) {
            if ((_u.wait_for_interrupt(I_osc32, 1000) & I_osc32) == 0) {
                return failure(ListenerIoStep::Oscillator);
            }
            if (_u.interruptReadFailed()) return failure(ListenerIoStep::Interrupt);
        }
    } else {
        uint32_t discard{};
        if (!get_irq(I_osc32, discard)) return failure(ListenerIoStep::Interrupt);
    }

    if (!_u.set_bit_register8(REG_AUXILIARY_DEFINITION, no_crc_rx)) return failure(ListenerIoStep::Idle);

    if (_auto_response_disabled && !_wakeup) {
        // M5_LIB_LOGE("A->I");
        if (!_u.clear_bit_register8(REG_NFCIP_1_PASSIVE_TARGET_DEFINITION, d_106_ac_a) ||
            !_u.writeDirectCommand(CMD_GO_TO_SENSE)) return failure(ListenerIoStep::Idle);
        _auto_response_disabled = false;
    }

    // EON, SELECT completion and the first RXE may all be cached before this
    // service round. Preserve that FIFO until Ready/Active validates the frame.
    const bool selected_rx = (_u._stored_irq & I_rxe32) && (_u._stored_irq & I_rxe_pta32) &&
                             (_u._stored_irq & I_wu_a32);
    if (!selected_rx && !recover_receive()) return EmulationLayerA::State::None;

    _wakeup = false;

    return update_idle();
    //    return EmulationLayerA::State::Idle;
}

EmulationLayerA::State ListenerST25R3916ForA::goto_ready()
{
    diagnostic_trace(EV_READY, _wakeup);
    uint32_t irq{};
    if (!get_irq(I_eof32, irq)) return failure(ListenerIoStep::Interrupt);
    if (irq) {
        return goto_off();
    }

    if (_bitrate == Bitrate::Invalid) return failure(ListenerIoStep::Bitrate);
    if (!_u.clear_bit_register8(REG_AUXILIARY_DEFINITION, no_crc_rx) ||
        !_u.writeBitrate(_bitrate, _bitrate) ||
        !_u.clear_bit_register8(REG_OPERATION_CONTROL, wu) ||
        !_u.writeModeDefinition(mode_listen_nfc_a)) return failure(ListenerIoStep::Ready);

    return update_ready();
    //    return EmulationLayerA::State::Ready;
}

EmulationLayerA::State ListenerST25R3916ForA::goto_active()
{
    diagnostic_trace(EV_ACTIVE);
    if (!_u.set_bit_register8(REG_NFCIP_1_PASSIVE_TARGET_DEFINITION, d_106_ac_a))
        return failure(ListenerIoStep::Active);
    _auto_response_disabled = true;
    uint32_t discard{};
    if (!get_irq(0, discard)) return failure(ListenerIoStep::Interrupt);
    // A pending first command owns its error flags. Only discard stale errors
    // when no RXE is cached, otherwise Active must validate the whole snapshot.
    if (!(_u._stored_irq & I_rxe32) && !get_irq(I_par32 | I_crc32 | I_err232 | I_err132, discard))
        return failure(ListenerIoStep::Interrupt);
    if (!_u.enable_interrupts(I_rxe32)) return failure(ListenerIoStep::Active);

    return update_active();
    //    return EmulationLayerA::State::Active;
}

EmulationLayerA::State ListenerST25R3916ForA::goto_halt()
{
    diagnostic_trace(EV_HALT, (uint32_t)_bitrate);
    if (!_u.clear_bit_register8(REG_NFCIP_1_PASSIVE_TARGET_DEFINITION, d_106_ac_a) ||
        !_u.writeDirectCommand(CMD_GO_TO_SLEEP) ||
        !_u.change_bit_register8(REG_MODE_DEFINITION, mode_bitrate_detection, mode_mask) ||
        !_u.clear_bit_register8(REG_ISO14443A_SETTINGS, nfc_f0) ||
        !_u.writeDirectCommand(CMD_UNMASK_RECEIVE_DATA) ||
        !_u.enable_interrupts(I_nfct32 | I_rxs32 | I_crc32 | I_err132 | I_err232 | I_par32 | I_eon32 | I_eof32 | mode_irq))
        return failure(ListenerIoStep::Halt);
    _auto_response_disabled = false;

    bool field{};
    if (!is_extra_field(field)) return failure(ListenerIoStep::Halt);
    if (!field) {
        return goto_off();
    }

    return update_halt();
    //    return EmulationLayerA::State::Halt;
}

// ------------------------------------------------------------
EmulationLayerA::State ListenerST25R3916ForA::update_off()
{
    // IRQ due to detection of external field
    uint32_t irq{};
    if (!get_irq(I_eon32, irq)) return failure(ListenerIoStep::Interrupt);
    if (irq & I_eon32) {
        return goto_idle();
    }
    return EmulationLayerA::State::Off;
}

EmulationLayerA::State ListenerST25R3916ForA::update_idle()
{
    uint32_t irq32{};
    if (!get_irq(I_nfct32 | I_rxe32 | I_eof32 | I_rxe_pta32, irq32)) return failure(ListenerIoStep::Interrupt);
    if (!irq32) {
        return EmulationLayerA::State::Idle;
    }

    if (is_eof(irq32)) return goto_off();
    if ((irq32 & I_nfct32) && !read_bitrate()) return EmulationLayerA::State::None;
    const bool selected_rx = (irq32 & I_rxe32) && (irq32 & I_rxe_pta32) && (_u._stored_irq & I_wu_a32);
    if (selected_rx) _u._stored_irq = _u._stored_irq | I_rxe32;
    if ((irq32 & I_rxe32) && !selected_rx) {
        uint32_t errors{};
        if (!get_irq(I_crc32 | I_par32 | I_err232 | I_err132, errors)) return failure(ListenerIoStep::Interrupt);
        if (errors) nfc_ram_event(7, irq32 | errors, 0, 0);
        nfc_ram_event(10, irq32);
        // Unselected frames are never delivered to the tag callback. Discard them
        // without FIFO reads, a 256-byte stack buffer or software CRC work.
        if (!recover_receive(true)) return EmulationLayerA::State::None;
        // A cached RXE_PTA from the same snapshot still needs to advance selection.
    }
    if ((irq32 & I_rxe_pta32) && _bitrate == Bitrate::Bps106K) {
        uint8_t pta{};
        const bool pta_ok = _u.readPassiveTargetDisplay(pta);
        nfc_ram_event(4, pta_ok, pta);
        if (!pta_ok) return failure(ListenerIoStep::PassiveTarget);
        if ((pta & 0x0F) > pta_state_idle) {
            // M5_LIB_LOGE("  I PTA:%02X", pta);
            return goto_ready();
        }
        // M5_LIB_LOGE("PTA:%02X", pta);
    }

    return EmulationLayerA::State::Idle;
}

EmulationLayerA::State ListenerST25R3916ForA::update_ready()
{
    uint32_t irq32{};
    if (!get_irq(I_eof32 | I_rxe32 | (_wakeup ? I_wu_ax32 : I_wu_a32), irq32)) return failure(ListenerIoStep::Interrupt);
    if (!irq32) {
        return EmulationLayerA::State::Ready;
    }
    diagnostic_trace(EV_READY_IRQ, irq32, _wakeup);

    if (is_eof(irq32)) {
        // M5_LIB_LOGE("OFF");
        return goto_off();
    }
    if (irq32 & (I_wu_a32 | I_wu_ax32)) {
        if (irq32 & I_rxe32) _u._stored_irq = _u._stored_irq | I_rxe32;
        return goto_active();
    }
    if (irq32 & I_rxe32) {
        uint32_t errors{};
        if (!get_irq(I_par32 | I_crc32 | I_err232 | I_err132, errors)) return failure(ListenerIoStep::Interrupt);
        if (errors) nfc_ram_event(7, irq32 | errors);
        nfc_ram_event(10, irq32);
        if (!recover_receive()) return EmulationLayerA::State::None;
    }

    return EmulationLayerA::State::Ready;
}

EmulationLayerA::State ListenerST25R3916ForA::update_active()
{
    uint32_t irq32{};
    if (!get_irq(I_eof32 | I_rxe32, irq32)) return failure(ListenerIoStep::Interrupt);
    if (!irq32) return EmulationLayerA::State::Active;
    if (is_eof(irq32)) {
        // RFAL also prioritizes field loss: never dispatch a command off-field.
        if (irq32 & I_rxe32) nfc_ram_event(12, irq32);
        return goto_off();
    }

    uint32_t errors{};
    if (!get_irq(I_par32 | I_crc32 | I_err232 | I_err132, errors)) return failure(ListenerIoStep::Interrupt);
    irq32 |= errors;
    uint16_t bytes{};
    uint8_t bits{};
    uint8_t rx[RX_BUFFER_SIZE];
    const bool size_ok = diagnostic_fifo_size(bytes, bits);
    const uint16_t payload = detail::active_payload_size(bytes, bits, sizeof(rx));
    // Reject before FIFO transfer. Invalid frames need no diagnostic payload read.
    if (!size_ok) return failure(ListenerIoStep::Active);
    if (!payload || errors) {
        nfc_ram_event(7, irq32, bytes, bits);
        if (!recover_receive()) return EmulationLayerA::State::None;
        return _wakeup ? goto_halt() : goto_idle();
    }
    // Match RFAL's status -> exact payload read, avoiding a second status read.
    const bool read_ok = _u.readFIFOExact(rx, payload);
    nfc_ram_event(6, read_ok, read_ok ? payload : 0, payload);
    if (!read_ok) {
        nfc_ram_event(7, irq32, bytes, bits);
        return failure(ListenerIoStep::Active);
    }
    nfc_ram_event(13, payload); // Count accepted frames even after ring rollover.
    auto state = _layer.receive_callback(rx, payload);
    if (state != EmulationLayerA::State::Active) {
        if (state == EmulationLayerA::State::Idle && _wakeup) state = EmulationLayerA::State::Halt;
        // Keep RXE enabled in waiting states so the next selection's first frame
        // survives Unit::update's enabled-IRQ filtering, including Halt/WUPA.
        return goto_state(state);
    }
    return EmulationLayerA::State::Active;
}

EmulationLayerA::State ListenerST25R3916ForA::update_halt()
{
    uint32_t irq32{};
    if (!get_irq(I_nfct32 | I_rxe32 | I_eof32 | I_rxe_pta32, irq32)) return failure(ListenerIoStep::Interrupt);
    if (!irq32) {
        return EmulationLayerA::State::Halt;
    }
    diagnostic_trace(EV_HALT_IRQ, irq32, (uint8_t)_bitrate);

    if (is_eof(irq32)) return goto_off();
    // initiator bit rate was recognized
    // The reader wakes a halted PICC with its own frame, so the bit rate has to be taken every
    // time and not only while it is unknown: a halt entered after a session still holds the rate
    // of that session
    if (irq32 & I_nfct32) {
        if (!read_bitrate()) return EmulationLayerA::State::None;
        // The mode is left as it is: a halted target keeps bit rate detection until it moves to
        // ready, where the bit rate is written before the fixed listen mode (see goto_ready).
        // Setting the mode here would leave the bit rate register holding the previous value
    }
    const bool selected_rx = (irq32 & I_rxe32) && (irq32 & I_rxe_pta32) && (_u._stored_irq & I_wu_ax32);
    if (selected_rx) _u._stored_irq = _u._stored_irq | I_rxe32;
    if ((irq32 & I_rxe32) && !selected_rx) {
        uint32_t discard{};
        if (!get_irq(I_crc32 | I_par32 | I_err232 | I_err132, discard)) return failure(ListenerIoStep::Interrupt);
        nfc_ram_event(10, irq32);
        if (!recover_receive()) return EmulationLayerA::State::None;
    }
    if ((irq32 & I_rxe_pta32) && _bitrate == Bitrate::Bps106K) {
        uint8_t pta{};
        const bool pta_ok = _u.readPassiveTargetDisplay(pta);
        nfc_ram_event(4, pta_ok, pta);
        diagnostic_trace(EV_PTA, irq32, pta);
        if (!pta_ok) return failure(ListenerIoStep::PassiveTarget);
        if ((pta & 0x0F) > pta_state_halt) {
            // M5_LIB_LOGE("  H PTA:%02X", pta);
            _wakeup = true;
            return goto_ready();
        }
    }
    return EmulationLayerA::State::Halt;
}

bool ListenerST25R3916ForA::is_extra_field(bool& present)
{
    uint8_t v{};
    const bool ok = _u.readAuxiliaryDisplay(v);
    present = ok && (v & efd_o);
    return ok;
}

//
namespace {
std::unique_ptr<EmulationLayerA::Adapter> make_st25r3916_adapter(EmulationLayerA& l, UnitST25R3916& u)
{
    return std::unique_ptr<EmulationLayerA::Adapter>(new ListenerST25R3916ForA(l, u));
}
}  // namespace

EmulationLayerA::EmulationLayerA(UnitST25R3916& u) : _impl(make_st25r3916_adapter(*this, u))
{
}

EmulationLayerA::EmulationLayerA(CapST25R3916& u) : _impl(make_st25r3916_adapter(*this, static_cast<UnitST25R3916&>(u)))
{
}

}  // namespace nfc
}  // namespace m5
