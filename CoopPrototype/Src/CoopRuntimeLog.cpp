#include "CoopRuntimeLog.h"

#include "ModMain.h"
#include "CoopRuntimeConfig.h"

#include <atomic>
#include <chrono>
#include <cstddef>
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

constexpr std::size_t kMaxRepeatKeys = 512;
constexpr double kRepeatWindowSeconds = 2.0;
constexpr uint32_t kRepeatBurst = 3;

void WriteRaw(std::string_view message)
{
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
