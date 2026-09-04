#include "CoopRuntimeLog.h"

#include "ModMain.h"
#include "CoopRuntimeConfig.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#include <windows.h>

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

// Keep the on-disk report deliberately small and cheap: WriteRaw already
// records every emitted line in the lock-free ring below. The file is a
// snapshot of that ring, refreshed at most once per second from the normal
// update thread, rather than a disk write on every hook/log call.
constexpr std::size_t kRuntimeFileMaxLines = 512;
constexpr std::chrono::seconds kRuntimeFileFlushInterval{1};
std::string BuildRuntimeFileHeader()
{
    SYSTEMTIME localTime = {};
    GetLocalTime(&localTime);

    std::ostringstream out;
    out << "CoopPrototype runtime log\n"
        << "format=1\n"
        << "bounded_recent_lines=512\n"
        << "lines_are_rate_limited_runtime_events\n"
        << "pid=" << GetCurrentProcessId() << "\n"
        << "started_local="
        << std::setfill('0') << std::setw(4) << localTime.wYear << "-"
        << std::setw(2) << localTime.wMonth << "-"
        << std::setw(2) << localTime.wDay << "T"
        << std::setw(2) << localTime.wHour << ":"
        << std::setw(2) << localTime.wMinute << ":"
        << std::setw(2) << localTime.wSecond << "."
        << std::setw(3) << localTime.wMilliseconds << "\n"
        << "---\n";
    return out.str();
}

std::mutex g_runtimeFileMutex;
std::filesystem::path g_runtimeFilePath;
std::string g_runtimeFileHeader;
std::chrono::steady_clock::time_point g_runtimeFileLastFlush{};
std::atomic<uint64_t> g_runtimeFileLastSequence{0};
std::atomic<bool> g_runtimeFileConfigured{false};

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
    // Stores sequence + 1 after text and length are fully published. A
    // separate sequence lets readers distinguish an old wrapped slot from a
    // line that is still reserved but not written.
    std::atomic<uint64_t> publishedSequence{0};
};

std::atomic<uint64_t> g_ringSequence{0};
std::atomic<uint64_t> g_ringPublishedThrough{0};
std::array<LogRingSlot, kRingSlots> g_ringSlots{};

void AdvancePublishedRing(uint64_t sequence)
{
    const uint64_t candidate = sequence + 1;
    uint64_t published = g_ringPublishedThrough.load(std::memory_order_acquire);
    if (candidate <= published)
        return;

    const uint64_t reserved = g_ringSequence.load(std::memory_order_acquire);
    const uint64_t target = candidate > reserved ? candidate : reserved;

    // Writers reserve slots independently. Only advance the public cursor
    // across a contiguous run, so a paused writer cannot make Flush() believe
    // that its reserved line is already on disk. The scan is bounded to one
    // ring length; a later publication can finish the rest without making a
    // log call spend unbounded time here.
    for (;;)
    {
        uint64_t scan = published;
        while (scan < target && scan - published <= kRingSlots)
        {
            const LogRingSlot& slot = g_ringSlots[scan % kRingSlots];
            const uint64_t slotSequence =
                slot.publishedSequence.load(std::memory_order_acquire);
            if (slotSequence == scan + 1)
            {
                ++scan;
                continue;
            }

            // If this slot already contains a newer sequence, the old
            // reservation has fallen out of the bounded ring and can no
            // longer be written without destroying a newer line. Treat it as
            // dropped and keep the published cursor moving.
            if (slotSequence > scan + 1)
            {
                ++scan;
                continue;
            }
            else
            {
                break;
            }
        }
        if (scan == published)
            return;

        if (g_ringPublishedThrough.compare_exchange_weak(
                published,
                scan,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return;
        }
        if (candidate <= published)
            return;
    }
}

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
    slot.publishedSequence.store(sequence + 1, std::memory_order_release);
    AdvancePublishedRing(sequence);
}

bool WriteRuntimeFileSnapshotLocked(uint64_t sequence)
{
    if (!g_runtimeFileConfigured || g_runtimeFilePath.empty())
        return false;

    std::filesystem::path temporaryPath = g_runtimeFilePath;
    temporaryPath += L".tmp.";
    temporaryPath += std::to_wstring(GetCurrentProcessId());

    std::ofstream output(
        temporaryPath,
        std::ios::binary | std::ios::out | std::ios::trunc);
    if (!output)
        return false;

    output.write(g_runtimeFileHeader.data(), static_cast<std::streamsize>(g_runtimeFileHeader.size()));
    const std::string recent = CoopRuntimeLog::RecentLines(kRuntimeFileMaxLines);
    if (!recent.empty())
        output.write(recent.data(), static_cast<std::streamsize>(recent.size()));
    output.flush();
    if (!output)
    {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        return false;
    }
    output.close();

    // Replace the previous complete snapshot only after the temporary file is
    // fully written. This keeps a crash or interrupted write from exposing a
    // truncated runtime report to the next bug-report collection.
    if (!MoveFileExW(
            temporaryPath.c_str(),
            g_runtimeFilePath.c_str(),
            MOVEFILE_REPLACE_EXISTING))
    {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        return false;
    }

    g_runtimeFileLastSequence.store(sequence, std::memory_order_release);
    return true;
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

bool CoopRuntimeLog::ConfigureFile(const std::filesystem::path& path)
{
    if (path.empty())
        return false;

    std::lock_guard<std::mutex> lock(g_runtimeFileMutex);
    if (g_runtimeFileConfigured.load(std::memory_order_acquire) && g_runtimeFilePath == path)
        return true;

    std::error_code error;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, error);
    if (error)
        return false;

    g_runtimeFilePath = path;
    g_runtimeFileHeader = BuildRuntimeFileHeader();
    g_runtimeFileConfigured.store(true, std::memory_order_release);
    g_runtimeFileLastFlush = std::chrono::steady_clock::now() - kRuntimeFileFlushInterval;
    g_runtimeFileLastSequence.store(0, std::memory_order_release);
    if (!WriteRuntimeFileSnapshotLocked(g_ringPublishedThrough.load(std::memory_order_acquire)))
    {
        g_runtimeFileConfigured.store(false, std::memory_order_release);
        g_runtimeFilePath.clear();
        g_runtimeFileHeader.clear();
        return false;
    }
    return true;
}

void CoopRuntimeLog::Flush()
{
    const uint64_t sequence = g_ringPublishedThrough.load(std::memory_order_acquire);
    if (!g_runtimeFileConfigured.load(std::memory_order_acquire) ||
        sequence == g_runtimeFileLastSequence.load(std::memory_order_acquire))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_runtimeFileMutex);
    if (!g_runtimeFileConfigured.load(std::memory_order_acquire))
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - g_runtimeFileLastFlush < kRuntimeFileFlushInterval)
        return;

    g_runtimeFileLastFlush = now;
    const uint64_t lockedSequence = g_ringPublishedThrough.load(std::memory_order_acquire);
    if (lockedSequence == g_runtimeFileLastSequence.load(std::memory_order_acquire))
        return;
    WriteRuntimeFileSnapshotLocked(lockedSequence);
}

void CoopRuntimeLog::CloseFile()
{
    std::lock_guard<std::mutex> lock(g_runtimeFileMutex);
    if (!g_runtimeFileConfigured.load(std::memory_order_acquire))
        return;

    WriteRuntimeFileSnapshotLocked(g_ringPublishedThrough.load(std::memory_order_acquire));
    g_runtimeFileConfigured.store(false, std::memory_order_release);
    g_runtimeFilePath.clear();
    g_runtimeFileHeader.clear();
    g_runtimeFileLastSequence.store(0, std::memory_order_release);
    g_runtimeFileLastFlush = {};
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

    const uint64_t next = g_ringPublishedThrough.load(std::memory_order_acquire);
    const uint64_t available = next < kRingSlots ? next : static_cast<uint64_t>(kRingSlots);
    const uint64_t take = std::min<uint64_t>(maxLines, available);
    std::string out;
    if (take > 0)
        out.reserve(take * 24);
    // Oldest to newest so the returned order is newest-last.
    for (uint64_t i = take; i > 0; --i)
    {
        const uint64_t sequence = next - i;
        const LogRingSlot& slot = g_ringSlots[sequence % kRingSlots];
        if (slot.publishedSequence.load(std::memory_order_acquire) != sequence + 1)
            continue;
        const std::uint16_t length = slot.length.load(std::memory_order_acquire);
        if (length == 0)
            continue;
        std::array<char, kRingLineCapacity> line = {};
        std::memcpy(line.data(), slot.text, length);
        if (slot.publishedSequence.load(std::memory_order_acquire) != sequence + 1)
            continue;
        out.append(line.data(), length);
        out.push_back('\n');
    }
    return out;
}
