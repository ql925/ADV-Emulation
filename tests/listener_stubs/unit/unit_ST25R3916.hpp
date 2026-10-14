#pragma once
#include <algorithm>
#include <vector>
#include "unit/ST25R3916_definition.hpp"
#include "nfc/a/nfca.hpp"
#include "NfcDiagnosticHook.h"

namespace m5 {
namespace unit {
// Bus fake only: the state machine, register constants, frame validation and
// NTAG215 command/response implementation are all the actual production code.
class UnitST25R3916 {
public:
    enum Op { Change, Set, Clear, Write, Command, ReadOperation, ReadAux, ReadBitrate,
              ReadPta, Mask, ClearIrq, Enable, Disable, BitrateWrite, PtMemory,
              FifoSize, FifoRead, Oscillator, DisableField, Transmit, FifoWrite, TxLength, FifoStatus, IrqRead };
    struct Call { Op op; uint32_t a, b; };
    std::vector<Call> calls;
    std::vector<uint8_t> received, transmitted, pt_memory;
    size_t fail_at{};
    uint32_t _stored_irq{}, fresh_irq{}, enabled_irq{};
    bool _interrupt_occurred{}, irq_failed{}, field{}, oscillator_ready{true};
    uint8_t bitrate_display{}, pta_display{2}, partial_bits{}, operation{};
    uint8_t passive_definition{0x5C};
    uint16_t fifo_bytes{};
    bool oscillator_wait_ok{true};

    bool call(Op op, uint32_t a = 0, uint32_t b = 0)
    {
        calls.push_back({op, a, b});
        return !fail_at || calls.size() != fail_at;
    }
    void reset_calls() { calls.clear(); fail_at = 0; }
    void poll(uint32_t bits) { _stored_irq |= bits & enabled_irq; }
    size_t count(Op op) const
    {
        return std::count_if(calls.begin(), calls.end(), [op](const Call& c) { return c.op == op; });
    }
    bool isNFCMode(m5::nfc::NFC mode) { return mode == m5::nfc::NFC::A; }
    bool change_bit_register8(uint16_t reg, uint8_t bits, uint8_t mask)
    {
        if (!call(Change, reg, uint32_t(bits) | (uint32_t(mask) << 8))) return false;
        if (reg == st25r3916::command::REG_NFCIP_1_PASSIVE_TARGET_DEFINITION)
            passive_definition = (passive_definition & ~mask) | (bits & mask);
        return true;
    }
    bool set_bit_register8(uint16_t reg, uint8_t bits)
    {
        if (!call(Set, reg, bits)) return false;
        if (reg == st25r3916::command::REG_OPERATION_CONTROL) operation |= bits;
        if (reg == st25r3916::command::REG_NFCIP_1_PASSIVE_TARGET_DEFINITION) passive_definition |= bits;
        return true;
    }
    bool clear_bit_register8(uint16_t reg, uint8_t bits)
    {
        if (!call(Clear, reg, bits)) return false;
        if (reg == st25r3916::command::REG_OPERATION_CONTROL) operation &= ~bits;
        if (reg == st25r3916::command::REG_NFCIP_1_PASSIVE_TARGET_DEFINITION) passive_definition &= ~bits;
        return true;
    }
    bool write_register8(uint16_t reg, uint8_t value) { return call(Write, reg, value); }
    bool writePtMemoryA(const uint8_t* data, uint16_t size)
    { if (!call(PtMemory, size)) return false; pt_memory.assign(data, data + size); return true; }
    bool writeAntennaTuningControl1(uint8_t value) { return call(Write, 0x26, value); }
    bool writeAntennaTuningControl2(uint8_t value) { return call(Write, 0x27, value); }
    bool writeOvershootProtectionConfiguration1(uint8_t value) { return call(Write, 0x130, value); }
    bool writeOvershootProtectionConfiguration2(uint8_t value) { return call(Write, 0x131, value); }
    bool writeUndershootProtectionConfiguration1(uint8_t value) { return call(Write, 0x132, value); }
    bool writeUndershootProtectionConfiguration2(uint8_t value) { return call(Write, 0x133, value); }
    bool writeDirectCommand(uint8_t cmd)
    {
        if (!call(Command, cmd)) return false;
        if (cmd == st25r3916::command::CMD_CLEAR_FIFO) {
            received.clear(); fifo_bytes = 0; partial_bits = 0; transmitted.clear();
        }
        return true;
    }
    bool writeMaskInterrupts(uint32_t value)
    { if (!call(Mask, value)) return false; enabled_irq = ~value; return true; }
    bool clearInterrupts()
    {
        if (!call(ClearIrq)) return false;
        _stored_irq = fresh_irq = 0; irq_failed = false;
        return true;
    }
    bool enable_interrupts(uint32_t mask)
    { if (!call(Enable, mask)) return false; enabled_irq |= mask; return true; }
    bool disable_interrupts(uint32_t mask)
    { if (!call(Disable, mask)) return false; enabled_irq &= ~mask; return true; }
    bool readOperationControl(uint8_t& value)
    { if (!call(ReadOperation)) return false; value = operation; return true; }
    bool readAuxiliaryDisplay(uint8_t& value)
    {
        if (!call(ReadAux)) return false;
        value = (field ? st25r3916::regval::efd_o : 0) | (oscillator_ready ? st25r3916::regval::osc_ok : 0);
        return true;
    }
    bool readBitrateDetectionDisplay(uint8_t& value)
    { if (!call(ReadBitrate)) return false; value = bitrate_display; return true; }
    bool readPassiveTargetDisplay(uint8_t& value)
    { if (!call(ReadPta)) return false; value = pta_display; return true; }
    bool writeBitrate(m5::nfc::Bitrate tx, m5::nfc::Bitrate rx)
    { return call(BitrateWrite, static_cast<uint8_t>(tx), static_cast<uint8_t>(rx)); }
    bool writeModeDefinition(uint8_t value) { return call(Write, st25r3916::command::REG_MODE_DEFINITION, value); }
    bool readInterrupts(uint32_t& value)
    {
        value = 0;
        if (!call(IrqRead)) { irq_failed = true; nfc_ram_event(9); return false; }
        value = fresh_irq; fresh_irq = 0;
        return true;
    }
    bool interruptReadFailed() const { return irq_failed; }
    uint32_t wait_for_interrupt(uint32_t bits, uint32_t) { return oscillator_wait_ok ? bits : 0; }
    bool readFIFOSize(uint16_t& bytes, uint8_t& bits)
    { if (!call(FifoSize)) return false; bytes = fifo_bytes; bits = partial_bits; return true; }
    bool readFIFOExact(uint8_t* data, uint16_t size)
    {
        if (!call(FifoRead, size) || received.size() < size) return false;
        std::copy_n(received.begin(), size, data);
        received.erase(received.begin(), received.begin() + size);
        fifo_bytes -= size;
        return true;
    }
    bool enable_osc() { return call(Oscillator); }
    bool disable_field() { return call(DisableField); }
    bool nfcaEmulationTransmit(const uint8_t* data, uint16_t size)
    {
        if (!call(Transmit, size)) return false;
        transmitted.assign(data, data + size);
        return true;
    }
    bool writeFIFO(const uint8_t* data, uint16_t size)
    {
        if (!call(FifoWrite, size)) return false;
        transmitted.insert(transmitted.end(), data, data + size);
        return true;
    }
    bool writeNumberOfTransmittedBytes(uint16_t size, uint8_t bits) { return call(TxLength, size, bits); }
    bool readFIFOStatus(uint16_t& status)
    { if (!call(FifoStatus)) return false; status = 64u << 8; return true; }
};
class CapST25R3916 : public UnitST25R3916 {};
}  // namespace unit
}  // namespace m5
