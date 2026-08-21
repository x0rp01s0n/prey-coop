#include "CoopPtrHygiene.h"

#include "CoopRuntimeConfig.h"
#include "CoopRuntimeLog.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{
constexpr const char* kEnvFlagName = "COOP_PTR_HYGIENE";
constexpr const char* kMarkerFileName = "CoopPtrHygiene.txt";
constexpr std::uint64_t kMarkerRecheckIntervalMs = 2000;

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_explicitlySet{false};
std::atomic<bool> g_initialized{false};
std::atomic<std::uint64_t> g_lastMarkerCheckMs{0};

bool EnvFlagValue()
{
    return CoopRuntimeConfig::Flag(kEnvFlagName);
}

// GetPreyProfileRoot() is intentionally duplicated per translation unit in
// this codebase (see ModMain.cpp and CoopPlayerSidecar.cpp), so the same
// 5-line USERPROFILE logic is duplicated here instead of reaching into
// ModMain internals.
std::filesystem::path GetPreyProfileRoot()
{
    const char* userProfile = std::getenv("USERPROFILE");
    if (!userProfile || !userProfile[0])
        return {};

    std::filesystem::path root(userProfile);
    root /= "Saved Games";
    root /= "Arkane Studios";
    root /= "Prey";
    return root;
}

bool MarkerFilePresent()
{
    const std::filesystem::path root = GetPreyProfileRoot();
    if (root.empty())
        return false;

    std::error_code error;
    const bool exists = std::filesystem::exists(root / kMarkerFileName, error);
    return exists && !error;
}

std::uint64_t NowMs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void SetEnabledInternal(bool enabled, bool explicitSet, const char* source)
{
    g_enabled.store(enabled, std::memory_order_release);
    if (explicitSet)
        g_explicitlySet.store(true, std::memory_order_release);
    char line[128];
    std::snprintf(line, sizeof(line), "ptr_hygiene state enabled=%d source=%s",
        enabled ? 1 : 0,
        source ? source : "unknown");
    CoopRuntimeLog::Write(line);
}
}

void CoopPtrHygiene::Initialize()
{
    bool expected = false;
    if (!g_initialized.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_relaxed))
    {
        // Already initialized (hot reload): keep the runtime state.
        return;
    }

    g_lastMarkerCheckMs.store(NowMs(), std::memory_order_relaxed);
    const bool env = EnvFlagValue();
    const bool marker = MarkerFilePresent();
    char line[160];
    std::snprintf(line, sizeof(line),
        "ptr_hygiene init enabled=%d env=%d marker=%d",
        (env || marker) ? 1 : 0,
        env ? 1 : 0,
        marker ? 1 : 0);
    CoopRuntimeLog::Write(line);
    if (env || marker)
        g_enabled.store(true, std::memory_order_release);
}

bool CoopPtrHygiene::Enabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

void CoopPtrHygiene::SetEnabled(bool enabled)
{
    SetEnabledInternal(enabled, true, "runtime_command");
}

void CoopPtrHygiene::Tick()
{
    // Steady state when tracing is pinned on (explicit command or env flag):
    // the marker cannot disable it, so only the atomic loads run.
    if (g_enabled.load(std::memory_order_relaxed) &&
        (g_explicitlySet.load(std::memory_order_relaxed) || EnvFlagValue()))
    {
        return;
    }

    const std::uint64_t now = NowMs();
    std::uint64_t last = g_lastMarkerCheckMs.load(std::memory_order_relaxed);
    if (now - last < kMarkerRecheckIntervalMs)
        return;
    if (!g_lastMarkerCheckMs.compare_exchange_weak(
            last, now, std::memory_order_relaxed, std::memory_order_relaxed))
        return;

    const bool marker = MarkerFilePresent();
    if (marker && !g_enabled.load(std::memory_order_relaxed))
    {
        SetEnabledInternal(true, false, "marker_file");
        return;
    }
    if (!marker &&
        g_enabled.load(std::memory_order_relaxed) &&
        !g_explicitlySet.load(std::memory_order_relaxed) &&
        !EnvFlagValue())
    {
        SetEnabledInternal(false, false, "marker_file_removed");
    }
}

void CoopPtrHygiene::LogPtr(const char* tag, const void* ptr)
{
    LogPtrWith(tag, ptr, nullptr);
}

void CoopPtrHygiene::LogPtrWith(const char* tag, const void* ptr, const char* extraTokens)
{
    if (!g_enabled.load(std::memory_order_relaxed))
        return;

    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(ptr);
    const int above32 = (address >> 32) != 0 ? 1 : 0;
    char line[256];
    if (extraTokens != nullptr && extraTokens[0] != '\0')
    {
        std::snprintf(line, sizeof(line),
            "ptr_hygiene tag=%s ptr=0x%016llX above32=%d %s",
            tag != nullptr ? tag : "?",
            static_cast<unsigned long long>(address),
            above32,
            extraTokens);
    }
    else
    {
        std::snprintf(line, sizeof(line),
            "ptr_hygiene tag=%s ptr=0x%016llX above32=%d",
            tag != nullptr ? tag : "?",
            static_cast<unsigned long long>(address),
            above32);
    }
    CoopRuntimeLog::Write(line);
}

void CoopPtrHygiene::CheckAbove32(const char* tag, const void* ptr)
{
    if (!g_enabled.load(std::memory_order_relaxed))
        return;

    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(ptr);
    if ((address >> 32) == 0)
        return;

    char line[128];
    std::snprintf(line, sizeof(line),
        "ptr_hygiene_above32 tag=%s ptr=0x%016llX",
        tag != nullptr ? tag : "?",
        static_cast<unsigned long long>(address));
    CoopRuntimeLog::WriteRateLimited(tag != nullptr ? tag : "ptr_hygiene_above32", line, 1.0, 3);
}

std::string CoopPtrHygiene::StatusReport()
{
    char report[128];
    std::snprintf(report, sizeof(report),
        "ptr_hygiene enabled=%d env=%d marker=%d explicit=%d",
        Enabled() ? 1 : 0,
        EnvFlagValue() ? 1 : 0,
        MarkerFilePresent() ? 1 : 0,
        g_explicitlySet.load(std::memory_order_relaxed) ? 1 : 0);
    return report;
}
