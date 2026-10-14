/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
/*
  Example using M5UnitUnified for ST25R3916
  NFC-A Emulation mode
*/
#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedNFC.h>
#include <M5Utility.h>
#include <wiring/m5_unit_unified_wiring.hpp>
#include <algorithm>
#include <cstring>
#include <esp_timer.h>
#include "AmiiboData.h"
#include "AmiiboAuth.h"
#include "Ntag215Emulator.h"
#include "ListenerDiagnostics.h"
#include "RfDiagnostics.h"
#include <NfcDiagnosticHook.h>
#include <NfcListenerStatus.h>
#if M5_NFC_RAM_DIAGNOSTICS_VERSION != 3
#error Use the project-local M5Unit-NFC diagnostic library
#endif
#if defined(M5_UNIT_NFC_EMULATION_TRACE)
#error Use RAM diagnostics without synchronous library serial trace
#endif
#include "ListenerRecovery.h"
#include "ListenerTiming.h"
#include "NfcFifoTx.h"
#include "FlashLog.h"

// *************************************************************
// Choose ONE define symbol to match the unit you are using
// NOTE: Emulation is supported ONLY on UnitNFC (ST25R3916) and CapCC1101NFC (ST25R3916).
//       UnitRFID2 (WS1850S) and M5Dial Builtin (WS1850S) do NOT support emulation.
// *************************************************************
#if !defined(USING_UNIT_NFC) && !defined(USING_CAP_CC1101)
// For UnitNFC (U216)
// #define USING_UNIT_NFC
// For CapCC1101 (U219)
// #define USING_CAP_CC1101
#endif
#if defined(USING_UNIT_RFID2) || defined(USING_M5DIAL_BUILTIN_WS1850S)
#error Emulation is NOT supported on UnitRFID2 / M5Dial Builtin (WS1850S). Use UnitNFC or CapCC1101NFC.
#endif

using namespace m5::nfc;
using namespace m5::nfc::a;
using namespace m5::nfc::a::mifare;
using namespace m5::nfc::a::mifare::classic;

namespace {
auto& lcd = M5.Display;
m5::unit::UnitUnified Units;

#if defined(USING_UNIT_NFC)
#pragma message("Choose UnitNFC")
m5::unit::UnitNFC unit{};  // I2C
#elif defined(USING_CAP_CC1101)
#pragma message("Choose CapCC1101NFC")
m5::unit::CapCC1101NFC unit{};  // CapCC1101 (SPI)
#else
#error Choose unit please!
#endif
// Select paired MRT step/value profiles through AMIIBO_RX_PROFILE.
constexpr bool USE_SHORT_RX_MASK = AMIIBO_RX_PROFILE == 1;
amiibo::TimingSnapshot listener_timing;
constexpr uint16_t MAX_FAST_READ_BYTES = amiibo::TAG_BYTES;
constexpr const char* BUILD_NAME = "NTAG215 embedded-zelda flash-log-v9-paper-listener-recovery";
// Compare 400000 with 100000 on hardware; BUS_CLOCK records configuration, not RF timing.
constexpr uint32_t NFC_I2C_CLOCK = 400000;
#if defined(M5_UNIT_NFC_EMULATION_TRACE)
constexpr bool LISTENER_TRACE_ENABLED = true;
#else
constexpr bool LISTENER_TRACE_ENABLED = false;
#endif
#if defined(ARDUINO)
FlashLog flash_log;
uint8_t save_failures{};
constexpr uint8_t AUTO_SAVE_FAILURE_LIMIT = 3;
#endif

// A running listener is never assumed healthy after a failed maintenance resume.
using amiibo::RebuildStep;
constexpr const char* rebuild_step_text(const uint8_t step)
{
    switch (static_cast<RebuildStep>(step)) {
        case RebuildStep::Configure: return "configureEmulationMode";
        case RebuildStep::Begin: return "emu.begin";
        case RebuildStep::Timing: return "configure_listener_timing";
        case RebuildStep::Recover: return "recovery";
        default: return "none";
    }
}
uint32_t ram_hook_calls{};
bool listener_running{};
bool recovery_pending{};
uint32_t last_recovery_attempt{};
uint32_t last_maintenance_probe{};
constexpr uint32_t RECOVERY_RETRY_MS = 1000;
constexpr uint32_t MAINTENANCE_PROBE_MS = 100;


bool configure_listener_timing()
{
    static_assert(m5::unit::st25r3916::regval::mrt_step == 0x08, "Update timing bit mapping");
    struct IO {
        bool read_passive(uint8_t& v) { return unit.readNFCIP1PassiveTargetDefinition(v); }
        bool write_passive(uint8_t v) { return unit.writeNFCIP1PassiveTargetDefinition(v); }
        bool read_control(uint8_t& v) { return unit.readTimerAndEMVControl(v); }
        bool write_control(uint8_t v) { return unit.writeTimerAndEMVControl(v); }
        bool read_mask(uint8_t& v) { return unit.readMaskReceiveTimer(v); }
        bool write_mask(uint8_t v) { return unit.writeMaskReceiveTimer(v); }
    } io;
    return amiibo::apply_listener_timing(io, amiibo::LISTENER_TIMING, listener_timing);
}

// Embedded NTAG215 dump: retain the original UID and every memory page.
constexpr Type type{Type::NTAG_215};
constexpr uint8_t uid[] = {AMIIBO_DATA[0], AMIIBO_DATA[1], AMIIBO_DATA[2],
                           AMIIBO_DATA[4], AMIIBO_DATA[5], AMIIBO_DATA[6], AMIIBO_DATA[7]};
uint8_t picc_memory[amiibo::TAG_BYTES]{};
PICC picc{};
bool credentials_restored{};

void initialize_amiibo_tag()
{
    static_assert(sizeof(picc_memory) == sizeof(AMIIBO_DATA), "Tag memory size mismatch");
    memcpy(picc_memory, AMIIBO_DATA, sizeof(picc_memory));
    credentials_restored = amiibo::restore_masked_credentials(picc_memory);
    M5.Log.printf("Amiibo auth: %s\n", credentials_restored ? "restored masked PWD/PACK from UID" : "using dump credentials");
}

// NTAG215 read/write protection and static/dynamic locks are enforced in software.
// AUTHLIM, real counters and originality signatures remain separate capabilities.
class AmiiboNTAG215 : public amiibo::Ntag215Emulator<decltype(unit)> {
public:
    using Base = amiibo::Ntag215Emulator<decltype(unit)>;
    using Base::Base;
    static_assert(static_cast<uint8_t>(State::None) == 0 && static_cast<uint8_t>(State::Off) == 1 &&
                  static_cast<uint8_t>(State::Idle) == 2 && static_cast<uint8_t>(State::Ready) == 3 &&
                  static_cast<uint8_t>(State::Active) == 4 && static_cast<uint8_t>(State::Halt) == 5,
                  "Update ListenerDiagnostics when the library state enum changes");

    // Record metadata only; never print or draw while a reader is waiting.
    State receive_callback(const uint8_t* rx, const uint32_t rx_len) override
    {
        const uint32_t started = static_cast<uint32_t>(esp_timer_get_time());
        prepare_trace();
        _listener_diag.command();
        const State result = Base::receive_callback(rx, rx_len);
        // Include diagnostic prelude in the callback-relative TX timestamp.
        if (_tx_result != amiibo::TxResult::Invalid) _tx_start_us += _callback_started - started;
        auto& entry = _trace[_next];
        entry = {m5::utility::millis(), static_cast<uint32_t>(esp_timer_get_time()) - started,
                 _tx_start_us, static_cast<uint16_t>(rx_len), _tx_size,
                 static_cast<uint8_t>(rx_len && rx ? rx[0] : 0), 0, 0, _nak, _tx_ok,
                 _auth_match, static_cast<uint8_t>(_tx_result)};
        // Do not record password bytes or written payloads.
        if (rx && rx_len > 1 && (rx[0] == 0x30 || rx[0] == 0x3A || rx[0] == 0xA2)) entry.first = rx[1];
        if (rx && rx_len > 2 && rx[0] == 0x3A) entry.last = rx[2];
        _next = (_next + 1) % TRACE_SIZE;
        if (_count < TRACE_SIZE) ++_count;
        ++_total;
        return result;
    }

    void clear_trace()
    {
        _next = _count = _state_next = _state_count = 0;
        _total = _state_total = 0;
        _max_service_us = 0;  // batch scope; _max_service_us_session is deliberately kept
        _rf.clear();
        _session.clear_statistics();
        _listener_diag.clear_batch(m5::utility::millis());
        _dirty = false;
        memset(_states, 0, sizeof(_states));
    }

    void record_state(const State value)
    {
        if (value != State::Active) _session.reset_auth();
        if (value == State::Off && !_dirty) return;
        prepare_trace();
        const uint32_t now = m5::utility::millis();
        const uint8_t previous = _listener_diag.state;
        const uint32_t elapsed = _listener_diag.observe(static_cast<uint8_t>(value), now);
        ++_states[static_cast<unsigned>(value)];
        _state_trace[_state_next] = {now, static_cast<uint8_t>(value), previous, elapsed};
        _state_next = (_state_next + 1) % STATE_TRACE_SIZE;
        if (_state_count < STATE_TRACE_SIZE) ++_state_count;
        ++_state_total;
    }

    void record_rf(uint8_t kind, uint32_t a, uint32_t b, uint32_t c)
    {
        prepare_trace();
        _rf.record(static_cast<uint32_t>(esp_timer_get_time()), static_cast<uint8_t>(state()), kind, a, b, c);
    }
    void service_timing(uint32_t start, uint32_t units_us, uint32_t listener_us, bool running)
    {
        _rf.service(start, units_us, listener_us, running);
    }
    bool pending_log() const { return _dirty; }
    void mark_saved() { _dirty = false; }
    void record_service_us(uint32_t elapsed, State previous_state)
    {
        if (elapsed > _max_service_us_session) _max_service_us_session = elapsed;
        if (!_dirty) return;
        _listener_diag.service(static_cast<uint8_t>(previous_state), elapsed);
        if (elapsed > _max_service_us) _max_service_us = elapsed;
    }
    // B1: count the failures so the next log file can carry them. This is the only
    // recovery channel when the emulation itself could not be restarted.
    void record_resume_failure(const uint8_t step)
    {
        if (_resume_failures < RESUME_FAILURE_LIMIT) ++_resume_failures;
        _resume_step = step;
    }
    void clear_resume_failures()
    {
        _resume_failures = 0;
        _resume_step     = 0;
    }
    uint8_t resume_failures() const { return _resume_failures; }
    uint8_t resume_step() const { return _resume_step; }
    void reset_session_auth() { _session.reset_auth(); _rf.has_previous = false; }
    void dump_trace() const { dump_trace(M5.Log); }

    template <class Output>
    void dump_trace(Output& out) const
    {
        out.printf("AUTH_MODE=STRICT RX_MASK=%s FIFO_PRELOAD=%u FAST_READ_MAX=%u\n",
                   USE_SHORT_RX_MASK ? "SHORT" : "PAPER", amiibo::TX_CHUNK, MAX_FAST_READ_BYTES);
        out.printf("RX_PROFILE=%s FDEL_EXPECTED=%u MRT_STEP_FC=%u MRT_EXPECTED=%02X "
                   "TIMING_VERIFIED=%u PASSIVE_TARGET=%02X TIMER_CONTROL=%02X MASK_RX_TIMER=%02X\n",
                   amiibo::RX_PROFILE_NAME, amiibo::LISTENER_TIMING.fdel, amiibo::LISTENER_TIMING.step_fc(),
                   amiibo::LISTENER_TIMING.mask, listener_timing.valid, listener_timing.passive,
                   listener_timing.control, listener_timing.mask);
        out.printf("ATQA=%04X SAK=%02X BUS_CLOCK=%u AUTH_RESTORED=%u\n",
                   picc.atqa, picc.sak, unit.component_config().clock, credentials_restored);
        out.printf("SIGNATURE_PRESENT=%u\n", has_signature());
        out.printf("==== NFC diagnostic (latest %u commands) ====\n", _count);
        out.printf("COMMAND_TOTAL=%u COMMAND_DROPPED=%u STATE_DROPPED=%u MAX_SERVICE_US=%u "
                   "MAX_SERVICE_US_SESSION=%u\n",
                   _total, _total - _count, _state_total - _state_count, _max_service_us,
                   _max_service_us_session);
        // E2: the three counters that answer "how far did the reader get?" without
        // scrolling through the per-command lines.
        out.printf("PWD_AUTH_SEEN=%u AUTH_OK=%u WROTE_PWD_PAGE=%u AUTH_NOW=%d\n",
                   _session.pwd_auth_seen, _session.auth_ok, _session.wrote_pwd_page,
                   _session.authenticated ? 1 : 0);
        out.printf("READY_TO_ACTIVE=%u READY_TO_OFF=%u ACTIVE_TO_OFF=%u ACTIVE_OFF_NO_CMD=%u "
                   "MAX_READY_MS=%u MAX_ACTIVE_MS=%u\n",
                   _listener_diag.ready_to_active, _listener_diag.ready_to_off,
                   _listener_diag.active_to_off, _listener_diag.active_off_no_command,
                   _listener_diag.max_ready_ms, _listener_diag.max_active_ms);
        out.printf("MAX_SERVICE_US_BY_STATE None/Off/Idle/Ready/Active/Halt: %u/%u/%u/%u/%u/%u\n",
                   _listener_diag.max_service_us[0], _listener_diag.max_service_us[1],
                   _listener_diag.max_service_us[2], _listener_diag.max_service_us[3],
                   _listener_diag.max_service_us[4], _listener_diag.max_service_us[5]);
        out.printf("LISTENER_RUNNING=%u RECOVERY_PENDING=%u LISTENER_TRACE=%u\n",
                   listener_running, recovery_pending, LISTENER_TRACE_ENABLED);
#if defined(ARDUINO)
        out.printf("SAVE_FAILURES=%u AUTO_SAVE=%s\n", save_failures,
                   save_failures < AUTO_SAVE_FAILURE_LIMIT ? "enabled" : "paused");
#endif
        out.printf("SNAPSHOT=%s RAM_HOOK_VERSION=%u RAM_HOOK_CALLS=%u RF_TOTAL=%u RF_DROPPED=%u\n",
                   listener_running ? "RUNNING" : "MAINTENANCE_PAUSED", M5_NFC_RAM_DIAGNOSTICS_VERSION,
                   ram_hook_calls, _rf.total, _rf.total - _rf.count);
        out.printf("IRQ_NONZERO_READS=%u IRQ_READ_FAILURES=%u RX_REJECTED=%u FIFO_READ_FAILURES=%u EXPIRY_COUNT=%u\n",
                   _rf.irq_reads, _rf.irq_failures, _rf.rx_errors, _rf.fifo_failures, _rf.expiry);
        out.printf("MAX_UNITS_US=%u MAX_LISTENER_US=%u MAX_POLL_GAP_US=%u SERVICE_GE_2MS=%u\n",
                   _rf.max_units_us, _rf.max_listener_us, _rf.max_gap_us, _rf.slow_services);
        out.printf("ACTIVE_EOF_RXE=%u RX_ACCEPTED=%u RX_STATUS_READS_PER_FRAME=1\n",
                   _rf.eof_rxe, _rf.rx_accepted);
        out.printf("UNSELECTED_RX_DISCARDED=%u LISTENER_IO_FAILURES=%u LAST_IO_STEP=%s\n",
                   _rf.unselected_rx_discarded, _rf.io_failures, m5::nfc::detail::listener_io_step_name(_rf.last_io_step));
        using namespace m5::unit::st25r3916::regval;
        const uint32_t masks[] = {I_eon32, I_eof32, I_nfct32, I_rxs32, I_rxe32, I_txe32,
                                 I_crc32, I_par32, I_err132, I_err232, I_rxe_pta32, I_wu_a32, I_wu_ax32};
        const char* names[] = {"EON", "EOF", "NFCT", "RXS", "RXE", "TXE", "CRC", "PAR", "ERR1", "ERR2", "RXE_PTA", "WU_A", "WU_AX"};
        out.printf("IRQ_OBSERVED");
        for (unsigned i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i)
            out.printf(" %s=%u", names[i], _rf.irq_count(masks[i]));
        out.printf("\nRF timestamps are software observation us (32-bit wrap), not RF/FDT measurements.\n");
        out.printf("RF kind: 1=IRQ(a=bits) 2=TRACE(a=event,b=irq,c=detail) 3=BITRATE(a=ok,b=raw) "
                   "4=PTA(a=ok,b=raw) 5=FIFO_SIZE(a=ok,b=bytes,c=bits) 6=FIFO_READ(a=ok,b=actual,c=capacity) "
                   "7=RX_REJECT(a=irq,b=length,c=context) 8=EXPIRY(a=state,b=limit_ms) 9=IRQ_FAIL "
                   "10=UNSELECTED_RX(a=irq) 11=SW_CRC(a=value,legacy) 12=ACTIVE_EOF_RXE(a=irq) "
                   "13=RX_ACCEPTED(a=payload_bytes) 14=LISTENER_IO_FAIL(a=step)\n");
        out.printf("TRACE event: 0=Off 1=Idle 2=Ready 3=Active 4=Halt 5=HaltIRQ 6=ReadyIRQ 7=PTA\n");
        for (uint16_t i = 0; i < _rf.count; ++i) {
            const auto& e = _rf.at(i);
            out.printf("RF US=%u STATE=%u KIND=%u A=%08X B=%08X C=%08X\n",
                       e.us, e.state, e.kind, e.a, e.b, e.c);
        }
        out.printf("Stage counters are loop-observed transitions, not RF/IRQ counts.\n");
        // Non-zero means a maintenance cycle could not restart the emulation.
        out.printf("RESUME_FAIL=%u RESUME_STEP=%s\n", _resume_failures, rebuild_step_text(_resume_step));
        out.printf("SENT=queued only; TX_START_US/HANDLE_US start at callback, not RF reception\n");
        out.printf("TX_RESULT: 0=queued 1=no-TX 2=setup 3=status 4=FIFO 5=empty 6=timeout 7=write\n");
        out.printf("State entries None/Off/Idle/Ready/Active/Halt: %u/%u/%u/%u/%u/%u\n",
                   _states[0], _states[1], _states[2], _states[3], _states[4], _states[5]);
        out.printf("Locks=%02X %02X dyn=%02X %02X %02X AUTH0=%02X ACCESS=%02X\n",
                   picc_memory[10], picc_memory[11], picc_memory[520], picc_memory[521], picc_memory[522],
                   picc_memory[527], picc_memory[528]);
        for (uint16_t i = 0; i < _state_count; ++i) {
            const auto& e = _state_trace[(_state_next + STATE_TRACE_SIZE - _state_count + i) % STATE_TRACE_SIZE];
            out.printf("%u STATE=%u PREV_STATE=%u PREV_MS=%u\n", e.ms, e.state, e.previous, e.elapsed_ms);
        }
        for (uint16_t i = 0; i < _count; ++i) {
            const auto& e = _trace[(_next + TRACE_SIZE - _count + i) % TRACE_SIZE];
            out.printf("%u CMD=%02X RX=%u PAGE=%02X..%02X TX=%u NIBBLE=%02X SENT=%u HANDLE_US=%u AUTH_MATCH=%d TX_START_US=%u TX_RESULT=%u\n",
                       e.ms, e.cmd, e.rx_size, e.first, e.last, e.tx_size, e.nak, e.ok,
                       e.handle_us, e.auth_match, e.tx_start_us, e.tx_result);
        }
    }

private:
    struct Entry {
        uint32_t ms, handle_us, tx_start_us;
        uint16_t rx_size, tx_size;
        uint8_t cmd, first, last, nak;
        bool ok;
        int8_t auth_match;
        uint8_t tx_result;
    };
    static_assert(sizeof(Entry) == 24, "Keep the 256-entry trace within 6 KiB");
    struct StateEntry { uint32_t ms; uint8_t state, previous; uint32_t elapsed_ms; };
    static constexpr uint16_t TRACE_SIZE = 256;
    static constexpr uint16_t STATE_TRACE_SIZE = 256;
    static constexpr uint8_t RESUME_FAILURE_LIMIT = 100;
    StateEntry _state_trace[STATE_TRACE_SIZE]{};
    uint16_t _state_next{}, _state_count{};
    uint32_t _total{}, _state_total{}, _max_service_us{}, _max_service_us_session{};
    bool _dirty{};
    amiibo::ListenerDiagnostics _listener_diag{};
    amiibo::RfDiagnostics _rf{};
    // Failure counters survive batch rotation and successful retries.
    uint8_t _resume_failures{};
    uint8_t _resume_step{};
    void prepare_trace()
    {
        if (!_dirty) clear_trace();
        _dirty = true;
    }
    Entry _trace[TRACE_SIZE]{};
    uint16_t _next{}, _count{};
    uint32_t _states[6]{};


};

AmiiboNTAG215 emu_a{unit};
// Snapshot for the optional serial memory dump; never included in Flash logs.
uint8_t picc_memory_seen[sizeof(picc_memory)]{};

void dump_emulated_memory(const bool only_changed)
{
    constexpr uint16_t BLOCK_SIZE{4};
    const uint16_t blocks = (sizeof(picc_memory) + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint16_t shown{};

    M5.Log.printf("==== Emulated memory (%s) ====\n", only_changed ? "changed since the last hold" : "all");
    for (uint16_t blk = 0; blk < blocks; ++blk) {
        const uint16_t offset = blk * BLOCK_SIZE;
        const uint16_t len    = std::min<uint16_t>(BLOCK_SIZE, sizeof(picc_memory) - offset);
        if (only_changed && memcmp(picc_memory + offset, picc_memory_seen + offset, len) == 0) {
            continue;
        }
        M5.Log.printf("[%03X]:", offset);
        for (uint16_t i = 0; i < len; ++i) {
            M5.Log.printf("%02X ", picc_memory[offset + i]);
        }
        M5.Log.printf("\n");
        ++shown;
    }
    if (only_changed) {
        if (!shown) {
            M5.Log.printf("No block has changed\n");
        }
        memcpy(picc_memory_seen, picc_memory, sizeof(picc_memory));
    }
}

constexpr uint16_t color_table[] = {
    //  None,      Off,     Idle,     Ready,   Active,      Halt };
    TFT_BLACK, TFT_RED, TFT_BLUE, TFT_YELLOW, TFT_GREEN, TFT_MAGENTA};
constexpr const char* state_table[] = {"-", "O", "I", "R", "A", "H"};

}  // namespace

extern "C" void m5_nfc_ram_event(uint8_t kind, uint32_t a, uint32_t b, uint32_t c)
{
    ++ram_hook_calls;
    if (listener_running) emu_a.record_rf(kind, a, b, c);
}

bool resume_after_maintenance();
#if defined(ARDUINO)
void display_log_status();
#endif

void setup()
{
    M5.begin();
#if defined(ARDUINO)
    Serial.begin(115200);
    flash_log.begin();
    M5.Log.printf("Flash log: %s; serial commands: logs, logcat [filename], diag, save\n", flash_log.status());
#endif
    M5.setTouchButtonHeightByRatio(100);

    // The screen shall be in landscape mode
    if (lcd.height() > lcd.width()) {
        lcd.setRotation(1);
    }

    // RF-activity-based expiry and rollover handling remain in M5Unit-NFC 0.1.2.
    emu_a.setExpiredTime(10000);

    // Emulation settings
    auto cfg      = unit.config();
    cfg.emulation = true;
    cfg.mode      = NFC::A;
    unit.config(cfg);

    bool unit_ready{};
#if defined(USING_UNIT_NFC)
    // A1: pass the clock explicitly instead of relying on the 0.1.2 component default (also 400 kHz).
    unit_ready = m5::unit::wiring::addI2C(Units, unit, NFC_I2C_CLOCK, m5::unit::wiring::NessoPort::PortA) &&
                 Units.begin();
#elif defined(USING_CAP_CC1101)
    // SPI mode 1 (CPOL=0, CPHA=1). Use literal so this builds in ESP-IDF native too
    // (Arduino's SPI_MODE1 is not defined there).
    unit_ready = m5::unit::wiring::addSPI(Units, unit, 10000000, 1) && Units.begin();
#endif
    if (!unit_ready) {
        M5_LOGE("Failed to begin");
        m5::unit::wiring::failStop();
    }
    M5_LOGI("M5UnitUnified initialized");
    M5_LOGI("%s", Units.debugInfo().c_str());
#if defined(USING_UNIT_NFC)
    M5.Log.printf("NFC I2C clock: requested %u, active %u\n", static_cast<unsigned>(NFC_I2C_CLOCK),
                  static_cast<unsigned>(unit.component_config().clock));
#endif

#if defined(ARDUINO)
    const bool saved = flash_log.save(BUILD_NAME, [](auto& out) {
        // Configured clock after Units.begin(); this is not a bus frequency measurement.
        out.printf("EVENT=BOOT RESET_REASON=%u BUS_CLOCK=%u RX_MASK=%s "
                   "RX_PROFILE=%s FDEL_EXPECTED=%u MRT_STEP_FC=%u MRT_EXPECTED=%02X\n",
                   static_cast<unsigned>(esp_reset_reason()), unit.component_config().clock,
                   USE_SHORT_RX_MASK ? "SHORT" : "PAPER", amiibo::RX_PROFILE_NAME,
                   amiibo::LISTENER_TIMING.fdel, amiibo::LISTENER_TIMING.step_fc(), amiibo::LISTENER_TIMING.mask);
    });
    M5.Log.printf("Flash log boot: %s %s\n", flash_log.status(), saved ? flash_log.last_path() : "");
#endif

    lcd.setFont(&fonts::Font2);

    //
    lcd.startWrite();
    lcd.fillScreen(TFT_RED);
    if (picc.emulate(type, uid, sizeof(uid))) {
        initialize_amiibo_tag();
        memcpy(picc_memory_seen, picc_memory, sizeof(picc_memory));
        listener_running = resume_after_maintenance();
        recovery_pending = !listener_running;
        last_recovery_attempt = m5::utility::millis();
        if (listener_running) {
            M5.Log.printf("Build: %s, RX profile=%s, FDEL=%u, MRT=%u/fc x %u\n", BUILD_NAME,
                          amiibo::RX_PROFILE_NAME, amiibo::LISTENER_TIMING.fdel,
                          amiibo::LISTENER_TIMING.step_fc(), amiibo::LISTENER_TIMING.mask);
            lcd.fillScreen(TFT_DARKGREEN);
            lcd.setCursor(0, 16);
            const auto& e_picc = emu_a.emulatePICC();
            M5.Log.printf("Emulation:%s %s ATQA:%04X SAK:%u\n", e_picc.typeAsString().c_str(),
                          e_picc.uidAsString().c_str(), e_picc.atqa, e_picc.sak);
            lcd.printf("%s\n%s\nATQA:%04X SAK:%u", e_picc.typeAsString().c_str(), e_picc.uidAsString().c_str(),
                       e_picc.atqa, e_picc.sak);
        }
    }
#if defined(ARDUINO)
    lcd.setCursor(0, lcd.height() - 16);
    lcd.printf("Log: %s", flash_log.display_status());
#endif
    lcd.fillRect(0, 0, 32, 16, color_table[0]);
    lcd.drawString(state_table[0], 0, 0);
    lcd.endWrite();
#if defined(ARDUINO)
    if (recovery_pending) display_log_status();
#endif
}

// The field probe is rate limited even when a save/export cannot start.
bool maintenance_field_absent()
{
    const uint32_t now = m5::utility::millis();
    if (now - last_maintenance_probe < MAINTENANCE_PROBE_MS) return false;
    last_maintenance_probe = now;
    uint8_t aux{};
    return unit.readAuxiliaryDisplay(aux) && !(aux & m5::unit::st25r3916::regval::efd_o);
}

bool pause_for_maintenance()
{
    // A failed resume leaves software listening stopped. Serial export remains usable.
    if (!listener_running) return true;
    if (!maintenance_field_absent()) return false;
    listener_running = false;
    emu_a.reset_session_auth();
    if (!emu_a.end()) {
        emu_a.record_resume_failure(static_cast<uint8_t>(RebuildStep::Recover));
        recovery_pending = true;
        last_recovery_attempt = m5::utility::millis();
        return false;
    }
    return true;
}

bool resume_after_maintenance()
{
    struct IO {
        bool configure() { return unit.configureEmulationMode(NFC::A); }
        bool begin() { return emu_a.begin(picc, picc_memory, sizeof(picc_memory)); }
        bool timing() { return configure_listener_timing(); }
        bool end() { return emu_a.end(); }
        void failed(RebuildStep step)
        {
            const auto value = static_cast<uint8_t>(step);
            M5_LOGE("Failed to start/resume NFC, step=%s", rebuild_step_text(value));
            emu_a.record_resume_failure(value);
        }
    } io;
    return amiibo::rebuild_listener(io);
}

#if defined(ARDUINO)
void display_log_status()
{
    lcd.startWrite();
    lcd.fillRect(0, lcd.height() - 16, lcd.width(), 16, TFT_DARKGREEN);
    lcd.setCursor(0, lcd.height() - 16);
    if (recovery_pending) lcd.print("NFC resume failed; retrying");
    else if (save_failures >= AUTO_SAVE_FAILURE_LIMIT) lcd.print("Log: auto-save paused (use save)");
    else lcd.printf("Log: %s", !strcmp(flash_log.status(), "saved") ? flash_log.last_path() : flash_log.display_status());
    lcd.endWrite();
}

bool save_diagnostic(const bool manual = false)
{
    if (manual) save_failures = 0;
    const bool saved = flash_log.save(BUILD_NAME, [](auto& out) { emu_a.dump_trace(out); });
    if (saved) {
        emu_a.mark_saved();
        emu_a.clear_resume_failures();
        save_failures = 0;
    } else if (save_failures < AUTO_SAVE_FAILURE_LIMIT) ++save_failures;
    M5.Log.printf("Flash log: %s %s; SAVE_FAILURES=%u\n", flash_log.status(),
                  saved ? flash_log.last_path() : "", save_failures);
    display_log_status();
    return saved;
}
#endif

void after_maintenance()
{
    last_recovery_attempt = m5::utility::millis();
    listener_running = resume_after_maintenance();
    recovery_pending = !listener_running;
#if defined(ARDUINO)
    if (recovery_pending) {
        // Preserve the RAM diagnostic for the next successful batch save. A single
        // immediate report is best effort; retries themselves never write Flash.
        if (flash_log.ready() && save_failures < AUTO_SAVE_FAILURE_LIMIT) {
            const bool saved = flash_log.save(BUILD_NAME, [](auto& out) {
                out.printf("EVENT=RESUME_FAIL COUNT=%u STEP=%s STATE=%u\n", emu_a.resume_failures(),
                           rebuild_step_text(emu_a.resume_step()), static_cast<unsigned>(emu_a.state()));
            });
            M5.Log.printf("Flash recovery report: %s %s\n", flash_log.status(), saved ? flash_log.last_path() : "");
        }
        display_log_status();
    }
#endif
}

void retry_listener_if_needed(const uint32_t now)
{
    if (!recovery_pending || now - last_recovery_attempt < RECOVERY_RETRY_MS) return;
    last_recovery_attempt = now;
    if (!maintenance_field_absent()) return;
    listener_running = resume_after_maintenance();
    recovery_pending = !listener_running;
    if (listener_running) {
        M5.Log.printf("NFC listener recovered\n");
#if defined(ARDUINO)
        display_log_status();
#endif
    }
}

void loop()
{
    const uint32_t service_started = static_cast<uint32_t>(esp_timer_get_time());
    const auto previous_state = emu_a.state();
    uint32_t units_done = service_started;
    if (listener_running) {
        Units.update();
        units_done = static_cast<uint32_t>(esp_timer_get_time());
        emu_a.update();
    }
    const uint32_t service_done = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t service_us = service_done - service_started;
    emu_a.service_timing(service_started, units_done - service_started,
                        service_done - units_done, listener_running);
    if (listener_running && emu_a.state() == EmulationLayerA::State::None) {
        // A failed adapter transition is fatal for this listener instance. Keep
        // diagnostics in RAM and reuse the rate-limited, off-field recovery path.
        listener_running = false;
        recovery_pending = true;
        last_recovery_attempt = m5::utility::millis();
        emu_a.reset_session_auth();
        emu_a.record_resume_failure(static_cast<uint8_t>(RebuildStep::Recover));
    }
    const auto state = emu_a.state();
    static EmulationLayerA::State latest{};
    static EmulationLayerA::State displayed{};
    static uint32_t last_busy{};
    const uint32_t now = m5::utility::millis();
    if (latest != state) {
        latest = state;
        emu_a.record_state(state);
    }
    emu_a.record_service_us(service_us, previous_state);
    if (state != EmulationLayerA::State::Off && state != EmulationLayerA::State::None) {
        last_busy = now;
        return;
    }
    if (now - last_busy < 1000) return;
    retry_listener_if_needed(now);
    if (listener_running && emu_a.state() != state) {
        latest = emu_a.state();
        last_busy = now;
        return;
    }

#if defined(ARDUINO)
    static uint32_t last_save_attempt{};
    if (flash_log.ready() && emu_a.pending_log() && !recovery_pending &&
        save_failures < AUTO_SAVE_FAILURE_LIMIT && now - last_save_attempt >= 5000) {
        if (!pause_for_maintenance()) return;
        last_save_attempt = now;
        save_diagnostic();
        after_maintenance();
        latest = emu_a.state();
        last_busy = m5::utility::millis();
        return;
    }

    // Bounded, non-blocking command reception. Execution waits until RF is absent.
    static char command[64]{};
    static uint8_t command_size{};
    static bool command_ready{}, command_overflow{};
    for (unsigned i = 0; i < sizeof(command) && !command_ready && Serial.available(); ++i) {
        const char ch = Serial.read();
        if (ch == '\n' || ch == '\r') {
            command[command_size] = 0;
            command_ready = command_size != 0 || command_overflow;
        } else if (command_size + 1 < sizeof(command)) {
            command[command_size++] = ch;
        } else command_overflow = true;
    }
    if (command_ready) {
        if (!pause_for_maintenance()) return;
        if (command_overflow) Serial.println("Command too long");
        else if (!strcmp(command, "diag")) emu_a.dump_trace(Serial);
        else if (!strcmp(command, "save")) {
            save_failures = 0;
            if (emu_a.pending_log()) save_diagnostic(true);
            else Serial.println("No pending diagnostic; automatic saving enabled");
        }
        else if (!strcmp(command, "logs")) flash_log.list(Serial);
        else if (!strcmp(command, "logcat")) flash_log.cat(nullptr, Serial);
        else if (!strncmp(command, "logcat ", 7)) flash_log.cat(command + 7, Serial);
        else Serial.println("Commands: logs | logcat [nfc-00000001.txt] | diag | save");
        command_size = 0;
        command_ready = command_overflow = false;
        after_maintenance();
        latest = emu_a.state();
        last_busy = m5::utility::millis();
        return;
    }
#endif
    M5.update();
    const bool clicked = M5.BtnA.wasClicked();
    const bool held = M5.BtnA.wasHold();
    if (clicked || held) {
        if (!pause_for_maintenance()) return;
        if (clicked) dump_emulated_memory(false);
        else {
            emu_a.dump_trace();
            dump_emulated_memory(true);  // Serial only; never persist page contents.
#if defined(ARDUINO)
            if (emu_a.pending_log()) save_diagnostic(true);
#endif
        }
        after_maintenance();
        latest = emu_a.state();
        last_busy = m5::utility::millis();
        return;
    }
    if (displayed != state) {
        displayed = state;
        lcd.startWrite();
        lcd.fillRect(0, 0, 32, 16, color_table[m5::stl::to_underlying(state)]);
        lcd.drawString(state_table[m5::stl::to_underlying(state)], 0, 0);
        lcd.endWrite();
    }
}

#if !defined(ARDUINO)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

#if CONFIG_FREERTOS_UNICORE
static inline void feedIdleTaskPeriodically(void)
{
    constexpr uint32_t FEED_INTERVAL_MS   = 2000;
    constexpr TickType_t FEED_SLEEP_TICKS = pdMS_TO_TICKS(5);
    static uint32_t s_next_feed_ms        = 0;
    const uint32_t now_ms                 = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (now_ms >= s_next_feed_ms) {
        s_next_feed_ms = now_ms + FEED_INTERVAL_MS;
        vTaskDelay(FEED_SLEEP_TICKS);
    }
}
#endif

extern "C" void app_main(void)
{
    setup();
    for (;;) {
#if CONFIG_FREERTOS_UNICORE
        feedIdleTaskPeriodically();
#endif
        loop();
    }
}
#endif
