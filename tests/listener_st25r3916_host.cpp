// Run the actual M5Unit-NFC layer, ST25R3916 adapter and NTAG215 subclass on a bus fake.
#if defined(_MSC_VER)
#define __attribute__(...)  // Host MSVC has no GNU weak/packed attributes.
#pragma warning(disable : 4068 4551)
#endif
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <vector>
#include "M5Utility.hpp"
#include "../main/Ntag215Emulator.h"
#include "../main/RfDiagnostics.h"
#include "../dependencies/M5Unit-NFC/src/nfc/layer/a/emulation_layer_a.cpp"
#include "../dependencies/M5Unit-NFC/src/nfc/layer/a/emulation_layer_a_ST25R3916.cpp"

namespace listener_test {
uint32_t clock_ms{};
amiibo::RfDiagnostics diagnostic;
unsigned checks{}, injected_failures{};
}
extern "C" void m5_nfc_ram_event(uint8_t kind, uint32_t a, uint32_t b, uint32_t c)
{
    listener_test::diagnostic.record(listener_test::clock_ms * 1000, 0, kind, a, b, c);
}
// Only unrelated PICC metadata/link helpers are stubbed; the listener header is real.
namespace m5 { namespace nfc { namespace a {
uint16_t get_unit_size(Type) { return 4; }
uint16_t get_number_of_blocks(Type) { return 135; }
uint16_t get_user_area_size(Type) { return 504; }
file_system_feature_t get_file_system_feature(Type) { return FILE_SYSTEM_FLAT_MEMORY; }
const uint8_t* get_version3_response(Type) { return amiibo::NTAG215_VERSION; }
} } }
namespace m5 { namespace unit { namespace st25r3916 {
uint8_t calculate_mrt(uint32_t, bool) { return 3; } // Unchanged timer helper is outside this test's scope.
} } }

using namespace listener_test;
using namespace m5::unit::st25r3916::regval;
using namespace m5::unit::st25r3916::command;
using Unit = m5::unit::UnitST25R3916;
using State = m5::nfc::EmulationLayerA::State;
using Step = m5::nfc::detail::ListenerIoStep;

#define CHECK(condition) do { ++checks; if (!(condition)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " + #condition); } while (0)

struct Tag : amiibo::Ntag215Emulator<Unit> {
    using Base = amiibo::Ntag215Emulator<Unit>;
    using Base::Base;
    unsigned callbacks{};
    State receive_callback(const uint8_t* data, uint32_t size) override
    {
        ++callbacks;
        return Base::receive_callback(data, size);
    }
    bool authenticated() const { return _session.authenticated; }
};
struct Fixture {
    Unit unit;
    Tag tag{unit};
    m5::nfc::a::PICC picc;
    uint8_t memory[amiibo::TAG_BYTES]{};
    Fixture()
    {
        clock_ms = 0; diagnostic.clear();
        picc.type = m5::nfc::a::Type::NTAG_215; picc.size = 7; picc.blocks = 135; picc.atqa = 0x0044;
        picc.uid[0] = 4;
        memory[amiibo::AUTH0_OFFSET] = 0xFF;
        for (unsigned i = 0; i < 4; ++i) memory[532 + i] = uint8_t(i + 1);
        memory[536] = memory[537] = 0x80;
    }
    void begin(bool field = false)
    {
        unit.field = field;
        CHECK(tag.begin(picc, memory, sizeof(memory)));
        CHECK(tag.state() == (field ? State::Idle : State::Off));
        unit.reset_calls(); diagnostic.clear();
    }
    void select()
    {
        if (tag.state() == State::None) begin();
        unit.field = true; unit.bitrate_display = 0; unit.pta_display = 2;
        unit._stored_irq |= I_eon32 | I_nfct32 | I_rxe_pta32 | I_wu_a32;
        tag.update();
        CHECK(tag.state() == State::Active);
        unit.reset_calls(); diagnostic.clear();
    }
    void frame(std::initializer_list<uint8_t> bytes)
    {
        unit.received.assign(bytes); unit.received.push_back(0); unit.received.push_back(0);
        unit.fifo_bytes = uint16_t(unit.received.size()); unit.partial_bits = 0;
        unit.poll(I_rxe32);
    }
    void halt()
    {
        select(); frame({0x50, 0}); tag.update();
        CHECK(tag.state() == State::Halt);
        unit.reset_calls(); diagnostic.clear();
    }
};

void startup_and_failures()
{
    for (bool field : {false, true}) {
        Fixture nominal; nominal.unit.field = field;
        CHECK(nominal.tag.begin(nominal.picc, nominal.memory, sizeof(nominal.memory)));
        const auto operations = nominal.unit.calls.size();
        for (size_t fail = 1; fail <= operations; ++fail) {
            Fixture f; f.unit.field = field; f.unit.fail_at = fail;
            CHECK(!f.tag.begin(f.picc, f.memory, sizeof(f.memory)));
            CHECK(f.tag.state() == State::None && f.tag.callbacks == 0);
            CHECK(diagnostic.io_failures == 1);
            CHECK(f.unit.calls.back().op == Unit::Command && f.unit.calls.back().a == CMD_CLEAR_FIFO);
            ++injected_failures;
        }
    }
    Fixture f; f.begin(true); f.select();
    CHECK(f.tag.state() == State::Active); // No new EON is needed after begin in an existing field.
    CHECK(f.unit.pt_memory.size() == 15);
    CHECK(std::equal(f.picc.uid, f.picc.uid + 7, f.unit.pt_memory.begin()));
    CHECK(f.unit.pt_memory[10] == 0x44 && f.unit.pt_memory[11] == 0 &&
          f.unit.pt_memory[12] == 4 && f.unit.pt_memory[13] == 0 && f.unit.pt_memory[14] == 0);
}

void sweep(const std::function<void(Fixture&)>& setup, const std::function<void(Fixture&)>& action)
{
    Fixture nominal; setup(nominal); nominal.unit.reset_calls(); action(nominal);
    CHECK(nominal.tag.state() != State::None);
    const auto operations = nominal.unit.calls.size();
    for (size_t fail = 1; fail <= operations; ++fail) {
        Fixture f; setup(f); f.unit.reset_calls(); f.unit.fail_at = fail; action(f);
        CHECK(f.tag.state() == State::None);
        CHECK(diagnostic.io_failures == 1);
        ++injected_failures;
    }
}

void transition_failures()
{
    sweep([](Fixture& f) { f.begin(); }, [](Fixture& f) {
        f.unit.field = true; f.unit._stored_irq = I_eon32; f.tag.update();
    });
    sweep([](Fixture& f) { f.begin(true); }, [](Fixture& f) {
        f.unit._stored_irq = I_nfct32 | I_rxe_pta32 | I_wu_a32; f.tag.update();
    });
    sweep([](Fixture& f) { f.select(); }, [](Fixture& f) {
        f.unit.field = false; f.unit._stored_irq = I_eof32; f.tag.update();
    });
    sweep([](Fixture& f) { f.select(); }, [](Fixture& f) {
        f.frame({0x50, 0}); f.tag.update();
    });
    sweep([](Fixture& f) { f.halt(); }, [](Fixture& f) {
        f.unit.pta_display = 10;
        f.unit._stored_irq = I_nfct32 | I_rxe_pta32 | I_wu_ax32; f.tag.update();
    });
    // The old oscillator failure recursively called Off -> Idle with the field present.
    Fixture f; f.begin(); f.unit.field = true; f.unit.oscillator_ready = false;
    f.unit.oscillator_wait_ok = false; f.unit._stored_irq = I_eon32; f.tag.update();
    CHECK(f.tag.state() == State::None && diagnostic.last_io_step == unsigned(Step::Oscillator));
}

void idle_discard_and_halt_wakeup()
{
    Fixture f; f.begin(true);
    f.unit._stored_irq = I_nfct32; f.tag.update();
    f.unit.reset_calls(); f.frame({0x30, 0}); f.tag.update();
    CHECK(f.tag.state() == State::Idle && f.tag.callbacks == 0);
    CHECK(!f.unit.count(Unit::FifoSize) && !f.unit.count(Unit::FifoRead));
    CHECK(diagnostic.unselected_rx_discarded == 1);
    CHECK(f.unit.count(Unit::Command) == 2); // CLEAR_FIFO + UNMASK, no payload or CRC work.
    f.unit.reset_calls(); f.frame({0x30, 0});
    f.unit._stored_irq |= I_rxe_pta32; f.tag.update();
    CHECK(f.tag.state() == State::Ready); // An unselected RXE still discards payload.
    f.unit._stored_irq |= I_wu_a32; f.tag.update();
    CHECK(f.tag.state() == State::Active); // RXE must not swallow selection from the same snapshot.
    CHECK(!f.unit.count(Unit::FifoSize) && !f.unit.count(Unit::FifoRead));
    f.frame({0x50, 0}); f.tag.update(); CHECK(f.tag.state() == State::Halt);
    f.unit.bitrate_display = 0x20; f.unit._stored_irq = I_nfct32; f.tag.update();
    CHECK(f.tag.state() == State::Halt);
    f.unit.reset_calls(); f.unit.bitrate_display = 0; f.unit.pta_display = 10;
    f.unit._stored_irq = I_nfct32 | I_rxe_pta32 | I_wu_ax32; f.tag.update();
    CHECK(f.tag.state() == State::Active && f.unit.count(Unit::ReadBitrate) == 1);
    size_t bitrate_write = f.unit.calls.size(), mode_write = f.unit.calls.size();
    for (size_t i = 0; i < f.unit.calls.size(); ++i) {
        const auto& c = f.unit.calls[i];
        if (c.op == Unit::BitrateWrite) { bitrate_write = i; CHECK(c.a == 0 && c.b == 0); }
        if (c.op == Unit::Write && c.a == REG_MODE_DEFINITION) mode_write = i;
    }
    CHECK(bitrate_write < mode_write);
}

void selection_with_first_frame()
{
    for (State entry : {State::Off, State::Idle, State::Halt}) {
        const bool halted = entry == State::Halt;
        for (bool corrupt : {false, true}) {
            Fixture f;
            if (halted) f.halt(); else f.begin(entry == State::Idle);
            const unsigned before = f.tag.callbacks;
            f.unit.pta_display = halted ? 10 : 2;
            f.unit.field = true;
            f.frame({0x60});
            f.unit._stored_irq |= I_nfct32 | I_rxe_pta32 | (halted ? I_wu_ax32 : I_wu_a32);
            if (entry == State::Off) f.unit._stored_irq |= I_eon32;
            if (corrupt) f.unit._stored_irq |= I_crc32;
            f.tag.update();
            CHECK(f.tag.callbacks == before + (corrupt ? 0 : 1));
            CHECK(f.unit.count(Unit::FifoRead) == (corrupt ? 0 : 1));
            CHECK(f.tag.state() == (corrupt ? (halted ? State::Halt : State::Idle) : State::Active));
            CHECK(bool(f.unit.passive_definition & d_106_ac_a) == !corrupt);
            if (!corrupt) CHECK(f.unit.transmitted.size() == 8);
        }
    }
    Fixture f;
    Unit unit; Tag tag(unit); unit.calls.clear();
    CHECK(tag.begin(f.picc, f.memory, sizeof(f.memory)));
    CHECK(std::any_of(unit.calls.begin(), unit.calls.end(), [](const Unit::Call& c) {
        return c.op == Unit::Enable && (c.a & I_rxe32);
    }));
}

void ready_unselected_frame()
{
    Fixture f; f.begin(true);
    f.unit._stored_irq = I_nfct32 | I_rxe_pta32; f.tag.update(); CHECK(f.tag.state() == State::Ready);
    f.unit.reset_calls(); f.frame({0x60}); f.unit.poll(I_crc32); f.tag.update();
    CHECK(f.tag.state() == State::Ready && f.tag.callbacks == 0 && !f.unit.count(Unit::FifoRead));
    CHECK(!f.unit.fifo_bytes && diagnostic.unselected_rx_discarded == 1);
    f.unit.reset_calls(); f.frame({0x60}); f.unit.poll(I_wu_a32); f.tag.update();
    CHECK(f.tag.state() == State::Active && f.tag.callbacks == 1 && f.unit.transmitted.size() == 8);
}

void config_lock_field_lifecycle()
{
    Fixture f; f.select();
    f.frame({0xA2, 0x84, 0x40, 0, 0, 0}); f.tag.update();
    CHECK(!f.tag.begin(f.picc, f.memory, sizeof(f.memory))); // Rejected begin must not simulate a field cycle.
    f.frame({0x50, 0}); f.tag.update(); CHECK(f.tag.state() == State::Halt);
    f.unit.pta_display = 10; f.unit._stored_irq = I_nfct32 | I_rxe_pta32 | I_wu_ax32;
    f.tag.update(); CHECK(f.tag.state() == State::Active);
    f.frame({0xA2, 0x83, 4, 0, 0, 0xFF}); f.tag.update();
    CHECK(f.unit.transmitted[0] == 0x0A && f.memory[524] == 4);
    f.unit.field = false; f.unit._stored_irq = I_eof32; f.tag.update(); CHECK(f.tag.state() == State::Off);
    f.select(); f.frame({0xA2, 0x83, 5, 0, 0, 0xFF}); f.tag.update();
    CHECK(f.unit.transmitted[0] == 0 && f.memory[524] == 4);
}

void rx_frames_and_errors()
{
    Fixture f; f.select(); f.frame({0x60}); f.tag.update();
    CHECK(f.tag.state() == State::Active && f.tag.callbacks == 1);
    CHECK(f.unit.count(Unit::FifoSize) == 1 && f.unit.count(Unit::FifoRead) == 1);
    CHECK(f.unit.transmitted.size() == 8 && diagnostic.rx_accepted == 1);
    for (auto op : {Unit::FifoSize, Unit::FifoRead}) {
        Fixture bad; bad.select(); bad.frame({0x60});
        bad.unit.fail_at = op == Unit::FifoSize ? 1 : 2;
        bad.tag.update();
        CHECK(bad.tag.state() == State::None && bad.tag.callbacks == 0 && diagnostic.io_failures == 1);
    }
    for (unsigned bytes : {0u, 1u, 2u, 258u, 259u, 512u}) {
        Fixture boundary; boundary.select();
        boundary.unit.received.assign(bytes, 0x60); boundary.unit.fifo_bytes = uint16_t(bytes);
        boundary.unit._stored_irq = I_rxe32; boundary.tag.update();
        CHECK(boundary.tag.callbacks == (bytes == 258 ? 1u : 0u));
        CHECK(boundary.unit.count(Unit::FifoRead) == (bytes == 258 ? 1u : 0u));
    }
    for (uint8_t bits = 1; bits <= 7; ++bits) {
        Fixture partial; partial.select(); partial.frame({0x60}); partial.unit.partial_bits = bits;
        partial.tag.update(); CHECK(!partial.tag.callbacks && !partial.unit.count(Unit::FifoRead));
    }
    for (uint32_t error : {I_crc32, I_par32, I_err132, I_err232}) {
        Fixture invalid; invalid.select(); invalid.frame({0x60}); invalid.unit._stored_irq |= error;
        invalid.tag.update(); CHECK(invalid.tag.state() == State::Idle && !invalid.tag.callbacks);
    }
    Fixture lost; lost.select(); lost.frame({0x60}); lost.unit.field = false;
    lost.unit._stored_irq |= I_eof32; lost.tag.update();
    CHECK(lost.tag.state() == State::Off && !lost.tag.callbacks && !lost.unit.count(Unit::FifoRead));
    CHECK(diagnostic.eof_rxe == 1);
    Fixture cleanup; cleanup.select(); cleanup.frame({0x60}); cleanup.unit.partial_bits = 1;
    cleanup.unit.fail_at = 2; cleanup.tag.update(); // Fail CLEAR_FIFO, still attempt UNMASK.
    CHECK(cleanup.tag.state() == State::None && diagnostic.last_io_step == unsigned(Step::ReceiveRecovery));
    CHECK(cleanup.unit.calls[2].op == Unit::Command && cleanup.unit.calls[2].a == CMD_UNMASK_RECEIVE_DATA);
}

void irq_and_auth_recovery()
{
    Fixture f; f.select(); f.frame({0x1B, 1, 2, 3, 4}); f.tag.update();
    CHECK(f.tag.authenticated());
    CHECK(!f.tag.begin(f.picc, f.memory, sizeof(f.memory)) && f.tag.authenticated());
    f.unit.reset_calls(); f.frame({0x60}); f.unit.irq_failed = true; f.tag.update();
    CHECK(f.tag.state() == State::None && !f.tag.authenticated() && f.tag.callbacks == 1);
    CHECK(diagnostic.last_io_step == unsigned(Step::Interrupt) && !f.unit.count(Unit::FifoRead));
    f.unit.field = false; f.begin(); CHECK(!f.unit.irq_failed && !f.tag.authenticated());
    f.select(); f.frame({0x60}); f.tag.update(); CHECK(f.tag.callbacks == 2);
    Fixture spi; spi.select(); spi.frame({0x60}); spi.unit._interrupt_occurred = true;
    spi.unit.fail_at = 1; spi.tag.update();
    CHECK(spi.tag.state() == State::None && !spi.tag.callbacks && diagnostic.irq_failures == 1);
}

void expiry_tests()
{
    Fixture f; f.select(); clock_ms = 10000; f.frame({0x60}); f.tag.update();
    CHECK(f.tag.state() == State::Active && diagnostic.expiry == 0 && f.tag.callbacks == 1);
    for (unsigned now = 15000; now <= 35000; now += 5000) {
        clock_ms = now; f.frame({0x60}); f.tag.update(); CHECK(f.tag.state() == State::Active);
    }
    clock_ms = 45000; f.unit.field = false; f.tag.update();
    CHECK(f.tag.state() == State::Off && diagnostic.expiry == 1);
    Fixture disabled; disabled.select(); disabled.tag.setExpiredTime(0); clock_ms = 120000;
    disabled.tag.update(); CHECK(disabled.tag.state() == State::Active && !diagnostic.expiry);
    Fixture rollover; clock_ms = 0xFFFFFF00u; rollover.select();
    clock_ms += 9999; rollover.tag.update(); CHECK(rollover.tag.state() == State::Active);
    clock_ms += 1; rollover.unit.field = false; rollover.tag.update();
    CHECK(rollover.tag.state() == State::Off && diagnostic.expiry == 1);
}

void stop_and_tx_tests()
{
    for (size_t fail = 1; fail <= 4; ++fail) {
        Fixture f; f.select(); f.unit.fail_at = fail;
        CHECK(!f.tag.end() && f.tag.state() == State::None);
        CHECK(f.unit.calls.size() == 4 && f.unit.calls.back().op == Unit::Write);
        CHECK(diagnostic.last_io_step == unsigned(Step::Stop));
    }
    Fixture write; write.select(); write.frame({0xA2, 4, 1, 2, 3, 4}); write.tag.update();
    CHECK(write.unit.transmitted.size() == 1 && write.unit.transmitted[0] == 0x0A);
    CHECK(std::any_of(write.unit.calls.begin(), write.unit.calls.end(), [](const Unit::Call& c) {
        return c.op == Unit::TxLength && c.a == 0 && c.b == 4;
    }));
    CHECK(write.unit.calls.back().a == CMD_TRANSMIT_WITHOUT_CRC);
    Fixture tx_fail; tx_fail.select(); tx_fail.frame({0x60}); tx_fail.unit.fail_at = 3;
    tx_fail.tag.update(); CHECK(tx_fail.tag.state() == State::Idle);
    Fixture long_read; long_read.select(); long_read.frame({0x3A, 0, 0x86}); long_read.tag.update();
    CHECK(long_read.tag.state() == State::Active && long_read.unit.transmitted.size() == 540);
    CHECK(long_read.unit.count(Unit::FifoWrite) == 5);
    for (unsigned i = 532; i < 538; ++i) CHECK(long_read.unit.transmitted[i] == 0);
}

int main()
{
    try {
        startup_and_failures(); transition_failures(); idle_discard_and_halt_wakeup();
        selection_with_first_frame(); ready_unselected_frame(); config_lock_field_lifecycle();
        rx_frames_and_errors(); irq_and_auth_recovery(); expiry_tests(); stop_and_tx_tests();
        std::printf("ST25R3916 production listener passed: %u checks, %u injected I/O failures\n", checks, injected_failures);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ST25R3916 production listener FAILED: %s\n", error.what());
        return 1;
    }
}
