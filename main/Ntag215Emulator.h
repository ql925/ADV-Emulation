#pragma once
#include <M5UnitUnifiedNFC.h>
#include <esp_timer.h>
#include "NtagCommands.h"
#include "NfcFifoTx.h"

namespace amiibo {
// 0.1.2 owns selection, HALT/WUPA, IRQ caching, RX buffering and expiry.
// Override tag commands to preserve credential masking, READ rollover,
// streamed long replies and four-bit ACK/NAK instead of generic base behavior.
template <class Unit>
class Ntag215Emulator : public m5::nfc::EmulationLayerA {
public:
    explicit Ntag215Emulator(Unit& unit) : EmulationLayerA(unit), _unit(unit) {}
    // Caller owns 32 bytes matching the UID, valid throughout emulation.
    void set_signature(const uint8_t (&signature)[32]) { _signature = signature; }
    void clear_signature() { _signature = nullptr; }
    bool has_signature() const { return _signature != nullptr; }
    State receive_callback(const uint8_t* rx, const uint32_t rx_len) override
    {
        _callback_started = static_cast<uint32_t>(esp_timer_get_time());
        _tx_start_us = 0;
        _tx_result = TxResult::Invalid;
        _auth_match = -1;
        _tx_size = 0;
        _nak = 0xFF;
        _tx_ok = false;
        if (!_memory || _memory_size != TAG_BYTES) {
            _session.reset_auth();
            return State::Idle;
        }
        const auto response = handle_ntag_command(_memory, rx, rx_len, _session,
                                                  _read_buffer, sizeof(_read_buffer), _signature);
        _auth_match = response.auth_match;
        State result = State::Idle;
        switch (response.kind) {
            case ReplyKind::Data: result = reply(response.data, response.size); break;
            case ReplyKind::Nibble: result = nibble(response.nibble); break;
            case ReplyKind::Halt: result = EmulationLayerA::receive_callback(rx, rx_len); break;
            case ReplyKind::Idle: break;
        }
        if (result != State::Active) _session.reset_auth();
        return result;
    }
    void update()
    {
        EmulationLayerA::update();
        if (state() == State::Off || state() == State::None) _session.end_field();
        else if (_memory) _session.begin_field(_memory);
        if (state() != State::Active) _session.reset_auth();
    }
    bool begin(const m5::nfc::a::PICC& picc, uint8_t* memory, uint32_t size)
    {
        if (state() != State::None || picc.type != m5::nfc::a::Type::NTAG_215 || picc.size != 7 || size != TAG_BYTES)
            return false;
        _session.end_field();
        const bool ok = EmulationLayerA::begin(picc, memory, size);
        if (ok && state() != State::Off) _session.begin_field(memory);
        return ok;
    }
    bool end()
    {
        _session.end_field();
        return EmulationLayerA::end();
    }
protected:
    NtagSession _session{};
    uint32_t _callback_started{}, _tx_start_us{};
    TxResult _tx_result{TxResult::Invalid};
    uint16_t _tx_size{};
    uint8_t _nak{0xFF};
    bool _tx_ok{};
    int8_t _auth_match{-1};
private:
    Unit& _unit;
    const uint8_t* _signature{};
    uint8_t _read_buffer[TAG_BYTES]{};
    struct FifoIO {
        Unit& unit;
        uint32_t callback_started;
        uint32_t tx_start_us{};
        bool clear() { return unit.writeDirectCommand(m5::unit::st25r3916::command::CMD_CLEAR_FIFO); }
        bool length(uint16_t n) { return unit.writeNumberOfTransmittedBytes(n, 0); }
        bool write(const uint8_t* data, uint16_t n) { return unit.writeFIFO(data, n); }
        bool start()
        {
            const bool ok = unit.writeDirectCommand(m5::unit::st25r3916::command::CMD_TRANSMIT_WITH_CRC);
            tx_start_us = now() - callback_started;
            return ok;
        }
        bool status(uint16_t& value) { return unit.readFIFOStatus(value); }
        uint32_t now() { return static_cast<uint32_t>(esp_timer_get_time()); }
        void abort() { unit.writeDirectCommand(m5::unit::st25r3916::command::CMD_STOP_ALL_ACTIVITIES); }
    };
    State reply(const uint8_t* data, const uint16_t size)
    {
        _tx_size = size;
        if (size <= amiibo::TX_CHUNK) {
            // The adapter queues synchronously; timeout is not a TX-complete wait.
            _tx_ok = transmit(data, size, 4);
            _tx_start_us = static_cast<uint32_t>(esp_timer_get_time()) - _callback_started;
            _tx_result = _tx_ok ? amiibo::TxResult::Queued : amiibo::TxResult::Setup;
            if (!_tx_ok) _unit.writeDirectCommand(m5::unit::st25r3916::command::CMD_STOP_ALL_ACTIVITIES);
            return _tx_ok ? State::Active : State::Idle;
        }
        FifoIO io{_unit, _callback_started};
        _tx_result = amiibo::queue_frame(io, data, size);
        _tx_start_us = io.tx_start_us;
        _tx_ok = _tx_result == amiibo::TxResult::Queued;
        return _tx_ok ? State::Active : State::Idle;
    }

    State nibble(const uint8_t value)
    {
        // The library's byte transmit adds CRC. Type 2 ACK/NAK must be exactly
        // four bits without CRC, otherwise Android can reject a page write.
        using namespace m5::unit::st25r3916::command;
        const bool sent = _unit.writeDirectCommand(CMD_CLEAR_FIFO) && _unit.writeFIFO(&value, 1) &&
                          _unit.writeNumberOfTransmittedBytes(0, 4) &&
                          _unit.writeDirectCommand(CMD_TRANSMIT_WITHOUT_CRC);
        if (!sent) _unit.writeDirectCommand(CMD_STOP_ALL_ACTIVITIES);
        _nak = value;
        _tx_ok = sent;
        _tx_start_us = static_cast<uint32_t>(esp_timer_get_time()) - _callback_started;
        _tx_result = sent ? amiibo::TxResult::Queued : amiibo::TxResult::Setup;
        return sent ? State::Active : State::Idle;
    }
};
}  // namespace amiibo
