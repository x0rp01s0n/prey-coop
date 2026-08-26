#pragma once
//
// CoopHookStats — feather-light self-profiling for the native hooks.
//
// Every instrumented hook contributes two atomic counters (call count +
// cumulative nanoseconds). BuildRuntimeCostReport() dumps the aggregate into
// the extractor status line / Game.log so any machine — including players'
// high-end rigs — can report exactly where mod frame time goes without any
// external tooling.
//
// Overhead per instrumented call is one QPC read + two relaxed atomic adds
// (~15-25 ns), which is noise even at 10k calls/frame.
//

#include <atomic>
#include <cstdint>

namespace CoopHookStats
{
struct Slot
{
    const char* name = nullptr;
    mutable std::atomic<uint64_t> calls{0};
    mutable std::atomic<uint64_t> totalNs{0};
};

// Registers a named slot once (call from a static initializer).
// Returns nullptr if the registry is full.
Slot* Register(const char* name);

// RAII scope timer bound to a registered slot.
class Scoped
{
public:
    explicit Scoped(Slot* slot);
    ~Scoped();
    Scoped(const Scoped&) = delete;
    Scoped& operator=(const Scoped&) = delete;

private:
    Slot* m_slot = nullptr;
    uint64_t m_startNs = 0;
};

// Formatted "name:calls:us" entries for every slot with calls > 0,
// joined by '|'. Empty string when nothing has fired.
std::string BuildReport();

// Reset all counters (used between telemetry windows).
void ResetAll();
}
