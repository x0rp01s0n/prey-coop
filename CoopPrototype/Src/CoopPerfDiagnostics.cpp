#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CoopPerfDiagnostics.h"

#include "CoopHookStats.h"
#include "CoopProtocol.h"
#include "CoopRuntimeConfig.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace CoopPerfDiagnostics
{
namespace
{
constexpr uint64_t kReportWindowNs = 2'000'000'000ull;
constexpr size_t kIntervalSampleCapacity = 128;
constexpr uintmax_t kMaxLogBytes = 4 * 1024 * 1024;

struct MetricCounters
{
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> totalNs{0};
    std::atomic<uint64_t> maxNs{0};
};

struct MetricSnapshot
{
    uint64_t calls = 0;
    uint64_t totalNs = 0;
    uint64_t maxNs = 0;
};

struct GuardBaseline
{
    uint64_t guardNs = 0;
    uint64_t vqCalls = 0;
    uint64_t vqNs = 0;
};

struct WindowCounters
{
    uint64_t updates = 0;
    uint64_t updateWallNs = 0;
    uint64_t updateWallMaxNs = 0;
    uint64_t intervalCount = 0;
    uint64_t intervalTotalNs = 0;
    uint64_t intervalMaxNs = 0;
    uint64_t slow33ms = 0;
    uint64_t slow50ms = 0;
    uint64_t slow100ms = 0;
    uint64_t frameTimeCount = 0;
    uint64_t frameTimeTotalUs = 0;
    uint64_t realFrameTimeCount = 0;
    uint64_t realFrameTimeTotalUs = 0;
    uint64_t gameReadyUpdates = 0;
    uint64_t sessionReadyUpdates = 0;
    uint64_t loadGuardUpdates = 0;
    uint64_t pendingInventoryTotal = 0;
    uint32_t pendingInventoryMax = 0;
    uint32_t updateFlagsOr = 0;
    uint8_t networkRoleMask = 0;
    const char* latestNetworkRole = "unknown";
    uint64_t sampleCount = 0;
    uint64_t sampleWrite = 0;
    std::array<uint64_t, kIntervalSampleCapacity> intervalSamples{};
};

GuardBaseline& CurrentGuardBaseline()
{
    static GuardBaseline baseline;
    return baseline;
}

uint64_t& PreviousReportNs()
{
    static uint64_t elapsedNs = 0;
    return elapsedNs;
}

uint64_t& PreviousWriteNs()
{
    static uint64_t elapsedNs = 0;
    return elapsedNs;
}

uint64_t& MaxReportNs()
{
    static uint64_t elapsedNs = 0;
    return elapsedNs;
}

uint64_t& MaxWriteNs()
{
    static uint64_t elapsedNs = 0;
    return elapsedNs;
}

bool IsEnabled()
{
    static const bool enabled = CoopRuntimeConfig::Flag("COOP_PERF_DIAGNOSTICS");
    return enabled;
}

uint64_t NowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::array<MetricCounters, static_cast<size_t>(Metric::Count)>& Counters()
{
    static std::array<MetricCounters, static_cast<size_t>(Metric::Count)> counters;
    return counters;
}

void Record(Metric metric, uint64_t elapsedNs)
{
    if (!IsEnabled())
        return;
    MetricCounters& counters = Counters()[static_cast<size_t>(metric)];
    counters.calls.fetch_add(1, std::memory_order_relaxed);
    counters.totalNs.fetch_add(elapsedNs, std::memory_order_relaxed);
    uint64_t previous = counters.maxNs.load(std::memory_order_relaxed);
    while (previous < elapsedNs && !counters.maxNs.compare_exchange_weak(
        previous, elapsedNs, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
}

MetricSnapshot TakeMetricSnapshot(Metric metric)
{
    MetricCounters& counters = Counters()[static_cast<size_t>(metric)];
    MetricSnapshot snapshot;
    snapshot.calls = counters.calls.exchange(0, std::memory_order_relaxed);
    snapshot.totalNs = counters.totalNs.exchange(0, std::memory_order_relaxed);
    snapshot.maxNs = counters.maxNs.exchange(0, std::memory_order_relaxed);
    return snapshot;
}

uint64_t ProcessId()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<uint64_t>(getpid());
#endif
}

std::ofstream g_output;
std::filesystem::path g_outputPath;
std::mutex g_outputMutex;
uint64_t g_outputSegment = 1;
std::atomic<bool> g_outputConfigured{false};

bool WriteHeader(std::ofstream& output)
{
    output << "CoopPrototype performance diagnostics\n"
           << "format=2\n"
           << "diagnosticVersion=lag-diagnostic-1\n"
           << "protocol=" << CoopProtocol::kProtocolVersion
           << " modBuild=" << CoopProtocol::kModBuild << "\n"
           << "pid=" << ProcessId() << "\n"
           << "window_ms=2000\n"
           << "file_rotates_at_bytes=4194304; segment increments on truncation\n"
           << "renderedFps=unavailable; updateHz is derived from MainUpdate-start intervals\n"
           << "interval_pXX uses at most 128 samples; slow-frame counts use every measured interval\n"
           << "restore_attempt is inclusive of nested work; recovery_journal_prepare includes file-write; do_not_sum scopes\n"
           << "previousReportMs/previousWriteMs measure the prior synchronous report and may contribute to this window's first interval\n"
           << "no identity, save paths, or inventory contents are recorded\n"
           << "---\n";
    return static_cast<bool>(output);
}

std::string CompactReport(std::string report)
{
    for (char& ch : report)
    {
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ')
            ch = '_';
    }
    return report;
}

uint64_t Percentile(const std::array<uint64_t, kIntervalSampleCapacity>& samples, size_t count, unsigned percentile)
{
    if (count == 0)
        return 0;
    const size_t index = std::min(count - 1, (count * percentile + 99) / 100 - 1);
    return samples[index];
}

void FlushWindow(uint64_t nowNs, uint64_t windowStartNs, WindowCounters& window)
{
    const uint64_t reportStartNs = NowNs();
    if (window.updates == 0)
        return;

    std::array<uint64_t, kIntervalSampleCapacity> samples{};
    const size_t sampleCount = static_cast<size_t>(std::min<uint64_t>(window.sampleCount, kIntervalSampleCapacity));
    const uint64_t firstSample = window.sampleCount > kIntervalSampleCapacity
        ? window.sampleWrite % kIntervalSampleCapacity
        : 0;
    for (size_t i = 0; i < sampleCount; ++i)
        samples[i] = window.intervalSamples[(firstSample + i) % kIntervalSampleCapacity];
    std::sort(samples.begin(), samples.begin() + sampleCount);

    const double wallMs = static_cast<double>(nowNs - windowStartNs) / 1'000'000.0;
    const double updateHz = window.intervalTotalNs == 0
        ? 0.0
        : static_cast<double>(window.intervalCount) * 1'000'000'000.0 / static_cast<double>(window.intervalTotalNs);
    const double updateWallAvgMs = window.updates == 0
        ? 0.0
        : static_cast<double>(window.updateWallNs) / static_cast<double>(window.updates) / 1'000'000.0;
    const double frameTimeAvgMs = window.frameTimeCount == 0
        ? 0.0
        : static_cast<double>(window.frameTimeTotalUs) / static_cast<double>(window.frameTimeCount) / 1000.0;
    const double realFrameTimeAvgMs = window.realFrameTimeCount == 0
        ? 0.0
        : static_cast<double>(window.realFrameTimeTotalUs) / static_cast<double>(window.realFrameTimeCount) / 1000.0;

    const MetricSnapshot restore = TakeMetricSnapshot(Metric::InventoryRestoreAttempt);
    const MetricSnapshot serialize = TakeMetricSnapshot(Metric::SidecarPayloadSerialize);
    const MetricSnapshot sidecarWrite = TakeMetricSnapshot(Metric::SidecarAtomicWrite);
    const MetricSnapshot recoveryPrepare = TakeMetricSnapshot(Metric::RecoveryJournalPrepare);
    const MetricSnapshot recoveryWrite = TakeMetricSnapshot(Metric::RecoveryJournalAtomicWrite);
    const CoopRuntimeGuards::GuardTelemetryTotals guards = CoopRuntimeGuards::GetGuardTelemetryTotals();
    const uint64_t vqCalls = CoopRuntimeGuards::GetGuardVqCalls();
    const uint64_t vqNs = CoopRuntimeGuards::GetGuardVqTotalNs();
    GuardBaseline& baseline = CurrentGuardBaseline();
    const uint64_t guardWindowNs = guards.guardedCallTotalNs - baseline.guardNs;
    const uint64_t vqCallWindow = vqCalls - baseline.vqCalls;
    const uint64_t vqNsWindow = vqNs - baseline.vqNs;
    baseline = {guards.guardedCallTotalNs, vqCalls, vqNs};

    const std::string hooks = CompactReport(CoopHookStats::BuildReport());
    const std::string guardOps = CompactReport(CoopRuntimeGuards::GetGuardOpReport());
    CoopHookStats::ResetAll();
    const auto writeMetric = [](std::ostream& out, const char* name, const MetricSnapshot& metric)
    {
        out << name << "Calls=" << metric.calls
            << ' ' << name << "AvgUs=" << (metric.calls ? metric.totalNs / metric.calls / 1000 : 0)
            << ' ' << name << "MaxUs=" << metric.maxNs / 1000;
    };

    std::lock_guard<std::mutex> lock(g_outputMutex);
    if (g_outputConfigured && g_output)
    {
        std::error_code error;
        const uintmax_t currentSize = std::filesystem::file_size(g_outputPath, error);
        if (!error && currentSize >= kMaxLogBytes)
        {
            g_output.close();
            g_output.open(g_outputPath, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!g_output || !WriteHeader(g_output))
                g_outputConfigured = false;
            else
                ++g_outputSegment;
        }
        if (g_outputConfigured && g_output)
        {
            static uint64_t windowId = 0;
            const uint64_t id = ++windowId;
            const uint64_t writeStartNs = NowNs();
            g_output << std::fixed << std::setprecision(2)
                     << "window=" << id
                     << " segment=" << g_outputSegment
                     << " wallMs=" << wallMs
                     << " updates=" << window.updates
                     << " updateHz=" << updateHz
                     << " intervalSamples=" << sampleCount
                     << " intervalP50Ms=" << Percentile(samples, sampleCount, 50) / 1'000'000.0
                     << " intervalP95Ms=" << Percentile(samples, sampleCount, 95) / 1'000'000.0
                     << " intervalP99Ms=" << Percentile(samples, sampleCount, 99) / 1'000'000.0
                     << " intervalSampleMaxMs=" << (sampleCount ? samples[sampleCount - 1] / 1'000'000.0 : 0.0)
                     << " intervalMaxStallMs=" << window.intervalMaxNs / 1'000'000.0
                     << " slow33ms=" << window.slow33ms
                     << " slow50ms=" << window.slow50ms
                     << " slow100ms=" << window.slow100ms
                     << " mainUpdateAvgMs=" << updateWallAvgMs
                     << " mainUpdateMaxMs=" << window.updateWallMaxNs / 1'000'000.0
                     << " engineFrameAvgMs=" << frameTimeAvgMs
                     << " engineRealFrameAvgMs=" << realFrameTimeAvgMs
                     << " networkRole=" << window.latestNetworkRole
                     << " networkRoleMask=" << static_cast<unsigned>(window.networkRoleMask)
                     << " gameReadyUpdates=" << window.gameReadyUpdates
                     << " sessionReadyUpdates=" << window.sessionReadyUpdates
                     << " loadGuardUpdates=" << window.loadGuardUpdates
                     << " pendingInventoryAvg=" << (window.updates ? window.pendingInventoryTotal / window.updates : 0)
                     << " pendingInventoryMax=" << window.pendingInventoryMax
                     << " updateFlagsOr=" << window.updateFlagsOr
                     << " previousReportMs=" << PreviousReportNs() / 1'000'000.0
                     << " previousReportMaxMs=" << MaxReportNs() / 1'000'000.0
                     << " previousWriteMs=" << PreviousWriteNs() / 1'000'000.0
                     << " previousWriteMaxMs=" << MaxWriteNs() / 1'000'000.0
                     << " renderedFps=unavailable\n";
            g_output << "inventoryRestoreAttemptInclusive ";
            writeMetric(g_output, "restore", restore);
            g_output << "\nsidecarPayloadEmit ";
            writeMetric(g_output, "serialize", serialize);
            g_output << "\nsidecarAtomicWrite ";
            writeMetric(g_output, "sidecarWrite", sidecarWrite);
            g_output << "\nrecoveryJournalPrepareInclusive ";
            writeMetric(g_output, "recoveryPrepare", recoveryPrepare);
            g_output << "\nrecoveryJournalAtomicWrite ";
            writeMetric(g_output, "recoveryWrite", recoveryWrite);
            g_output << "\nguardWindowNs=" << guardWindowNs
                     << " countedVQCalls=" << vqCallWindow
                     << " countedVQNs=" << vqNsWindow
                     << " effectiveReadMode=" << CoopRuntimeGuards::RuntimeReadEffectiveModeName()
                     << " effectiveObjectProbeMode=" << CoopRuntimeGuards::RuntimeObjectProbeEffectiveModeName()
                     << " hookStatsCurrentWindow=" << (hooks.empty() ? "none" : hooks)
                     << " guardOpsCumulativeAtWindow=" << (guardOps.empty() ? "none" : guardOps)
                     << "\n---\n";
            g_output.flush();
            PreviousWriteNs() = NowNs() - writeStartNs;
            MaxWriteNs() = std::max(MaxWriteNs(), PreviousWriteNs());
            if (!g_output)
            {
                g_outputConfigured = false;
                g_output.close();
            }
        }
    }

    PreviousReportNs() = NowNs() - reportStartNs;
    MaxReportNs() = std::max(MaxReportNs(), PreviousReportNs());

    window = {};
}

WindowCounters& CurrentWindow()
{
    static WindowCounters window;
    return window;
}

uint64_t& WindowStartNs()
{
    static uint64_t startNs = 0;
    return startNs;
}

uint64_t& LastUpdateStartNs()
{
    static uint64_t lastNs = 0;
    return lastNs;
}
}

bool Enabled()
{
    return IsEnabled();
}

bool Capturing()
{
    return IsEnabled() && g_outputConfigured.load(std::memory_order_acquire);
}

bool ConfigureOutputDirectory(const std::filesystem::path& directory)
{
    if (!IsEnabled() || directory.empty())
        return false;

    std::lock_guard<std::mutex> lock(g_outputMutex);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const std::filesystem::path outputPath = directory / ("coop_perf_diagnostics_" + std::to_string(ProcessId()) + ".log");
    if (g_outputConfigured.load(std::memory_order_acquire) && g_outputPath == outputPath)
        return true;
    g_outputConfigured.store(false, std::memory_order_release);
    if (g_output.is_open())
        g_output.close();
    g_output.clear();
    g_outputPath = outputPath;
    g_output.open(g_outputPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!g_output || !WriteHeader(g_output))
    {
        g_output.close();
        g_outputPath.clear();
        return false;
    }
    g_output.flush();
    g_outputConfigured = static_cast<bool>(g_output);
    g_outputSegment = 1;
    return g_outputConfigured;
}

ScopedTimer::ScopedTimer(Metric metric)
    : m_metric(metric)
    , m_startNs(Capturing() ? NowNs() : 0)
{
}

ScopedTimer::~ScopedTimer()
{
    Stop();
}

void ScopedTimer::Stop()
{
    if (m_startNs == 0)
        return;
    Record(m_metric, NowNs() - m_startNs);
    m_startNs = 0;
}

UpdateScope::UpdateScope(float frameTime, float realFrameTime, unsigned updateFlags)
    : m_startNs(Capturing() ? NowNs() : 0)
    , m_updateFlags(updateFlags)
{
    if (m_startNs == 0)
        return;

    WindowCounters& window = CurrentWindow();
    if (WindowStartNs() == 0)
    {
        WindowStartNs() = m_startNs;
        const CoopRuntimeGuards::GuardTelemetryTotals guards = CoopRuntimeGuards::GetGuardTelemetryTotals();
        CurrentGuardBaseline() = {
            guards.guardedCallTotalNs,
            CoopRuntimeGuards::GetGuardVqCalls(),
            CoopRuntimeGuards::GetGuardVqTotalNs()};
    }

    const uint64_t previousStart = LastUpdateStartNs();
    LastUpdateStartNs() = m_startNs;
    ++window.updates;

    if (previousStart != 0 && m_startNs >= previousStart)
    {
        const uint64_t intervalNs = m_startNs - previousStart;
        ++window.intervalCount;
        window.intervalTotalNs += intervalNs;
        window.intervalMaxNs = std::max(window.intervalMaxNs, intervalNs);
        if (intervalNs > 33'000'000ull)
            ++window.slow33ms;
        if (intervalNs > 50'000'000ull)
            ++window.slow50ms;
        if (intervalNs > 100'000'000ull)
            ++window.slow100ms;
        window.intervalSamples[window.sampleWrite % kIntervalSampleCapacity] = intervalNs;
        ++window.sampleWrite;
        ++window.sampleCount;
    }

    if (std::isfinite(frameTime) && frameTime >= 0.0f)
    {
        ++window.frameTimeCount;
        window.frameTimeTotalUs += static_cast<uint64_t>(frameTime * 1'000'000.0f);
    }
    if (std::isfinite(realFrameTime) && realFrameTime >= 0.0f)
    {
        ++window.realFrameTimeCount;
        window.realFrameTimeTotalUs += static_cast<uint64_t>(realFrameTime * 1'000'000.0f);
    }
    m_updateFlags = updateFlags;
}

void UpdateScope::SetContext(
    const char* networkRole,
    bool gameReady,
    bool sessionReady,
    bool loadGuard,
    uint32_t pendingInventory)
{
    if (m_startNs == 0)
        return;

    WindowCounters& window = CurrentWindow();
    const char* role = networkRole && networkRole[0] ? networkRole : "unknown";
    window.latestNetworkRole = role;
    if (role[0] == 'o')
        window.networkRoleMask |= 1u;
    else if (role[0] == 'h')
        window.networkRoleMask |= 2u;
    else if (role[0] == 'c')
        window.networkRoleMask |= 4u;
    if (gameReady)
        ++window.gameReadyUpdates;
    if (sessionReady)
        ++window.sessionReadyUpdates;
    if (loadGuard)
        ++window.loadGuardUpdates;
    window.pendingInventoryTotal += pendingInventory;
    window.pendingInventoryMax = std::max(window.pendingInventoryMax, pendingInventory);
    window.updateFlagsOr |= m_updateFlags;
}

UpdateScope::~UpdateScope()
{
    if (m_startNs == 0)
        return;

    const uint64_t nowNs = NowNs();
    WindowCounters& window = CurrentWindow();
    const uint64_t updateNs = nowNs - m_startNs;
    window.updateWallNs += updateNs;
    window.updateWallMaxNs = std::max(window.updateWallMaxNs, updateNs);
    if (nowNs - WindowStartNs() >= kReportWindowNs)
    {
        FlushWindow(nowNs, WindowStartNs(), window);
        WindowStartNs() = nowNs;
    }
}
}
