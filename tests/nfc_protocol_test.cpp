#include "../main/NtagRead.h"
#include "../main/NfcFifoTx.h"

// The password window a real NTAG215 never discloses through READ/FAST_READ.
constexpr unsigned SECRET_FROM = amiibo::PWD_OFFSET;
constexpr unsigned SECRET_TO   = amiibo::PACK_OFFSET + 2;

constexpr bool is_secret(const unsigned offset)
{
    return offset >= SECRET_FROM && offset < SECRET_TO;
}

// What an unauthenticated READ/FAST_READ must return at a given memory offset.
constexpr uint8_t expected_unauthenticated(const uint8_t* memory, const unsigned offset)
{
    return is_secret(offset % amiibo::TAG_BYTES) ? 0 : memory[offset % amiibo::TAG_BYTES];
}

constexpr bool read_boundaries()
{
    uint8_t memory[540]{};
    for (unsigned i = 0; i < 540; ++i) memory[i] = (i % 251) + 1;
    uint8_t out[542]{};

    // The protected window itself must be exactly the six secret bytes.
    if (amiibo::PROTECTED_FROM != 532 || amiibo::PROTECTED_TO != 538) return false;
    for (unsigned offset = 0; offset < 540; ++offset) {
        if (amiibo::protected_offset(offset) != is_secret(offset)) return false;
    }
    // Bytes outside the window are never protected, including page 0x86 RFUI bytes.
    if (amiibo::protected_offset(530) || amiibo::protected_offset(531) || amiibo::protected_offset(538) ||
        amiibo::protected_offset(539))
        return false;

    for (unsigned first = 0; first < 256; ++first) {
        for (unsigned last = 0; last < 256; ++last) {
            const auto size = amiibo::fast_read_size(first, last);
            if (size != (first <= last && last < 135 ? 4 * (last - first + 1) : 0)) return false;
        }
    }

    // Full dump, unauthenticated: both secret pages masked, nothing else disturbed.
    out[540] = 0xAB;
    amiibo::read_pages(memory, 0, out, 540);
    for (unsigned i = 0; i < 540; ++i) {
        if (out[i] != expected_unauthenticated(memory, i)) return false;
    }
    if (out[540] != 0xAB) return false;

    // READ at every valid page, including rollover. Authentication cannot reveal secrets.
    for (unsigned page = 0; page < 135; ++page) {
        out[16] = 0xBC;
        amiibo::read_pages(memory, page, out, 16);
        for (unsigned i = 0; i < 16; ++i) {
            const unsigned offset = (4 * page + i) % 540;
            if (out[i] != expected_unauthenticated(memory, offset)) return false;
        }
        if (out[16] != 0xBC) return false;
    }
    return true;
}
static_assert(read_boundaries(), "NTAG215 range checks, unconditional secret masking and READ rollover");

constexpr bool full_reads_from_every_page()
{
    uint8_t memory[540]{};
    for (unsigned i = 0; i < 540; ++i) memory[i] = (i % 251) + 1;
    uint8_t out[542]{};
    out[0] = 0xAB;
    out[541] = 0xCD;
    for (unsigned page = 0; page < 135; ++page) {
        amiibo::read_pages(memory, page, out + 1, 540);
        for (unsigned i = 0; i < 540; ++i)
            if (out[i + 1] != expected_unauthenticated(memory, 4 * page + i)) return false;
        if (out[0] != 0xAB || out[541] != 0xCD) return false;
    }
    out[1] = 0xEF;
    amiibo::read_pages(memory, 134, out + 1, 0);
    return out[1] == 0xEF;
}
static_assert(full_reads_from_every_page(), "Cursor wrap, secret masking, buffer guards and empty reads");

constexpr bool fast_read_masking(unsigned begin, unsigned end)
{
    uint8_t memory[540]{};
    for (unsigned i = 0; i < 540; ++i) memory[i] = (i % 251) + 1;
    uint8_t out[540]{};

    // FAST_READ masks the same bytes as READ. Every legal range must zero exactly the
    // secret bytes it covers and leave every other byte untouched.
    for (unsigned first = begin; first < end; ++first) {
        for (unsigned last = first; last < 135; ++last) {
            const uint16_t count = amiibo::fast_read_size(first, last);
            for (unsigned i = 0; i < count; ++i) out[i] = memory[4 * first + i];
            amiibo::mask_protected(out, 4 * first, count);
            for (unsigned i = 0; i < count; ++i) {
                const unsigned offset = 4 * first + i;
                const uint8_t want = is_secret(offset) ? 0 : memory[offset];
                if (out[i] != want) return false;
            }
        }
    }

    // A range that starts mid-page keeps the bytes preceding the protected window; this is
    // what the previous whole-tail memset got wrong.
    const uint8_t sentinel = 0x5A;
    for (unsigned i = 0; i < 8; ++i) out[i] = sentinel;
    for (unsigned i = 0; i < 4; ++i) out[i] = memory[4 * 0x84 + i];
    amiibo::mask_protected(out, 4 * 0x84, 8);
    for (unsigned i = 0; i < 4; ++i) {
        if (out[i] != memory[4 * 0x84 + i]) return false;
    }
    for (unsigned i = 4; i < 8; ++i) {
        if (out[i] != 0) return false;
    }
    return true;
}
// Split exhaustive checks so each expression stays below GCC's default constexpr budget.
static_assert(fast_read_masking(0, 10), "FAST_READ masking: start pages 0..9");
static_assert(fast_read_masking(10, 20), "FAST_READ masking: start pages 10..19");
static_assert(fast_read_masking(20, 30), "FAST_READ masking: start pages 20..29");
static_assert(fast_read_masking(30, 40), "FAST_READ masking: start pages 30..39");
static_assert(fast_read_masking(40, 50), "FAST_READ masking: start pages 40..49");
static_assert(fast_read_masking(50, 60), "FAST_READ masking: start pages 50..59");
static_assert(fast_read_masking(60, 70), "FAST_READ masking: start pages 60..69");
static_assert(fast_read_masking(70, 80), "FAST_READ masking: start pages 70..79");
static_assert(fast_read_masking(80, 90), "FAST_READ masking: start pages 80..89");
static_assert(fast_read_masking(90, 100), "FAST_READ masking: start pages 90..99");
static_assert(fast_read_masking(100, 110), "FAST_READ masking: start pages 100..109");
static_assert(fast_read_masking(110, 120), "FAST_READ masking: start pages 110..119");
static_assert(fast_read_masking(120, 130), "FAST_READ masking: start pages 120..129");
static_assert(fast_read_masking(130, 135), "FAST_READ masking: start pages 130..134");

struct FakeIO {
    uint16_t used{}, length_value{}, copied{}, writes{}, preload{}, maximum{};
    uint16_t drain{8};
    uint8_t bytes[540]{};
    uint8_t error{};
    bool started{}, aborted{};
    uint32_t clock{};
    constexpr bool clear() { used = 0; return error != 1; }
    constexpr bool length(uint16_t n) { length_value = n; return error != 2; }
    constexpr bool start() { started = true; preload = copied; return error != 3; }
    constexpr void abort() { aborted = true; }
    constexpr uint32_t now() { clock += 1000; return clock; }
    constexpr bool write(const uint8_t* data, uint16_t n)
    {
        ++writes;
        if (error == 4 || (error == 5 && started)) return false;
        if (used + n > 512 || copied + n > length_value) return false;
        for (unsigned i = 0; i < n; ++i) bytes[copied++] = data[i];
        used += n;
        if (used > maximum) maximum = used;
        return true;
    }
    constexpr bool status(uint16_t& out)
    {
        if (error == 6) return false;
        used = used > drain ? used - drain : 0;
        if (error == 8) used = 0;
        if (error == 9) used = 512; // No RF progress: must time out, never overflow.
        out = ((used & 0xFF) << 8) | ((used >> 8) << 6);
        if (error == 7) out |= 0x20;
        if (error == 10) out |= 0x10;
        return true;
    }
};

constexpr bool every_frame(uint16_t drain)
{
    uint8_t data[540]{};
    for (unsigned i = 0; i < 540; ++i) data[i] = i % 251;
    for (unsigned size = 1; size <= 540; ++size) {
        FakeIO io;
        io.drain = drain;
        // Verify wrap-safe timeout arithmetic too.
        io.clock = 0xFFFFF000;
        if (amiibo::queue_frame(io, data, size) != amiibo::TxResult::Queued) return false;
        if (!io.started || io.aborted || io.maximum > 512 || io.copied != size) return false;
        if (io.preload != (size < 128 ? size : 128)) return false;
        for (unsigned i = 0; i < size; ++i) if (io.bytes[i] != data[i]) return false;
    }
    return true;
}
static_assert(every_frame(1), "All 1..540-byte responses: slow drain, FIFO capacity and ordering");
static_assert(every_frame(32), "All 1..540-byte responses: fast drain and timer wraparound");

constexpr bool errors()
{
    uint8_t data[540]{};
    constexpr amiibo::TxResult expected[] = {
        amiibo::TxResult::Queued, amiibo::TxResult::Setup, amiibo::TxResult::Setup,
        amiibo::TxResult::Setup, amiibo::TxResult::Setup, amiibo::TxResult::Write,
        amiibo::TxResult::Status, amiibo::TxResult::FifoError, amiibo::TxResult::Empty,
        amiibo::TxResult::Timeout, amiibo::TxResult::FifoError};
    for (unsigned e = 1; e <= 10; ++e) {
        FakeIO io;
        io.error = e;
        if (amiibo::queue_frame(io, data, 540) != expected[e] || !io.aborted) return false;
    }
    FakeIO io;
    return amiibo::queue_frame(io, nullptr, 1) == amiibo::TxResult::Invalid &&
           amiibo::queue_frame(io, data, 0) == amiibo::TxResult::Invalid &&
           amiibo::queue_frame(io, data, 541) == amiibo::TxResult::Invalid && !io.writes;
}
static_assert(errors(), "I/O failures, underflow, overflow, timeout and invalid inputs abort safely");
