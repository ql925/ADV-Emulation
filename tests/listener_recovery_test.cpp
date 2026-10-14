#include "../main/ListenerRecovery.h"

struct RecoveryIO {
    unsigned fail_at{}, calls{}, errors{}, stopped{};
    bool stop_ok{true};
    amiibo::RebuildStep first{}, last{};
    constexpr bool configure() { ++calls; return fail_at != 1; }
    constexpr bool begin() { ++calls; return fail_at != 2; }
    constexpr bool timing() { ++calls; return fail_at != 3; }
    constexpr bool end() { ++stopped; return stop_ok; }
    constexpr void failed(amiibo::RebuildStep step) {
        if (!errors++) first = step;
        last = step;
    }
};

constexpr bool failure_then_retry()
{
    for (unsigned step = 1; step <= 3; ++step) {
        for (unsigned cleanup = 0; cleanup < 2; ++cleanup) {
            RecoveryIO io;
            io.fail_at = step;
            io.stop_ok = cleanup != 0;
            if (amiibo::rebuild_listener(io) || io.calls != step || io.stopped != 1 ||
                io.first != static_cast<amiibo::RebuildStep>(step) ||
                io.errors != (cleanup ? 1u : 2u)) return false;
            if (!cleanup && io.last != amiibo::RebuildStep::Recover) return false;
            // A subsequent attempt can recover after each initialization failure.
            io.fail_at = 0;
            if (!amiibo::rebuild_listener(io) || io.calls != step + 3 || io.stopped != 1) return false;
        }
    }
    RecoveryIO io;
    return amiibo::rebuild_listener(io) && io.calls == 3 && !io.errors && !io.stopped;
}
static_assert(failure_then_retry(), "Startup/maintenance failure, cleanup failure and successful retry");
