#include "../main/NtagCommands.h"

constexpr bool authentication_sequence()
{
    uint8_t memory[540]{};
    uint8_t scratch[541]{};
    amiibo::NtagSession session;
    for (unsigned i = 0; i < 540; ++i) memory[i] = (i % 251) + 1;
    const uint8_t pwd[] = {0x1B, memory[532], memory[533], memory[534], memory[535]};
    auto reply = amiibo::handle_ntag_command(memory, pwd, 5, session, scratch, 540);
    if (reply.kind != amiibo::ReplyKind::Data || reply.size != 2 || reply.auth_match != 1 ||
        reply.data[0] != memory[536] || reply.data[1] != memory[537] || !session.authenticated)
        return false;

    const uint8_t read[] = {0x30, 0x85};
    reply = amiibo::handle_ntag_command(memory, read, 2, session, scratch, 540);
    if (reply.size != 16 || reply.auth_match != -1 || !session.authenticated) return false;
    for (unsigned i = 0; i < 6; ++i) if (reply.data[i] != 0) return false;
    if (reply.data[6] != memory[538] || reply.data[8] != memory[0]) return false;

    const uint8_t fast[] = {0x3A, 0, 0x86};
    scratch[540] = 0xBC;
    reply = amiibo::handle_ntag_command(memory, fast, 3, session, scratch, 540);
    if (reply.size != 540 || !session.authenticated || scratch[540] != 0xBC) return false;
    for (unsigned i = 0; i < 540; ++i)
        if (reply.data[i] != (i >= 532 && i < 538 ? 0 : memory[i])) return false;

    // Leaving the field clears authorization, but preserves the unsaved log statistics.
    session.reset_auth();
    if (session.authenticated || session.pwd_auth_seen != 1 || session.auth_ok != 1) return false;
    const uint8_t bad[] = {0x1B, 0, 0, 0, 0};
    reply = amiibo::handle_ntag_command(memory, bad, 5, session, scratch, 540);
    if (reply.kind != amiibo::ReplyKind::Nibble || reply.nibble != 4 || reply.auth_match != 0 ||
        session.authenticated || session.pwd_auth_seen != 2 || session.auth_ok != 1) return false;

    // Malformed authentication is still observed, but no password comparison occurs.
    reply = amiibo::handle_ntag_command(memory, bad, 4, session, scratch, 540);
    if (reply.nibble != 0 || reply.auth_match != -1 || session.pwd_auth_seen != 3) return false;
    session.clear_statistics();
    return session.pwd_auth_seen == 0 && session.auth_ok == 0;
}
static_assert(authentication_sequence(), "Authentication, permanent credential masking and batch persistence");

constexpr bool writes_and_reauthentication()
{
    uint8_t memory[540]{};
    uint8_t scratch[540]{};
    amiibo::NtagSession session;
    memory[amiibo::AUTH0_OFFSET] = 0xFF;
    const uint8_t uid_write[] = {0xA2, 1, 1, 2, 3, 4};
    if (amiibo::handle_ntag_command(memory, uid_write, 6, session, scratch, 540).nibble != 0 || memory[4])
        return false;
    memory[8] = 0x12;
    memory[9] = 0x34;
    memory[10] = 0x01;
    memory[11] = 0x80;
    const uint8_t lock[] = {0xA2, 2, 0xEE, 0xFF, 0x02, 0x04};
    if (amiibo::handle_ntag_command(memory, lock, 6, session, scratch, 540).nibble != 0x0A ||
        memory[8] != 0x12 || memory[9] != 0x34 || memory[10] != 3 || memory[11] != 0x84) return false;
    const uint8_t pwd_write[] = {0xA2, 0x85, 1, 2, 3, 4};
    const uint8_t pack_write[] = {0xA2, 0x86, 0x80, 0x80, 0, 0};
    if (amiibo::handle_ntag_command(memory, pwd_write, 6, session, scratch, 540).nibble != 0x0A ||
        amiibo::handle_ntag_command(memory, pack_write, 6, session, scratch, 540).nibble != 0x0A ||
        session.wrote_pwd_page != 2) return false;
    const uint8_t pwd[] = {0x1B, 1, 2, 3, 4};
    auto reply = amiibo::handle_ntag_command(memory, pwd, 5, session, scratch, 540);
    if (reply.auth_match != 1 || reply.data[0] != 0x80 || reply.data[1] != 0x80) return false;
    const uint8_t changed_pwd[] = {0xA2, 0x85, 5, 6, 7, 8};
    amiibo::handle_ntag_command(memory, changed_pwd, 6, session, scratch, 540);
    return amiibo::handle_ntag_command(memory, pwd, 5, session, scratch, 540).auth_match == 0 &&
           !session.authenticated && session.wrote_pwd_page == 3;
}
static_assert(writes_and_reauthentication(), "UID retention, monotonic static locks and changed credentials");

constexpr bool command_boundaries()
{
    uint8_t memory[540]{};
    uint8_t scratch[540]{};
    amiibo::NtagSession session;
    uint8_t rx[] = {0x30, 0x87, 0, 0, 0, 0};
    if (amiibo::handle_ntag_command(memory, rx, 2, session, scratch, 540).nibble != 0) return false;
    rx[0] = 0x3A; rx[1] = 0x86; rx[2] = 0x85;
    if (amiibo::handle_ntag_command(memory, rx, 3, session, scratch, 540).kind != amiibo::ReplyKind::Nibble)
        return false;
    rx[1] = 0; rx[2] = 0x86;
    if (amiibo::handle_ntag_command(memory, rx, 3, session, scratch, 539).kind != amiibo::ReplyKind::Nibble)
        return false;
    rx[0] = 0x60;
    auto reply = amiibo::handle_ntag_command(memory, rx, 1, session, scratch, 540);
    if (reply.size != 8 || reply.data[6] != 0x11) return false;
    if (amiibo::handle_ntag_command(memory, rx, 2, session, scratch, 540).kind != amiibo::ReplyKind::Nibble)
        return false;
    rx[0] = 0x39; rx[1] = 2;
    reply = amiibo::handle_ntag_command(memory, rx, 2, session, scratch, 540);
    if (reply.size != 3 || reply.data[0] != 0) return false;
    rx[1] = 1;
    if (amiibo::handle_ntag_command(memory, rx, 2, session, scratch, 540).kind != amiibo::ReplyKind::Nibble)
        return false;
    rx[0] = 0x3C; rx[1] = 0;
    if (amiibo::handle_ntag_command(memory, rx, 2, session, scratch, 540).kind != amiibo::ReplyKind::Nibble)
        return false;
    uint8_t signature[32]{};
    for (unsigned i = 0; i < 32; ++i) signature[i] = i + 1;
    reply = amiibo::handle_ntag_command(memory, rx, 2, session, scratch, 540, signature);
    if (reply.size != 32 || reply.kind != amiibo::ReplyKind::Data) return false;
    for (unsigned i = 0; i < 32; ++i) if (reply.data[i] != signature[i]) return false;
    rx[0] = 0x50;
    if (amiibo::handle_ntag_command(memory, rx, 2, session, scratch, 540).kind != amiibo::ReplyKind::Halt)
        return false;
    rx[1] = 1;
    if (amiibo::handle_ntag_command(memory, rx, 2, session, scratch, 540).kind != amiibo::ReplyKind::Idle)
        return false;
    return amiibo::handle_ntag_command(memory, nullptr, 0, session, scratch, 540).kind == amiibo::ReplyKind::Idle;
}
static_assert(command_boundaries(), "Command lengths, page bounds, reply formats and scratch capacity");
