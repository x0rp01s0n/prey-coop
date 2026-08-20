#include "CoopRuntimeLog.h"

#include "ModMain.h"
#include "CoopRuntimeConfig.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <Chairloader/IChairLogger.h>

namespace
{
struct RateLimitState
{
    std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    uint32_t emitted = 0;
    uint64_t suppressed = 0;
};

std::mutex g_rateLimitMutex;
std::unordered_map<std::string, RateLimitState> g_rateLimits;
std::unordered_map<std::string, RateLimitState> g_repeatLimits;
std::atomic<uint64_t> g_suppressedCount = 0;

// Lock-free recent-line ring for crash traces (see RecentLines in the
// header). No mutexes: the crash handler may run on a faulting thread where
// a locked mutex could deadlock. Fixed slots, atomic sequence index, length
// published with release after the text copy (a mid-copy crash leaves the
// previous line's length, so readers never see a torn length).
constexpr std::size_t kRingSlots = 512;
constexpr std::size_t kRingLineCapacity = 128;

struct LogRingSlot
{
    char text[kRingLineCapacity] = {};
    std::atomic<std::uint16_t> length{0};
};

std::atomic<uint64_t> g_ringSequence{0};
std::array<LogRingSlot, kRingSlots> g_ringSlots{};

void RecordRingLine(std::string_view message)
{
    const uint64_t sequence = g_ringSequence.fetch_add(1, std::memory_order_relaxed);
    LogRingSlot& slot = g_ringSlots[sequence % kRingSlots];
    const std::size_t copyLength =
        std::min<std::size_t>(message.size(), kRingLineCapacity - 1);
    if (copyLength > 0)
        std::memcpy(slot.text, message.data(), copyLength);
    slot.text[copyLength] = '\0';
    slot.length.store(static_cast<std::uint16_t>(copyLength), std::memory_order_release);
}

constexpr std::size_t kMaxRepeatKeys = 512;
constexpr double kRepeatWindowSeconds = 2.0;
constexpr uint32_t kRepeatBurst = 3;

void WriteRaw(std::string_view message)
{
    RecordRingLine(message);
    if (gMod)
        gMod->RecordRuntimeLogEmission();
    CryLog("[CoopPrototype] {}", message);
    if (CoopRuntimeConfig::Flag("COOP_OVERLAY_LOG"))
        OverlayLog("[CoopPrototype] {}", message);
}
}

void CoopRuntimeLog::Write(std::string_view message)
{
    const auto now = std::chrono::steady_clock::now();
    const std::string key(message);
    uint64_t rollup = 0;
    bool emit = false;
    {
        std::lock_guard<std::mutex> lock(g_rateLimitMutex);
        auto it = g_repeatLimits.find(key);
        if (it == g_repeatLimits.end())
        {
            if (g_repeatLimits.size() >= kMaxRepeatKeys)
                g_repeatLimits.clear();
            it = g_repeatLimits.emplace(key, RateLimitState{}).first;
        }

        RateLimitState& state = it->second;
        const double elapsed = std::chrono::duration<double>(now - state.windowStart).count();
        if (elapsed >= kRepeatWindowSeconds)
        {
            rollup = state.suppressed;
            state.windowStart = now;
            state.emitted = 0;
            state.suppressed = 0;
        }
        if (state.emitted < kRepeatBurst)
        {
            ++state.emitted;
            emit = true;
        }
        else
        {
            ++state.suppressed;
            ++g_suppressedCount;
        }
    }

    if (rollup > 0)
        WriteRaw("log repeat suppressed=" + std::to_string(rollup) + " message=" + key);
    if (emit)
        WriteRaw(message);
}

bool CoopRuntimeLog::WriteRateLimited(
    std::string_view key,
    std::string_view message,
    double intervalSeconds,
    uint32_t burst)
{
    const auto now = std::chrono::steady_clock::now();
    uint64_t rollup = 0;
    bool emit = false;
    {
        std::lock_guard<std::mutex> lock(g_rateLimitMutex);
        RateLimitState& state = g_rateLimits[std::string(key)];
        const double elapsed = std::chrono::duration<double>(now - state.windowStart).count();
        if (elapsed >= intervalSeconds)
        {
            rollup = state.suppressed;
            state.windowStart = now;
            state.emitted = 0;
            state.suppressed = 0;
        }
        if (state.emitted < burst)
        {
            ++state.emitted;
            emit = true;
        }
        else
        {
            ++state.suppressed;
            ++g_suppressedCount;
        }
    }

    if (rollup > 0)
    {
        WriteRaw(
            "log rate limit key=" + std::string(key) +
            " suppressed=" + std::to_string(rollup));
    }
    if (emit)
        WriteRaw(message);
    return emit;
}

uint64_t CoopRuntimeLog::SuppressedCount()
{
    return g_suppressedCount.load();
}

void CoopRuntimeLog::ResetRateLimits()
{
    std::lock_guard<std::mutex> lock(g_rateLimitMutex);
    g_rateLimits.clear();
    g_repeatLimits.clear();
    g_suppressedCount = 0;
}

std::string CoopRuntimeLog::RecentLines(std::size_t maxLines)
{
    if (maxLines == 0)
        return {};

    const uint64_t next = g_ringSequence.load(std::memory_order_relaxed);
    const uint64_t available = next < kRingSlots ? next : static_cast<uint64_t>(kRingSlots);
    const uint64_t take = std::min<uint64_t>(maxLines, available);
    std::string out;
    if (take > 0)
        out.reserve(take * 24);
    // Oldest to newest so the returned order is newest-last.
    for (uint64_t i = take; i > 0; --i)
    {
        const LogRingSlot& slot = g_ringSlots[(next - i) % kRingSlots];
        const std::uint16_t length = slot.length.load(std::memory_order_acquire);
        if (length == 0)
            continue;
        out.append(slot.text, length);
        out.push_back('\n');
    }
    return out;
}
