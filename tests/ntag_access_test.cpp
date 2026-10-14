#include "../main/NtagCommands.h"

constexpr bool access_protection()
{
    uint8_t memory[540]{};
    uint8_t scratch[17]{};
    amiibo::NtagSession session;
    for (unsigned page = 0; page < 135; ++page)
        for (unsigned byte = 0; byte < 4; ++byte) memory[4 * page + byte] = uint8_t(page);
    memory[10] = memory[11] = memory[520] = memory[522] = 0;
    memory[amiibo::AUTH0_OFFSET] = 4;
    memory[amiibo::ACCESS_OFFSET] = 0x80;
    scratch[16] = 0xCC;
    const uint8_t read[] = {0x30, 3};
    auto reply = amiibo::handle_ntag_command(memory, read, 2, session, scratch, 16);
    const uint8_t pages[] = {3, 0, 1, 2};
    if (reply.kind != amiibo::ReplyKind::Data || reply.size != 16 || scratch[16] != 0xCC) return false;
    for (unsigned i = 0; i < 16; ++i) if (reply.data[i] != memory[pages[i / 4] * 4 + i % 4]) return false;
    const uint8_t protected_read[] = {0x30, 4};
    const uint8_t crossing_fast[] = {0x3A, 3, 4};
    const uint8_t write[] = {0xA2, 4, 1, 2, 3, 4};
    if (amiibo::handle_ntag_command(memory, protected_read, 2, session, scratch, 16).nibble != 4 ||
        amiibo::handle_ntag_command(memory, crossing_fast, 3, session, scratch, 16).nibble != 4 ||
        amiibo::handle_ntag_command(memory, write, 6, session, scratch, 16).nibble != 4 || memory[16] != 4) return false;
    memory[amiibo::ACCESS_OFFSET] = 0; // PROT=0: reads open, writes still protected.
    if (amiibo::handle_ntag_command(memory, protected_read, 2, session, scratch, 16).kind != amiibo::ReplyKind::Data ||
        amiibo::handle_ntag_command(memory, write, 6, session, scratch, 16).nibble != 4) return false;
    const uint8_t pwd[] = {0x1B, memory[532], memory[533], memory[534], memory[535]};
    if (amiibo::handle_ntag_command(memory, pwd, 5, session, scratch, 16).auth_match != 1) return false;
    if (amiibo::handle_ntag_command(memory, write, 6, session, scratch, 16).nibble != 0x0A || memory[16] != 1)
        return false;
    memory[amiibo::ACCESS_OFFSET] = 0x80;
    if (amiibo::handle_ntag_command(memory, crossing_fast, 3, session, scratch, 16).kind != amiibo::ReplyKind::Data)
        return false;
    const uint8_t pwd_read[] = {0x30, 0x85};
    reply = amiibo::handle_ntag_command(memory, pwd_read, 2, session, scratch, 16);
    for (unsigned i = 0; i < 6; ++i) if (reply.data[i]) return false;
    session.reset_auth();
    memory[amiibo::AUTH0_OFFSET] = 135;
    if (amiibo::handle_ntag_command(memory, protected_read, 2, session, scratch, 16).kind != amiibo::ReplyKind::Data)
        return false;
    memory[amiibo::AUTH0_OFFSET] = 0;
    const uint8_t first_read[] = {0x30, 0};
    return amiibo::handle_ntag_command(memory, first_read, 2, session, scratch, 16).nibble == 4;
}
static_assert(access_protection(), "AUTH0/PROT, protected READ rollover, FAST_READ rejection and authentication");

constexpr bool permanent_locks()
{
    const uint8_t static_masks[] = {0x08, 0x10, 0x20, 0x40, 0x80, 1, 2, 4, 8, 16, 32, 64, 128};
    for (unsigned lock = 0; lock < 13; ++lock) {
        uint8_t memory[540]{};
        uint8_t scratch[16]{};
        amiibo::NtagSession session;
        memory[amiibo::AUTH0_OFFSET] = 0xFF;
        memory[lock < 5 ? 10 : 11] = static_masks[lock];
        for (uint8_t page = 3; page <= 15; ++page) {
            const uint8_t write[] = {0xA2, page, 0xA5, 0, 0, 0};
            const auto reply = amiibo::handle_ntag_command(memory, write, 6, session, scratch, 16);
            if ((reply.nibble == 0x0A) != (page != lock + 3)) return false;
            if (page == lock + 3 && memory[page * 4]) return false;
        }
    }
    const uint8_t boundaries[] = {16, 32, 48, 64, 80, 96, 112, 128, 130};
    for (uint8_t lock = 0; lock < 8; ++lock) {
        uint8_t memory[540]{};
        uint8_t scratch[16]{};
        amiibo::NtagSession session;
        memory[amiibo::AUTH0_OFFSET] = 0xFF;
        session.begin_field(memory); session.authenticated = true;
        memory[520] = uint8_t(1u << lock);
        for (uint8_t page = 16; page <= 129; ++page) {
            const uint8_t write[] = {0xA2, page, 0xA5, 0, 0, 0};
            const bool locked = page >= boundaries[lock] && page < boundaries[lock + 1];
            const auto reply = amiibo::handle_ntag_command(memory, write, 6, session, scratch, 16);
            if ((reply.nibble == 0x0A) == locked || (locked && memory[page * 4])) return false;
        }
    }
    return true;
}
static_assert(permanent_locks(), "All static pages/dynamic NTAG215 groups reject writes, even after authentication");

constexpr bool lock_programming()
{
    uint8_t memory[540]{};
    memory[8] = 0x12; memory[9] = 0x34;
    amiibo::write_static_locks(memory, 7, 0); // Freeze all groups without locking any page.
    amiibo::write_static_locks(memory, 0xFF, 0xFF);
    if (memory[10] != 7 || memory[11] || memory[8] != 0x12 || memory[9] != 0x34) return false;
    for (unsigned group = 0; group < 4; ++group) {
        memory[520] = memory[522] = 0;
        memory[521] = 0x80; memory[523] = 0xBD;
        amiibo::write_dynamic_locks(memory, 0, uint8_t(1u << group));
        amiibo::write_dynamic_locks(memory, 0xFF, 0xFF);
        const uint8_t expected[] = {0xFC, 0xF3, 0xCF, 0x3F};
        if (memory[520] != expected[group] || memory[522] != 0x0F ||
            memory[521] != 0x80 || memory[523] != 0xBD) return false;
        amiibo::write_dynamic_locks(memory, 0, 0);
        if (memory[520] != expected[group] || memory[522] != 0x0F) return false;
    }
    return true;
}
static_assert(lock_programming(), "Static/dynamic block locks freeze locking bits, preserve RFUI and cannot be cleared");

constexpr bool config_lock_and_otp()
{
    uint8_t memory[540]{};
    uint8_t scratch[16]{};
    amiibo::NtagSession session;
    memory[amiibo::AUTH0_OFFSET] = 0xFF;
    const uint8_t set_cfg_lock[] = {0xA2, 0x84, 0x40, 0, 0, 0};
    const uint8_t clear_cfg_lock[] = {0xA2, 0x84, 0, 0, 0, 0};
    const uint8_t cfg0[] = {0xA2, 0x83, 0x04, 0, 0, 0xFF};
    if (amiibo::handle_ntag_command(memory, set_cfg_lock, 6, session, scratch, 16).nibble != 0x0A ||
        amiibo::handle_ntag_command(memory, clear_cfg_lock, 6, session, scratch, 16).nibble != 0x0A ||
        memory[amiibo::ACCESS_OFFSET] != 0x40 ||
        amiibo::handle_ntag_command(memory, cfg0, 6, session, scratch, 16).nibble != 0x0A) return false;
    session.reset_auth(); // HALT/selection alone does not latch a new CFGLCK.
    if (amiibo::handle_ntag_command(memory, cfg0, 6, session, scratch, 16).nibble != 0x0A) return false;
    session.end_field();
    if (amiibo::handle_ntag_command(memory, cfg0, 6, session, scratch, 16).nibble != 0 ||
        amiibo::handle_ntag_command(memory, clear_cfg_lock, 6, session, scratch, 16).nibble != 0) return false;
    const uint8_t pwd[] = {0xA2, 0x85, 1, 2, 3, 4};
    if (amiibo::handle_ntag_command(memory, pwd, 6, session, scratch, 16).nibble != 0x0A) return false;
    memory[12] = 0xE1; memory[13] = 0x10; memory[14] = 0x3E;
    const uint8_t cc[] = {0xA2, 3, 0, 0, 0, 0x0F};
    if (amiibo::handle_ntag_command(memory, cc, 6, session, scratch, 16).nibble != 0x0A) return false;
    return memory[12] == 0xE1 && memory[13] == 0x10 && memory[14] == 0x3E && memory[15] == 0x0F;
}
static_assert(config_lock_and_otp(), "CFGLCK becomes active at the next field, protects only CFG0/CFG1, CC is OTP");
