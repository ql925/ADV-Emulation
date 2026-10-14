#pragma once
#include "NtagRead.h"
#include "NtagAccess.h"

namespace amiibo {
// Authentication belongs to a selection session; statistics belong to a log batch.
struct NtagSession {
    bool authenticated{};
    bool field_active{}, config_locked{};
    uint32_t pwd_auth_seen{}, auth_ok{}, wrote_pwd_page{};
    constexpr void reset_auth() { authenticated = false; }
    constexpr void clear_statistics() { pwd_auth_seen = auth_ok = wrote_pwd_page = 0; }
    constexpr void begin_field(const uint8_t* memory)
    {
        if (field_active) return;
        field_active = true;
        config_locked = (memory[ACCESS_OFFSET] & 0x40) != 0;
        reset_auth();
    }
    constexpr void end_field() { field_active = false; reset_auth(); }
};

enum class ReplyKind : uint8_t { Data, Nibble, Halt, Idle };
struct CommandReply {
    ReplyKind kind;
    const uint8_t* data{};
    uint16_t size{};
    uint8_t nibble{};
    int8_t auth_match{-1};  // Result of this command only, never the session state.
};

constexpr uint8_t NTAG215_VERSION[] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03};
constexpr uint8_t EMPTY_COUNTER[3]{};

constexpr CommandReply data_reply(const uint8_t* data, uint16_t size, int8_t auth_match = -1)
{
    return {ReplyKind::Data, data, size, 0, auth_match};
}
constexpr CommandReply nak_reply(uint8_t nibble = 0x00, int8_t auth_match = -1)
{
    return {ReplyKind::Nibble, nullptr, 0, nibble, auth_match};
}

// No hardware I/O. The caller supplies a full-tag scratch buffer for READ rollover
// and FAST_READ ranges containing credentials. CFGLCK latches on a new RF field;
// authentication never overrides permanent locks or reveals credentials.
constexpr CommandReply handle_ntag_command(uint8_t* memory, const uint8_t* rx, uint32_t rx_len,
                                           NtagSession& session, uint8_t* scratch, uint16_t capacity,
                                           const uint8_t* signature = nullptr)
{
    if (!memory || !rx || !rx_len) return {ReplyKind::Idle};
    session.begin_field(memory);
    switch (rx[0]) {
        case 0x30: {
            if (rx_len != 2 || rx[1] >= PAGE_COUNT) return nak_reply();
            const bool read_protection = !session.authenticated && (memory[ACCESS_OFFSET] & 0x80) &&
                                         memory[AUTH0_OFFSET] < PAGE_COUNT;
            const uint16_t accessible = read_protection ? memory[AUTH0_OFFSET] : PAGE_COUNT;
            if (rx[1] >= accessible) return nak_reply(0x04);
            if (rx[1] <= 0x81 && rx[1] + 4 <= accessible) return data_reply(memory + 4 * rx[1], 16);
            if (!scratch || capacity < 16) return nak_reply();
            read_pages(memory, rx[1], scratch, 16, accessible);
            return data_reply(scratch, 16);
        }
        case 0x3A: {
            if (rx_len != 3) return nak_reply();
            const uint16_t count = fast_read_size(rx[1], rx[2]);
            if (!count) return nak_reply();
            if ((memory[ACCESS_OFFSET] & 0x80) && password_required(memory, rx[2], session.authenticated))
                return nak_reply(0x04);
            if (rx[2] < 0x85) return data_reply(memory + 4 * rx[1], count);
            if (!scratch || capacity < count) return nak_reply();
            for (uint16_t i = 0; i < count; ++i) scratch[i] = memory[4 * rx[1] + i];
            mask_protected(scratch, 4 * rx[1], count);
            return data_reply(scratch, count);
        }
        case 0xA2: {
            if (rx_len != 6 || rx[1] < 2 || rx[1] >= PAGE_COUNT) return nak_reply();
            if (password_required(memory, rx[1], session.authenticated)) return nak_reply(0x04);
            if (permanently_locked(memory, rx[1]) ||
                (session.config_locked && (rx[1] == 0x83 || rx[1] == 0x84))) return nak_reply();
            const uint16_t offset = 4 * rx[1];
            if (rx[1] == 2) {
                write_static_locks(memory, rx[4], rx[5]);
            } else if (rx[1] == 0x82) {
                write_dynamic_locks(memory, rx[2], rx[4]);
            } else if (rx[1] == 3) {
                for (uint8_t i = 0; i < 4; ++i) memory[offset + i] |= rx[2 + i];
            } else {
                const uint8_t old_access = memory[ACCESS_OFFSET];
                for (uint8_t i = 0; i < 4; ++i) memory[offset + i] = rx[2 + i];
                if (rx[1] == 0x84) memory[ACCESS_OFFSET] |= old_access & 0x40; // CFGLCK cannot be cleared.
            }
            if (rx[1] == 0x85 || rx[1] == 0x86) ++session.wrote_pwd_page;
            return nak_reply(0x0A);
        }
        case 0x1B: {
            ++session.pwd_auth_seen;
            if (rx_len != 5) return nak_reply();
            bool matched = true;
            for (uint8_t i = 0; i < 4; ++i)
                if (rx[1 + i] != memory[PWD_OFFSET + i]) matched = false;
            session.authenticated = matched;
            if (!matched) return nak_reply(0x04, 0);
            ++session.auth_ok;
            return data_reply(memory + PACK_OFFSET, 2, 1);
        }
        case 0x39:
            return rx_len == 2 && rx[1] == 0x02 ? data_reply(EMPTY_COUNTER, 3) : nak_reply();
        case 0x60:
            return rx_len == 1 ? data_reply(NTAG215_VERSION, 8) : nak_reply();
        case 0x3C:
            return rx_len == 2 && rx[1] == 0 && signature ? data_reply(signature, 32) : nak_reply();
        case 0x50:
            // Non-standard HLTA frames follow the existing silent Idle behavior.
            return {rx_len == 2 && rx[1] == 0 ? ReplyKind::Halt : ReplyKind::Idle};
        default:
            return nak_reply();
    }
}
}  // namespace amiibo
